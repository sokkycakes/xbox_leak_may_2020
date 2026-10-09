/*
 * vsh_synth: translate hand-encoded NV2A vertex program instructions that
 * exercise rules none of the sample shaders use, check the GLSL for the
 * expected statements, and (with -g) compile it with glslcheck.
 *
 *   vsh_synth [-o outdir] [-g glslcheck]
 *
 * Build (any word size):
 *   gcc -std=gnu11 -Wall -Isrc/hle -o vsh_synth tests/vsh_synth.c src/hle/vsh.c
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nv2a_shaders.h"

/* ---------------------------------------------------------------- encoder */

enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { K_NONE, K_R, K_V, K_C };
enum { O_POS = 0, O_D0 = 3, O_D1 = 4, O_FOG = 5, O_PTS = 6, O_B0 = 7, O_B1 = 8, O_T0 = 9 };

#define SWZ(x, y, z, w) (((x) << 6) | ((y) << 4) | ((z) << 2) | (w))
#define XYZW SWZ(0, 1, 2, 3)
#define XXXX SWZ(0, 0, 0, 0)
#define WWWW SWZ(3, 3, 3, 3)
#define M_X 8
#define M_Y 4
#define M_Z 2
#define M_W 1
#define M_XYZW 15

typedef struct { unsigned kind, reg, neg, swz; } src;
typedef struct {
    unsigned ilu, mac, cidx, vidx;
    src a, b, c;
    unsigned mac_mask, out_r, ilu_mask;
    unsigned o_mask, o_is_out, o_addr, o_from_ilu, a0x, final;
} ins;

#define R(r, swz) { K_R, (r), 0, (swz) }
#define V(swz) { K_V, 0, 0, (swz) }
#define C(swz) { K_C, 0, 0, (swz) }
#define NEGV(swz) { K_V, 0, 1, (swz) }
#define NEGC(swz) { K_C, 0, 1, (swz) }

static void encode(const ins *i, uint32_t *w)
{
    w[0] = 0;
    w[1] = (i->ilu << 25) | (i->mac << 21) | (i->cidx << 13) | (i->vidx << 9) | (i->a.neg << 8) | i->a.swz;
    w[2] = (i->a.reg << 28) | (i->a.kind << 26) | (i->b.neg << 25) | (i->b.swz << 17) | (i->b.reg << 13) |
           (i->b.kind << 11) | (i->c.neg << 10) | (i->c.swz << 2) | (i->c.reg >> 2);
    w[3] = ((i->c.reg & 3) << 30) | (i->c.kind << 28) | (i->mac_mask << 24) | (i->out_r << 20) |
           (i->ilu_mask << 16) | (i->o_mask << 12) | (i->o_is_out << 11) | (i->o_addr << 3) |
           (i->o_from_ilu << 2) | (i->a0x << 1) | i->final;
}

/* ------------------------------------------------------------------ cases */

typedef struct {
    const char *name;
    ins code[12];
    unsigned n;
    const char *expect[8];      /* substrings the GLSL must contain */
    const char *forbid[4];      /* substrings it must not contain */
} testcase;

