/*
 * a64.h - AArch64 instruction encoder used by the translator.
 *
 * Every function returns or emits one 32-bit A64 instruction word. Register
 * number 31 means WZR/XZR or SP depending on the instruction, exactly as in
 * the architecture manual. Functions prefixed with "w" operate on 32-bit
 * registers, "x" on 64-bit registers.
 */
#ifndef XBT_A64_H
#define XBT_A64_H

#include <stdint.h>
#include <stddef.h>

typedef struct a64buf {
    uint32_t *start;
    uint32_t *p;
    uint32_t *end;
    int overflow;
} a64buf;

static inline void a64_put(a64buf *b, uint32_t ins)
{
    if (b->p < b->end)
        *b->p++ = ins;
    else
        b->overflow = 1;
}

#define ZR 31
#define SP 31

/* Condition codes. */
enum {
    CC_EQ = 0, CC_NE, CC_HS, CC_LO, CC_MI, CC_PL, CC_VS, CC_VC,
    CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE, CC_AL, CC_NV
};
#define CC_CS CC_HS
#define CC_CC CC_LO
static inline int cc_inv(int c) { return c ^ 1; }

/* Shift types for shifted-register operands. */
enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2, SH_ROR = 3 };
/* Extend options for extended-register operands and register-offset loads. */
enum { EX_UXTB = 0, EX_UXTH = 1, EX_UXTW = 2, EX_UXTX = 3,
       EX_SXTB = 4, EX_SXTH = 5, EX_SXTW = 6, EX_SXTX = 7 };

/* ------------------------------------------------------------------ */
/* Arithmetic                                                          */
/* ------------------------------------------------------------------ */

