/*
 * psh_render: run translated pixel shaders on the real driver and check the
 * pixels they produce.  Fixed-function vertex processing (as the FVF titles
 * use) feeds gl_Color and gl_TexCoord[0..2]; three 8x8 textures sit on
 * units 0..2: red, blue and a black/white checkerboard.
 *
 *   psh_render [out.ppm]
 *
 * Needs a display (xvfb-run).  Build:
 *   gcc -std=gnu11 -Wall -I src/hle $(pkg-config --cflags sdl2) tests/psh_render.c src/hle/psh.c \
 *       $(pkg-config --libs sdl2 gl) -o psh_render
 *   (without pkg-config, e.g. 32-bit: gcc -m32 -std=gnu11 -Wall -I src/hle -I/usr/include/SDL2
 *    -D_REENTRANT tests/psh_render.c src/hle/psh.c -lSDL2 -lGL -lm -o psh_render)
 */
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "nv2a_shaders.h"

#define PS_TEXTUREMODES(t0, t1, t2, t3) (((t3) << 15) | ((t2) << 10) | ((t1) << 5) | (t0))
#define PS_COMBINERCOUNT(count, flags) (((flags) << 8) | (count))
#define PS_COMBINERINPUTS(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define PS_COMBINEROUTPUTS(ab, cd, mux_sum, flags) (((flags) << 12) | ((mux_sum) << 8) | ((ab) << 4) | (cd))
enum { ZERO = 0, C0 = 1, C1 = 2, FOG = 3, V0 = 4, V1 = 5, T0 = 8, T1 = 9, T2 = 10, T3 = 11, R0 = 12, R1 = 13, DISCARD = 0 };
enum { UNSIGNED_INVERT = 0x20, EXPAND_NORMAL = 0x40, SIGNED_IDENTITY = 0xc0, SIGNED_NEGATE = 0xe0 };
enum { RGB = 0, BLUE = 0, ALPHA = 0x10 };
enum { IDENTITY = 0, SHIFTLEFT_1 = 0x10, AB_DOT_PRODUCT = 0x02, CD_DOT_PRODUCT = 0x01, AB_CD_MUX = 0x04 };
enum { MUX_MSB = 1, UNIQUE_C0 = 0x10, UNIQUE_C1 = 0x100 };
#define ONE (ZERO | UNSIGNED_INVERT)
enum { D_ALPHAIN = 0, D_FCABCD = 8, D_FCEFG = 9, D_CONST0 = 10, D_ALPHAOUT = 26, D_RGBIN = 34, D_RGBOUT = 45,
       D_COUNT = 53, D_TEXMODES = 54 };

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
static GLint (APIENTRY *p_glGetUniformLocation)(GLuint, const char *);
static void (APIENTRY *p_glUniform1i)(GLint, GLint);
static void (APIENTRY *p_glUniform4fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *p_glActiveTexture)(GLenum);
static void (APIENTRY *p_glMultiTexCoord2f)(GLenum, GLfloat, GLfloat);
static void (APIENTRY *p_glFogCoordf)(GLfloat);
static void (APIENTRY *p_glUniform2fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *p_glMultiTexCoord4fv)(GLenum, const GLfloat *);
static void (APIENTRY *p_glSecondaryColor3fv)(const GLfloat *);
#define GL_FOG_COORD_SRC_ 0x8450
#define GL_FOG_COORD_ 0x8451

#define W 64
#define H 64
static unsigned char pixels[H][W][4];
static int failures;

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
        printf("compile failed:\n%s\n", log);
        return 0;
    }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, fs);
    p_glLinkProgram(prog);
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { printf("link failed\n"); return 0; }
    return prog;
}

static void texture(int unit, void (*fill)(int, int, unsigned char *))
{
    static unsigned char data[8][8][4];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) fill(x, y, data[y][x]);
    GLuint t;
    glGenTextures(1, &t);
    p_glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
}

