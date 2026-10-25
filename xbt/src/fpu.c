/*
 * fpu.c - x87 operations implemented in C (called from translated code).
 */
#include <math.h>
#include <string.h>
#include "xbt.h"

enum {
    X87H_F2XM1 = 1, X87H_FYL2X, X87H_FPTAN, X87H_FPATAN, X87H_FXTRACT, X87H_FPREM1,
    X87H_FPREM, X87H_FYL2XP1, X87H_FSINCOS, X87H_FSCALE, X87H_FSIN, X87H_FCOS,
    X87H_FXAM, X87H_FNINIT, X87H_FLDCW_SYNC,
    X87M_FLD80 = 32, X87M_FSTP80, X87M_FBLD, X87M_FBSTP, X87M_FLDENV, X87M_FNSTENV,
    X87M_FRSTOR, X87M_FNSAVE,
};

#define ST(i) cpu->fpr[(cpu->ftop + (i)) & 7]

static void push(xbt_cpu *cpu, double v)
{
    cpu->ftop = (cpu->ftop - 1) & 7;
    ST(0) = v;
}

static void pop(xbt_cpu *cpu) { cpu->ftop = (cpu->ftop + 1) & 7; }

static void set_c(xbt_cpu *cpu, int c0, int c1, int c2, int c3)
{
    uint16_t s = cpu->fsw & (uint16_t)~0x4700u;
    s |= (uint16_t)((c0 ? 0x100 : 0) | (c1 ? 0x200 : 0) | (c2 ? 0x400 : 0) | (c3 ? 0x4000 : 0));
    cpu->fsw = s;
}

/* mirror FCW.RC into the host FPCR (rounding mode used by translated code) */
void xbt_fpcr_from_fcw(xbt_cpu *cpu)
{
    uint32_t rc = (cpu->fcw >> 10) & 3;
    uint32_t arm = ((rc & 1) << 1) | (rc >> 1);
    cpu->fpcr = (cpu->fpcr & ~(3u << 22)) | arm << 22;
#if defined(__aarch64__)
    uint64_t f;
    __asm__ volatile("mrs %0, fpcr" : "=r"(f));
    f = (f & ~(3ull << 22)) | (uint64_t)arm << 22;
    __asm__ volatile("msr fpcr, %0" : : "r"(f));
#endif
}

/* ---- 80-bit extended <-> double ---- */

static double from_ext(const uint8_t *p)
{
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, p, 8);
    memcpy(&se, p + 8, 2);
    int sign = se >> 15;
    int exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0)
        v = 0.0;
    else if (exp == 0x7fff && (mant << 1)) {
        uint64_t d = 0x7ff8000000000000ull | ((mant >> 11) & 0x000fffffffffffffull);
        memcpy(&v, &d, 8);
    } else if (exp == 0x7fff)
        v = INFINITY;
    else
        v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

