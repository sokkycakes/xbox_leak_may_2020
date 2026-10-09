/*
 * vsh_exec: run translated Xbox vertex programs on the real GL driver and
 * compare every output with an independent CPU interpreter of the microcode.
 *
 *   vsh_exec [-n vertices] [-v] file.xvu...
 *
 * For each .xvu the GLSL from vsh_translate() is linked with transform
 * feedback of gl_Position, the colours, the texture coordinates, the fog
 * coordinate and the point size, and run over pseudo-random vertices and
 * constants (deterministic).  The interpreter below implements the NV2A
 * rules from scratch (it shares no code with src/hle/vsh.c), applies the
 * same clip-space conversion the header documents, and the two are compared
 * component by component.  Needs a display (xvfb-run).
 *
 * Build (32-bit like the rest of the tree, or 64-bit if SDL2 is there):
 *   gcc -m32 -std=gnu11 -Wall -Isrc/hle $(PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig \
 *       pkg-config --cflags sdl2 gl) -o vsh_exec tests/vsh_exec.c src/hle/vsh.c \
 *       $(PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --libs sdl2 gl) -lm
 */
#include <SDL.h>
#include <SDL_opengl.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_shaders.h"

/* ------------------------------------------------------- the interpreter */

enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };

typedef struct {
    float R[13][4];       /* R12 is oPos */
    float o[16][4];       /* o[0] is unused: oPos lives in R[12] */
    float c[192][4];
    int a0;
} machine;

#define BITS(w, dw, lo, n) (((w)[dw] >> (lo)) & ((1u << (n)) - 1))

static float g_attr[16][4];   /* the vertex being interpreted */

static void fetch(const machine *m, const uint32_t *w, int kind, unsigned reg, unsigned neg, unsigned swz,
                  float out[4])
{
    const float *base;
    static const float zero[4] = { 0, 0, 0, 0 };
    unsigned cidx = BITS(w, 1, 13, 8), vidx = BITS(w, 1, 9, 4), cin = BITS(w, 3, 1, 1);
    switch (kind) {
    case 1: base = m->R[reg > 12 ? 0 : reg]; break;
    case 2: base = g_attr[vidx]; break;
    case 3: {
        int i = (int)cidx + (cin ? m->a0 : 0);
        if (i < 0) i = 0;
        if (i > 191) i = 191;
        base = m->c[i];
        break;
    }
    default: base = zero; break;
    }
    for (int k = 0; k < 4; k++) {
        float v = base[(swz >> (6 - 2 * k)) & 3];
        out[k] = neg ? -v : v;
    }
}

static float mul0(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }

static float rcc(float s)
{
    float t = 1.0f / s;
    if (t > 0.0f || (t == 0.0f && s > 0.0f))
        return fminf(fmaxf(t, 5.42101086e-20f), 1.84467441e19f);
    return fminf(fmaxf(t, -1.84467441e19f), -5.42101086e-20f);
}

