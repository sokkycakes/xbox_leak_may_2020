/*
 * psh_modes: render hand-built pixel shader defs through psh_translate() on
 * the real driver and compare the centre pixel with values worked out by
 * hand from the NV2A / D3D semantics: every texture shader mode (projective
 * 2D, passthru, clip plane, bump env with each matrix entry, luminance
 * saturation, dependent AR/GB, the dot product chains with each dot
 * mapping, STR 3D / cube, diffuse and specular reflection, Z/W depth), the
 * final combiner's V1+R0 sum (clamp, complements) and E*F product, LSB/MSB
 * mux, per-stage constants, blue-to-alpha and the fog modes.
 *
 * Fixed-function vertex processing feeds constant texture coordinates
 * (glMultiTexCoord4f), gl_Color, gl_SecondaryColor and the fog coordinate.
 * Textures are uploaded as raw RGBA8 bytes, the way the host uploads them
 * (bump formats with du/dv in r/g and luminance in b; HILO as a:r / g:b).
 *
 *   psh_modes
 *
 * Needs a display (xvfb-run).  Build (32-bit, like the host):
 *   gcc -m32 -std=gnu11 -Wall -I src/hle -I/usr/include/SDL2 -D_REENTRANT tests/psh_modes.c \
 *       src/hle/psh.c -lSDL2 -lGL -lm -o psh_modes
 */
#include <SDL.h>
#include <SDL_opengl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_shaders.h"

#define PS_TEXTUREMODES(t0, t1, t2, t3) (((t3) << 15) | ((t2) << 10) | ((t1) << 5) | (t0))
#define PS_DOTMAPPING(t0, t1, t2, t3) (((t3) << 8) | ((t2) << 4) | (t1))
#define PS_COMPAREMODE(t0, t1, t2, t3) (((t3) << 12) | ((t2) << 8) | ((t1) << 4) | (t0))
#define PS_INPUTTEXTURE(t0, t1, t2, t3) (((t3) << 20) | ((t2) << 16))
#define PS_COMBINERCOUNT(count, flags) (((flags) << 8) | (count))
#define PS_COMBINERINPUTS(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define PS_COMBINEROUTPUTS(ab, cd, mux_sum, flags) (((flags) << 12) | ((mux_sum) << 8) | ((ab) << 4) | (cd))

enum {
    TM_NONE, TM_PROJECT2D, TM_PROJECT3D, TM_CUBEMAP, TM_PASSTHRU, TM_CLIPPLANE, TM_BUMPENVMAP,
    TM_BUMPENVMAP_LUM, TM_BRDF, TM_DOT_ST, TM_DOT_ZW, TM_DOT_RFLCT_DIFF, TM_DOT_RFLCT_SPEC,
    TM_DOT_STR_3D, TM_DOT_STR_CUBE, TM_DPNDNT_AR, TM_DPNDNT_GB, TM_DOTPRODUCT, TM_DOT_RFLCT_SPEC_CONST,
};
enum { ZERO = 0, C0 = 1, C1 = 2, FOG = 3, V0 = 4, V1 = 5, T0 = 8, T1 = 9, T2 = 10, T3 = 11, R0 = 12, R1 = 13,
       V1R0_SUM = 14, EF_PROD = 15, DISCARD = 0 };
enum { UNSIGNED_INVERT = 0x20, EXPAND_NORMAL = 0x40, SIGNED_IDENTITY = 0xc0 };
enum { RGB = 0, BLUE = 0, ALPHA = 0x10 };
enum { IDENTITY = 0, AB_BLUE_TO_ALPHA = 0x80, AB_DOT_PRODUCT = 0x02, AB_CD_MUX = 0x04 };
enum { MUX_MSB = 1, UNIQUE_C0 = 0x10, UNIQUE_C1 = 0x100 };
enum { CLAMP_SUM = 0x80, COMPLEMENT_V1 = 0x40, COMPLEMENT_R0 = 0x20 };
enum { DM_01, DM_D3D, DM_GL, DM_NV, DM_HILO1, DM_HEMI_D3D, DM_HEMI_GL, DM_HEMI };
#define ONE (ZERO | UNSIGNED_INVERT)

/* render state slots */
enum { RS_ALPHAIN = 0, RS_FCABCD = 8, RS_FCEFG = 9, RS_ALPHAOUT = 26, RS_RGBIN = 34, RS_COMPARE = 42,
       RS_RGBOUT = 45, RS_COUNT = 53, RS_DOTMAP = 55, RS_INPUTTEX = 56, RS_FOGENABLE = 82,
       RS_FOGTABLEMODE = 83, RS_TEXMODES = 117 };

