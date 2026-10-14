/*
 * x87, MMX and SSE for the xlat interpreter.
 *
 * x87 registers hold doubles, as Box86 and most translators keep them: the
 * Xbox's games set 53-bit precision or don't care, and 80-bit loads and
 * stores convert.  The rounding mode is honored where it decides integer
 * results (fist, frndint); arithmetic rounds to nearest.  MMX registers are
 * kept apart from the x87 ones (code switches between them with emms).
 * SSE is the Pentium III's: single precision only, no SSE2.
 */
#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "xlat.h"

#define C0 0x0100u
#define C1 0x0200u
#define C2 0x0400u
#define C3 0x4000u

#define ST(i) c->st[(c->top + (i)) & 7]

void xi_fpu_push(xcpu *c, double v)
{
    c->top = (c->top - 1) & 7;
    c->st[c->top] = v;
    c->ftag &= ~(1u << c->top);
}

static inline void fpush(xcpu *c, double v) { xi_fpu_push(c, v); }

static inline void fpop(xcpu *c)
{
    c->ftag |= 1u << c->top;
    c->top = (c->top + 1) & 7;
}

static inline float rdf32(uint32_t a) { float f; memcpy(&f, (const void *)(uintptr_t)a, 4); return f; }
static inline double rdf64(uint32_t a) { double d; memcpy(&d, (const void *)(uintptr_t)a, 8); return d; }
static inline void wrf32(uint32_t a, float f) { memcpy((void *)(uintptr_t)a, &f, 4); }
static inline void wrf64(uint32_t a, double d) { memcpy((void *)(uintptr_t)a, &d, 8); }

double x87_load80(const void *p)
{
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, p, 8);
    memcpy(&se, (const char *)p + 8, 2);
    int sign = se >> 15, exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void x87_store80(void *p, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, 8);
    uint16_t sign = (uint16_t)(bits >> 63) << 15;
    int exp = (int)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & ((1ull << 52) - 1), mant = 0;
    uint16_t se;
    if (exp == 0 && frac == 0) {
        se = sign;
    } else if (exp == 0x7FF) {
        se = sign | 0x7FFF;
        mant = (1ull << 63) | (frac << 11);
    } else {
        if (exp == 0) {   /* subnormal double: normalize */
            exp = 1;
            while (!(frac & (1ull << 52))) { frac <<= 1; exp--; }
            frac &= (1ull << 52) - 1;
        }
        se = sign | (uint16_t)(exp - 1023 + 16383);
        mant = (1ull << 63) | (frac << 11);
    }
    memcpy(p, &mant, 8);
    memcpy((char *)p + 8, &se, 2);
}