static void to_ext(uint8_t *p, double v)
{
    uint64_t mant = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) {
        uint64_t d;
        memcpy(&d, &v, 8);
        /* keep the payload, as FLD m64 does; SNaNs become quiet */
        mant = 0xc000000000000000ull | (d & 0x000fffffffffffffull) << 11;
        se |= 0x7fff;
    } else if (isinf(v)) {
        mant = 0x8000000000000000ull;
        se |= 0x7fff;
    } else if (v != 0.0) {
        int e;
        double m = frexp(fabs(v), &e);       /* m in [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8);
    memcpy(p + 8, &se, 2);
}

static uint16_t tag_word(xbt_cpu *cpu)
{
    uint16_t tw = 0;
    for (int i = 0; i < 8; i++) {
        int t;
        if (cpu->ftw & (1u << i))
            t = 3;
        else if (cpu->fpr[i] == 0.0)
            t = 1;
        else if (!isfinite(cpu->fpr[i]))
            t = 2;
        else
            t = 0;
        tw |= (uint16_t)(t << (2 * i));
    }
    return tw;
}

static uint16_t full_fsw(xbt_cpu *cpu)
{
    return (uint16_t)((cpu->fsw & ~0x3800u) | (cpu->ftop & 7) << 11);
}

static void store_env(xbt_cpu *cpu, uint8_t *p)
{
    uint32_t w[7] = { 0xffff0000u | cpu->fcw, 0xffff0000u | full_fsw(cpu),
                      0xffff0000u | tag_word(cpu), 0, 0, 0, 0xffff0000u };
    memcpy(p, w, 28);
}

static void load_env(xbt_cpu *cpu, const uint8_t *p)
{
    uint32_t w[3];
    memcpy(w, p, 12);
    cpu->fcw = (uint16_t)w[0];
    cpu->fsw = (uint16_t)w[1] & (uint16_t)~0x3800u;
    cpu->ftop = (w[1] >> 11) & 7;
    cpu->ftw = 0;
    for (int i = 0; i < 8; i++)
        if (((w[2] >> (2 * i)) & 3) == 3)
            cpu->ftw |= (uint16_t)(1u << i);
    xbt_fpcr_from_fcw(cpu);
}

static void fninit(xbt_cpu *cpu)
{
    cpu->fcw = 0x037f;
    cpu->fsw = 0;
    cpu->ftw = 0xff;
    cpu->ftop = 0;
    xbt_fpcr_from_fcw(cpu);
}

static void x87_helper(xbt_cpu *cpu, uint32_t op)
{
    double a = ST(0), b = ST(1);
    switch (op) {
    case X87H_F2XM1: ST(0) = exp2(a) - 1.0; break;
    case X87H_FYL2X: ST(1) = b * log2(a); pop(cpu); break;
    case X87H_FPTAN: ST(0) = tan(a); push(cpu, 1.0); set_c(cpu, 0, 0, 0, 0); break;
    case X87H_FPATAN: ST(1) = atan2(b, a); pop(cpu); break;
    case X87H_FXTRACT: {
        if (a == 0.0) {
            ST(0) = -INFINITY;
            push(cpu, a);
        } else {
            int e;
            double m = frexp(a, &e);
            ST(0) = (double)(e - 1);
            push(cpu, m * 2.0);
        }
        break;
    }
    case X87H_FPREM: case X87H_FPREM1: {
        double q = a / b;
        q = op == X87H_FPREM ? trunc(q) : nearbyint(q);
        double r = op == X87H_FPREM ? fmod(a, b) : remainder(a, b);
        long long qi = isfinite(q) ? (long long)fmod(fabs(q), 8.0) : 0;
        ST(0) = r;
        set_c(cpu, (int)(qi >> 2) & 1, (int)qi & 1, 0, (int)(qi >> 1) & 1);
        break;
    }
    case X87H_FYL2XP1: ST(1) = b * log2(a + 1.0); pop(cpu); break;
    case X87H_FSINCOS: ST(0) = sin(a); push(cpu, cos(a)); set_c(cpu, 0, 0, 0, 0); break;
    case X87H_FSCALE: ST(0) = ldexp(a, (int)trunc(b)); break;
    case X87H_FSIN: ST(0) = sin(a); set_c(cpu, 0, 0, 0, 0); break;
    case X87H_FCOS: ST(0) = cos(a); set_c(cpu, 0, 0, 0, 0); break;
    case X87H_FXAM: {
        int c0, c2, c3, c1 = signbit(a) ? 1 : 0;
        if (cpu->ftw & (1u << (cpu->ftop & 7))) { c3 = 1; c2 = 0; c0 = 1; }
        else if (isnan(a)) { c3 = 0; c2 = 0; c0 = 1; }
        else if (isinf(a)) { c3 = 0; c2 = 1; c0 = 1; }
        else if (a == 0.0) { c3 = 1; c2 = 0; c0 = 0; }
        else if (fpclassify(a) == FP_SUBNORMAL) { c3 = 1; c2 = 1; c0 = 0; }
        else { c3 = 0; c2 = 1; c0 = 0; }
        set_c(cpu, c0, c1, c2, c3);
        break;
    }
    case X87H_FNINIT: fninit(cpu); break;
    case X87H_FLDCW_SYNC: xbt_fpcr_from_fcw(cpu); break;
    }
}

static void x87_mem_helper(xbt_cpu *cpu, uint32_t op, uint32_t addr)
{
    uint8_t *p = (uint8_t *)(uintptr_t)addr;
    switch (op) {
    case X87M_FLD80: push(cpu, from_ext(p)); break;
    case X87M_FSTP80: to_ext(p, ST(0)); pop(cpu); break;
    case X87M_FBLD: {
        double v = 0;
        for (int i = 8; i >= 0; i--)
            v = v * 100 + (p[i] >> 4) * 10 + (p[i] & 15);
        push(cpu, (p[9] & 0x80) ? -v : v);
        break;
    }
    case X87M_FBSTP: {
        double v = nearbyint(ST(0));
        int neg = v < 0;
        unsigned long long u = (unsigned long long)fabs(v);
        for (int i = 0; i < 9; i++) {
            p[i] = (uint8_t)((u % 10) | ((u / 10) % 10) << 4);
            u /= 100;
        }
        p[9] = neg ? 0x80 : 0;
        pop(cpu);
        break;
    }
    case X87M_FLDENV: load_env(cpu, p); break;
    case X87M_FNSTENV: store_env(cpu, p); cpu->fcw |= 0x3f; break;
    case X87M_FRSTOR:
        load_env(cpu, p);
        for (int i = 0; i < 8; i++)
            ST(i) = from_ext(p + 28 + 10 * i);
        break;
    case X87M_FNSAVE:
        store_env(cpu, p);
        for (int i = 0; i < 8; i++)
            to_ext(p + 28 + 10 * i, ST(i));
        fninit(cpu);
        break;
    }
}

/* fxsave/fxrstor (used by the SSE translator) */
void xbt_fxsave(xbt_cpu *cpu, uint8_t *p)
{
    memset(p, 0, 512);
    uint16_t fsw = full_fsw(cpu);
    uint8_t ftw = 0;
    for (int i = 0; i < 8; i++)
        if (!(cpu->ftw & (1u << i)))
            ftw |= (uint8_t)(1u << i);
    memcpy(p, &cpu->fcw, 2);
    memcpy(p + 2, &fsw, 2);
    p[4] = ftw;
    memcpy(p + 24, &cpu->mxcsr, 4);
    uint32_t mask = 0xffbf;
    memcpy(p + 28, &mask, 4);
    for (int i = 0; i < 8; i++)
        to_ext(p + 32 + 16 * i, ST(i));
    memcpy(p + 160, cpu->xmm, 128);
}

void xbt_fxrstor(xbt_cpu *cpu, const uint8_t *p)
{
    uint16_t fsw;
    memcpy(&cpu->fcw, p, 2);
    memcpy(&fsw, p + 2, 2);
    cpu->fsw = fsw & (uint16_t)~0x3800u;
    cpu->ftop = (fsw >> 11) & 7;
    cpu->ftw = 0;
    for (int i = 0; i < 8; i++)
        if (!(p[4] & (1u << i)))
            cpu->ftw |= (uint16_t)(1u << i);
    memcpy(&cpu->mxcsr, p + 24, 4);
    for (int i = 0; i < 8; i++)
        ST(i) = from_ext(p + 32 + 16 * i);
    memcpy(cpu->xmm, p + 160, 128);
    xbt_fpcr_from_fcw(cpu);
}

void *xbt_x87_helper = (void *)x87_helper;
void *xbt_x87_mem_helper = (void *)x87_mem_helper;