static void red(int x, int y, unsigned char *p) { p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 128; }
static void blue(int x, int y, unsigned char *p) { p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255; }
static void checker(int x, int y, unsigned char *p)
{
    unsigned char v = ((x / 4) ^ (y / 4)) & 1 ? 255 : 0;
    p[0] = p[1] = p[2] = v;
    p[3] = 255;
}

/* Draw a full-window quad with the program; texcoords of units 0..2 span
   the quad, colour `rgba`. */
static void render(GLuint prog, const float *rgba)
{
    glViewport(0, 0, W, H);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glUseProgram(prog);
    for (int u = 0; u < 3; u++) {
        char name[8];
        snprintf(name, sizeof name, "tex%d", u);
        GLint loc = p_glGetUniformLocation(prog, name);
        if (loc >= 0) p_glUniform1i(loc, u);
    }
    static const float scale[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    GLint loc = p_glGetUniformLocation(prog, "tex_scale");
    if (loc >= 0) p_glUniform4fv(loc, 4, scale);
    glColor4fv(rgba);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++) {
        float s = (i == 1 || i == 2) ? 1.0f : 0.0f, t = (i >= 2) ? 1.0f : 0.0f;
        for (int u = 0; u < 3; u++) p_glMultiTexCoord2f(GL_TEXTURE0 + u, s, t);
        glVertex2f(s * 2 - 1, t * 2 - 1);
    }
    glEnd();
    p_glUseProgram(0);
    glFinish();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

static void expect_pixel(const char *what, int x, int y, int r, int g, int b, int a)
{
    const unsigned char *p = pixels[y][x];
    int tol = 3;
    if (abs(p[0] - r) > tol || abs(p[1] - g) > tol || abs(p[2] - b) > tol || abs(p[3] - a) > tol) {
        printf("FAIL %s at (%d,%d): got %d %d %d %d, expected %d %d %d %d\n", what, x, y,
               p[0], p[1], p[2], p[3], r, g, b, a);
        failures++;
    } else {
        printf("ok   %s at (%d,%d): %d %d %d %d\n", what, x, y, p[0], p[1], p[2], p[3]);
    }
}

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) fwrite(pixels[y][x], 1, 3, f);
    fclose(f);
}


/* Regressions for NV2A state that supplements the shader definition.
   Constant coordinates keep sample choice independent of raster position. */
static float nv2a_coords[4][4];
static unsigned char nv2a_texel[4];

static void nv2a_fill(int x, int y, unsigned char *p)
{
    (void)x; (void)y;
    memcpy(p, nv2a_texel, 4);
}

static void nv2a_solid(int unit, int r, int g, int b, int a)
{
    nv2a_texel[0] = r; nv2a_texel[1] = g;
    nv2a_texel[2] = b; nv2a_texel[3] = a;
    texture(unit, nv2a_fill);
}

static void nv2a_rs(uint32_t *rs, unsigned reg, uint32_t modes)
{
    memset(rs, 0, 128 * sizeof *rs);
    rs[117] = modes;
    rs[D_FCABCD] = reg;
    rs[D_FCEFG] = (reg | ALPHA) << 8;
}

static GLuint nv2a_program(const uint32_t *rs, const psh_options *options)
{
    char *source = psh_translate_ex(rs, options);
    GLuint program = source ? make_program(source) : 0;
    free(source);
    if (!program) { printf("FAIL NV2A regression shader\n"); failures++; return 0; }
    p_glUseProgram(program);
    static const float scale[16] = { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 };
    p_glUniform4fv(p_glGetUniformLocation(program, "tex_scale"), 4, scale);
    for (int i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof name, "tex%d", i);
        p_glUniform1i(p_glGetUniformLocation(program, name), i);
        snprintf(name, sizeof name, "cube%d", i);
        p_glUniform1i(p_glGetUniformLocation(program, name), i);
    }
    return program;
}

static void nv2a_draw(GLuint program, const float *secondary)
{
    static const float black[3] = {0,0,0};
    glViewport(0, 0, W, H);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glUseProgram(program);
    glColor4f(1,1,1,1);
    p_glSecondaryColor3fv(secondary ? secondary : black);
    glBegin(GL_QUADS);
    for (int corner = 0; corner < 4; corner++) {
        for (int unit = 0; unit < 4; unit++)
            p_glMultiTexCoord4fv(GL_TEXTURE0 + unit, nv2a_coords[unit]);
        glVertex2f(corner == 1 || corner == 2 ? 1 : -1, corner >= 2 ? 1 : -1);
    }
    glEnd();
    p_glUseProgram(0);
    glFinish();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
}