static GLuint (APIENTRY *p_glCreateShader)(GLenum);
static void (APIENTRY *p_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (APIENTRY *p_glCompileShader)(GLuint);
static void (APIENTRY *p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (APIENTRY *p_glCreateProgram)(void);
static void (APIENTRY *p_glAttachShader)(GLuint, GLuint);
static void (APIENTRY *p_glLinkProgram)(GLuint);
static void (APIENTRY *p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_glUseProgram)(GLuint);
static void (APIENTRY *p_glDeleteProgram)(GLuint);
static void (APIENTRY *p_glDeleteShader)(GLuint);
static GLint (APIENTRY *p_glGetUniformLocation)(GLuint, const char *);
static void (APIENTRY *p_glUniform1i)(GLint, GLint);
static void (APIENTRY *p_glUniform2fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *p_glUniform4fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *p_glActiveTexture)(GLenum);
static void (APIENTRY *p_glMultiTexCoord4f)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
static void (APIENTRY *p_glFogCoordf)(GLfloat);
static void (APIENTRY *p_glSecondaryColor3f)(GLfloat, GLfloat, GLfloat);
static void (APIENTRY *p_glTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                       const void *);
#define GL_FOG_COORD_SRC_ 0x8450
#define GL_FOG_COORD_ 0x8451

#define W 16
#define H 16

static int failures, passes;

/* ---- per-draw inputs ---- */
typedef struct {
    float tc[4][4];
    float color[4], spec[3], fogcoord;
    float c0[8][4], c1[8][4], fc0[4], fc1[4];
    float bump_env[4][4], bump_lum[4][2];
    float fog_start, fog_end, fog_density;
} inputs;

static void inputs_default(inputs *in)
{
    memset(in, 0, sizeof *in);
    for (int i = 0; i < 4; i++) in->tc[i][3] = 1;
    for (int i = 0; i < 4; i++) in->color[i] = 1;
}

/* ---- textures: unit u holds one of each kind ---- */
static GLuint tex_2d[4], tex_cube[4], tex_3d[4];

static void set_tex2d(int unit, int w, int h, const uint8_t *rgba)
{
    if (!tex_2d[unit]) glGenTextures(1, &tex_2d[unit]);
    p_glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex_2d[unit]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

static void set_texel(int unit, int r, int g, int b, int a)
{
    uint8_t px[4] = { r, g, b, a };
    set_tex2d(unit, 1, 1, px);
}

/* 4x4 texture whose texel (x, y) is grid_color(x, y) */
static void grid_color(int x, int y, uint8_t *p)
{
    p[0] = 16 + x * 64;
    p[1] = 16 + y * 64;
    p[2] = 250 - x * 20 - y * 50;
    p[3] = 255 - x * 8 - y * 32;
}

static void set_grid(int unit)
{
    uint8_t px[4][4][4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) grid_color(x, y, px[y][x]);
    set_tex2d(unit, 4, 4, &px[0][0][0]);
}

static const uint8_t face_color[6][4] = {
    { 255, 0, 0, 255 },     /* +X */
    { 0, 255, 255, 255 },   /* -X */
    { 0, 0, 255, 255 },     /* +Y */
    { 255, 255, 0, 255 },   /* -Y */
    { 255, 0, 255, 255 },   /* +Z */
    { 128, 128, 128, 255 }, /* -Z */
};

static void set_cube(int unit)
{
    if (!tex_cube[unit]) glGenTextures(1, &tex_cube[unit]);
    p_glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, tex_cube[unit]);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    for (int f = 0; f < 6; f++) {
        uint8_t px[2][2][4];
        for (int i = 0; i < 4; i++) memcpy(px[i / 2][i % 2], face_color[f], 4);
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
}

static void vol_color(int x, int y, int z, uint8_t *p)
{
    p[0] = 30 + x * 200;
    p[1] = 30 + y * 200;
    p[2] = 30 + z * 200;
    p[3] = 255;
}

static void set_vol(int unit)
{
    uint8_t px[2][2][2][4];
    for (int z = 0; z < 2; z++)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) vol_color(x, y, z, px[z][y][x]);
    if (!tex_3d[unit]) glGenTextures(1, &tex_3d[unit]);
    p_glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_3D, tex_3d[unit]);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    p_glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 2, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

/* ---- running one shader ---- */
static unsigned char pixel[4];
static float depth_value;

static GLuint make_program(const char *src)
{
    GLuint fs = p_glCreateShader(GL_FRAGMENT_SHADER);
    p_glShaderSource(fs, 1, &src, NULL);
    p_glCompileShader(fs);
    GLint ok = 0;
    p_glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        p_glGetShaderInfoLog(fs, sizeof log, NULL, log);
        printf("compile failed:\n%s\n%s\n", log, src);
        return 0;
    }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, fs);
    p_glLinkProgram(prog);
    p_glDeleteShader(fs);
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { printf("link failed\n"); return 0; }
    return prog;
}

static void uniform4(GLuint prog, const char *name, int n, const float *v)
{
    GLint loc = p_glGetUniformLocation(prog, name);
    if (loc >= 0) p_glUniform4fv(loc, n, v);
}

/* Translate rs, draw a full-window quad with `in`, read the centre pixel
   and depth.  Returns 0 if the shader could not be built. */