/* Runs one program; returns the mask of o registers it writes. */
static unsigned run(machine *m, const uint32_t *code, unsigned count)
{
    unsigned written = 0;
    for (unsigned i = 0; i < 12; i++) m->R[i][0] = m->R[i][1] = m->R[i][2] = m->R[i][3] = 0;
    m->R[12][0] = m->R[12][1] = m->R[12][2] = 0; m->R[12][3] = 1;
    for (unsigned i = 0; i < 16; i++) { m->o[i][0] = m->o[i][1] = m->o[i][2] = 0; m->o[i][3] = 1; }
    m->a0 = 0;
    for (unsigned pc = 0; pc < count; pc++) {
        const uint32_t *w = code + pc * 4;
        unsigned ilu = BITS(w, 1, 25, 3), mac = BITS(w, 1, 21, 4);
        unsigned rwm = BITS(w, 3, 24, 4), rw = BITS(w, 3, 20, 4), swm = BITS(w, 3, 16, 4);
        unsigned owm = BITS(w, 3, 12, 4), orb = BITS(w, 3, 11, 1), oaddr = BITS(w, 3, 3, 8);
        unsigned om = BITS(w, 3, 2, 1), eos = BITS(w, 3, 0, 1);
        if (mac == MAC_NOP && ilu == ILU_NOP) { if (eos) break; continue; }

        float a[4], b[4], c[4], t[4] = { 0, 0, 0, 0 }, u[4] = { 0, 0, 0, 0 };
        fetch(m, w, BITS(w, 2, 26, 2), BITS(w, 2, 28, 4), BITS(w, 1, 8, 1), BITS(w, 1, 0, 8), a);
        fetch(m, w, BITS(w, 2, 11, 2), BITS(w, 2, 13, 4), BITS(w, 2, 25, 1), BITS(w, 2, 17, 8), b);
        fetch(m, w, BITS(w, 3, 28, 2), (BITS(w, 2, 0, 2) << 2) | BITS(w, 3, 30, 2), BITS(w, 2, 10, 1),
              BITS(w, 2, 2, 8), c);

        bool have_t = false, have_u = false;
        int new_a0 = m->a0;
        switch (mac) {
        case MAC_NOP: break;
        case MAC_MOV: memcpy(t, a, sizeof t); have_t = true; break;
        case MAC_MUL: for (int k = 0; k < 4; k++) t[k] = mul0(a[k], b[k]); have_t = true; break;
        case MAC_ADD: for (int k = 0; k < 4; k++) t[k] = a[k] + c[k]; have_t = true; break;
        case MAC_MAD: for (int k = 0; k < 4; k++) t[k] = mul0(a[k], b[k]) + c[k]; have_t = true; break;
        case MAC_DP3: { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; for (int k = 0; k < 4; k++) t[k] = d; have_t = true; break; }
        case MAC_DPH: { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3]; for (int k = 0; k < 4; k++) t[k] = d; have_t = true; break; }
        case MAC_DP4: { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; for (int k = 0; k < 4; k++) t[k] = d; have_t = true; break; }
        case MAC_DST: t[0] = 1; t[1] = a[1] * b[1]; t[2] = a[2]; t[3] = b[3]; have_t = true; break;
        case MAC_MIN: for (int k = 0; k < 4; k++) t[k] = fminf(a[k], b[k]); have_t = true; break;
        case MAC_MAX: for (int k = 0; k < 4; k++) t[k] = fmaxf(a[k], b[k]); have_t = true; break;
        case MAC_SLT: for (int k = 0; k < 4; k++) t[k] = a[k] < b[k] ? 1.0f : 0.0f; have_t = true; break;
        case MAC_SGE: for (int k = 0; k < 4; k++) t[k] = a[k] >= b[k] ? 1.0f : 0.0f; have_t = true; break;
        case MAC_ARL: new_a0 = (int)floorf(a[0] + 0.001f); break;
        default: fprintf(stderr, "bad MAC opcode %u\n", mac); exit(3);
        }
        float s = c[0];
        switch (ilu) {
        case ILU_NOP: break;
        case ILU_MOV: memcpy(u, c, sizeof u); have_u = true; break;
        case ILU_RCP: for (int k = 0; k < 4; k++) u[k] = 1.0f / s; have_u = true; break;
        case ILU_RCC: for (int k = 0; k < 4; k++) u[k] = rcc(s); have_u = true; break;
        case ILU_RSQ: for (int k = 0; k < 4; k++) u[k] = 1.0f / sqrtf(fabsf(s)); have_u = true; break;
        case ILU_EXP: u[0] = exp2f(floorf(s)); u[1] = s - floorf(s); u[2] = exp2f(s); u[3] = 1; have_u = true; break;
        case ILU_LOG: {
            float x = fabsf(s);
            if (x == 0.0f) { u[0] = -FLT_MAX; u[1] = 1; u[2] = -FLT_MAX; u[3] = 1; }
            else { float l = log2f(x), f = floorf(l); u[0] = f; u[1] = x / exp2f(f); u[2] = l; u[3] = 1; }
            have_u = true;
            break;
        }
        case ILU_LIT: {
            float x = fmaxf(c[0], 0), y = fmaxf(c[1], 0), ww = fminf(fmaxf(c[3], -127.9961f), 127.9961f);
            u[0] = 1; u[1] = x; u[3] = 1;
            u[2] = x > 0 ? (y > 0 ? exp2f(ww * log2f(y)) : (ww == 0 ? 1.0f : 0.0f)) : 0.0f;
            have_u = true;
            break;
        }
        }
        /* Writes, after both reads. */
        bool paired = mac != MAC_NOP && ilu != ILU_NOP;
        if (have_t && rwm && !(paired && rw == 1 && swm)) {
            unsigned r = rw > 12 ? 0 : rw;
            for (int k = 0; k < 4; k++) if (rwm & (8 >> k)) m->R[r][k] = t[k];
        }
        if (have_u && swm) {
            unsigned r = paired ? 1 : (rw > 12 ? 0 : rw);
            for (int k = 0; k < 4; k++) if (swm & (8 >> k)) m->R[r][k] = u[k];
        }
        if (owm) {
            const float *src = om ? (have_u ? u : NULL) : (have_t ? t : NULL);
            if (src) {
                if (orb) {
                    unsigned idx = oaddr & 15;
                    written |= 1u << idx;
                    float *dst = idx == 0 ? m->R[12] : m->o[idx];
                    if (idx == 5 || idx == 6) {
                        int first = 0;
                        while (first < 4 && !(owm & (8 >> first))) first++;
                        dst[0] = src[first];
                    } else {
                        for (int k = 0; k < 4; k++) if (owm & (8 >> k)) dst[k] = src[k];
                    }
                } else if (oaddr < 192) {
                    for (int k = 0; k < 4; k++) if (owm & (8 >> k)) m->c[oaddr][k] = src[k];
                }
            }
        }
        m->a0 = new_a0;
        if (eos) break;
    }
    return written;
}