static const testcase cases[] = {
    {
        /* Paired: the MAC aims at R1 while the ILU writes a temp: the MAC
           result is dropped, the ILU lands in R1 (not in out_r). */
        "paired-mac-r1-dropped",
        { { .mac = MAC_MUL, .ilu = ILU_RCP, .cidx = 96, .a = V(XYZW), .b = C(XYZW), .c = V(WWWW),
            .mac_mask = M_X | M_Y | M_Z, .out_r = 1, .ilu_mask = M_W,
            .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_POS, .o_from_ilu = 0 },
          { .mac = MAC_MOV, .a = R(1, XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_D0, .final = 1 } },
        2,
        { "R1.w = u.w;", "oPos = t;", "t = _mul(v0, c[96]);", "u = _rcp(v0.w);" },
        { "R1.xyz = t.xyz;" }
    },
    {
        /* Paired, but the ILU only feeds the output: the MAC may use R1. */
        "paired-mac-r1-kept",
        { { .mac = MAC_MUL, .ilu = ILU_RCP, .cidx = 96, .a = V(XYZW), .b = C(XYZW), .c = V(WWWW),
            .mac_mask = M_X | M_Y | M_Z, .out_r = 1, .ilu_mask = 0,
            .o_mask = M_X, .o_is_out = 1, .o_addr = O_FOG, .o_from_ilu = 1 },
          { .mac = MAC_MOV, .a = R(1, XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_POS, .final = 1 } },
        2,
        { "R1.xyz = t.xyz;", "oFog.x = u.x;" },
        { "R1.w = u" }
    },
    {
        /* Paired ILU goes to R1 whatever out_r says (the assembler leaves 7
           there); an unpaired ILU uses out_r. */
        "ilu-temp-register",
        { { .mac = MAC_MUL, .ilu = ILU_RCC, .cidx = 58, .a = R(12, XYZW), .b = C(XYZW), .c = R(12, WWWW),
            .mac_mask = 0, .out_r = 7, .ilu_mask = M_X,
            .o_mask = M_X | M_Y | M_Z, .o_is_out = 1, .o_addr = O_POS, .o_from_ilu = 0 },
          { .ilu = ILU_RSQ, .c = V(XXXX), .out_r = 7, .ilu_mask = M_W },
          { .mac = MAC_MAD, .cidx = 59, .a = R(12, XYZW), .b = R(1, XXXX), .c = C(XYZW),
            .o_mask = M_X | M_Y | M_Z, .o_is_out = 1, .o_addr = O_POS },
          { .mac = MAC_MOV, .a = R(7, XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_D0, .final = 1 } },
        4,
        { "R1.x = u.x;", "R7.w = u.w;", "oPos.xyz = t.xyz;", "u = _rcc(oPos.w);", "u = _rsq(v0.x);" },
        { "R7.x = u" }
    },
    {
        /* Relative addressing applies to the reads of the instruction, the
           constant written as the output is absolute. */
        "relative-read-absolute-write",
        { { .mac = MAC_ARL, .a = V(SWZ(2, 2, 2, 2)) },
          { .mac = MAC_MUL, .cidx = 100, .a = C(XYZW), .b = C(SWZ(1, 1, 1, 1)), .a0x = 1,
            .o_mask = M_XYZW, .o_is_out = 0, .o_addr = 5 },
          { .mac = MAC_MOV, .cidx = 5, .a = C(XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_POS, .final = 1 } },
        3,
        { "A0 = int(floor(v0.z + 0.001));", "t = _mul(cc[ridx(A0 + 100)], cc[ridx(A0 + 100)].yyyy);",
          "cc[5] = t;", "t = cc[5];", "for (int i = 0; i < 192; i++) cc[i] = c[i];" },
        { "cc[ridx(A0 + 5)]", "c[100]" }
    },
    {
        /* The scalar outputs take the most significant masked component. */
        "fog-and-point-size-masks",
        { { .mac = MAC_MOV, .vidx = 1, .a = V(XYZW), .o_mask = M_Z | M_W, .o_is_out = 1, .o_addr = O_FOG },
          { .ilu = ILU_MOV, .vidx = 2, .c = V(XYZW), .o_mask = M_Y, .o_is_out = 1, .o_addr = O_PTS, .o_from_ilu = 1 },
          { .mac = MAC_MOV, .a = V(XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_POS, .final = 1 } },
        3,
        { "oFog.x = t.z;", "oPts.x = u.y;", "gl_FogFragCoord = oFog.x;", "gl_PointSize = oPts.x;" },
        { "oFog.zw =", "oPts.y =" }
    },
    {
        /* Every opcode once, with negation and swizzles everywhere, plus the
           back colours and all texture outputs. */
        "all-opcodes",
        { { .mac = MAC_DP3, .ilu = ILU_LIT, .cidx = 97, .a = NEGV(SWZ(1, 2, 0, 3)), .b = C(XYZW), .c = V(SWZ(0, 1, 3, 3)),
            .mac_mask = M_X, .out_r = 2, .ilu_mask = M_XYZW, .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_D0, .o_from_ilu = 1 },
          { .mac = MAC_DPH, .ilu = ILU_EXP, .cidx = 98, .a = R(2, XXXX), .b = NEGC(XYZW), .c = R(1, SWZ(3, 0, 0, 0)),
            .mac_mask = M_Y, .out_r = 2, .ilu_mask = M_Y | M_W, .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_B0, .o_from_ilu = 0 },
          { .mac = MAC_DST, .ilu = ILU_LOG, .cidx = 99, .a = R(2, XYZW), .b = C(XYZW), .c = NEGV(WWWW),
            .mac_mask = M_XYZW, .out_r = 3, .ilu_mask = M_Z, .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_B1, .o_from_ilu = 1 },
          { .mac = MAC_MIN, .a = R(3, XYZW), .b = R(1, XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_T0 },
          { .mac = MAC_MAX, .a = R(3, XYZW), .b = R(1, XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_T0 + 1 },
          { .mac = MAC_SLT, .a = R(3, XYZW), .b = R(1, XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_T0 + 2 },
          { .mac = MAC_SGE, .a = R(3, XYZW), .b = R(1, XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_T0 + 3 },
          { .mac = MAC_ADD, .ilu = ILU_RSQ, .a = R(3, XYZW), .c = R(1, SWZ(2, 2, 2, 2)), .ilu_mask = M_XYZW,
            .mac_mask = M_XYZW, .out_r = 4, .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_D1, .o_from_ilu = 0 },
          { .mac = MAC_MAD, .cidx = 96, .a = R(4, XYZW), .b = C(XYZW), .c = R(1, XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_POS },
          { .mac = MAC_MUL, .ilu = ILU_RCP, .a = R(12, XYZW), .b = R(4, XYZW), .c = R(12, WWWW), .ilu_mask = M_X,
            .o_mask = M_X, .o_is_out = 1, .o_addr = O_PTS, .o_from_ilu = 1, .final = 1 },
          { .mac = MAC_MOV, .a = V(XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_POS } },
        11,
        { "dot((-v0.yzxw).xyz, (c[97]).xyz)", "u = _lit(v0.xyww);", "dot(vec4((R2.xxxx).xyz, 1.0), -c[98])",
          "u = _exp(R1.w);", "_dst(R2, c[99])", "u = _log(-v0.w);", "gl_BackColor = clamp(oB0, 0.0, 1.0);" },
        { "/* 10:", "gl_BackColor = clamp(oD0" }
    },
    {
        /* A program that stops at the final bit (instruction 11 above is
           after it) and never writes oD0/oB0: the back colours fall back. */
        "defaults",
        { { .mac = MAC_MOV, .a = V(XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_POS, .final = 1 },
          { .mac = MAC_MOV, .a = V(XYZW), .o_mask = M_XYZW, .o_is_out = 1, .o_addr = O_D0 } },
        2,
        { "gl_BackColor = clamp(oD0, 0.0, 1.0);", "gl_BackSecondaryColor = clamp(oD1, 0.0, 1.0);" },
        { "/* 1:" }
    },
};

/* -------------------------------------------------------------------- main */

static void quote(const char *s, char *out, size_t n)
{
    size_t i = 0;
    if (i + 1 < n) out[i++] = '\'';
    for (; *s && i + 5 < n; s++) {
        if (*s == '\'') { memcpy(out + i, "'\\''", 4); i += 4; }
        else out[i++] = *s;
    }
    if (i + 1 < n) out[i++] = '\'';
    out[i] = 0;
}

int main(int argc, char **argv)
{
    const char *outdir = "/tmp/claude-0/xbe/vsh-synth";
    const char *glslcheck = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) glslcheck = argv[++i];
        else { fprintf(stderr, "usage: vsh_synth [-o outdir] [-g glslcheck]\n"); return 2; }
    }
    if (mkdir(outdir, 0777) && errno != EEXIST) { perror(outdir); return 2; }

    int failed = 0, total = 0;
    for (size_t t = 0; t < sizeof(cases) / sizeof(cases[0]); t++) {
        const testcase *tc = &cases[t];
        total++;
        uint32_t code[12 * 4];
        for (unsigned i = 0; i < tc->n; i++) encode(&tc->code[i], code + i * 4);
        char *glsl = vsh_translate(code, tc->n);
        if (!glsl) { printf("%s: NOT TRANSLATED\n", tc->name); failed++; continue; }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.glsl", outdir, tc->name);
        FILE *f = fopen(path, "w");
        if (f) { fputs(glsl, f); fclose(f); }
        int bad = 0;
        for (int k = 0; k < 8 && tc->expect[k]; k++)
            if (!strstr(glsl, tc->expect[k])) { printf("%s: missing \"%s\"\n", tc->name, tc->expect[k]); bad = 1; }
        for (int k = 0; k < 4 && tc->forbid[k]; k++)
            if (strstr(glsl, tc->forbid[k])) { printf("%s: unexpected \"%s\"\n", tc->name, tc->forbid[k]); bad = 1; }
        free(glsl);
        if (glslcheck) {
            char q1[600], q2[1200], cmd[2048];
            quote(glslcheck, q1, sizeof q1);
            quote(path, q2, sizeof q2);
            snprintf(cmd, sizeof cmd, "%s vert %s", q1, q2);
            fflush(stdout);
            if (system(cmd) != 0) { printf("%s: GLSL FAILED\n", tc->name); bad = 1; }
        }
        if (bad) failed++;
        else printf("%s: ok (%s)\n", tc->name, path);
    }
    printf("%d of %d synthetic programs OK\n", total - failed, total);
    return failed ? 1 : 0;
}