static int draw(const char *name, const uint32_t *rs, const inputs *in)
{
    char *src = psh_translate(rs);
    if (!src) { printf("FAIL %s: psh_translate returned NULL\n", name); failures++; return 0; }
    GLuint prog = make_program(src);
    free(src);
    if (!prog) { printf("FAIL %s: shader did not build\n", name); failures++; return 0; }

    glViewport(0, 0, W, H);
    glClearColor(0, 1, 0, 1);   /* green: a discarded fragment */
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    p_glUseProgram(prog);
    for (int u = 0; u < 4; u++) {
        char n[8];
        snprintf(n, sizeof n, "tex%d", u);
        GLint loc = p_glGetUniformLocation(prog, n);
        if (loc >= 0) p_glUniform1i(loc, u);
        snprintf(n, sizeof n, "cube%d", u);
        loc = p_glGetUniformLocation(prog, n);
        if (loc >= 0) p_glUniform1i(loc, u);
        snprintf(n, sizeof n, "vol%d", u);
        loc = p_glGetUniformLocation(prog, n);
        if (loc >= 0) p_glUniform1i(loc, u);
    }
    static const float ones[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    uniform4(prog, "tex_scale", 4, ones);
    uniform4(prog, "c0", 8, &in->c0[0][0]);
    uniform4(prog, "c1", 8, &in->c1[0][0]);
    uniform4(prog, "fc0", 1, in->fc0);
    uniform4(prog, "fc1", 1, in->fc1);
    uniform4(prog, "bump_env", 4, &in->bump_env[0][0]);
    GLint loc = p_glGetUniformLocation(prog, "bump_lum");
    if (loc >= 0) p_glUniform2fv(loc, 4, &in->bump_lum[0][0]);

    static const float fogcol[4] = { 0.2f, 0.4f, 0.6f, 1 };
    glFogfv(GL_FOG_COLOR, fogcol);
    glFogf(GL_FOG_START, in->fog_start);
    glFogf(GL_FOG_END, in->fog_end);
    glFogf(GL_FOG_DENSITY, in->fog_density);
    glFogi(GL_FOG_COORD_SRC_, GL_FOG_COORD_);

    glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++) {
        float x = (i == 1 || i == 2) ? 1.0f : -1.0f, y = (i >= 2) ? 1.0f : -1.0f;
        for (int u = 0; u < 4; u++)
            p_glMultiTexCoord4f(GL_TEXTURE0 + u, in->tc[u][0], in->tc[u][1], in->tc[u][2], in->tc[u][3]);
        glColor4fv(in->color);
        p_glSecondaryColor3f(in->spec[0], in->spec[1], in->spec[2]);
        p_glFogCoordf(in->fogcoord);
        glVertex3f(x, y, 0.0f);   /* window depth 0.5 */
    }
    glEnd();
    p_glUseProgram(0);
    p_glDeleteProgram(prog);
    glFinish();
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glReadPixels(W / 2, H / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth_value);
    return 1;
}

static void expect_rgba(const char *name, int r, int g, int b, int a, int tol)
{
    const unsigned char *p = pixel;
    if (abs(p[0] - r) > tol || abs(p[1] - g) > tol || abs(p[2] - b) > tol || (a >= 0 && abs(p[3] - a) > tol)) {
        printf("FAIL %-34s got %3d %3d %3d %3d, expected %3d %3d %3d %3d\n", name, p[0], p[1], p[2], p[3], r, g,
               b, a);
        failures++;
    } else {
        printf("ok   %-34s %3d %3d %3d %3d\n", name, p[0], p[1], p[2], p[3]);
        passes++;
    }
}

static void expect_color(const char *name, const uint8_t *c)
{
    expect_rgba(name, c[0], c[1], c[2], c[3], 2);
}

static void expect_grid(const char *name, int x, int y)
{
    uint8_t c[4];
    grid_color(x, y, c);
    expect_color(name, c);
}

static int to8(float f)
{
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    return (int)(f * 255.0f + 0.5f);
}

/* ---- def builders ---- */

/* No combiner stages; the final combiner outputs register `reg` (rgb) and
   its alpha. */
static void show_def(uint32_t *rs, int reg, uint32_t modes)
{
    memset(rs, 0, 128 * sizeof *rs);
    rs[RS_TEXMODES] = modes;
    rs[RS_FCABCD] = PS_COMBINERINPUTS(ZERO, ZERO, ZERO, reg | RGB);
    rs[RS_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, reg | ALPHA, 0);
}

/* ---- the tests ---- */

static void test_project_passthru_clip(void)
{
    uint32_t rs[128];
    inputs in;

    set_grid(1);
    show_def(rs, T1, PS_TEXTUREMODES(TM_NONE, TM_PROJECT2D, TM_NONE, TM_NONE));
    inputs_default(&in);
    in.tc[1][0] = 0.625f * 2; in.tc[1][1] = 0.375f * 2; in.tc[1][3] = 2;
    if (draw("project2d q=2", rs, &in)) expect_grid("project2d q=2 -> texel (2,1)", 2, 1);

    show_def(rs, T0, PS_TEXTUREMODES(TM_PASSTHRU, TM_NONE, TM_NONE, TM_NONE));
    inputs_default(&in);
    in.tc[0][0] = 1.5f; in.tc[0][1] = -0.5f; in.tc[0][2] = 0.25f; in.tc[0][3] = 0.75f;
    if (draw("passthru", rs, &in)) expect_rgba("passthru clamps to 0..1", 255, 0, 64, 191, 2);

    /* stage 0 clip plane, output fc0 when the fragment survives */
    memset(rs, 0, sizeof rs);
    rs[RS_TEXMODES] = PS_TEXTUREMODES(TM_CLIPPLANE, TM_NONE, TM_NONE, TM_NONE);
    rs[RS_FCABCD] = PS_COMBINERINPUTS(ZERO, ZERO, ZERO, C0 | RGB);
    rs[RS_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, C0 | ALPHA, 0);
    inputs_default(&in);
    in.fc0[0] = 0.2f; in.fc0[1] = 0.4f; in.fc0[2] = 0.6f; in.fc0[3] = 0.8f;
    in.tc[0][0] = 0.1f; in.tc[0][1] = 0.1f; in.tc[0][2] = -0.1f; in.tc[0][3] = 0.1f;
    if (draw("clipplane r<0", rs, &in)) expect_rgba("clipplane all LT, r<0 -> killed", 0, 255, 0, 255, 1);
    rs[RS_COMPARE] = PS_COMPAREMODE(4, 0, 0, 0);
    if (draw("clipplane r GE", rs, &in)) expect_rgba("clipplane R_GE, r<0 -> drawn", 51, 102, 153, 204, 2);
    in.tc[0][2] = 0.1f;
    if (draw("clipplane r GE kill", rs, &in)) expect_rgba("clipplane R_GE, r>=0 -> killed", 0, 255, 0, 255, 1);
    rs[RS_COMPARE] = PS_COMPAREMODE(1 | 2 | 4 | 8, 0, 0, 0);
    in.tc[0][0] = in.tc[0][1] = in.tc[0][2] = in.tc[0][3] = -0.1f;
    if (draw("clipplane all GE", rs, &in)) expect_rgba("clipplane all GE, all <0 -> drawn", 51, 102, 153, 204, 2);
}