static void nv2a_cube(int unit)
{
    static const unsigned char faces[6][4] = {
        {255,0,0,255}, {0,255,255,255}, {0,255,0,255},
        {255,0,255,255}, {0,0,255,255}, {255,255,0,255}
    };
    GLuint texture_id;
    glGenTextures(1, &texture_id);
    p_glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture_id);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    for (int i = 0; i < 6; i++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA8,
                     1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, faces[i]);
}

static void nv2a_depth(int bits)
{
    GLuint texture_id;
    const float depth = 0.5f;
    glGenTextures(1, &texture_id);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_R_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glTexImage2D(GL_TEXTURE_2D, 0, bits == 16 ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24,
                 1, 1, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
}

static void nv2a_halves(int x, int y, unsigned char *p)
{
    (void)y;
    p[0] = x < 4 ? 255 : 0; p[1] = 0;
    p[2] = x < 4 ? 0 : 255; p[3] = 255;
}

static void test_nv2a_state(void)
{
    uint32_t rs[128];
    psh_options options = {0};
    GLuint program;
    const float secondary[3] = {0.75f, 0.25f, 0.5f};
    for (int i = 0; i < 4; i++) {
        nv2a_coords[i][0] = nv2a_coords[i][1] = nv2a_coords[i][2] = 0.5f;
        nv2a_coords[i][3] = 1;
    }
    glEnable(GL_COLOR_SUM);

    /* Implicit specular sum saturates before fog; alpha comes from R0. */
    nv2a_solid(0, 255,0,0,128);
    memset(rs, 0, sizeof rs);
    rs[117] = PS_TEXTUREMODES(1,0,0,0);
    rs[D_COUNT] = 1;
    rs[D_RGBIN] = PS_COMBINERINPUTS(T0, ONE, ZERO, ZERO);
    rs[D_ALPHAIN] = PS_COMBINERINPUTS(T0 | ALPHA, ONE, ZERO, ZERO);
    rs[D_RGBOUT] = rs[D_ALPHAOUT] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, 0);
    program = nv2a_program(rs, &options);
    if (!program) return;
    nv2a_draw(program, secondary);
    expect_pixel("implicit specular off", 20,20, 255,0,0,128);
    rs[93] = 1;
    program = nv2a_program(rs, &options);
    if (!program) return;
    nv2a_draw(program, secondary);
    expect_pixel("implicit specular on", 20,20, 255,64,128,128);
    rs[82] = 1;
    const float fog_color[4] = {0,1,1,1};
    glFogfv(GL_FOG_COLOR, fog_color);
    glFogi(GL_FOG_COORD_SRC_, GL_FOG_COORD_);
    p_glFogCoordf(0.25f);
    program = nv2a_program(rs, &options);
    if (!program) return;
    nv2a_draw(program, secondary);
    expect_pixel("specular saturates before fog", 20,20, 64,207,223,128);
    rs[D_FCABCD] = T0;
    rs[D_FCEFG] = (T0 | ALPHA) << 8;
    program = nv2a_program(rs, &options);
    if (!program) return;
    nv2a_draw(program, secondary);
    expect_pixel("explicit final ignores implicit specular and fog", 20,20, 255,0,0,128);

    /* Alpha kill is equality with zero, and occurs before key substitution. */
    nv2a_rs(rs, T0, PS_TEXTUREMODES(1,0,0,0));
    options.alpha_kill = 1;
    nv2a_solid(0, 64,128,192,0);
    program = nv2a_program(rs, &options);
    if (!program) return;
    nv2a_draw(program, NULL);
    expect_pixel("alpha kill zero", 20,20, 0,255,0,255);
    nv2a_solid(0, 64,128,192,1);
    nv2a_draw(program, NULL);
    expect_pixel("alpha kill smallest nonzero byte survives", 20,20, 64,128,192,1);

    float keys[16] = {64/255.0f,128/255.0f,192/255.0f,1/255.0f};
    for (int operation = 1; operation <= 3; operation++) {
        options.color_key[0] = operation;
        program = nv2a_program(rs, &options);
        if (!program) return;
        p_glUniform4fv(p_glGetUniformLocation(program, "key_color"), 4, keys);
        nv2a_draw(program, NULL);
        if (operation == 1)
            expect_pixel("key alpha zero after alpha kill", 20,20, 64,128,192,0);
        else if (operation == 2)
            expect_pixel("key RGBA zero", 20,20, 0,0,0,0);
        else
            expect_pixel("key discard", 20,20, 0,255,0,255);
    }
    keys[3] = 1;
    program = nv2a_program(rs, &options);
    if (!program) return;
    p_glUniform4fv(p_glGetUniformLocation(program, "key_color"), 4, keys);
    nv2a_draw(program, NULL);
    expect_pixel("ARGB key compares alpha", 20,20, 64,128,192,1);
    options.color_key_ignore_alpha = 1;
    program = nv2a_program(rs, &options);
    if (!program) return;
    p_glUniform4fv(p_glGetUniformLocation(program, "key_color"), 4, keys);
    nv2a_draw(program, NULL);
    expect_pixel("XRGB key ignores alpha", 20,20, 0,255,0,255);

    /* Neither source texel matches the key; their filtered result does. */
    static const unsigned char filtered[2][4] = {{0,0,0,255},{128,0,0,255}};
    p_glActiveTexture(GL_TEXTURE0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2,1,0,GL_RGBA,GL_UNSIGNED_BYTE,filtered);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    keys[0] = 64/255.0f; keys[1] = keys[2] = 0; keys[3] = 1;
    p_glUseProgram(program);
    p_glUniform4fv(p_glGetUniformLocation(program, "key_color"), 4, keys);
    nv2a_draw(program, NULL);
    expect_pixel("color key compares filtered texel", 20,20, 0,255,0,255);

    /* Non-sampling PASSTHRU must not run texture kill or key operations. */
    nv2a_rs(rs, T0, PS_TEXTUREMODES(4,0,0,0));
    nv2a_coords[0][0] = 0.25f; nv2a_coords[0][1] = 0.5f;
    nv2a_coords[0][2] = 0.75f; nv2a_coords[0][3] = 0;
    program = nv2a_program(rs, &options);
    if (!program) return;
    nv2a_draw(program, NULL);
    expect_pixel("passthru skips sampled texture controls",20,20,64,128,191,0);

    /* Constant eye reflection: non-unit normal rejects an unnormalized
       reflection formula. Changing eye state must change the cube face. */
    memset(&options, 0, sizeof options);
    nv2a_rs(rs, T3, PS_TEXTUREMODES(4,17,17,18));
    memset(nv2a_coords, 0, sizeof nv2a_coords);
    nv2a_coords[0][0] = 1; nv2a_coords[0][3] = 1;
    nv2a_coords[1][0] = 2; nv2a_coords[1][3] = 7;
    nv2a_coords[2][3] = 9; nv2a_coords[3][3] = 11;
    nv2a_cube(3);
    program = nv2a_program(rs, &options);
    if (!program) return;
    float eye[4] = {0.2f,1,0,0};
    p_glUniform4fv(p_glGetUniformLocation(program, "eye_vector"),1,eye);
    nv2a_draw(program,NULL);
    expect_pixel("constant eye with non-unit normal selects -Y",20,20,255,0,255,255);
    eye[0] = -1; eye[1] = 0;
    p_glUseProgram(program);
    p_glUniform4fv(p_glGetUniformLocation(program, "eye_vector"),1,eye);
    nv2a_draw(program,NULL);
    expect_pixel("constant eye update selects -X",20,20,0,255,255,255);

    /* X8L8V8U8 shares an upload encoding with X8R8G8B8; interpreted as
       a bump source, uploaded (L,V,U) supplies U,V displacements and L. */
    nv2a_rs(rs,T1,PS_TEXTUREMODES(1,7,0,0));
    options.bump_bgra = 1;
    nv2a_solid(0,64,0,127,255);
    texture(1,nv2a_halves);
    nv2a_coords[0][0] = nv2a_coords[0][1] = 0.5f;
    nv2a_coords[0][2] = 0; nv2a_coords[0][3] = 1;
    nv2a_coords[1][0] = 0.1f; nv2a_coords[1][1] = 0.5f;
    nv2a_coords[1][2] = 0; nv2a_coords[1][3] = 1;
    program = nv2a_program(rs,&options);
    if (!program) return;
    float bump[16] = {0};
    float luminance[8] = {0};
    bump[4] = 0.5f; luminance[2] = 1;
    p_glUniform4fv(p_glGetUniformLocation(program,"bump_env"),4,bump);
    p_glUniform2fv(p_glGetUniformLocation(program,"bump_lum"),4,luminance);
    nv2a_draw(program,NULL);
    expect_pixel("X8L8V8U8 uses U displacement and L luminance",20,20,0,0,64,64);

    /* Alpha kill follows luminance modulation, not the unmodified fetch. */
    options.alpha_kill = 2;
    program = nv2a_program(rs,&options);
    if (!program) return;
    luminance[2] = 0;
    p_glUniform4fv(p_glGetUniformLocation(program,"bump_env"),4,bump);
    p_glUniform2fv(p_glGetUniformLocation(program,"bump_lum"),4,luminance);
    nv2a_draw(program,NULL);
    expect_pixel("alpha kill observes zero luminance",20,20,0,255,0,255);

    /* Native D16/D24 references use z/q. PROJECT2D compares zero even
       when incoming z is nonzero; PROJECT3D enables the depth reference. */
    memset(&options,0,sizeof options);
    for (int bits = 16; bits <= 24; bits += 8) {
        nv2a_depth(bits);
        float maximum = bits == 16 ? 65535.0f : 16777215.0f;
        for (int mode = 1; mode <= 2; mode++) {
            nv2a_rs(rs,T0,PS_TEXTUREMODES(mode,0,0,0));
            psh_shadow_stages = 1;
            program = nv2a_program(rs,&options);
            psh_shadow_stages = 0;
            if (!program) return;
            float scale[16] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
            scale[2] = 1.0f / maximum;
            p_glUniform4fv(p_glGetUniformLocation(program,"tex_scale"),4,scale);
            nv2a_coords[0][0] = nv2a_coords[0][1] = 1;
            nv2a_coords[0][2] = maximum * 1.5f;
            nv2a_coords[0][3] = 2;
            nv2a_draw(program,NULL);
            char label[80];
            snprintf(label,sizeof label,"D%d PROJECT%d high projected reference",bits,mode+1);
            int value = mode == 1 ? 255 : 0;
            expect_pixel(label,20,20,value,value,value,255);
            nv2a_coords[0][2] = maximum * 0.5f;
            nv2a_draw(program,NULL);
            snprintf(label,sizeof label,"D%d PROJECT%d low projected reference",bits,mode+1);
            expect_pixel(label,20,20,255,255,255,255);
        }
    }
    glDisable(GL_COLOR_SUM);
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_Window *w = SDL_CreateWindow("psh_render", 0, 0, W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!w || !SDL_GL_CreateContext(w)) { fprintf(stderr, "GL: %s\n", SDL_GetError()); return 2; }
#define LOAD(n) p_##n = SDL_GL_GetProcAddress(#n)
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram);
    LOAD(glGetProgramiv); LOAD(glUseProgram); LOAD(glGetUniformLocation); LOAD(glUniform1i);
    LOAD(glUniform4fv); LOAD(glActiveTexture); LOAD(glMultiTexCoord2f); LOAD(glFogCoordf);
    LOAD(glUniform2fv); LOAD(glMultiTexCoord4fv); LOAD(glSecondaryColor3fv);

    texture(0, red);
    texture(1, blue);
    texture(2, checker);

    uint32_t rs[128];
    const uint32_t out_ab = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, IDENTITY);

    /* 1. ModifyPixelShader: r0 = t2; r0 = t0 mux t1 (on r0.a's MSB); r0 *= v0; out r0, alpha 1 */
    memset(rs, 0, sizeof rs);
    rs[D_COUNT] = PS_COMBINERCOUNT(3, MUX_MSB | UNIQUE_C0 | UNIQUE_C1);
    rs[117] = PS_TEXTUREMODES(1, 1, 1, 0);
    rs[D_RGBIN + 0] = PS_COMBINERINPUTS(T2 | RGB, ONE | RGB, ZERO, ZERO);
    rs[D_ALPHAIN + 0] = PS_COMBINERINPUTS(T2 | BLUE, ONE | ALPHA, ZERO, ZERO);
    rs[D_RGBOUT + 0] = rs[D_ALPHAOUT + 0] = out_ab;
    rs[D_RGBIN + 1] = PS_COMBINERINPUTS(T0 | RGB, ONE | RGB, T1 | RGB, ONE | RGB);
    rs[D_ALPHAIN + 1] = PS_COMBINERINPUTS(T0 | ALPHA, ONE | ALPHA, T1 | ALPHA, ONE | ALPHA);
    rs[D_RGBOUT + 1] = rs[D_ALPHAOUT + 1] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, R0, IDENTITY | AB_CD_MUX);
    rs[D_RGBIN + 2] = PS_COMBINERINPUTS(R0 | RGB, V0 | RGB, ZERO, ZERO);
    rs[D_ALPHAIN + 2] = PS_COMBINERINPUTS(R0 | ALPHA, V0 | ALPHA, ZERO, ZERO);
    rs[D_RGBOUT + 2] = rs[D_ALPHAOUT + 2] = out_ab;
    rs[D_FCABCD] = PS_COMBINERINPUTS(ONE | RGB, R0 | RGB, ZERO, ZERO);
    rs[D_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, ONE | RGB | UNSIGNED_INVERT, 0);
    char *src = psh_translate(rs);
    GLuint prog = src ? make_program(src) : 0;
    free(src);
    if (!prog) { printf("FAIL mux shader\n"); return 1; }
    static const float white[4] = { 1, 1, 1, 1 }, half[4] = { 0.5f, 0.5f, 0.5f, 1 };
    render(prog, white);
    /* checker texel (x/4 ^ y/4): black at (0,0) -> r0.a = 0 -> AB = t0 = red; white -> CD = t1 = blue */
    expect_pixel("mux black square -> t0 (red)", 4, 4, 255, 0, 0, 255);
    expect_pixel("mux white square -> t1 (blue)", 36, 4, 0, 0, 255, 255);
    expect_pixel("mux white square -> t1 (blue)", 4, 36, 0, 0, 255, 255);
    expect_pixel("mux black square -> t0 (red)", 36, 36, 255, 0, 0, 255);
    render(prog, half);
    expect_pixel("mux * v0=0.5 (red)", 4, 4, 128, 0, 0, 255);
    expect_pixel("mux * v0=0.5 (blue)", 36, 4, 0, 0, 128, 255);
    if (argc > 1) save_ppm(argv[1]);

    /* 2. PixelShader.xpu: mov r1, t1; lrp r0, v0, t0, r1 -> v0*t0 + (1-v0)*t1, default final combiner */
    memset(rs, 0, sizeof rs);
    rs[D_COUNT] = PS_COMBINERCOUNT(2, 0);
    rs[117] = PS_TEXTUREMODES(1, 1, 0, 0);
    rs[D_RGBIN + 0] = PS_COMBINERINPUTS(T1 | RGB, ONE | RGB, ZERO, ZERO);
    rs[D_ALPHAIN + 0] = PS_COMBINERINPUTS(T1 | ALPHA, ONE | ALPHA, ZERO, ZERO);
    rs[D_RGBOUT + 0] = rs[D_ALPHAOUT + 0] = PS_COMBINEROUTPUTS(R1, DISCARD, DISCARD, IDENTITY);
    rs[D_RGBIN + 1] = PS_COMBINERINPUTS(V0 | RGB, T0 | RGB, R1 | RGB, V0 | RGB | UNSIGNED_INVERT);
    rs[D_ALPHAIN + 1] = PS_COMBINERINPUTS(V0 | ALPHA, T0 | ALPHA, R1 | ALPHA, V0 | ALPHA | UNSIGNED_INVERT);
    rs[D_RGBOUT + 1] = rs[D_ALPHAOUT + 1] = PS_COMBINEROUTPUTS(DISCARD, DISCARD, R0, IDENTITY);
    src = psh_translate(rs);
    prog = src ? make_program(src) : 0;
    free(src);
    if (!prog) { printf("FAIL lrp shader\n"); return 1; }
    static const float quarter[4] = { 0.25f, 0.25f, 0.25f, 0.25f };
    render(prog, quarter);
    /* 0.25*red(1,0,0,0.5) + 0.75*blue(0,0,1,1) = (0.25, 0, 0.75, 0.875) */
    expect_pixel("lrp v0=0.25", 20, 20, 64, 0, 191, 223);

    /* 3. dot product with expand mapping and x2 output: r0 = dot(t2_bx2, t0_bx2) * 2
          black checker: t2_bx2 = (-1,-1,-1), red_bx2 = (1,-1,-1) -> dot = 1 -> 2 -> clamp 1
          white: (1,1,1).(1,-1,-1) = -1 -> clamp 0 in the framebuffer; alpha: t0.a */
    memset(rs, 0, sizeof rs);
    rs[D_COUNT] = PS_COMBINERCOUNT(1, 0);
    rs[117] = PS_TEXTUREMODES(1, 1, 1, 0);
    rs[D_RGBIN + 0] = PS_COMBINERINPUTS(T2 | RGB | EXPAND_NORMAL, T0 | RGB | EXPAND_NORMAL, ZERO, ZERO);
    rs[D_ALPHAIN + 0] = PS_COMBINERINPUTS(T0 | ALPHA, ONE | ALPHA, ZERO, ZERO);
    rs[D_RGBOUT + 0] = PS_COMBINEROUTPUTS(R0, DISCARD, DISCARD, SHIFTLEFT_1 | AB_DOT_PRODUCT);
    rs[D_ALPHAOUT + 0] = out_ab;
    src = psh_translate(rs);
    prog = src ? make_program(src) : 0;
    free(src);
    if (!prog) { printf("FAIL dot shader\n"); return 1; }
    render(prog, white);
    expect_pixel("dot3 black square", 4, 4, 255, 255, 255, 128);
    expect_pixel("dot3 white square", 36, 4, 0, 0, 0, 128);

    /* 4. final combiner with fog register, D3DFOG_NONE: out = f*t0 + (1-f)*fogcolor, f = gl_FogFragCoord */
    memset(rs, 0, sizeof rs);
    rs[117] = PS_TEXTUREMODES(1, 0, 0, 0);
    rs[82] = 1;
    rs[83] = 0;
    rs[D_FCABCD] = PS_COMBINERINPUTS(FOG | ALPHA, T0 | RGB, FOG | RGB, ZERO);
    rs[D_FCEFG] = PS_COMBINERINPUTS(ZERO, ZERO, T0 | ALPHA, 0);
    src = psh_translate(rs);
    prog = src ? make_program(src) : 0;
    free(src);
    if (!prog) { printf("FAIL fog shader\n"); return 1; }
    static const float fogcol[4] = { 0, 1, 1, 1 };
    glFogfv(GL_FOG_COLOR, fogcol);
    glFogi(GL_FOG_COORD_SRC_, GL_FOG_COORD_);
    p_glFogCoordf(0.25f);
    render(prog, white);
    /* 0.25*red + 0.75*cyan = (0.25, 0.75, 0.75), alpha t0.a */
    expect_pixel("fog register blend", 20, 20, 64, 191, 191, 128);

    test_nv2a_state();

    printf("%s: %d failures\n", failures ? "FAILED" : "all ok", failures);
    return failures != 0;
}
