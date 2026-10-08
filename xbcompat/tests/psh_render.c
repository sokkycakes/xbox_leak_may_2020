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

    printf("%s: %d failures\n", failures ? "FAILED" : "all ok", failures);
    return failures != 0;
}