/* ------------------------------------------------------------------ GL */

static GLuint (APIENTRY *p_glCreateShader)(GLenum);
static void (APIENTRY *p_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (APIENTRY *p_glCompileShader)(GLuint);
static void (APIENTRY *p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (APIENTRY *p_glCreateProgram)(void);
static void (APIENTRY *p_glAttachShader)(GLuint, GLuint);
static void (APIENTRY *p_glLinkProgram)(GLuint);
static void (APIENTRY *p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static void (APIENTRY *p_glBindAttribLocation)(GLuint, GLuint, const char *);
static void (APIENTRY *p_glUseProgram)(GLuint);
static GLint (APIENTRY *p_glGetUniformLocation)(GLuint, const char *);
static void (APIENTRY *p_glUniform4fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *p_glUniform1f)(GLint, GLfloat);
static void (APIENTRY *p_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (APIENTRY *p_glEnableVertexAttribArray)(GLuint);
static void (APIENTRY *p_glGenBuffers)(GLsizei, GLuint *);
static void (APIENTRY *p_glBindBuffer)(GLenum, GLuint);
static void (APIENTRY *p_glBufferData)(GLenum, ptrdiff_t, const void *, GLenum);
static void (APIENTRY *p_glBindBufferBase)(GLenum, GLuint, GLuint);
static void (APIENTRY *p_glTransformFeedbackVaryings)(GLuint, GLsizei, const char *const *, GLenum);
static void (APIENTRY *p_glBeginTransformFeedback)(GLenum);
static void (APIENTRY *p_glEndTransformFeedback)(void);
static void (APIENTRY *p_glGetBufferSubData)(GLenum, ptrdiff_t, ptrdiff_t, void *);
static void (APIENTRY *p_glDeleteShader)(GLuint);
static void (APIENTRY *p_glDeleteProgram)(GLuint);

#define GLE_RASTERIZER_DISCARD 0x8C89
#define GLE_TRANSFORM_FEEDBACK_BUFFER 0x8C8E
#define GLE_INTERLEAVED_ATTRIBS 0x8C8C
#define GLE_ARRAY_BUFFER 0x8892
#define GLE_STATIC_DRAW 0x88E4
#define GLE_STATIC_READ 0x88E1
#define GLE_COMPILE_STATUS 0x8B81
#define GLE_LINK_STATUS 0x8B82
#define GLE_INFO_LOG_LENGTH 0x8B84
#define GLE_VERTEX_SHADER 0x8B31

static const char *const varyings[] = {
    "gl_Position", "gl_FrontColor", "gl_FrontSecondaryColor", "gl_BackColor", "gl_BackSecondaryColor",
    "gl_TexCoord[0]", "gl_TexCoord[1]", "gl_TexCoord[2]", "gl_TexCoord[3]", "gl_FogFragCoord", "gl_PointSize"
};
static const int varying_size[] = { 4, 4, 4, 4, 4, 4, 4, 4, 4, 1, 1 };
#define NVARY 11
#define STRIDE 38

static GLuint build(const char *src, char *log, size_t loglen)
{
    GLuint sh = p_glCreateShader(GLE_VERTEX_SHADER);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GLE_COMPILE_STATUS, &ok);
    if (!ok) { p_glGetShaderInfoLog(sh, loglen, NULL, log); p_glDeleteShader(sh); return 0; }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, sh);
    for (int i = 0; i < 16; i++) {
        char name[8];
        snprintf(name, sizeof name, "v%d", i);
        p_glBindAttribLocation(prog, i, name);
    }
    p_glTransformFeedbackVaryings(prog, NVARY, varyings, GLE_INTERLEAVED_ATTRIBS);
    p_glLinkProgram(prog);
    p_glGetProgramiv(prog, GLE_LINK_STATUS, &ok);
    if (!ok) { p_glGetProgramInfoLog(prog, loglen, NULL, log); p_glDeleteProgram(prog); p_glDeleteShader(sh); return 0; }
    p_glDeleteShader(sh);
    return prog;
}

/* --------------------------------------------------------------- driver */

static uint32_t rng = 12345;
static float frand(void)   /* in -1 .. 1 */
{
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / (float)(1u << 23) - 1.0f;
}

static const char *vary_name(int f, int *comp)
{
    for (int v = 0, off = 0; v < NVARY; off += varying_size[v], v++)
        if (f < off + varying_size[v]) { *comp = f - off; return varyings[v]; }
    *comp = 0;
    return "?";
}

static unsigned char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n > 0 ? n : 1);
    if (n > 0 && fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *size = n > 0 ? n : 0;
    return buf;
}

int main(int argc, char **argv)
{
    int nverts = 24, verbose = 0, argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "-n") && argi + 1 < argc) nverts = atoi(argv[argi + 1]), argi += 2;
        else if (!strcmp(argv[argi], "-v")) verbose = 1, argi++;
        else { fprintf(stderr, "usage: vsh_exec [-n vertices] [-v] file.xvu...\n"); return 2; }
    }
    if (argi >= argc || nverts < 1 || nverts > 4096) { fprintf(stderr, "usage: vsh_exec [-n vertices] [-v] file.xvu...\n"); return 2; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window *win = SDL_CreateWindow("vsh_exec", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win || !SDL_GL_CreateContext(win)) { fprintf(stderr, "GL: %s\n", SDL_GetError()); return 2; }
#define LOAD(n) do { p_##n = SDL_GL_GetProcAddress(#n); if (!p_##n) { fprintf(stderr, "no %s\n", #n); return 2; } } while (0)
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram);
    LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog); LOAD(glBindAttribLocation); LOAD(glUseProgram);
    LOAD(glGetUniformLocation); LOAD(glUniform4fv); LOAD(glUniform1f); LOAD(glVertexAttribPointer);
    LOAD(glEnableVertexAttribArray); LOAD(glGenBuffers); LOAD(glBindBuffer); LOAD(glBufferData);
    LOAD(glBindBufferBase); LOAD(glTransformFeedbackVaryings); LOAD(glBeginTransformFeedback);
    LOAD(glEndTransformFeedback); LOAD(glGetBufferSubData); LOAD(glDeleteShader); LOAD(glDeleteProgram);