static void test_bumpenv(void)
{
    uint32_t rs[128];
    inputs in;

    /* du = +64, dv = -64 (signed bytes) -> +-64/127 */
    set_texel(0, 0x40, 0xC0, 0x00, 0xFF);
    set_grid(1);
    show_def(rs, T1, PS_TEXTUREMODES(TM_PROJECT2D, TM_BUMPENVMAP, TM_NONE, TM_NONE));
    inputs_default(&in);
    in.tc[1][0] = 0.125f; in.tc[1][1] = 0.125f;
    /* s' = s + m00*du + m10*dv, t' = t + m01*du + m11*dv */
    in.bump_env[1][0] = 0.5f;   /* m00: s += 0.25 */
    if (draw("bumpenv m00", rs, &in)) expect_grid("bumpenv m00*du -> s", 1, 0);
    in.bump_env[1][0] = 0; in.bump_env[1][1] = 0.5f;   /* m01: t += 0.25 */
    if (draw("bumpenv m01", rs, &in)) expect_grid("bumpenv m01*du -> t", 0, 1);
    in.bump_env[1][1] = 0; in.bump_env[1][2] = 0.5f;   /* m10: s -= 0.25, wraps */
    if (draw("bumpenv m10", rs, &in)) expect_grid("bumpenv m10*dv -> s (wraps)", 3, 0);
    in.bump_env[1][2] = 0; in.bump_env[1][3] = -0.5f;  /* m11: t += 0.25 */
    if (draw("bumpenv m11", rs, &in)) expect_grid("bumpenv m11*dv -> t", 0, 1);

    /* luminance at stage 2 from stage 1 (PSInputTexture), L = 128/255 */
    set_texel(1, 0, 0, 128, 255);
    set_texel(2, 100, 150, 200, 200);
    show_def(rs, T2, PS_TEXTUREMODES(TM_NONE, TM_PROJECT2D, TM_BUMPENVMAP_LUM, TM_NONE));
    rs[RS_INPUTTEX] = PS_INPUTTEXTURE(0, 0, 1, 0);
    inputs_default(&in);
    in.tc[2][0] = 0.5f; in.tc[2][1] = 0.5f;
    float L = 128 / 255.0f;
    in.bump_lum[2][0] = 0.5f; in.bump_lum[2][1] = 0.25f;
    float f = 0.5f * L + 0.25f;
    if (draw("bumpenv lum 0.5L+0.25", rs, &in))
        expect_rgba("bumpenv_lum scales the texel", to8(100 / 255.0f * f), to8(150 / 255.0f * f),
                    to8(200 / 255.0f * f), to8(200 / 255.0f * f), 2);
    in.bump_lum[2][0] = 4.0f; in.bump_lum[2][1] = 0;   /* factor ~2: saturates */
    if (draw("bumpenv lum 4L", rs, &in))
        expect_rgba("bumpenv_lum saturates at 1", to8(100 / 255.0f * 4 * L), 255, 255, 255, 2);
}

static void test_dependent(void)
{
    uint32_t rs[128];
    inputs in;

    set_grid(1);
    set_grid(3);
    /* AR: (a, r) = (159, 32)/255 -> (0.62, 0.13) -> texel (2, 0) */
    set_texel(0, 32, 0, 0, 159);
    show_def(rs, T1, PS_TEXTUREMODES(TM_PROJECT2D, TM_DPNDNT_AR, TM_NONE, TM_NONE));
    inputs_default(&in);
    if (draw("dependent ar", rs, &in)) expect_grid("dpndnt_ar (a, r)", 2, 0);
    /* GB at stage 3 from stage 2: (g, b) = (223, 96)/255 -> texel (3, 1) */
    set_texel(2, 0, 223, 96, 0);
    show_def(rs, T3, PS_TEXTUREMODES(TM_NONE, TM_NONE, TM_PROJECT2D, TM_DPNDNT_GB));
    rs[RS_INPUTTEX] = PS_INPUTTEXTURE(0, 0, 0, 2);
    if (draw("dependent gb", rs, &in)) expect_grid("dpndnt_gb (g, b), input t2", 3, 1);
}