static inline uint32_t enc_addsub_sreg(int sf, int sub, int s, int rd, int rn,
                                       int rm, int sh, int amt)
{
    return 0x0B000000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 |
           (uint32_t)s << 29 | (uint32_t)sh << 22 | (uint32_t)rm << 16 |
           (uint32_t)(amt & 63) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t enc_addsub_ereg(int sf, int sub, int s, int rd, int rn,
                                       int rm, int opt, int amt)
{
    return 0x0B200000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 |
           (uint32_t)s << 29 | (uint32_t)rm << 16 | (uint32_t)opt << 13 |
           (uint32_t)(amt & 7) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t enc_addsub_imm(int sf, int sub, int s, int rd, int rn,
                                      uint32_t imm12, int sh12)
{
    return 0x11000000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 |
           (uint32_t)s << 29 | (uint32_t)sh12 << 22 | (imm12 & 0xfff) << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}

/* rd = rn + (rm <<sh amt) */
#define w_add(b, d, n, m)            a64_put(b, enc_addsub_sreg(0, 0, 0, d, n, m, SH_LSL, 0))
#define w_adds(b, d, n, m)           a64_put(b, enc_addsub_sreg(0, 0, 1, d, n, m, SH_LSL, 0))
#define w_sub(b, d, n, m)            a64_put(b, enc_addsub_sreg(0, 1, 0, d, n, m, SH_LSL, 0))
#define w_subs(b, d, n, m)           a64_put(b, enc_addsub_sreg(0, 1, 1, d, n, m, SH_LSL, 0))
#define w_add_sh(b, d, n, m, t, a)   a64_put(b, enc_addsub_sreg(0, 0, 0, d, n, m, t, a))
#define w_adds_sh(b, d, n, m, t, a)  a64_put(b, enc_addsub_sreg(0, 0, 1, d, n, m, t, a))
#define w_sub_sh(b, d, n, m, t, a)   a64_put(b, enc_addsub_sreg(0, 1, 0, d, n, m, t, a))
#define w_subs_sh(b, d, n, m, t, a)  a64_put(b, enc_addsub_sreg(0, 1, 1, d, n, m, t, a))
#define w_cmp(b, n, m)               w_subs(b, ZR, n, m)
#define w_cmn(b, n, m)               w_adds(b, ZR, n, m)
#define w_cmp_sh(b, n, m, t, a)      w_subs_sh(b, ZR, n, m, t, a)
#define w_neg(b, d, m)               w_sub(b, d, ZR, m)
#define w_negs(b, d, m)              w_subs(b, d, ZR, m)
#define x_add(b, d, n, m)            a64_put(b, enc_addsub_sreg(1, 0, 0, d, n, m, SH_LSL, 0))
#define x_sub(b, d, n, m)            a64_put(b, enc_addsub_sreg(1, 1, 0, d, n, m, SH_LSL, 0))
#define x_add_sh(b, d, n, m, t, a)   a64_put(b, enc_addsub_sreg(1, 0, 0, d, n, m, t, a))
/* x_add_ext: xd = xn + extend(wm) << amt, usable with SP as rn/rd */
#define x_add_ext(b, d, n, m, o, a)  a64_put(b, enc_addsub_ereg(1, 0, 0, d, n, m, o, a))

/* Immediate forms (imm must be encodable, see a64_addimm_ok). */
#define w_addi(b, d, n, i)    a64_put(b, enc_addsub_imm(0, 0, 0, d, n, i, 0))
#define w_subi(b, d, n, i)    a64_put(b, enc_addsub_imm(0, 1, 0, d, n, i, 0))
#define w_addsi(b, d, n, i)   a64_put(b, enc_addsub_imm(0, 0, 1, d, n, i, 0))
#define w_subsi(b, d, n, i)   a64_put(b, enc_addsub_imm(0, 1, 1, d, n, i, 0))
#define w_cmpi(b, n, i)       w_subsi(b, ZR, n, i)
#define w_cmni(b, n, i)       w_addsi(b, ZR, n, i)
#define x_addi(b, d, n, i)    a64_put(b, enc_addsub_imm(1, 0, 0, d, n, i, 0))
#define x_subi(b, d, n, i)    a64_put(b, enc_addsub_imm(1, 1, 0, d, n, i, 0))
#define w_addi12(b, d, n, i)  a64_put(b, enc_addsub_imm(0, 0, 0, d, n, i, 1))
#define w_subi12(b, d, n, i)  a64_put(b, enc_addsub_imm(0, 1, 0, d, n, i, 1))
#define x_addi12(b, d, n, i)  a64_put(b, enc_addsub_imm(1, 0, 0, d, n, i, 1))
#define w_mov_sp(b, d, n)     w_addi(b, d, n, 0)
#define x_mov_sp(b, d, n)     x_addi(b, d, n, 0)

/* Add/sub with carry. */
static inline uint32_t enc_adc(int sf, int sub, int s, int rd, int rn, int rm)
{
    return 0x1A000000u | (uint32_t)sf << 31 | (uint32_t)sub << 30 |
           (uint32_t)s << 29 | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define w_adc(b, d, n, m)   a64_put(b, enc_adc(0, 0, 0, d, n, m))
#define w_adcs(b, d, n, m)  a64_put(b, enc_adc(0, 0, 1, d, n, m))
#define w_sbc(b, d, n, m)   a64_put(b, enc_adc(0, 1, 0, d, n, m))
#define w_sbcs(b, d, n, m)  a64_put(b, enc_adc(0, 1, 1, d, n, m))

/* ------------------------------------------------------------------ */
/* Logical                                                             */
/* ------------------------------------------------------------------ */

enum { LOG_AND = 0, LOG_ORR = 1, LOG_EOR = 2, LOG_ANDS = 3 };

static inline uint32_t enc_log_sreg(int sf, int opc, int neg, int rd, int rn,
                                    int rm, int sh, int amt)
{
    return 0x0A000000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 |
           (uint32_t)sh << 22 | (uint32_t)neg << 21 | (uint32_t)rm << 16 |
           (uint32_t)(amt & 63) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define w_and(b, d, n, m)          a64_put(b, enc_log_sreg(0, LOG_AND, 0, d, n, m, 0, 0))
#define w_orr(b, d, n, m)          a64_put(b, enc_log_sreg(0, LOG_ORR, 0, d, n, m, 0, 0))
#define w_eor(b, d, n, m)          a64_put(b, enc_log_sreg(0, LOG_EOR, 0, d, n, m, 0, 0))
#define w_ands(b, d, n, m)         a64_put(b, enc_log_sreg(0, LOG_ANDS, 0, d, n, m, 0, 0))
#define w_bic(b, d, n, m)          a64_put(b, enc_log_sreg(0, LOG_AND, 1, d, n, m, 0, 0))
#define w_orn(b, d, n, m)          a64_put(b, enc_log_sreg(0, LOG_ORR, 1, d, n, m, 0, 0))
#define w_tst(b, n, m)             w_ands(b, ZR, n, m)
#define w_mvn(b, d, m)             w_orn(b, d, ZR, m)
#define w_mov(b, d, m)             w_orr(b, d, ZR, m)
#define x_mov(b, d, m)             a64_put(b, enc_log_sreg(1, LOG_ORR, 0, d, ZR, m, 0, 0))
#define w_and_sh(b, d, n, m, t, a) a64_put(b, enc_log_sreg(0, LOG_AND, 0, d, n, m, t, a))
#define w_orr_sh(b, d, n, m, t, a) a64_put(b, enc_log_sreg(0, LOG_ORR, 0, d, n, m, t, a))
#define w_eor_sh(b, d, n, m, t, a) a64_put(b, enc_log_sreg(0, LOG_EOR, 0, d, n, m, t, a))
#define w_ands_sh(b, d, n, m, t, a) a64_put(b, enc_log_sreg(0, LOG_ANDS, 0, d, n, m, t, a))
#define w_tst_sh(b, n, m, t, a)    w_ands_sh(b, ZR, n, m, t, a)
#define x_orr_sh(b, d, n, m, t, a) a64_put(b, enc_log_sreg(1, LOG_ORR, 0, d, n, m, t, a))

/*
 * Encode a logical (bitmask) immediate. Returns 0 on failure, otherwise the
 * N:immr:imms field (bits 22..10 of the instruction) | 0x80000000 as a marker.
 */
static inline uint32_t a64_bitmask(uint64_t imm, int is64)
{
    if (!is64)
        imm = (imm & 0xffffffffu) | (imm << 32);
    if (imm == 0 || imm == ~0ull)
        return 0;
    /* find element size */
    int size = 64;
    while (size > 2) {
        int h = size / 2;
        uint64_t mask = (1ull << h) - 1;
        if ((imm & mask) != ((imm >> h) & mask))
            break;
        size = h;
    }
    uint64_t emask = size == 64 ? ~0ull : (1ull << size) - 1;
    uint64_t e = imm & emask;
    /* rotate so that the run of ones starts at bit 0 */
    int rot = 0;
    /* find a position where bit 0 is 1 and the bit at the top of the element is 0 */
    for (rot = 0; rot < size; rot++) {
        uint64_t r = ((e >> rot) | (e << (size - rot))) & emask;
        if (size == 64)
            r = rot ? ((e >> rot) | (e << (64 - rot))) : e;
        if ((r & 1) && !(r >> (size - 1) & 1)) {
            /* r must be a contiguous run of ones from bit 0 */
            if ((r & (r + 1)) != 0)
                return 0;
            int ones = __builtin_popcountll(r);
            int immr = (size - rot) % size;
            int imms;
            int n = 0;
            switch (size) {
            case 64: n = 1; imms = ones - 1; break;
            case 32: imms = ones - 1; break;
            case 16: imms = 0x20 | (ones - 1); break;
            case 8:  imms = 0x30 | (ones - 1); break;
            case 4:  imms = 0x38 | (ones - 1); break;
            default: imms = 0x3c | (ones - 1); break;
            }
            return 0x80000000u | (uint32_t)n << 12 | (uint32_t)immr << 6 | (uint32_t)imms;
        }
    }
    return 0;
}

static inline uint32_t enc_log_imm(int sf, int opc, int rd, int rn, uint32_t nrs)
{
    return 0x12000000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 |
           (nrs & 0x1fff) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}

/* ------------------------------------------------------------------ */
/* Moves                                                               */
/* ------------------------------------------------------------------ */

static inline uint32_t enc_movw(int sf, int opc, int rd, uint32_t imm16, int hw)
{
    return 0x12800000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 |
           (uint32_t)hw << 21 | (imm16 & 0xffff) << 5 | (uint32_t)rd;
}
#define w_movz(b, d, i, hw) a64_put(b, enc_movw(0, 2, d, i, hw))
#define w_movn(b, d, i, hw) a64_put(b, enc_movw(0, 0, d, i, hw))
#define w_movk(b, d, i, hw) a64_put(b, enc_movw(0, 3, d, i, hw))
#define x_movz(b, d, i, hw) a64_put(b, enc_movw(1, 2, d, i, hw))
#define x_movk(b, d, i, hw) a64_put(b, enc_movw(1, 3, d, i, hw))

/* Load a 32-bit constant into wd with the fewest instructions. */
static inline void w_movi(a64buf *b, int rd, uint32_t v)
{
    uint32_t lo = v & 0xffff, hi = v >> 16;
    if (hi == 0) {
        w_movz(b, rd, lo, 0);
    } else if (lo == 0) {
        w_movz(b, rd, hi, 1);
    } else if (hi == 0xffff) {
        w_movn(b, rd, ~lo & 0xffff, 0);
    } else if (lo == 0xffff) {
        w_movn(b, rd, ~hi & 0xffff, 1);
    } else {
        uint32_t nrs = a64_bitmask(v, 0);
        if (nrs) {
            a64_put(b, enc_log_imm(0, LOG_ORR, rd, ZR, nrs));
        } else {
            w_movz(b, rd, lo, 0);
            w_movk(b, rd, hi, 1);
        }
    }
}

/* Load a 64-bit constant. */
static inline void x_movi(a64buf *b, int rd, uint64_t v)
{
    int first = 1;
    for (int i = 0; i < 4; i++) {
        uint32_t part = (uint32_t)(v >> (16 * i)) & 0xffff;
        if (part == 0)
            continue;
        if (first) {
            x_movz(b, rd, part, i);
            first = 0;
        } else {
            x_movk(b, rd, part, i);
        }
    }
    if (first)
        x_movz(b, rd, 0, 0);
}

/* Logical immediate with fallback through a scratch register. */
static inline void w_logi(a64buf *b, int opc, int rd, int rn, uint32_t imm, int scratch)
{
    uint32_t nrs = a64_bitmask(imm, 0);
    if (nrs) {
        a64_put(b, enc_log_imm(0, opc, rd, rn, nrs));
    } else {
        w_movi(b, scratch, imm);
        a64_put(b, enc_log_sreg(0, opc, 0, rd, rn, scratch, 0, 0));
    }
}
#define w_andi(b, d, n, i, s)  w_logi(b, LOG_AND, d, n, i, s)
#define w_orri(b, d, n, i, s)  w_logi(b, LOG_ORR, d, n, i, s)
#define w_eori(b, d, n, i, s)  w_logi(b, LOG_EOR, d, n, i, s)
#define w_andsi(b, d, n, i, s) w_logi(b, LOG_ANDS, d, n, i, s)
#define w_tsti(b, n, i, s)     w_logi(b, LOG_ANDS, ZR, n, i, s)

static inline int a64_addimm_ok(uint32_t v)
{
    return v < 4096 || ((v & 0xfff) == 0 && v < (4096u << 12));
}

/* rd = rn + imm (32-bit, wrapping); handles any imm, uses scratch if needed. */
static inline void w_addimm(a64buf *b, int rd, int rn, uint32_t imm, int scratch)
{
    uint32_t neg = (uint32_t)-imm;
    if (imm == 0) {
        if (rd != rn)
            w_mov(b, rd, rn);
    } else if (imm < 4096) {
        w_addi(b, rd, rn, imm);
    } else if (neg < 4096) {
        w_subi(b, rd, rn, neg);
    } else if ((imm & 0xfff) == 0 && imm < (4096u << 12)) {
        w_addi12(b, rd, rn, imm >> 12);
    } else if ((neg & 0xfff) == 0 && neg < (4096u << 12)) {
        w_subi12(b, rd, rn, neg >> 12);
    } else if (imm < (1u << 24)) {
        w_addi12(b, rd, rn, imm >> 12);
        w_addi(b, rd, rd, imm & 0xfff);
    } else if (neg < (1u << 24)) {
        w_subi12(b, rd, rn, neg >> 12);
        w_subi(b, rd, rd, neg & 0xfff);
    } else {
        w_movi(b, scratch, imm);
        w_add(b, rd, rn, scratch);
    }
}

/* ------------------------------------------------------------------ */
/* Bitfield / shifts                                                   */
/* ------------------------------------------------------------------ */

static inline uint32_t enc_bfm(int sf, int opc, int rd, int rn, int immr, int imms)
{
    return 0x13000000u | (uint32_t)sf << 31 | (uint32_t)opc << 29 |
           (uint32_t)sf << 22 | (uint32_t)(immr & 63) << 16 |
           (uint32_t)(imms & 63) << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define W_SBFM 0
#define W_BFM 1
#define W_UBFM 2
#define w_lsli(b, d, n, s)       a64_put(b, enc_bfm(0, W_UBFM, d, n, (32 - (s)) & 31, 31 - (s)))
#define w_lsri(b, d, n, s)       a64_put(b, enc_bfm(0, W_UBFM, d, n, s, 31))
#define w_asri(b, d, n, s)       a64_put(b, enc_bfm(0, W_SBFM, d, n, s, 31))
#define w_ubfx(b, d, n, l, w)    a64_put(b, enc_bfm(0, W_UBFM, d, n, l, (l) + (w) - 1))
#define w_sbfx(b, d, n, l, w)    a64_put(b, enc_bfm(0, W_SBFM, d, n, l, (l) + (w) - 1))
#define w_bfi(b, d, n, l, w)     a64_put(b, enc_bfm(0, W_BFM, d, n, (32 - (l)) & 31, (w) - 1))
#define w_bfxil(b, d, n, l, w)   a64_put(b, enc_bfm(0, W_BFM, d, n, l, (l) + (w) - 1))
#define w_ubfiz(b, d, n, l, w)   a64_put(b, enc_bfm(0, W_UBFM, d, n, (32 - (l)) & 31, (w) - 1))
#define w_sxtb(b, d, n)          a64_put(b, enc_bfm(0, W_SBFM, d, n, 0, 7))
#define w_sxth(b, d, n)          a64_put(b, enc_bfm(0, W_SBFM, d, n, 0, 15))
#define w_uxtb(b, d, n)          a64_put(b, enc_bfm(0, W_UBFM, d, n, 0, 7))
#define w_uxth(b, d, n)          a64_put(b, enc_bfm(0, W_UBFM, d, n, 0, 15))
#define x_sxtw(b, d, n)          a64_put(b, enc_bfm(1, W_SBFM, d, n, 0, 31))
#define x_lsri(b, d, n, s)       a64_put(b, enc_bfm(1, W_UBFM, d, n, s, 63))
#define x_asri(b, d, n, s)       a64_put(b, enc_bfm(1, W_SBFM, d, n, s, 63))
#define x_lsli(b, d, n, s)       a64_put(b, enc_bfm(1, W_UBFM, d, n, (64 - (s)) & 63, 63 - (s)))
#define x_ubfx(b, d, n, l, w)    a64_put(b, enc_bfm(1, W_UBFM, d, n, l, (l) + (w) - 1))
#define x_bfi(b, d, n, l, w)     a64_put(b, enc_bfm(1, W_BFM, d, n, (64 - (l)) & 63, (w) - 1))

static inline uint32_t enc_extr(int sf, int rd, int rn, int rm, int lsb)
{
    return 0x13800000u | (uint32_t)sf << 31 | (uint32_t)sf << 22 |
           (uint32_t)rm << 16 | (uint32_t)lsb << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define w_rori(b, d, n, s)       a64_put(b, enc_extr(0, d, n, n, s))
#define w_extr(b, d, n, m, l)    a64_put(b, enc_extr(0, d, n, m, l))
#define x_extr(b, d, n, m, l)    a64_put(b, enc_extr(1, d, n, m, l))

/* Data processing (2 source). */
static inline uint32_t enc_dp2(int sf, int op, int rd, int rn, int rm)
{
    return 0x1AC00000u | (uint32_t)sf << 31 | (uint32_t)rm << 16 |
           (uint32_t)op << 10 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define w_udiv(b, d, n, m) a64_put(b, enc_dp2(0, 2, d, n, m))
#define w_sdiv(b, d, n, m) a64_put(b, enc_dp2(0, 3, d, n, m))
#define x_udiv(b, d, n, m) a64_put(b, enc_dp2(1, 2, d, n, m))
#define x_sdiv(b, d, n, m) a64_put(b, enc_dp2(1, 3, d, n, m))
#define w_lslv(b, d, n, m) a64_put(b, enc_dp2(0, 8, d, n, m))
#define w_lsrv(b, d, n, m) a64_put(b, enc_dp2(0, 9, d, n, m))
#define w_asrv(b, d, n, m) a64_put(b, enc_dp2(0, 10, d, n, m))
#define w_rorv(b, d, n, m) a64_put(b, enc_dp2(0, 11, d, n, m))
#define x_lslv(b, d, n, m) a64_put(b, enc_dp2(1, 8, d, n, m))
#define x_lsrv(b, d, n, m) a64_put(b, enc_dp2(1, 9, d, n, m))
#define x_asrv(b, d, n, m) a64_put(b, enc_dp2(1, 10, d, n, m))
#define x_rorv(b, d, n, m) a64_put(b, enc_dp2(1, 11, d, n, m))

/* Data processing (1 source). */
static inline uint32_t enc_dp1(int sf, int op, int rd, int rn)
{
    return 0x5AC00000u | (uint32_t)sf << 31 | (uint32_t)op << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
#define w_rbit(b, d, n)  a64_put(b, enc_dp1(0, 0, d, n))
#define w_rev16(b, d, n) a64_put(b, enc_dp1(0, 1, d, n))
#define w_rev(b, d, n)   a64_put(b, enc_dp1(0, 2, d, n))
#define w_clz(b, d, n)   a64_put(b, enc_dp1(0, 4, d, n))

/* Data processing (3 source). */
static inline uint32_t enc_dp3(int sf, int op31, int o0, int rd, int rn, int rm, int ra)
{
    return 0x1B000000u | (uint32_t)sf << 31 | (uint32_t)op31 << 21 |
           (uint32_t)rm << 16 | (uint32_t)o0 << 15 | (uint32_t)ra << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
#define w_madd(b, d, n, m, a) a64_put(b, enc_dp3(0, 0, 0, d, n, m, a))
#define w_msub(b, d, n, m, a) a64_put(b, enc_dp3(0, 0, 1, d, n, m, a))
#define w_mul(b, d, n, m)     w_madd(b, d, n, m, ZR)
#define x_mul(b, d, n, m)     a64_put(b, enc_dp3(1, 0, 0, d, n, m, ZR))
#define x_msub(b, d, n, m, a) a64_put(b, enc_dp3(1, 0, 1, d, n, m, a))
#define x_smull(b, d, n, m)   a64_put(b, enc_dp3(1, 1, 0, d, n, m, ZR))
#define x_umull(b, d, n, m)   a64_put(b, enc_dp3(1, 5, 0, d, n, m, ZR))

/* Conditional select. */
static inline uint32_t enc_csel(int sf, int op, int op2, int rd, int rn, int rm, int cond)
{
    return 0x1A800000u | (uint32_t)sf << 31 | (uint32_t)op << 30 |
           (uint32_t)rm << 16 | (uint32_t)cond << 12 | (uint32_t)op2 << 10 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
#define w_csel(b, d, n, m, c)  a64_put(b, enc_csel(0, 0, 0, d, n, m, c))
#define w_csinc(b, d, n, m, c) a64_put(b, enc_csel(0, 0, 1, d, n, m, c))
#define w_csinv(b, d, n, m, c) a64_put(b, enc_csel(0, 1, 0, d, n, m, c))
#define w_csneg(b, d, n, m, c) a64_put(b, enc_csel(0, 1, 1, d, n, m, c))
#define w_cset(b, d, c)        w_csinc(b, d, ZR, ZR, cc_inv(c))
#define w_csetm(b, d, c)       w_csinv(b, d, ZR, ZR, cc_inv(c))
#define x_csel(b, d, n, m, c)  a64_put(b, enc_csel(1, 0, 0, d, n, m, c))

/* Conditional compare: if cond then flags = cmp(rn, rm) else flags = nzcv. */
static inline uint32_t enc_ccmp(int sf, int cmn, int rn, int rm, int nzcv, int cond, int imm)
{
    return (cmn ? 0x3A400000u : 0x7A400000u) | (uint32_t)sf << 31 |
           (uint32_t)rm << 16 | (uint32_t)cond << 12 | (uint32_t)imm << 11 |
           (uint32_t)rn << 5 | (uint32_t)nzcv;
}
#define w_ccmp(b, n, m, nzcv, c)   a64_put(b, enc_ccmp(0, 0, n, m, nzcv, c, 0))
#define w_ccmpi(b, n, i, nzcv, c)  a64_put(b, enc_ccmp(0, 0, n, i, nzcv, c, 1))

/* System registers. */
#define x_mrs_nzcv(b, t)  a64_put(b, 0xD53B4200u | (uint32_t)(t))
#define x_msr_nzcv(b, t)  a64_put(b, 0xD51B4200u | (uint32_t)(t))
#define x_mrs_fpcr(b, t)  a64_put(b, 0xD53B4400u | (uint32_t)(t))
#define x_msr_fpcr(b, t)  a64_put(b, 0xD51B4400u | (uint32_t)(t))
#define a64_nop(b)        a64_put(b, 0xD503201Fu)
#define a64_brk(b, i)     a64_put(b, 0xD4200000u | ((uint32_t)(i) & 0xffff) << 5)

/* ------------------------------------------------------------------ */
/* Loads and stores                                                    */
/* ------------------------------------------------------------------ */

/* size: 0=8, 1=16, 2=32, 3=64 bits. opc: 0=store, 1=load zx, 2=load sx64, 3=load sx32 */
static inline uint32_t enc_ldst_uimm(int size, int opc, int rt, int rn, uint32_t imm12)
{
    return 0x39000000u | (uint32_t)size << 30 | (uint32_t)opc << 22 |
           (imm12 & 0xfff) << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}
static inline uint32_t enc_ldst_simm9(int size, int opc, int rt, int rn, int imm9, int mode)
{
    /* mode: 0 = unscaled (ldur), 1 = post-index, 3 = pre-index */
    return 0x38000000u | (uint32_t)size << 30 | (uint32_t)opc << 22 |
           ((uint32_t)imm9 & 0x1ff) << 12 | (uint32_t)mode << 10 |
           (uint32_t)rn << 5 | (uint32_t)rt;
}
static inline uint32_t enc_ldst_reg(int size, int opc, int rt, int rn, int rm, int opt, int s)
{
    return 0x38200800u | (uint32_t)size << 30 | (uint32_t)opc << 22 |
           (uint32_t)rm << 16 | (uint32_t)opt << 13 | (uint32_t)s << 12 |
           (uint32_t)rn << 5 | (uint32_t)rt;
}

/* Generic load/store with any offset; falls back to a scratch register. */
static inline int a64_ldst_off(a64buf *b, int size, int opc, int rt, int rn, int32_t off)
{
    int scale = size;
    if (off >= 0 && (off & ((1 << scale) - 1)) == 0 && (off >> scale) < 4096) {
        a64_put(b, enc_ldst_uimm(size, opc, rt, rn, (uint32_t)off >> scale));
        return 1;
    }
    if (off >= -256 && off < 256) {
        a64_put(b, enc_ldst_simm9(size, opc, rt, rn, off, 0));
        return 1;
    }
    return 0;
}

#define LDST_STR 0
#define LDST_LDR 1
#define LDST_LDRSX 2
#define LDST_LDRSW 3

/* Convenience wrappers; offsets must be in range (asserted by caller). */
#define w_ldr(b, t, n, off)   a64_ldst_off(b, 2, LDST_LDR, t, n, off)
#define w_str(b, t, n, off)   a64_ldst_off(b, 2, LDST_STR, t, n, off)
#define x_ldr(b, t, n, off)   a64_ldst_off(b, 3, LDST_LDR, t, n, off)
#define x_str(b, t, n, off)   a64_ldst_off(b, 3, LDST_STR, t, n, off)
#define w_ldrb(b, t, n, off)  a64_ldst_off(b, 0, LDST_LDR, t, n, off)
#define w_strb(b, t, n, off)  a64_ldst_off(b, 0, LDST_STR, t, n, off)
#define w_ldrh(b, t, n, off)  a64_ldst_off(b, 1, LDST_LDR, t, n, off)
#define w_strh(b, t, n, off)  a64_ldst_off(b, 1, LDST_STR, t, n, off)
#define w_ldrsb(b, t, n, off) a64_ldst_off(b, 0, LDST_LDRSW, t, n, off)
#define w_ldrsh(b, t, n, off) a64_ldst_off(b, 1, LDST_LDRSW, t, n, off)

/* Pre/post-indexed 32-bit (used for push/pop). */
#define w_str_pre(b, t, n, imm)  a64_put(b, enc_ldst_simm9(2, LDST_STR, t, n, imm, 3))
#define w_ldr_post(b, t, n, imm) a64_put(b, enc_ldst_simm9(2, LDST_LDR, t, n, imm, 1))
#define x_str_pre(b, t, n, imm)  a64_put(b, enc_ldst_simm9(3, LDST_STR, t, n, imm, 3))
#define x_ldr_post(b, t, n, imm) a64_put(b, enc_ldst_simm9(3, LDST_LDR, t, n, imm, 1))

/* Register offset: [xn, wm, uxtw #(s?size:0)] */
#define ldst_reg(b, size, opc, t, n, m, opt, s) a64_put(b, enc_ldst_reg(size, opc, t, n, m, opt, s))

/* Load/store pair. opc: 0 = 32-bit, 2 = 64-bit. mode: 2 = offset, 1 = post, 3 = pre */
static inline uint32_t enc_ldp(int opc, int load, int mode, int rt, int rt2, int rn, int imm7)
{
    return 0x28000000u | (uint32_t)opc << 30 | (uint32_t)mode << 23 |
           (uint32_t)load << 22 | ((uint32_t)imm7 & 0x7f) << 15 |
           (uint32_t)rt2 << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}
#define w_stp(b, t, t2, n, off) a64_put(b, enc_ldp(0, 0, 2, t, t2, n, (off) / 4))
#define w_ldp(b, t, t2, n, off) a64_put(b, enc_ldp(0, 1, 2, t, t2, n, (off) / 4))
#define x_stp(b, t, t2, n, off) a64_put(b, enc_ldp(2, 0, 2, t, t2, n, (off) / 8))
#define x_ldp(b, t, t2, n, off) a64_put(b, enc_ldp(2, 1, 2, t, t2, n, (off) / 8))
#define x_stp_pre(b, t, t2, n, off)  a64_put(b, enc_ldp(2, 0, 3, t, t2, n, (off) / 8))
#define x_ldp_post(b, t, t2, n, off) a64_put(b, enc_ldp(2, 1, 1, t, t2, n, (off) / 8))

/* SIMD&FP loads/stores. sz: 0=B 1=H 2=S 3=D 4=Q */
static inline uint32_t enc_fldst_uimm(int sz, int load, int rt, int rn, uint32_t imm12)
{
    int size = sz == 4 ? 0 : sz;
    int opc = (sz == 4 ? 2 : 0) | (load ? 1 : 0);
    return 0x3D000000u | (uint32_t)size << 30 | (uint32_t)opc << 22 |
           (imm12 & 0xfff) << 10 | (uint32_t)rn << 5 | (uint32_t)rt;
}
static inline uint32_t enc_fldst_simm9(int sz, int load, int rt, int rn, int imm9)
{
    int size = sz == 4 ? 0 : sz;
    int opc = (sz == 4 ? 2 : 0) | (load ? 1 : 0);
    return 0x3C000000u | (uint32_t)size << 30 | (uint32_t)opc << 22 |
           ((uint32_t)imm9 & 0x1ff) << 12 | (uint32_t)rn << 5 | (uint32_t)rt;
}
static inline uint32_t enc_fldst_reg(int sz, int load, int rt, int rn, int rm, int opt, int s)
{
    int size = sz == 4 ? 0 : sz;
    int opc = (sz == 4 ? 2 : 0) | (load ? 1 : 0);
    return 0x3C200800u | (uint32_t)size << 30 | (uint32_t)opc << 22 |
           (uint32_t)rm << 16 | (uint32_t)opt << 13 | (uint32_t)s << 12 |
           (uint32_t)rn << 5 | (uint32_t)rt;
}
static inline int a64_fldst_off(a64buf *b, int sz, int load, int rt, int rn, int32_t off)
{
    int scale = sz;
    if (off >= 0 && (off & ((1 << scale) - 1)) == 0 && (off >> scale) < 4096) {
        a64_put(b, enc_fldst_uimm(sz, load, rt, rn, (uint32_t)off >> scale));
        return 1;
    }
    if (off >= -256 && off < 256) {
        a64_put(b, enc_fldst_simm9(sz, load, rt, rn, off));
        return 1;
    }
    return 0;
}
#define FSZ_B 0
#define FSZ_H 1
#define FSZ_S 2
#define FSZ_D 3
#define FSZ_Q 4
#define f_ldr(b, sz, t, n, off) a64_fldst_off(b, sz, 1, t, n, off)
#define f_str(b, sz, t, n, off) a64_fldst_off(b, sz, 0, t, n, off)

/* ------------------------------------------------------------------ */
/* Branches                                                            */
/* ------------------------------------------------------------------ */

static inline uint32_t enc_b(intptr_t off_bytes)  { return 0x14000000u | ((uint32_t)(off_bytes >> 2) & 0x3ffffff); }
static inline uint32_t enc_bl(intptr_t off_bytes) { return 0x94000000u | ((uint32_t)(off_bytes >> 2) & 0x3ffffff); }
static inline uint32_t enc_bcond(intptr_t off_bytes, int cond)
{
    return 0x54000000u | ((uint32_t)(off_bytes >> 2) & 0x7ffff) << 5 | (uint32_t)cond;
}
static inline uint32_t enc_cbz(int sf, int nz, int rt, intptr_t off_bytes)
{
    return (nz ? 0x35000000u : 0x34000000u) | (uint32_t)sf << 31 |
           ((uint32_t)(off_bytes >> 2) & 0x7ffff) << 5 | (uint32_t)rt;
}
static inline uint32_t enc_tbz(int nz, int rt, int bit, intptr_t off_bytes)
{
    return (nz ? 0x37000000u : 0x36000000u) | (uint32_t)(bit >> 5) << 31 |
           (uint32_t)(bit & 31) << 19 | ((uint32_t)(off_bytes >> 2) & 0x3fff) << 5 |
           (uint32_t)rt;
}
#define a64_br(b, n)  a64_put(b, 0xD61F0000u | (uint32_t)(n) << 5)
#define a64_blr(b, n) a64_put(b, 0xD63F0000u | (uint32_t)(n) << 5)
#define a64_ret(b)    a64_put(b, 0xD65F03C0u)

static inline uint32_t enc_adr(int rd, intptr_t off)
{
    return 0x10000000u | ((uint32_t)off & 3) << 29 | ((uint32_t)(off >> 2) & 0x7ffff) << 5 | (uint32_t)rd;
}
static inline uint32_t enc_adrp(int rd, intptr_t pageoff)
{
    return 0x90000000u | ((uint32_t)pageoff & 3) << 29 | ((uint32_t)(pageoff >> 2) & 0x7ffff) << 5 | (uint32_t)rd;
}

/* Branch placeholders, patched later. */
static inline uint32_t *a64_here(a64buf *b) { return b->p; }

static inline void a64_patch_b(uint32_t *site, const void *target)
{
    *site = enc_b((intptr_t)target - (intptr_t)site);
}
static inline void a64_patch_bcond(uint32_t *site, const void *target)
{
    uint32_t cond = *site & 0xf;
    *site = enc_bcond((intptr_t)target - (intptr_t)site, (int)cond);
}
static inline void a64_patch_cbz(uint32_t *site, const void *target)
{
    uint32_t keep = *site & 0xff00001fu;
    *site = keep | ((uint32_t)(((intptr_t)target - (intptr_t)site) >> 2) & 0x7ffff) << 5;
}
static inline void a64_patch_tbz(uint32_t *site, const void *target)
{
    uint32_t keep = *site & 0xfff8001fu;
    *site = keep | ((uint32_t)(((intptr_t)target - (intptr_t)site) >> 2) & 0x3fff) << 5;
}
static inline int a64_b_in_range(const void *from, const void *to)
{
    intptr_t d = (intptr_t)to - (intptr_t)from;
    return d >= -(1l << 27) && d < (1l << 27);
}

/* ------------------------------------------------------------------ */
/* Floating point (scalar). type: 0 = single, 1 = double               */
/* ------------------------------------------------------------------ */

static inline uint32_t enc_fp2(int type, uint32_t op, int rd, int rn, int rm)
{
    return 0x1E200800u | (uint32_t)type << 22 | (uint32_t)rm << 16 | op << 12 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
#define f_mul(b, t, d, n, m)   a64_put(b, enc_fp2(t, 0, d, n, m))
#define f_div(b, t, d, n, m)   a64_put(b, enc_fp2(t, 1, d, n, m))
#define f_add(b, t, d, n, m)   a64_put(b, enc_fp2(t, 2, d, n, m))
#define f_sub(b, t, d, n, m)   a64_put(b, enc_fp2(t, 3, d, n, m))
#define f_max(b, t, d, n, m)   a64_put(b, enc_fp2(t, 4, d, n, m))
#define f_min(b, t, d, n, m)   a64_put(b, enc_fp2(t, 5, d, n, m))

static inline uint32_t enc_fp1(int type, uint32_t opc, int rd, int rn)
{
    return 0x1E204000u | (uint32_t)type << 22 | opc << 15 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define f_mov(b, t, d, n)    a64_put(b, enc_fp1(t, 0, d, n))
#define f_abs(b, t, d, n)    a64_put(b, enc_fp1(t, 1, d, n))
#define f_neg(b, t, d, n)    a64_put(b, enc_fp1(t, 2, d, n))
#define f_sqrt(b, t, d, n)   a64_put(b, enc_fp1(t, 3, d, n))
#define f_cvt_s2d(b, d, n)   a64_put(b, enc_fp1(0, 5, d, n))
#define f_cvt_d2s(b, d, n)   a64_put(b, enc_fp1(1, 4, d, n))
#define f_frintn(b, t, d, n) a64_put(b, enc_fp1(t, 8, d, n))
#define f_frintp(b, t, d, n) a64_put(b, enc_fp1(t, 9, d, n))
#define f_frintm(b, t, d, n) a64_put(b, enc_fp1(t, 10, d, n))
#define f_frintz(b, t, d, n) a64_put(b, enc_fp1(t, 11, d, n))
#define f_frintx(b, t, d, n) a64_put(b, enc_fp1(t, 14, d, n))
#define f_frinti(b, t, d, n) a64_put(b, enc_fp1(t, 15, d, n))

#define f_cmp(b, t, n, m)    a64_put(b, 0x1E202000u | (uint32_t)(t) << 22 | (uint32_t)(m) << 16 | (uint32_t)(n) << 5)
#define f_cmp0(b, t, n)      a64_put(b, 0x1E202008u | (uint32_t)(t) << 22 | (uint32_t)(n) << 5)
#define f_csel(b, t, d, n, m, c) a64_put(b, 0x1E200C00u | (uint32_t)(t) << 22 | (uint32_t)(m) << 16 | (uint32_t)(c) << 12 | (uint32_t)(n) << 5 | (uint32_t)(d))

/* Int <-> FP conversions. */
static inline uint32_t enc_fcvtint(int sf, int type, int rmode, int opcode, int rd, int rn)
{
    return 0x1E200000u | (uint32_t)sf << 31 | (uint32_t)type << 22 |
           (uint32_t)rmode << 19 | (uint32_t)opcode << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define f_scvtf(b, sf, t, d, n)  a64_put(b, enc_fcvtint(sf, t, 0, 2, d, n))
#define f_ucvtf(b, sf, t, d, n)  a64_put(b, enc_fcvtint(sf, t, 0, 3, d, n))
#define f_fcvtzs(b, sf, t, d, n) a64_put(b, enc_fcvtint(sf, t, 3, 0, d, n))
#define f_fcvtns(b, sf, t, d, n) a64_put(b, enc_fcvtint(sf, t, 0, 0, d, n))
#define f_fcvtms(b, sf, t, d, n) a64_put(b, enc_fcvtint(sf, t, 2, 0, d, n))
#define f_fcvtps(b, sf, t, d, n) a64_put(b, enc_fcvtint(sf, t, 1, 0, d, n))
#define f_fmov_ws(b, d, n)       a64_put(b, enc_fcvtint(0, 0, 0, 6, d, n))
#define f_fmov_sw(b, d, n)       a64_put(b, enc_fcvtint(0, 0, 0, 7, d, n))
#define f_fmov_xd(b, d, n)       a64_put(b, enc_fcvtint(1, 1, 0, 6, d, n))
#define f_fmov_dx(b, d, n)       a64_put(b, enc_fcvtint(1, 1, 0, 7, d, n))
#define f_fmovi(b, t, d, imm8)   a64_put(b, 0x1E201000u | (uint32_t)(t) << 22 | (uint32_t)(imm8) << 13 | (uint32_t)(d))

/* ------------------------------------------------------------------ */
/* Advanced SIMD (vector). q: 1 = 128-bit                              */
/* ------------------------------------------------------------------ */

static inline uint32_t enc_v3(uint32_t base, int q, int size, int rd, int rn, int rm)
{
    return base | (uint32_t)q << 30 | (uint32_t)size << 22 | (uint32_t)rm << 16 |
           (uint32_t)rn << 5 | (uint32_t)rd;
}
static inline uint32_t enc_v2(uint32_t base, int q, int size, int rd, int rn)
{
    return base | (uint32_t)q << 30 | (uint32_t)size << 22 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define V3(b, base, q, sz, d, n, m) a64_put(b, enc_v3(base, q, sz, d, n, m))
#define V2(b, base, q, sz, d, n)    a64_put(b, enc_v2(base, q, sz, d, n))

/* float vector ops (size field: 0 = .4s/.2s, 1 = .2d) */
#define VF_ADD   0x0E20D400u
#define VF_SUB   0x0EA0D400u
#define VF_MUL   0x2E20DC00u
#define VF_DIV   0x2E20FC00u
#define VF_MAX   0x0E20F400u
#define VF_MIN   0x0EA0F400u
#define VF_CMEQ  0x0E20E400u
#define VF_CMGE  0x2E20E400u
#define VF_CMGT  0x2EA0E400u
#define VF_SQRT  0x2EA1F800u
#define VF_RSQRTE 0x2EA1D800u
#define VF_RECPE 0x0EA1D800u
#define VF_FCVTZS 0x0EA1B800u
#define VF_FCVTNS 0x0E21A800u
#define VF_SCVTF 0x0E21D800u
#define VF_FCVTL 0x0E217800u /* size bit 22: 0 = half->single, 1 = single->double */
#define VF_FCVTN 0x0E216800u
/* bitwise (size field selects op, pass 0) */
#define V_AND    0x0E201C00u
#define V_BIC    0x0E601C00u
#define V_ORR    0x0EA01C00u
#define V_ORN    0x0EE01C00u
#define V_EOR    0x2E201C00u
#define V_BSL    0x2E601C00u
#define V_BIT    0x2EA01C00u
#define V_NOT    0x2E205800u
/* integer (size: 0=b,1=h,2=s,3=d) */
#define VI_ADD   0x0E208400u
#define VI_SUB   0x2E208400u
#define VI_SQADD 0x0E200C00u
#define VI_UQADD 0x2E200C00u
#define VI_SQSUB 0x0E202C00u
#define VI_UQSUB 0x2E202C00u
#define VI_CMEQ  0x2E208C00u
#define VI_CMGT  0x0E203400u
#define VI_CMHI  0x2E203400u
#define VI_MUL   0x0E209C00u
#define VI_SMAX  0x0E206400u
#define VI_UMAX  0x2E206400u
#define VI_SMIN  0x0E206C00u
#define VI_UMIN  0x2E206C00u
#define VI_URHADD 0x2E201400u
#define VI_SSHL  0x0E204400u
#define VI_USHL  0x2E204400u
#define VI_SMULL 0x0E20C000u
#define VI_UMULL 0x2E20C000u
#define VI_ADDP  0x0E20BC00u
#define VI_UABD  0x2E207400u
#define VI_NEG   0x2E20B800u
#define VI_XTN   0x0E212800u
#define VI_SQXTN 0x0E214800u
#define VI_UQXTN 0x2E214800u
#define VI_SQXTUN 0x2E212800u
#define VI_UADDLV 0x2E303800u
#define VI_ADDV  0x0E31B800u
#define VI_CNT   0x0E205800u
#define VI_ZIP1  0x0E003800u
#define VI_ZIP2  0x0E007800u
#define VI_UZP1  0x0E001800u
#define VI_UZP2  0x0E005800u
#define VI_TRN1  0x0E002800u
#define VI_TRN2  0x0E006800u
#define VI_SADDLP 0x0E202800u
#define VI_UADDLP 0x2E202800u

/* Shift by immediate: esize in bits (8,16,32,64). */
static inline uint32_t enc_vshift(uint32_t base, int q, int esize, int shift_field, int rd, int rn)
{
    return base | (uint32_t)q << 30 | (uint32_t)shift_field << 16 | (uint32_t)rn << 5 | (uint32_t)rd;
}
#define VS_SHL  0x0F005400u
#define VS_USHR 0x2F000400u
#define VS_SSHR 0x0F000400u
static inline void v_shl(a64buf *b, int q, int esize, int rd, int rn, int sh)
{
    a64_put(b, enc_vshift(VS_SHL, q, esize, esize + sh, rd, rn));
}
static inline void v_ushr(a64buf *b, int q, int esize, int rd, int rn, int sh)
{
    a64_put(b, enc_vshift(VS_USHR, q, esize, 2 * esize - sh, rd, rn));
}
static inline void v_sshr(a64buf *b, int q, int esize, int rd, int rn, int sh)
{
    a64_put(b, enc_vshift(VS_SSHR, q, esize, 2 * esize - sh, rd, rn));
}

/* Element moves. imm5 encodes size+index. */
static inline int imm5_for(int esize_log2, int idx)
{
    return ((idx << 1) | 1) << esize_log2;
}
/* INS Vd.T[di], Vn.T[si] */
static inline void v_ins_elem(a64buf *b, int esl2, int rd, int di, int rn, int si)
{
    a64_put(b, 0x6E000400u | (uint32_t)imm5_for(esl2, di) << 16 |
               (uint32_t)(si << esl2) << 11 | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* INS Vd.T[di], Wn/Xn */
static inline void v_ins_gen(a64buf *b, int esl2, int rd, int di, int rn)
{
    a64_put(b, 0x4E001C00u | (uint32_t)imm5_for(esl2, di) << 16 | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* UMOV Wd/Xd, Vn.T[i] */
static inline void v_umov(a64buf *b, int esl2, int rd, int rn, int idx)
{
    a64_put(b, 0x0E003C00u | (uint32_t)(esl2 == 3) << 30 |
               (uint32_t)imm5_for(esl2, idx) << 16 | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* DUP Vd.T, Vn.T[i] */
static inline void v_dup_elem(a64buf *b, int q, int esl2, int rd, int rn, int idx)
{
    a64_put(b, 0x0E000400u | (uint32_t)q << 30 | (uint32_t)imm5_for(esl2, idx) << 16 |
               (uint32_t)rn << 5 | (uint32_t)rd);
}
/* DUP Vd.T, Wn */
static inline void v_dup_gen(a64buf *b, int q, int esl2, int rd, int rn)
{
    a64_put(b, 0x0E000C00u | (uint32_t)q << 30 | (uint32_t)imm5_for(esl2, 0) << 16 |
               (uint32_t)rn << 5 | (uint32_t)rd);
}
/* EXT Vd, Vn, Vm, #imm (bytes) */
static inline void v_ext(a64buf *b, int q, int rd, int rn, int rm, int imm4)
{
    a64_put(b, 0x2E000000u | (uint32_t)q << 30 | (uint32_t)rm << 16 | (uint32_t)imm4 << 11 |
               (uint32_t)rn << 5 | (uint32_t)rd);
}
/* TBL Vd, {Vn}, Vm (single register table) */
static inline void v_tbl1(a64buf *b, int q, int rd, int rn, int rm)
{
    a64_put(b, 0x0E000000u | (uint32_t)q << 30 | (uint32_t)rm << 16 | (uint32_t)rn << 5 | (uint32_t)rd);
}
/* MOVI Vd.2D, #0 */
static inline void v_zero(a64buf *b, int rd)
{
    a64_put(b, 0x6F00E400u | (uint32_t)rd);
}
/* MOV Vd.16B, Vn.16B (ORR alias) */
static inline void v_mov(a64buf *b, int rd, int rn)
{
    V3(b, V_ORR, 1, 0, rd, rn, rn);
}

#endif