#undef LOAD

    float (*attrs)[16][4] = calloc(nverts, sizeof(*attrs));
    float *result = calloc(nverts * STRIDE, sizeof(float));
    GLuint abo, tbo;
    p_glGenBuffers(1, &abo);
    p_glGenBuffers(1, &tbo);
    p_glBindBuffer(GLE_TRANSFORM_FEEDBACK_BUFFER, tbo);
    p_glBufferData(GLE_TRANSFORM_FEEDBACK_BUFFER, nverts * STRIDE * sizeof(float), NULL, GLE_STATIC_READ);
    p_glBindBufferBase(GLE_TRANSFORM_FEEDBACK_BUFFER, 0, tbo);
    glEnable(GLE_RASTERIZER_DISCARD);

    int failed = 0, total = 0;
    for (; argi < argc; argi++) {
        const char *path = argv[argi];
        total++;
        size_t size;
        unsigned char *blob = slurp(path, &size);
        if (!blob || size < 20) { printf("%s: cannot read\n", path); failed++; free(blob); continue; }
        unsigned count = blob[2] | blob[3] << 8;
        if (!count || 4 + 16u * count > size) { printf("%s: bad header\n", path); failed++; free(blob); continue; }
        uint32_t *code = malloc(16 * count);
        memcpy(code, blob + 4, 16 * count);
        free(blob);
        char *glsl = vsh_translate(code, count);
        if (!glsl) { printf("%s: NOT TRANSLATED\n", path); failed++; free(code); continue; }
        char log[4096] = "";
        GLuint prog = build(glsl, log, sizeof log);
        free(glsl);
        if (!prog) { printf("%s: GLSL build failed:\n%s\n", path, log); failed++; free(code); continue; }

        /* Inputs: everything pseudo-random, the viewport constants real. */
        machine m;
        for (int i = 0; i < 192; i++) for (int k = 0; k < 4; k++) m.c[i][k] = frand() * 2.0f;
        m.c[58][0] = 320; m.c[58][1] = -240; m.c[58][2] = 16777215.0f * 0.9f; m.c[58][3] = 0;
        m.c[59][0] = 320; m.c[59][1] = 240; m.c[59][2] = 16777215.0f * 0.05f; m.c[59][3] = 0;
        for (int v = 0; v < nverts; v++)
            for (int i = 0; i < 16; i++) for (int k = 0; k < 4; k++) attrs[v][i][k] = frand() * 2.0f;
        float flip = (total & 1) ? 1.0f : -1.0f;

        p_glUseProgram(prog);
        GLint loc = p_glGetUniformLocation(prog, "c");
        if (loc >= 0) p_glUniform4fv(loc, 192, &m.c[0][0]);
        loc = p_glGetUniformLocation(prog, "flip_y");
        if (loc >= 0) p_glUniform1f(loc, flip);
        p_glBindBuffer(GLE_ARRAY_BUFFER, abo);
        p_glBufferData(GLE_ARRAY_BUFFER, nverts * sizeof(*attrs), attrs, GLE_STATIC_DRAW);
        for (int i = 0; i < 16; i++) {
            p_glEnableVertexAttribArray(i);
            p_glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(*attrs), (const void *)(uintptr_t)(i * 16));
        }
        p_glBeginTransformFeedback(GL_POINTS);
        glDrawArrays(GL_POINTS, 0, nverts);
        p_glEndTransformFeedback();
        glFinish();
        p_glGetBufferSubData(GLE_TRANSFORM_FEEDBACK_BUFFER, 0, nverts * STRIDE * sizeof(float), result);

        int bad = 0;
        for (int v = 0; v < nverts; v++) {
            memcpy(g_attr, attrs[v], sizeof g_attr);
            machine mv = m;   /* constants written by the program do not persist */
            unsigned written = run(&mv, code, count);
            float expect[STRIDE];
            float n[3];
            for (int k = 0; k < 3; k++)
                n[k] = mv.c[58][k] != 0.0f ? (mv.R[12][k] - mv.c[59][k]) / mv.c[58][k] : 0.0f;
            float w = mv.R[12][3];
            w = w >= 0.0f ? fminf(fmaxf(w, 5.42101086e-20f), 1.84467441e19f)
                          : fminf(fmaxf(w, -1.84467441e19f), -5.42101086e-20f);
            expect[0] = n[0] * w; expect[1] = n[1] * flip * w; expect[2] = (2.0f * n[2] - 1.0f) * w; expect[3] = w;
            const float *d0 = mv.o[3], *d1 = mv.o[4], *b0 = (written & (1u << 7)) ? mv.o[7] : mv.o[3],
                        *b1 = (written & (1u << 8)) ? mv.o[8] : mv.o[4];
            for (int k = 0; k < 4; k++) {
                expect[4 + k] = fminf(fmaxf(d0[k], 0), 1);
                expect[8 + k] = fminf(fmaxf(d1[k], 0), 1);
                expect[12 + k] = fminf(fmaxf(b0[k], 0), 1);
                expect[16 + k] = fminf(fmaxf(b1[k], 0), 1);
                for (int tx = 0; tx < 4; tx++) expect[20 + tx * 4 + k] = mv.o[9 + tx][k];
            }
            expect[36] = mv.o[5][0];
            expect[37] = mv.o[6][0];
            const float *got = result + v * STRIDE;
            for (int f = 0; f < STRIDE; f++) {
                float e = expect[f], g = got[f];
                bool ok;
                if (isnan(e) || isnan(g) || isinf(e) || isinf(g)) ok = (isnan(e) && isnan(g)) || (isinf(e) && isinf(g) && (e > 0) == (g > 0));
                else {
                    float scale = fmaxf(1.0f, fmaxf(fabsf(e), fabsf(g)));
                    ok = fabsf(e - g) <= 2e-3f * scale || (fabsf(e) > 1e15f && fabsf(g) > 1e15f && (e > 0) == (g > 0));
                }
                if (!ok) {
                    if (bad < 8 || verbose) {
                        int comp;
                        const char *name = vary_name(f, &comp);
                        printf("%s: vertex %d %s.%c: interpreter %g, GL %g\n", path, v, name, "xyzw"[comp], e, g);
                    }
                    bad++;
                }
            }
        }
        if (bad) { printf("%s: %d mismatching values over %d vertices\n", path, bad, nverts); failed++; }
        else printf("%s: %u instructions, %d vertices match\n", path, count, nverts);
        p_glDeleteProgram(prog);
        free(code);
    }
    printf("%d of %d shaders match the interpreter\n", total - failed, total);
    return failed ? 1 : 0;
}