/* stage 1 DOTPRODUCT + stage 2 DOT_ST on the normal in t0 */
static void dot_st_case(const char *name, const uint8_t *texel, int map, const float *tc1, const float *tc2, int x,
                        int y)
{
    uint32_t rs[128];
    inputs in;
    set_texel(0, texel[0], texel[1], texel[2], texel[3]);
    set_grid(2);
    show_def(rs, T2, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOT_ST, TM_NONE));
    rs[RS_DOTMAP] = PS_DOTMAPPING(0, map, map, 0);
    inputs_default(&in);
    memcpy(in.tc[1], tc1, 3 * sizeof(float));
    memcpy(in.tc[2], tc2, 3 * sizeof(float));
    if (draw(name, rs, &in)) expect_grid(name, x, y);
}

static void test_dot_chains(void)
{
    {
        /* (1, 0.502, 0): dot1 = 0.3 + 0.251 = 0.551, dot2 = 0.1 + 0.201 = 0.301 */
        static const uint8_t t[4] = { 255, 128, 0, 255 };
        static const float a[3] = { 0.3f, 0.5f, 0.7f }, b[3] = { 0.1f, 0.4f, 0.9f };
        dot_st_case("dot_st zero_to_one", t, DM_01, a, b, 2, 1);
    }
    {
        /* (x - 128) / 127: (1, 0, -1.008); dot1 = 0.4 - 0.050 = 0.350, dot2 = 0.9 - 0.101 = 0.799 */
        static const uint8_t t[4] = { 255, 128, 0, 255 };
        static const float a[3] = { 0.4f, 0.9f, 0.05f }, b[3] = { 0.9f, 0.0f, 0.1f };
        dot_st_case("dot_st minus1_to_1_d3d", t, DM_D3D, a, b, 1, 3);
    }
    {
        /* NV: two's complement / 127: (0.504, -0.504, 1); dot1 = 0.504, dot2 = 0.504 + 0.2 */
        static const uint8_t t[4] = { 0x40, 0xC0, 0x7F, 255 };
        static const float a[3] = { 1, 0, 0 }, b[3] = { 0, -1, 0.2f };
        dot_st_case("dot_st minus1_to_1 (nv)", t, DM_NV, a, b, 2, 2);
    }
    {
        /* GL: (2c + 1) / 255: (0.506, -0.498, 1); dot2 = 0.498 -> row 1 (NV would give row 2) */
        static const uint8_t t[4] = { 0x40, 0xC0, 0x7F, 255 };
        static const float a[3] = { 1, 0, 0 }, b[3] = { 0, -1, 0 };
        dot_st_case("dot_st minus1_to_1_gl", t, DM_GL, a, b, 2, 1);
    }
    {
        /* HILO_1: hi = a:r = 0x8000, lo = g:b = 0x4000 -> (0.5, 0.25, 1) */
        static const uint8_t t[4] = { 0x00, 0x40, 0x00, 0x80 };
        static const float a[3] = { 1, 0, 0.1f }, b[3] = { 0, 1, 0.4f };
        dot_st_case("dot_st hilo_1", t, DM_HILO1, a, b, 2, 2);
    }
    {
        /* HILO hemisphere (nv): hi = 0x2000 -> 0.25, lo = 0xE000 -> -0.25, z = 0.935 */
        static const uint8_t t[4] = { 0x00, 0xE0, 0x00, 0x20 };
        static const float a[3] = { 0, 0, 0.5f }, b[3] = { 1, 0, 0.5f };
        dot_st_case("dot_st hilo_hemisphere", t, DM_HEMI, a, b, 1, 2);
    }

    uint32_t rs[128];
    inputs in;

    /* DOT_STR_3D: (dot1, dot2, dot3) with an all-ones normal = tc x's */
    set_texel(0, 255, 255, 255, 255);
    set_vol(3);
    show_def(rs, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOTPRODUCT, TM_DOT_STR_3D));
    inputs_default(&in);
    in.tc[1][0] = 0.3f; in.tc[2][0] = 0.7f; in.tc[3][0] = 0.8f;
    uint8_t c[4];
    vol_color(0, 1, 1, c);
    if (draw("dot_str_3d", rs, &in)) expect_color("dot_str_3d (dot1, dot2, dot3)", c);

    set_cube(3);
    show_def(rs, T3, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOTPRODUCT, TM_DOT_STR_CUBE));
    inputs_default(&in);
    in.tc[1][0] = -0.9f; in.tc[2][0] = 0.1f; in.tc[3][0] = 0.2f;
    if (draw("dot_str_cube", rs, &in)) expect_color("dot_str_cube -> -X face", face_color[1]);

    /* reflections: n = (dot1, dot2, dot3) = (0.2, 0, 1); diffuse looks up n
       (+Z); specular reflects e = (q1, q2, q3) = (2, 0, 1) about n:
       r = 2 n (n.e) / (n.n) - e = 2 (0.2,0,1) 1.4 / 1.04 - (2,0,1) = (-1.46, 0, 1.69) -> +Z;
       with e = (4, 0, 1): r = 2 (0.2,0,1) 1.8 / 1.04 - (4,0,1) = (-3.31, 0, 2.46) -> -X */
    set_cube(2);
    set_cube(3);
    show_def(rs, T2, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOT_RFLCT_DIFF, TM_DOT_RFLCT_SPEC));
    inputs_default(&in);
    in.tc[1][0] = 0.2f; in.tc[3][0] = 1.0f;
    in.tc[1][3] = 2; in.tc[2][3] = 0; in.tc[3][3] = 1;
    if (draw("dot_rflct_diff", rs, &in)) expect_color("dot_rflct_diff n -> +Z face", face_color[4]);
    rs[RS_FCABCD] = PS_COMBINERINPUTS(ZERO, ZERO, ZERO, T3 | RGB);
    rs[RS_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, T3 | ALPHA, 0);
    if (draw("dot_rflct_spec", rs, &in)) expect_color("dot_rflct_spec e=(2,0,1) -> +Z", face_color[4]);
    in.tc[1][3] = 4;
    if (draw("dot_rflct_spec2", rs, &in)) expect_color("dot_rflct_spec e=(4,0,1) -> -X", face_color[1]);
    /* same through DOTPRODUCT in stage 2 instead of the diffuse lookup */
    rs[RS_TEXMODES] = PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOTPRODUCT, TM_DOT_RFLCT_SPEC);
    if (draw("dot_rflct_spec3", rs, &in)) expect_color("dot_rflct_spec after 2 dotproducts", face_color[1]);

    /* DOT_ZW: depth = dot1 / dot2 in 24-bit units */
    show_def(rs, T0, PS_TEXTUREMODES(TM_PROJECT2D, TM_DOTPRODUCT, TM_DOT_ZW, TM_NONE));
    inputs_default(&in);
    in.tc[1][0] = 0.25f * 16777215.0f;
    in.tc[2][0] = 1.0f;
    if (draw("dot_zw", rs, &in)) {
        if (fabsf(depth_value - 0.25f) > 1e-4f) {
            printf("FAIL %-34s depth %f, expected 0.25\n", "dot_zw depth", depth_value);
            failures++;
        } else {
            printf("ok   %-34s depth %f\n", "dot_zw depth = z/w / 2^24-1", depth_value);
            passes++;
        }
    }
}