static double fround(const xcpu *c, double v)
{
    switch ((c->fpcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

/* To a bits-wide integer, rounding as the control word says; the integer
   indefinite value when it doesn't fit. */
static uint64_t f2i(const xcpu *c, double v, int bits)
{
    double r = fround(c, v), lim = ldexp(1.0, bits - 1);
    if (!(r >= -lim && r < lim)) return 1ull << (bits - 1);
    return (uint64_t)(int64_t)r;
}

static void fcom(xcpu *c, double a, double b)
{
    uint32_t sw = c->fpsw & ~(C0 | C1 | C2 | C3);
    if (isnan(a) || isnan(b)) sw |= C0 | C2 | C3;
    else if (a < b) sw |= C0;
    else if (a == b) sw |= C3;
    c->fpsw = sw;
}

static void fcomi(xcpu *c, double a, double b)
{
    uint32_t f = 0;
    bool pf = false;
    if (isnan(a) || isnan(b)) { f = XF_Z | XF_C; pf = true; }
    else if (a < b) f = XF_C;
    else if (a == b) f = XF_Z;
    c->nzcv = f;
    c->pf_res = pf ? 0 : 1;
    c->af = 0;
    c->fpsw &= ~C1;
}

static double farith(int op, double x, double y)
{
    switch (op) {
    case 0: return x + y;
    case 1: return x * y;
    case 4: return x - y;
    case 5: return y - x;
    case 6: return x / y;
    default: return y / x;
    }
}

static uint32_t fsw_word(const xcpu *c) { return (c->fpsw & ~0x3800u) | (c->top << 11); }

static uint32_t full_tag(const xcpu *c)
{
    uint32_t t = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t v;
        if (c->ftag & (1u << i)) v = 3;
        else if (c->st[i] == 0.0) v = 1;
        else if (!isfinite(c->st[i]) || fpclassify(c->st[i]) == FP_SUBNORMAL) v = 2;
        else v = 0;
        t |= v << (2 * i);
    }
    return t;
}

static void fnstenv(xcpu *c, uint32_t a)
{
    wr32(a, c->fpcw | 0xFFFF0000u);
    wr32(a + 4, fsw_word(c) | 0xFFFF0000u);
    wr32(a + 8, full_tag(c) | 0xFFFF0000u);
    wr32(a + 12, 0);
    wr32(a + 16, 0);
    wr32(a + 20, 0);
    wr32(a + 24, 0xFFFF0000u);
}

static void fldenv(xcpu *c, uint32_t a)
{
    c->fpcw = rd16(a);
    uint32_t sw = rd16(a + 4);
    c->top = (sw >> 11) & 7;
    c->fpsw = sw & ~0x3800u;
    uint32_t tag = rd16(a + 8);
    c->ftag = 0;
    for (int i = 0; i < 8; i++)
        if (((tag >> (2 * i)) & 3) == 3) c->ftag |= 1u << i;
}

static void fninit(xcpu *c)
{
    c->fpcw = 0x37F;
    c->fpsw = 0;
    c->top = 0;
    c->ftag = 0xFF;
}

static void fxam(xcpu *c)
{
    double v = ST(0);
    uint32_t sw = c->fpsw & ~(C0 | C1 | C2 | C3);
    if (signbit(v)) sw |= C1;
    if (c->ftag & (1u << c->top)) sw |= C3 | C0;
    else if (isnan(v)) sw |= C0;
    else if (isinf(v)) sw |= C2 | C0;
    else if (v == 0.0) sw |= C3;
    else if (fpclassify(v) == FP_SUBNORMAL) sw |= C3 | C2;
    else sw |= C2;
    c->fpsw = sw;
}

static void fprem(xcpu *c, bool ieee)
{
    double a = ST(0), b = ST(1);
    /* The whole reduction at once (C2 clear); the quotient's low three bits
       from the remainder modulo 8 * b, which is exact. */
    double r = ieee ? remainder(a, b) : fmod(a, b);
    double r8 = fmod(a, 8.0 * b);
    uint64_t qi = 0;
    if (isfinite(r) && isfinite(r8)) qi = (uint64_t)(int64_t)fabs(nearbyint((r8 - r) / b));
    uint32_t sw = c->fpsw & ~(C0 | C1 | C2 | C3);
    if (qi & 1) sw |= C1;
    if (qi & 2) sw |= C3;
    if (qi & 4) sw |= C0;
    c->fpsw = sw;
    ST(0) = r;
}

static int x87_reg(xcpu *c, const xinsn *in)
{
    unsigned i = in->rm, grp = in->op - 0xD8;
    double *s0 = &ST(0), *si = &ST(i);
    switch (grp) {
    case 0:   /* D8: st0 op st(i) */
        if (in->reg == 2) { fcom(c, *s0, *si); return XI_OK; }
        if (in->reg == 3) { fcom(c, *s0, *si); fpop(c); return XI_OK; }
        *s0 = farith(in->reg, *s0, *si);
        return XI_OK;
    case 1:
        switch (in->reg) {
        case 0: { double v = *si; fpush(c, v); return XI_OK; }
        case 1: { double t = *s0; *s0 = *si; *si = t; return XI_OK; }
        case 2: if (i == 0) return XI_OK; break;   /* fnop */
        case 4:
            switch (i) {
            case 0: *s0 = -*s0; return XI_OK;
            case 1: *s0 = fabs(*s0); return XI_OK;
            case 4: fcom(c, *s0, 0.0); return XI_OK;
            case 5: fxam(c); return XI_OK;
            }
            break;
        case 5: {
            static const double k[7] = { 1.0, 3.32192809488736234787, 1.44269504088896340736,
                                         3.14159265358979323846, 0.30102999566398119521,
                                         0.69314718055994530942, 0.0 };
            if (i < 7) { fpush(c, k[i]); return XI_OK; }
            break;
        }
        case 6:
            switch (i) {
            case 0: *s0 = exp2(*s0) - 1.0; return XI_OK;                     /* f2xm1 */
            case 1: ST(1) = ST(1) * log2(*s0); fpop(c); return XI_OK;        /* fyl2x */
            case 2: *s0 = tan(*s0); c->fpsw &= ~C2; fpush(c, 1.0); return XI_OK;   /* fptan */
            case 3: ST(1) = atan2(ST(1), *s0); fpop(c); return XI_OK;        /* fpatan */
            case 4: {                                                        /* fxtract */
                double v = *s0;
                if (v == 0.0) { *s0 = -INFINITY; fpush(c, v); return XI_OK; }
                if (!isfinite(v)) { *s0 = isnan(v) ? v : INFINITY; fpush(c, v); return XI_OK; }
                int e;
                double m = frexp(v, &e);
                *s0 = (double)(e - 1);
                fpush(c, m * 2.0);
                return XI_OK;
            }
            case 5: fprem(c, true); return XI_OK;                            /* fprem1 */
            case 6: c->top = (c->top - 1) & 7; return XI_OK;                 /* fdecstp */
            case 7: c->top = (c->top + 1) & 7; return XI_OK;                 /* fincstp */
            }
            break;
        case 7:
            switch (i) {
            case 0: fprem(c, false); return XI_OK;
            case 1: ST(1) = ST(1) * (log1p(*s0) / M_LN2); fpop(c); return XI_OK;   /* fyl2xp1 */
            case 2: *s0 = sqrt(*s0); return XI_OK;
            case 3: { double v = *s0; *s0 = sin(v); fpush(c, cos(v)); c->fpsw &= ~C2; return XI_OK; }
            case 4: *s0 = fround(c, *s0); return XI_OK;
            case 5: {                                                        /* fscale */
                double e = trunc(ST(1));
                if (isnan(e)) { *s0 = NAN; return XI_OK; }
                if (isinf(e)) {
                    if ((e > 0 && *s0 == 0.0) || (e < 0 && isinf(*s0))) *s0 = NAN;
                    else if (e > 0) *s0 = copysign(isnan(*s0) ? *s0 : INFINITY, *s0);
                    else *s0 = copysign(isnan(*s0) ? *s0 : 0.0, *s0);
                    return XI_OK;
                }
                int n = e > 4096 ? 4096 : e < -4096 ? -4096 : (int)e;
                *s0 = ldexp(*s0, n);
                return XI_OK;
            }
            case 6: *s0 = sin(*s0); c->fpsw &= ~C2; return XI_OK;
            case 7: *s0 = cos(*s0); c->fpsw &= ~C2; return XI_OK;
            }
            break;
        }
        return XI_INVALID;
    case 2:   /* DA: fcmovb/e/be/u, fucompp */
        if (in->reg < 4) {
            bool t = in->reg == 0 ? xi_cond(c, 2) : in->reg == 1 ? xi_cond(c, 4) : in->reg == 2 ? xi_cond(c, 6)
                   : xi_cond(c, 10);
            if (t) *s0 = *si;
            return XI_OK;
        }
        if (in->reg == 5 && i == 1) { fcom(c, *s0, ST(1)); fpop(c); fpop(c); return XI_OK; }
        return XI_INVALID;
    case 3:   /* DB */
        if (in->reg < 4) {
            bool t = in->reg == 0 ? xi_cond(c, 3) : in->reg == 1 ? xi_cond(c, 5) : in->reg == 2 ? xi_cond(c, 7)
                   : xi_cond(c, 11);
            if (t) *s0 = *si;
            return XI_OK;
        }
        if (in->reg == 4) {
            if (i == 2) { c->fpsw &= ~0x80FFu; return XI_OK; }   /* fnclex */
            if (i == 3) { fninit(c); return XI_OK; }
            if (i < 2 || i == 4) return XI_OK;                   /* feni, fdisi, fsetpm */
            return XI_INVALID;
        }
        if (in->reg == 5 || in->reg == 6) { fcomi(c, *s0, *si); return XI_OK; }
        return XI_INVALID;
    case 4:   /* DC: st(i) op st0 */
        if (in->reg == 2) { fcom(c, *s0, *si); return XI_OK; }
        if (in->reg == 3) { fcom(c, *s0, *si); fpop(c); return XI_OK; }
        *si = farith(in->reg >= 4 ? in->reg ^ 1 : in->reg, *si, *s0);
        return XI_OK;
    case 5:   /* DD */
        switch (in->reg) {
        case 0: c->ftag |= 1u << ((c->top + i) & 7); return XI_OK;   /* ffree */
        case 1: { double t = *s0; *s0 = *si; *si = t; return XI_OK; }
        case 2: *si = *s0; c->ftag &= ~(1u << ((c->top + i) & 7)); return XI_OK;
        case 3: *si = *s0; c->ftag &= ~(1u << ((c->top + i) & 7)); fpop(c); return XI_OK;
        case 4: fcom(c, *s0, *si); return XI_OK;
        case 5: fcom(c, *s0, *si); fpop(c); return XI_OK;
        }
        return XI_INVALID;
    case 6:   /* DE: st(i) op st0, pop */
        if (in->reg == 3) {
            if (i != 1) return XI_INVALID;
            fcom(c, *s0, ST(1)); fpop(c); fpop(c);   /* fcompp */
            return XI_OK;
        }
        if (in->reg == 2) { fcom(c, *s0, *si); fpop(c); return XI_OK; }
        *si = farith(in->reg >= 4 ? in->reg ^ 1 : in->reg, *si, *s0);
        fpop(c);
        return XI_OK;
    default:  /* DF */
        if (in->reg == 4 && i == 0) {   /* fnstsw ax */
            c->r[XR_EAX] = (c->r[XR_EAX] & 0xFFFF0000u) | fsw_word(c);
            return XI_OK;
        }
        if (in->reg == 5 || in->reg == 6) { fcomi(c, *s0, *si); fpop(c); return XI_OK; }
        if (in->reg == 0) { c->ftag |= 1u << ((c->top + i) & 7); fpop(c); return XI_OK; }   /* ffreep */
        if (in->reg == 1) { double t = *s0; *s0 = *si; *si = t; return XI_OK; }
        if (in->reg == 2 || in->reg == 3) { *si = *s0; c->ftag &= ~(1u << ((c->top + i) & 7)); fpop(c); return XI_OK; }
        return XI_INVALID;
    }
}

int xi_x87(xcpu *c, const xinsn *in, uint32_t a)
{
    if (in->mod == 3) return x87_reg(c, in);
    unsigned grp = in->op - 0xD8, r = in->reg;
    double v;
    switch (grp) {
    case 0: case 2: case 4: case 6: {   /* arithmetic with a memory operand */
        switch (grp) {
        case 0: v = rdf32(a); break;
        case 2: v = (int32_t)rd32(a); break;
        case 4: v = rdf64(a); break;
        default: v = (int16_t)rd16(a); break;
        }
        if (r == 2) { fcom(c, ST(0), v); return XI_OK; }
        if (r == 3) { fcom(c, ST(0), v); fpop(c); return XI_OK; }
        ST(0) = farith(r, ST(0), v);
        return XI_OK;
    }
    case 1:
        switch (r) {
        case 0: fpush(c, rdf32(a)); return XI_OK;
        case 2: wrf32(a, (float)ST(0)); return XI_OK;
        case 3: wrf32(a, (float)ST(0)); fpop(c); return XI_OK;
        case 4: fldenv(c, a); return XI_OK;
        case 5: c->fpcw = rd16(a); return XI_OK;
        case 6: fnstenv(c, a); c->fpcw |= 0x3F; return XI_OK;
        case 7: wr16(a, (uint16_t)c->fpcw); return XI_OK;
        }
        return XI_INVALID;
    case 3:
        switch (r) {
        case 0: fpush(c, (int32_t)rd32(a)); return XI_OK;
        case 1: wr32(a, (uint32_t)f2i(&(xcpu){ .fpcw = 0xC00 }, ST(0), 32)); fpop(c); return XI_OK;   /* fisttp */
        case 2: wr32(a, (uint32_t)f2i(c, ST(0), 32)); return XI_OK;
        case 3: wr32(a, (uint32_t)f2i(c, ST(0), 32)); fpop(c); return XI_OK;
        case 5: fpush(c, x87_load80((const void *)(uintptr_t)a)); return XI_OK;
        case 7: x87_store80((void *)(uintptr_t)a, ST(0)); fpop(c); return XI_OK;
        }
        return XI_INVALID;
    case 5:
        switch (r) {
        case 0: fpush(c, rdf64(a)); return XI_OK;
        case 1: wr64(a, f2i(&(xcpu){ .fpcw = 0xC00 }, ST(0), 64)); fpop(c); return XI_OK;
        case 2: wrf64(a, ST(0)); return XI_OK;
        case 3: wrf64(a, ST(0)); fpop(c); return XI_OK;
        case 4:     /* frstor */
            fldenv(c, a);
            for (int i = 0; i < 8; i++) ST(i) = x87_load80((const void *)(uintptr_t)(a + 28 + 10 * i));
            return XI_OK;
        case 6:     /* fnsave */
            fnstenv(c, a);
            for (int i = 0; i < 8; i++) x87_store80((void *)(uintptr_t)(a + 28 + 10 * i), ST(i));
            fninit(c);
            return XI_OK;
        case 7: wr16(a, (uint16_t)fsw_word(c)); return XI_OK;
        }
        return XI_INVALID;
    default:   /* DF */
        switch (r) {
        case 0: fpush(c, (int16_t)rd16(a)); return XI_OK;
        case 1: wr16(a, (uint16_t)f2i(&(xcpu){ .fpcw = 0xC00 }, ST(0), 16)); fpop(c); return XI_OK;
        case 2: wr16(a, (uint16_t)f2i(c, ST(0), 16)); return XI_OK;
        case 3: wr16(a, (uint16_t)f2i(c, ST(0), 16)); fpop(c); return XI_OK;
        case 4: {   /* fbld */
            double d = 0;
            for (int i = 8; i >= 0; i--) {
                uint8_t b = rd8(a + i);
                d = d * 100 + (b >> 4) * 10 + (b & 15);
            }
            fpush(c, (rd8(a + 9) & 0x80) ? -d : d);
            return XI_OK;
        }
        case 5: fpush(c, (double)(int64_t)rd64(a)); return XI_OK;
        case 6: {   /* fbstp */
            double d = fround(c, ST(0));
            uint64_t n = (uint64_t)fabs(d);
            for (int i = 0; i < 9; i++) {
                uint8_t lo = n % 10; n /= 10;
                uint8_t hi = n % 10; n /= 10;
                wr8(a + i, (uint8_t)(hi << 4 | lo));
            }
            wr8(a + 9, signbit(d) ? 0x80 : 0);
            fpop(c);
            return XI_OK;
        }
        case 7: wr64(a, f2i(c, ST(0), 64)); fpop(c); return XI_OK;
        }
        return XI_INVALID;
    }
}

/* ---- MMX and SSE ------------------------------------------------------ */

static inline void mmx_enter(xcpu *c) { c->top = 0; c->ftag = 0; }

static inline mmx_t mm_src(const xcpu *c, const xinsn *in, uint32_t a)
{
    mmx_t v;
    if (in->mod == 3) v = c->mm[in->rm];
    else v.q = rd64(a);
    return v;
}

static inline xmm_t xmm_src(const xcpu *c, const xinsn *in, uint32_t a)
{
    xmm_t v;
    if (in->mod == 3) v = c->xmm[in->rm];
    else memcpy(&v, (const void *)(uintptr_t)a, 16);
    return v;
}

static inline float ss_src(const xcpu *c, const xinsn *in, uint32_t a)
{
    return in->mod == 3 ? c->xmm[in->rm].f[0] : rdf32(a);
}

static inline uint32_t gpr_or_mem32(const xcpu *c, const xinsn *in, uint32_t a)
{
    return in->mod == 3 ? c->r[in->rm] : rd32(a);
}

static int32_t cvt_f2i(const xcpu *c, float f, bool trunc_)
{
    double d = f;
    if (trunc_) d = trunc(d);
    else switch ((c->mxcsr >> 13) & 3) {
        case 0: d = nearbyint(d); break;
        case 1: d = floor(d); break;
        case 2: d = ceil(d); break;
        default: d = trunc(d); break;
    }
    if (!(d >= -2147483648.0 && d < 2147483648.0)) return INT32_MIN;
    return (int32_t)d;
}

static float sse_op(int op, float a, float b)
{
    switch (op) {
    case 0x58: return a + b;
    case 0x59: return a * b;
    case 0x5C: return a - b;
    case 0x5D: return a < b ? a : b;
    case 0x5E: return a / b;
    case 0x5F: return a > b ? a : b;
    case 0x51: return sqrtf(b);
    case 0x52:
    case 0x53: {
        /* The estimates treat denormal inputs and results as zero. */
        if (fpclassify(b) == FP_SUBNORMAL) b = copysignf(0.0f, b);
        float r = op == 0x52 ? 1.0f / sqrtf(b) : 1.0f / b;
        return fpclassify(r) == FP_SUBNORMAL ? copysignf(0.0f, r) : r;
    }
    default: return 0;
    }
}

static bool sse_cmp(int pred, float a, float b)
{
    switch (pred & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return isnan(a) || isnan(b);
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return !isnan(a) && !isnan(b);
    }
}

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline int8_t sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }
static inline uint8_t satu8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }
static inline uint16_t satu16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }

