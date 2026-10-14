/*
 * CPU interpreter for NV2A vertex programs, used for state shaders
 * (D3DDevice_RunVertexStateShader): programs that read v0 and the constant
 * registers and write constants, run once on the CPU rather than per vertex.
 * The same interpreter checks the GLSL translator in tests/vsh_exec.c.
 */
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nv2a_shaders.h"

enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };

typedef struct {
    float R[13][4];       /* R12 is oPos */
    float o[16][4];       /* o[0] is unused: oPos lives in R[12] */
    float (*c)[4];        /* the constant registers, hardware numbering, written in place */
    int a0;
} machine;

#define BITS(w, dw, lo, n) (((w)[dw] >> (lo)) & ((1u << (n)) - 1))

static float g_attr[16][4];   /* the vertex being interpreted (v0 = the state shader input) */

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
        default: break;
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


void vsh_run_state(const uint32_t *code, unsigned count, const float v0[4], float consts[192][4])
{
    machine m;
    memset(&m, 0, sizeof m);
    m.c = consts;
    memset(g_attr, 0, sizeof g_attr);
    if (v0) memcpy(g_attr[0], v0, 16);
    run(&m, code, count);
}