static void test_final_combiner(void)
{
    uint32_t rs[128];
    inputs in;

    /* stage 0: r0 = c0 (0.6, 0.2, 0.9, 0.5); v1 = (0.3, 0.5, 0.4) */
    memset(rs, 0, sizeof rs);
    rs[RS_COUNT] = PS_COMBINERCOUNT(1, 0);
    rs[RS_RGBIN] = PS_COMBINERINPUTS(C0 | RGB, ONE | RGB, ZERO, ZERO);
    rs[RS_ALPHAIN] = PS_COMBINERINPUTS(C0 | ALPHA, ONE | ALPHA, ZERO, ZERO);
    rs[RS_RGBOUT] = rs[RS_ALPHAOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
    /* out = s * fc0 + (1 - s) * fc1 with fc0 = 0.5, fc1 = 1: 1 - 0.5 s */
    rs[RS_FCABCD] = PS_COMBINERINPUTS(V1R0_SUM | RGB, C0 | RGB, C1 | RGB, ZERO);
    inputs_default(&in);
    float r0[3] = { 0.6f, 0.2f, 0.9f }, v1[3] = { 0.3f, 0.5f, 0.4f };
    memcpy(in.c0[0], r0, sizeof r0);
    in.c0[0][3] = 0.5f;
    memcpy(in.spec, v1, sizeof v1);
    for (int i = 0; i < 4; i++) { in.fc0[i] = 0.5f; in.fc1[i] = 1; }
    static const struct { const char *name; int settings; } cases[] = {
        { "v1r0 sum unclamped", 0 },
        { "v1r0 sum clamped", CLAMP_SUM },
        { "v1r0 complement r0", COMPLEMENT_R0 },
        { "v1r0 complement v1, clamp", COMPLEMENT_V1 | CLAMP_SUM },
    };
    for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
        int st = cases[k].settings;
        rs[RS_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, R0 | ALPHA, st);
        int e[3];
        for (int i = 0; i < 3; i++) {
            float a = (st & COMPLEMENT_V1) ? 1 - v1[i] : v1[i];
            float b = (st & COMPLEMENT_R0) ? 1 - r0[i] : r0[i];
            float s = a + b;
            if (st & CLAMP_SUM) s = s > 1 ? 1 : s;
            e[i] = to8(1 - 0.5f * s);
        }
        if (draw(cases[k].name, rs, &in)) expect_rgba(cases[k].name, e[0], e[1], e[2], 128, 2);
    }

    /* E*F = r0 * v1 */
    rs[RS_FCABCD] = PS_COMBINERINPUTS(ZERO, ZERO, ZERO, EF_PROD | RGB);
    rs[RS_FCEFG] = PS_COMBINERINPUTS(R0 | RGB, V1 | RGB, R0 | ALPHA, 0);
    if (draw("ef product", rs, &in))
        expect_rgba("ef product", to8(0.18f), to8(0.1f), to8(0.36f), 128, 2);
    /* E*F with an inverted input, through A (the lerp factor) */
    rs[RS_FCABCD] = PS_COMBINERINPUTS(EF_PROD | RGB, ONE | RGB, ZERO, ZERO);
    rs[RS_FCEFG] = PS_COMBINERINPUTS(R0 | RGB | UNSIGNED_INVERT, V1 | RGB, ZERO | ALPHA | UNSIGNED_INVERT, 0);
    if (draw("ef product invert", rs, &in))
        expect_rgba("ef product (1-r0)*v1 as A", to8(0.4f * 0.3f), to8(0.8f * 0.5f), to8(0.1f * 0.4f), 255, 2);
}