/* MMX shifts by a count (register/memory operand or immediate) */
static mmx_t mmx_shift(mmx_t d, int kind, unsigned elem, uint64_t n)
{
    unsigned bits = elem * 8;
    if (kind == 2 && n >= bits) n = bits - 1;   /* arithmetic: fill with the sign */
    else if (kind != 2 && n >= bits) { d.q = 0; return d; }
    switch (elem) {
    case 2:
        for (int i = 0; i < 4; i++)
            d.w[i] = kind == 0 ? (uint16_t)(d.w[i] >> n) : kind == 1 ? (uint16_t)(d.w[i] << n) : (uint16_t)(d.sw[i] >> n);
        break;
    case 4:
        for (int i = 0; i < 2; i++)
            d.u[i] = kind == 0 ? d.u[i] >> n : kind == 1 ? d.u[i] << n : (uint32_t)(d.i[i] >> n);
        break;
    default:
        d.q = kind == 0 ? d.q >> n : d.q << n;
        break;
    }
    return d;
}

static int mmx_op(xcpu *c, const xinsn *in, uint32_t a)
{
    unsigned op = in->op & 0xFF;
    mmx_enter(c);
    mmx_t *d = &c->mm[in->reg], s;
    switch (op) {
    case 0x6E: d->u[0] = gpr_or_mem32(c, in, a); d->u[1] = 0; return XI_OK;
    case 0x6F: *d = mm_src(c, in, a); return XI_OK;
    case 0x7E:
        if (in->mod == 3) c->r[in->rm] = d->u[0];
        else wr32(a, d->u[0]);
        return XI_OK;
    case 0x7F: case 0xE7:
        if (in->mod == 3) c->mm[in->rm] = *d;
        else wr64(a, d->q);
        return XI_OK;
    case 0x70: {
        s = mm_src(c, in, a);
        mmx_t r;
        for (int i = 0; i < 4; i++) r.w[i] = s.w[(in->imm >> (2 * i)) & 3];
        *d = r;
        return XI_OK;
    }
    case 0x71: case 0x72: case 0x73: {
        if (in->mod != 3) return XI_INVALID;
        unsigned elem = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
        int kind = in->reg == 2 ? 0 : in->reg == 6 ? 1 : in->reg == 4 && elem != 8 ? 2 : -1;
        if (kind < 0) return XI_INVALID;
        c->mm[in->rm] = mmx_shift(c->mm[in->rm], kind, elem, in->imm & 0xFF);
        return XI_OK;
    }
    case 0xC4:
        d->w[in->imm & 3] = (uint16_t)(in->mod == 3 ? c->r[in->rm] : rd16(a));
        return XI_OK;
    case 0xC5:
        if (in->mod != 3) return XI_INVALID;
        c->r[in->reg] = c->mm[in->rm].w[in->imm & 3];
        return XI_OK;
    case 0xD7: {
        if (in->mod != 3) return XI_INVALID;
        uint32_t m = 0;
        for (int i = 0; i < 8; i++) m |= (uint32_t)(c->mm[in->rm].b[i] >> 7) << i;
        c->r[in->reg] = m;
        return XI_OK;
    }
    case 0xF7: {   /* maskmovq: bytes of mm(reg) where mm(rm) has the top bit, to [edi] */
        if (in->mod != 3) return XI_INVALID;
        uint32_t dst = c->r[XR_EDI] + (in->seg == XSEG_FS ? c->fs_base : 0);
        for (int i = 0; i < 8; i++)
            if (c->mm[in->rm].b[i] & 0x80) wr8(dst + i, d->b[i]);
        return XI_OK;
    }
    }
    s = mm_src(c, in, a);
    mmx_t r = *d;
    switch (op) {
    case 0x60: for (int i = 3; i >= 0; i--) { r.b[2 * i + 1] = s.b[i]; r.b[2 * i] = d->b[i]; } break;
    case 0x61: for (int i = 1; i >= 0; i--) { r.w[2 * i + 1] = s.w[i]; r.w[2 * i] = d->w[i]; } break;
    case 0x62: r.u[1] = s.u[0]; r.u[0] = d->u[0]; break;
    case 0x68: for (int i = 0; i < 4; i++) { r.b[2 * i] = d->b[4 + i]; r.b[2 * i + 1] = s.b[4 + i]; } break;
    case 0x69: for (int i = 0; i < 2; i++) { r.w[2 * i] = d->w[2 + i]; r.w[2 * i + 1] = s.w[2 + i]; } break;
    case 0x6A: r.u[0] = d->u[1]; r.u[1] = s.u[1]; break;
    case 0x63: for (int i = 0; i < 4; i++) { r.sb[i] = sat8(d->sw[i]); r.sb[4 + i] = sat8(s.sw[i]); } break;
    case 0x67: for (int i = 0; i < 4; i++) { r.b[i] = satu8(d->sw[i]); r.b[4 + i] = satu8(s.sw[i]); } break;
    case 0x6B: for (int i = 0; i < 2; i++) { r.sw[i] = sat16(d->i[i]); r.sw[2 + i] = sat16(s.i[i]); } break;
    case 0x64: for (int i = 0; i < 8; i++) r.b[i] = d->sb[i] > s.sb[i] ? 0xFF : 0; break;
    case 0x65: for (int i = 0; i < 4; i++) r.w[i] = d->sw[i] > s.sw[i] ? 0xFFFF : 0; break;
    case 0x66: for (int i = 0; i < 2; i++) r.u[i] = d->i[i] > s.i[i] ? 0xFFFFFFFFu : 0; break;
    case 0x74: for (int i = 0; i < 8; i++) r.b[i] = d->b[i] == s.b[i] ? 0xFF : 0; break;
    case 0x75: for (int i = 0; i < 4; i++) r.w[i] = d->w[i] == s.w[i] ? 0xFFFF : 0; break;
    case 0x76: for (int i = 0; i < 2; i++) r.u[i] = d->u[i] == s.u[i] ? 0xFFFFFFFFu : 0; break;
    case 0xD1: r = mmx_shift(*d, 0, 2, s.q); break;
    case 0xD2: r = mmx_shift(*d, 0, 4, s.q); break;
    case 0xD3: r = mmx_shift(*d, 0, 8, s.q); break;
    case 0xE1: r = mmx_shift(*d, 2, 2, s.q); break;
    case 0xE2: r = mmx_shift(*d, 2, 4, s.q); break;
    case 0xF1: r = mmx_shift(*d, 1, 2, s.q); break;
    case 0xF2: r = mmx_shift(*d, 1, 4, s.q); break;
    case 0xF3: r = mmx_shift(*d, 1, 8, s.q); break;
    case 0xD5: for (int i = 0; i < 4; i++) r.w[i] = (uint16_t)(d->sw[i] * s.sw[i]); break;
    case 0xE5: for (int i = 0; i < 4; i++) r.w[i] = (uint16_t)((d->sw[i] * s.sw[i]) >> 16); break;
    case 0xE4: for (int i = 0; i < 4; i++) r.w[i] = (uint16_t)(((uint32_t)d->w[i] * s.w[i]) >> 16); break;
    case 0xF5: for (int i = 0; i < 2; i++) r.i[i] = d->sw[2 * i] * s.sw[2 * i] + d->sw[2 * i + 1] * s.sw[2 * i + 1]; break;
    case 0xD8: for (int i = 0; i < 8; i++) r.b[i] = satu8(d->b[i] - s.b[i]); break;
    case 0xD9: for (int i = 0; i < 4; i++) r.w[i] = satu16(d->w[i] - s.w[i]); break;
    case 0xDC: for (int i = 0; i < 8; i++) r.b[i] = satu8(d->b[i] + s.b[i]); break;
    case 0xDD: for (int i = 0; i < 4; i++) r.w[i] = satu16(d->w[i] + s.w[i]); break;
    case 0xE8: for (int i = 0; i < 8; i++) r.sb[i] = sat8(d->sb[i] - s.sb[i]); break;
    case 0xE9: for (int i = 0; i < 4; i++) r.sw[i] = sat16(d->sw[i] - s.sw[i]); break;
    case 0xEC: for (int i = 0; i < 8; i++) r.sb[i] = sat8(d->sb[i] + s.sb[i]); break;
    case 0xED: for (int i = 0; i < 4; i++) r.sw[i] = sat16(d->sw[i] + s.sw[i]); break;
    case 0xDA: for (int i = 0; i < 8; i++) r.b[i] = d->b[i] < s.b[i] ? d->b[i] : s.b[i]; break;
    case 0xDE: for (int i = 0; i < 8; i++) r.b[i] = d->b[i] > s.b[i] ? d->b[i] : s.b[i]; break;
    case 0xEA: for (int i = 0; i < 4; i++) r.sw[i] = d->sw[i] < s.sw[i] ? d->sw[i] : s.sw[i]; break;
    case 0xEE: for (int i = 0; i < 4; i++) r.sw[i] = d->sw[i] > s.sw[i] ? d->sw[i] : s.sw[i]; break;
    case 0xE0: for (int i = 0; i < 8; i++) r.b[i] = (uint8_t)((d->b[i] + s.b[i] + 1) >> 1); break;
    case 0xE3: for (int i = 0; i < 4; i++) r.w[i] = (uint16_t)((d->w[i] + s.w[i] + 1) >> 1); break;
    case 0xF6: {
        uint32_t sum = 0;
        for (int i = 0; i < 8; i++) sum += (uint32_t)abs(d->b[i] - s.b[i]);
        r.q = sum;
        break;
    }
    case 0xDB: r.q = d->q & s.q; break;
    case 0xDF: r.q = ~d->q & s.q; break;
    case 0xEB: r.q = d->q | s.q; break;
    case 0xEF: r.q = d->q ^ s.q; break;
    case 0xF8: for (int i = 0; i < 8; i++) r.b[i] = (uint8_t)(d->b[i] - s.b[i]); break;
    case 0xF9: for (int i = 0; i < 4; i++) r.w[i] = (uint16_t)(d->w[i] - s.w[i]); break;
    case 0xFA: for (int i = 0; i < 2; i++) r.u[i] = d->u[i] - s.u[i]; break;
    case 0xFC: for (int i = 0; i < 8; i++) r.b[i] = (uint8_t)(d->b[i] + s.b[i]); break;
    case 0xFD: for (int i = 0; i < 4; i++) r.w[i] = (uint16_t)(d->w[i] + s.w[i]); break;
    case 0xFE: for (int i = 0; i < 2; i++) r.u[i] = d->u[i] + s.u[i]; break;
    default: return XI_INVALID;
    }
    *d = r;
    return XI_OK;
}