static void test_combiners(void)
{
    uint32_t rs[128];
    inputs in;

    /* mux: stage 0 r0.a = c0.a, stage 1 r0 = mux(t0 = red, t1 = blue) */
    set_texel(0, 255, 0, 0, 255);
    set_texel(1, 0, 0, 255, 128);
    memset(rs, 0, sizeof rs);
    rs[RS_TEXMODES] = PS_TEXTUREMODES(TM_PROJECT2D, TM_PROJECT2D, TM_NONE, TM_NONE);
    rs[RS_ALPHAIN] = PS_COMBINERINPUTS(C0 | ALPHA, ONE | ALPHA, ZERO, ZERO);
    rs[RS_ALPHAOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
    rs[RS_RGBIN + 1] = PS_COMBINERINPUTS(T0 | RGB, ONE | RGB, T1 | RGB, ONE | RGB);
    rs[RS_ALPHAIN + 1] = PS_COMBINERINPUTS(T0 | ALPHA, ONE | ALPHA, T1 | ALPHA, ONE | ALPHA);
    rs[RS_RGBOUT + 1] = rs[RS_ALPHAOUT + 1] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, R0, AB_CD_MUX);
    inputs_default(&in);
    static const struct { const char *name; int flags, a8, cd; } mux[] = {
        { "mux lsb, r0.a = 3/255 -> cd", 0, 3, 1 },
        { "mux lsb, r0.a = 4/255 -> ab", 0, 4, 0 },
        { "mux lsb, r0.a = 255/255 -> cd", 0, 255, 1 },
        { "mux msb, r0.a = 128/255 -> cd", MUX_MSB, 128, 1 },
        { "mux msb, r0.a = 127/255 -> ab", MUX_MSB, 127, 0 },
    };
    for (unsigned k = 0; k < sizeof mux / sizeof *mux; k++) {
        rs[RS_COUNT] = PS_COMBINERCOUNT(2, mux[k].flags);
        in.c0[0][3] = mux[k].a8 / 255.0f;
        if (draw(mux[k].name, rs, &in)) {
            if (mux[k].cd) expect_rgba(mux[k].name, 0, 0, 255, 128, 2);
            else expect_rgba(mux[k].name, 255, 0, 0, 255, 2);
        }
    }

    /* per-stage constants: r0 = c0; r0 = r0 + c0 */
    memset(rs, 0, sizeof rs);
    rs[RS_RGBIN] = PS_COMBINERINPUTS(C0 | RGB, ONE | RGB, ZERO, ZERO);
    rs[RS_ALPHAIN] = PS_COMBINERINPUTS(C0 | ALPHA, ONE | ALPHA, ZERO, ZERO);
    rs[RS_RGBOUT] = rs[RS_ALPHAOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
    rs[RS_RGBIN + 1] = PS_COMBINERINPUTS(R0 | RGB, ONE | RGB, C0 | RGB, ONE | RGB);
    rs[RS_ALPHAIN + 1] = PS_COMBINERINPUTS(R0 | ALPHA, ONE | ALPHA, C0 | ALPHA, ONE | ALPHA);
    rs[RS_RGBOUT + 1] = rs[RS_ALPHAOUT + 1] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, R0, IDENTITY);
    inputs_default(&in);
    for (int i = 0; i < 4; i++) { in.c0[0][i] = 0.1f * (i + 1); in.c0[1][i] = 0.5f; }
    rs[RS_COUNT] = PS_COMBINERCOUNT(2, UNIQUE_C0);
    if (draw("unique c0", rs, &in)) expect_rgba("unique c0: c0[0] + c0[1]", to8(0.6f), to8(0.7f), to8(0.8f), to8(0.9f), 2);
    rs[RS_COUNT] = PS_COMBINERCOUNT(2, 0);
    if (draw("same c0", rs, &in)) expect_rgba("same c0: 2 * c0[0]", to8(0.2f), to8(0.4f), to8(0.6f), to8(0.8f), 2);

    /* dot product into r0 with blue to alpha; the alpha half writes nothing */
    set_texel(0, 255, 128, 128, 0);
    set_texel(1, 192, 192, 64, 0);
    memset(rs, 0, sizeof rs);
    rs[RS_COUNT] = PS_COMBINERCOUNT(1, 0);
    rs[RS_TEXMODES] = PS_TEXTUREMODES(TM_PROJECT2D, TM_PROJECT2D, TM_NONE, TM_NONE);
    rs[RS_RGBIN] = PS_COMBINERINPUTS(T0 | RGB | EXPAND_NORMAL, T1 | RGB | EXPAND_NORMAL, ZERO, ZERO);
    rs[RS_RGBOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, AB_DOT_PRODUCT | AB_BLUE_TO_ALPHA);
    inputs_default(&in);
    float a0 = 2 * 255 / 255.0f - 1, a1 = 2 * 128 / 255.0f - 1, b0 = 2 * 192 / 255.0f - 1, b2 = 2 * 64 / 255.0f - 1;
    float d = a0 * b0 + a1 * b0 + a1 * b2;
    if (draw("dp3 blue to alpha", rs, &in)) expect_rgba("dp3 + blue to alpha", to8(d), to8(d), to8(d), to8(d), 2);

    /* signed values survive between stages: r1 = -c0 (signed negate), r0 = -r1 */
    memset(rs, 0, sizeof rs);
    rs[RS_COUNT] = PS_COMBINERCOUNT(2, 0);
    rs[RS_RGBIN] = PS_COMBINERINPUTS(C0 | RGB | 0xe0, ONE | RGB, ZERO, ZERO);
    rs[RS_RGBOUT] = PS_COMBINEROUTPUTS(R1, DISCARD, DISCARD, IDENTITY);
    rs[RS_RGBIN + 1] = PS_COMBINERINPUTS(R1 | RGB | 0xe0, ONE | RGB, ZERO, ZERO);
    rs[RS_RGBOUT + 1] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
    inputs_default(&in);
    in.c0[0][0] = 0.25f; in.c0[0][1] = 0.5f; in.c0[0][2] = 0.75f;
    if (draw("signed negate chain", rs, &in)) expect_rgba("signed negate twice", 64, 128, 191, -1, 2);
}

static void test_fog(void)
{
    uint32_t rs[128];
    inputs in;

    /* out.rgb = fog factor, out.a = 1 */
    memset(rs, 0, sizeof rs);
    rs[RS_FCABCD] = PS_COMBINERINPUTS(ZERO, ZERO, ZERO, FOG | ALPHA);
    rs[RS_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, ONE | ALPHA, 0);
    inputs_default(&in);
    in.fog_start = 1; in.fog_end = 5; in.fog_density = 0.5f; in.fogcoord = 2;
    static const struct { const char *name; int enable, mode; float f; } cases[] = {
        { "fog disabled -> 1", 0, 3, 1.0f },
        { "fog linear (5-2)/4", 1, 3, 0.75f },
        { "fog exp e^-(0.5*2)", 1, 1, 0.36788f },
        { "fog exp2 e^-(0.5*2)^2", 1, 2, 0.36788f },
    };
    for (unsigned k = 0; k < sizeof cases / sizeof *cases; k++) {
        rs[RS_FOGENABLE] = cases[k].enable;
        rs[RS_FOGTABLEMODE] = cases[k].mode;
        int v = to8(cases[k].f);
        if (draw(cases[k].name, rs, &in)) expect_rgba(cases[k].name, v, v, v, 255, 2);
    }
    in.fogcoord = 0.25f;
    rs[RS_FOGENABLE] = 1;
    rs[RS_FOGTABLEMODE] = 0;
    if (draw("fog none", rs, &in)) expect_rgba("fog none: factor = fog coord", 64, 64, 64, 255, 2);

    /* no xfc, fog on: the library's spec/fog final combiner */
    memset(rs, 0, sizeof rs);
    rs[RS_COUNT] = PS_COMBINERCOUNT(1, 0);
    rs[RS_RGBIN] = PS_COMBINERINPUTS(V0 | RGB, ONE | RGB, ZERO, ZERO);
    rs[RS_ALPHAIN] = PS_COMBINERINPUTS(V0 | ALPHA, ONE | ALPHA, ZERO, ZERO);
    rs[RS_RGBOUT] = rs[RS_ALPHAOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);
    rs[RS_FOGENABLE] = 1;
    rs[RS_FOGTABLEMODE] = 3;
    inputs_default(&in);
    in.fog_start = 1; in.fog_end = 5; in.fogcoord = 2;
    in.color[0] = 1; in.color[1] = 1; in.color[2] = 1; in.color[3] = 0.5f;
    /* 0.75 * white + 0.25 * (0.2, 0.4, 0.6) */
    if (draw("default xfc with fog", rs, &in))
        expect_rgba("no xfc + fog: lerp(fogcolor, r0, f)", to8(0.75f + 0.05f), to8(0.75f + 0.1f), to8(0.75f + 0.15f),
                    128, 2);
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window *w = SDL_CreateWindow("psh_modes", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!w || !SDL_GL_CreateContext(w)) { fprintf(stderr, "GL: %s\n", SDL_GetError()); return 2; }
#define LOAD(n) p_##n = SDL_GL_GetProcAddress(#n)
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram);
    LOAD(glGetProgramiv); LOAD(glUseProgram); LOAD(glDeleteProgram); LOAD(glDeleteShader);
    LOAD(glGetUniformLocation); LOAD(glUniform1i); LOAD(glUniform2fv); LOAD(glUniform4fv);
    LOAD(glActiveTexture); LOAD(glMultiTexCoord4f); LOAD(glFogCoordf); LOAD(glSecondaryColor3f);
    LOAD(glTexImage3D);

    test_project_passthru_clip();
    test_bumpenv();
    test_dependent();
    test_dot_chains();
    test_final_combiner();
    test_combiners();
    test_fog();

    printf("%d passed, %d failed\n", passes, failures);
    return failures != 0;
}