static void fxsave(const xcpu *c, uint32_t a)
{
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    uint16_t w = (uint16_t)c->fpcw;
    memcpy(buf, &w, 2);
    w = (uint16_t)fsw_word(c);
    memcpy(buf + 2, &w, 2);
    buf[4] = (uint8_t)~c->ftag;
    memcpy(buf + 24, &c->mxcsr, 4);
    uint32_t mask = 0xFFBF;
    memcpy(buf + 28, &mask, 4);
    for (int i = 0; i < 8; i++) x87_store80(buf + 32 + 16 * i, c->st[(c->top + i) & 7]);
    memcpy(buf + 160, c->xmm, 128);
    memcpy((void *)(uintptr_t)a, buf, sizeof(buf));
}

static void fxrstor(xcpu *c, uint32_t a)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)a;
    c->fpcw = rd16(a);
    uint32_t sw = rd16(a + 2);
    c->top = (sw >> 11) & 7;
    c->fpsw = sw & ~0x3800u;
    c->ftag = (uint8_t)~p[4];
    c->mxcsr = rd32(a + 24);
    for (int i = 0; i < 8; i++) c->st[(c->top + i) & 7] = x87_load80(p + 32 + 16 * i);
    memcpy(c->xmm, p + 160, 128);
}

int xi_sse(xcpu *c, const xinsn *in, uint32_t a)
{
    unsigned op = in->op & 0xFF;
    bool f3 = in->pfx & XP_REP;
    if (in->op < 0x110 || (in->pfx & (XP_OPSZ | XP_REPNE))) {
        /* 66/F2 forms are SSE2 and later: not on a Pentium III. */
        return XI_INVALID;
    }
    if ((op >= 0x60 && op <= 0x7F && op != 0x77) || (op >= 0xD0 && op != 0xD6) || op == 0xC4 || op == 0xC5) {
        if (f3) return XI_INVALID;
        return mmx_op(c, in, a);
    }
    xmm_t *d = &c->xmm[in->reg];
    switch (op) {
    case 0x77: c->ftag = 0xFF; return XI_OK;   /* emms */
    case 0x10:
        if (f3) {
            if (in->mod == 3) d->f[0] = c->xmm[in->rm].f[0];
            else { d->u[0] = rd32(a); d->u[1] = d->u[2] = d->u[3] = 0; }
        } else {
            *d = xmm_src(c, in, a);
        }
        return XI_OK;
    case 0x11:
        if (f3) {
            if (in->mod == 3) c->xmm[in->rm].u[0] = d->u[0];
            else wr32(a, d->u[0]);
        } else if (in->mod == 3) {
            c->xmm[in->rm] = *d;
        } else {
            memcpy((void *)(uintptr_t)a, d, 16);
        }
        return XI_OK;
    case 0x28: if (f3) return XI_INVALID; *d = xmm_src(c, in, a); return XI_OK;
    case 0x29: case 0x2B:
        if (f3) return XI_INVALID;
        if (in->mod == 3) {
            if (op == 0x2B) return XI_INVALID;
            c->xmm[in->rm] = *d;
        } else {
            memcpy((void *)(uintptr_t)a, d, 16);
        }
        return XI_OK;
    case 0x12:
        if (f3) return XI_INVALID;
        if (in->mod == 3) d->q[0] = c->xmm[in->rm].q[1];   /* movhlps */
        else d->q[0] = rd64(a);
        return XI_OK;
    case 0x13:
        if (f3 || in->mod == 3) return XI_INVALID;
        wr64(a, d->q[0]);
        return XI_OK;
    case 0x16:
        if (f3) return XI_INVALID;
        if (in->mod == 3) d->q[1] = c->xmm[in->rm].q[0];   /* movlhps */
        else d->q[1] = rd64(a);
        return XI_OK;
    case 0x17:
        if (f3 || in->mod == 3) return XI_INVALID;
        wr64(a, d->q[1]);
        return XI_OK;
    case 0x14: case 0x15: {
        if (f3) return XI_INVALID;
        xmm_t s = xmm_src(c, in, a), r;
        int h = op == 0x15 ? 2 : 0;
        r.u[0] = d->u[h]; r.u[1] = s.u[h]; r.u[2] = d->u[h + 1]; r.u[3] = s.u[h + 1];
        *d = r;
        return XI_OK;
    }
    case 0x2A:
        if (f3) { d->f[0] = (float)(int32_t)gpr_or_mem32(c, in, a); return XI_OK; }
        else {
            mmx_t s = mm_src(c, in, a);
            mmx_enter(c);
            d->f[0] = (float)s.i[0];
            d->f[1] = (float)s.i[1];
            return XI_OK;
        }
    case 0x2C: case 0x2D: {
        bool t = op == 0x2C;
        if (f3) {
            c->r[in->reg] = (uint32_t)cvt_f2i(c, ss_src(c, in, a), t);
        } else {
            mmx_t s;
            if (in->mod == 3) { s.u[0] = c->xmm[in->rm].u[0]; s.u[1] = c->xmm[in->rm].u[1]; }
            else s.q = rd64(a);
            mmx_enter(c);
            float f0, f1;
            memcpy(&f0, &s.u[0], 4);
            memcpy(&f1, &s.u[1], 4);
            c->mm[in->reg].i[0] = cvt_f2i(c, f0, t);
            c->mm[in->reg].i[1] = cvt_f2i(c, f1, t);
        }
        return XI_OK;
    }
    case 0x2E: case 0x2F: {
        if (f3) return XI_INVALID;
        float x = d->f[0], y = ss_src(c, in, a);
        uint32_t f = 0;
        bool pf = false;
        if (isnan(x) || isnan(y)) { f = XF_Z | XF_C; pf = true; }
        else if (x < y) f = XF_C;
        else if (x == y) f = XF_Z;
        c->nzcv = f;
        c->pf_res = pf ? 0 : 1;
        c->af = 0;
        return XI_OK;
    }
    case 0x50: {
        if (f3 || in->mod != 3) return XI_INVALID;
        const xmm_t *s = &c->xmm[in->rm];
        c->r[in->reg] = (s->u[0] >> 31) | (s->u[1] >> 31) << 1 | (s->u[2] >> 31) << 2 | (s->u[3] >> 31) << 3;
        return XI_OK;
    }
    case 0x51: case 0x52: case 0x53:
    case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        if (f3) {
            d->f[0] = sse_op(op, d->f[0], ss_src(c, in, a));
        } else {
            xmm_t s = xmm_src(c, in, a);
            for (int i = 0; i < 4; i++) d->f[i] = sse_op(op, d->f[i], s.f[i]);
        }
        return XI_OK;
    case 0x54: case 0x55: case 0x56: case 0x57: {
        if (f3) return XI_INVALID;
        xmm_t s = xmm_src(c, in, a);
        for (int i = 0; i < 4; i++)
            d->u[i] = op == 0x54 ? d->u[i] & s.u[i] : op == 0x55 ? ~d->u[i] & s.u[i]
                    : op == 0x56 ? d->u[i] | s.u[i] : d->u[i] ^ s.u[i];
        return XI_OK;
    }
    case 0xC2:
        if (f3) {
            d->u[0] = sse_cmp(in->imm, d->f[0], ss_src(c, in, a)) ? 0xFFFFFFFFu : 0;
        } else {
            xmm_t s = xmm_src(c, in, a);
            for (int i = 0; i < 4; i++) d->u[i] = sse_cmp(in->imm, d->f[i], s.f[i]) ? 0xFFFFFFFFu : 0;
        }
        return XI_OK;
    case 0xC6: {
        if (f3) return XI_INVALID;
        xmm_t s = xmm_src(c, in, a), r;
        unsigned k = in->imm;
        r.u[0] = d->u[k & 3];
        r.u[1] = d->u[(k >> 2) & 3];
        r.u[2] = s.u[(k >> 4) & 3];
        r.u[3] = s.u[(k >> 6) & 3];
        *d = r;
        return XI_OK;
    }
    case 0xAE:
        if (f3) return XI_INVALID;
        if (in->mod == 3) {
            if (in->reg >= 5) { __atomic_thread_fence(__ATOMIC_SEQ_CST); return XI_OK; }   /* lfence mfence sfence */
            return XI_INVALID;
        }
        switch (in->reg) {
        case 0: fxsave(c, a); return XI_OK;
        case 1: fxrstor(c, a); return XI_OK;
        case 2: c->mxcsr = rd32(a); return XI_OK;
        case 3: wr32(a, c->mxcsr); return XI_OK;
        case 7: return XI_OK;   /* clflush */
        }
        return XI_INVALID;
    }
    return XI_INVALID;
}
