/*
 * A32 (ARM mode) instruction encoders for xlat's code generator, ARMv7-A
 * with VFPv4 and NEON (the Raspberry Pi 3's Cortex-A53 in 32-bit mode).
 */
#ifndef XLAT_ARM_H
#define XLAT_ARM_H

#include <stdint.h>

enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC };
enum { C_EQ, C_NE, C_CS, C_CC, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL };
enum { OP_AND, OP_EOR, OP_SUB, OP_RSB, OP_ADD, OP_ADC, OP_SBC, OP_RSC,
       OP_TST, OP_TEQ, OP_CMP, OP_CMN, OP_ORR, OP_MOV, OP_BIC, OP_MVN };
enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR };

typedef struct {
    uint32_t *p, *start, *end;
} asm_t;

static inline void emit(asm_t *a, uint32_t w)
{
    if (a->p < a->end) *a->p = w;
    a->p++;
}

/* ARM modified immediate: 8 bits rotated right by an even amount, or -1. */
static inline int arm_imm(uint32_t v)
{
    for (int r = 0; r < 16; r++) {
        uint32_t x = r ? (v << (2 * r)) | (v >> (32 - 2 * r)) : v;
        if (x < 256) return r << 8 | (int)x;
    }
    return -1;
}

/* data processing */
static inline uint32_t dp_r(int cond, int op, int s, int rd, int rn, int rm, int sh, int amt)
{
    return (uint32_t)cond << 28 | (uint32_t)op << 21 | (uint32_t)s << 20 | (uint32_t)rn << 16 | (uint32_t)rd << 12 |
           (uint32_t)(amt & 31) << 7 | (uint32_t)sh << 5 | (uint32_t)rm;
}
static inline uint32_t dp_rr(int cond, int op, int s, int rd, int rn, int rm, int sh, int rs)
{
    return (uint32_t)cond << 28 | (uint32_t)op << 21 | (uint32_t)s << 20 | (uint32_t)rn << 16 | (uint32_t)rd << 12 |
           (uint32_t)rs << 8 | (uint32_t)sh << 5 | 1u << 4 | (uint32_t)rm;
}
static inline uint32_t dp_i(int cond, int op, int s, int rd, int rn, int enc)
{
    return (uint32_t)cond << 28 | 1u << 25 | (uint32_t)op << 21 | (uint32_t)s << 20 | (uint32_t)rn << 16 |
           (uint32_t)rd << 12 | (uint32_t)enc;
}

#define A_MOV(rd, rm)          dp_r(C_AL, OP_MOV, 0, rd, 0, rm, SH_LSL, 0)
#define A_MOVc(c, rd, rm)      dp_r(c, OP_MOV, 0, rd, 0, rm, SH_LSL, 0)

static inline uint32_t a_movw(int cond, int rd, uint32_t imm)
{
    return (uint32_t)cond << 28 | 0x03000000u | (imm & 0xF000) << 4 | (uint32_t)rd << 12 | (imm & 0xFFF);
}
static inline uint32_t a_movt(int cond, int rd, uint32_t imm)
{
    return (uint32_t)cond << 28 | 0x03400000u | (imm & 0xF000) << 4 | (uint32_t)rd << 12 | (imm & 0xFFF);
}

/* load/store word or byte, 12-bit immediate offset */
static inline uint32_t a_ldst(int cond, int load, int byte, int rt, int rn, int32_t off, int pre, int wb)
{
    uint32_t u = off >= 0 ? 1 : 0, imm = (uint32_t)(off >= 0 ? off : -off) & 0xFFF;
    return (uint32_t)cond << 28 | 0x04000000u | (uint32_t)pre << 24 | u << 23 | (uint32_t)byte << 22 |
           (uint32_t)wb << 21 | (uint32_t)load << 20 | (uint32_t)rn << 16 | (uint32_t)rt << 12 | imm;
}
#define A_LDR(rt, rn, off)   a_ldst(C_AL, 1, 0, rt, rn, off, 1, 0)
#define A_STR(rt, rn, off)   a_ldst(C_AL, 0, 0, rt, rn, off, 1, 0)
#define A_LDRB(rt, rn, off)  a_ldst(C_AL, 1, 1, rt, rn, off, 1, 0)
#define A_STRB(rt, rn, off)  a_ldst(C_AL, 0, 1, rt, rn, off, 1, 0)
#define A_PUSH1(rt, rn)      a_ldst(C_AL, 0, 0, rt, rn, -4, 1, 1)   /* str rt, [rn, #-4]! */
#define A_POP1(rt, rn)       a_ldst(C_AL, 1, 0, rt, rn, 4, 0, 0)    /* ldr rt, [rn], #4 */

/* load/store word or byte, register offset with shift */
static inline uint32_t a_ldst_r(int cond, int load, int byte, int rt, int rn, int rm, int shift)
{
    return (uint32_t)cond << 28 | 0x07800000u | (uint32_t)byte << 22 | (uint32_t)load << 20 | (uint32_t)rn << 16 |
           (uint32_t)rt << 12 | (uint32_t)shift << 7 | (uint32_t)rm;
}

/* load/store word or byte, post-indexed by a register: rn += rm afterwards */
static inline uint32_t a_ldst_post(int cond, int load, int byte, int rt, int rn, int rm)
{
    return (uint32_t)cond << 28 | 0x06800000u | (uint32_t)byte << 22 | (uint32_t)load << 20 | (uint32_t)rn << 16 |
           (uint32_t)rt << 12 | (uint32_t)rm;
}

/* halfword and signed loads: kind 0xB = h, 0xD = sb, 0xF = sh; 8-bit offset */
static inline uint32_t a_ldsth(int cond, int load, int kind, int rt, int rn, int32_t off)
{
    uint32_t u = off >= 0 ? 1 : 0, imm = (uint32_t)(off >= 0 ? off : -off) & 0xFF;
    return (uint32_t)cond << 28 | 0x01400000u | 1u << 24 | u << 23 | (uint32_t)load << 20 | (uint32_t)rn << 16 |
           (uint32_t)rt << 12 | (imm >> 4) << 8 | (uint32_t)kind << 4 | (imm & 15);
}
static inline uint32_t a_ldsth_r(int cond, int load, int kind, int rt, int rn, int rm)
{
    return (uint32_t)cond << 28 | 0x01000000u | 1u << 24 | 1u << 23 | (uint32_t)load << 20 | (uint32_t)rn << 16 |
           (uint32_t)rt << 12 | (uint32_t)kind << 4 | (uint32_t)rm;
}

static inline uint32_t a_ldm(int rn, uint32_t list) { return 0xE8900000u | (uint32_t)rn << 16 | list; }
static inline uint32_t a_stm(int rn, uint32_t list) { return 0xE8800000u | (uint32_t)rn << 16 | list; }
#define A_PUSH(list) (0xE92D0000u | (list))
#define A_POP(list)  (0xE8BD0000u | (list))

static inline uint32_t a_b(int cond, const uint32_t *at, const void *target)
{
    int32_t off = (int32_t)((intptr_t)target - ((intptr_t)at + 8)) >> 2;
    return (uint32_t)cond << 28 | 0x0A000000u | ((uint32_t)off & 0xFFFFFF);
}
static inline uint32_t a_bl(int cond, const uint32_t *at, const void *target)
{
    int32_t off = (int32_t)((intptr_t)target - ((intptr_t)at + 8)) >> 2;
    return (uint32_t)cond << 28 | 0x0B000000u | ((uint32_t)off & 0xFFFFFF);
}
static inline bool a_b_in_range(const uint32_t *at, const void *target)
{
    intptr_t d = (intptr_t)target - ((intptr_t)at + 8);
    return d >= -(32 << 20) && d < (32 << 20);
}
#define A_BX(rm)        (0xE12FFF10u | (rm))
#define A_BXc(c, rm)    ((uint32_t)(c) << 28 | 0x012FFF10u | (rm))
#define A_BLX(rm)       (0xE12FFF30u | (rm))

/* multiply */
#define A_MUL(rd, rn, rm)        (0xE0000090u | (rd) << 16 | (rm) << 8 | (rn))
#define A_MULS(rd, rn, rm)       (0xE0100090u | (rd) << 16 | (rm) << 8 | (rn))
#define A_SMULL(lo, hi, rn, rm)  (0xE0C00090u | (hi) << 16 | (lo) << 12 | (rm) << 8 | (rn))
#define A_UMULL(lo, hi, rn, rm)  (0xE0800090u | (hi) << 16 | (lo) << 12 | (rm) << 8 | (rn))
#define A_MLS(d, n, m, a)        (0xE0600090u | (d) << 16 | (a) << 12 | (m) << 8 | (n))   /* d = a - n * m */
#define A_SDIV(rd, rn, rm)       (0xE710F010u | (rd) << 16 | (rm) << 8 | (rn))
#define A_UDIV(rd, rn, rm)       (0xE730F010u | (rd) << 16 | (rm) << 8 | (rn))

/* bit fields and misc */
#define A_UBFX(rd, rn, lsb, w)   (0xE7E00050u | ((w) - 1) << 16 | (rd) << 12 | (lsb) << 7 | (rn))
#define A_SBFX(rd, rn, lsb, w)   (0xE7A00050u | ((w) - 1) << 16 | (rd) << 12 | (lsb) << 7 | (rn))
#define A_BFI(rd, rn, lsb, w)    (0xE7C00010u | ((lsb) + (w) - 1) << 16 | (rd) << 12 | (lsb) << 7 | (rn))
#define A_BFC(rd, lsb, w)        (0xE7C0001Fu | ((lsb) + (w) - 1) << 16 | (rd) << 12 | (lsb) << 7)
#define A_UXTB(rd, rm)           (0xE6EF0070u | (rd) << 12 | (rm))
#define A_UXTH(rd, rm)           (0xE6FF0070u | (rd) << 12 | (rm))
#define A_SXTB(rd, rm)           (0xE6AF0070u | (rd) << 12 | (rm))
#define A_SXTH(rd, rm)           (0xE6BF0070u | (rd) << 12 | (rm))
#define A_REV(rd, rm)            (0xE6BF0F30u | (rd) << 12 | (rm))
#define A_REV16(rd, rm)          (0xE6BF0FB0u | (rd) << 12 | (rm))
#define A_CLZ(rd, rm)            (0xE16F0F10u | (rd) << 12 | (rm))
#define A_RBIT(rd, rm)           (0xE6FF0F30u | (rd) << 12 | (rm))
#define A_MRS(rd)                (0xE10F0000u | (rd) << 12)
#define A_MSR(rn)                (0xE128F000u | (rn))        /* msr APSR_nzcvq, rn */
#define A_DMB                    0xF57FF05Bu                 /* dmb ish */
#define A_LDREX(rt, rn)          (0xE1900F9Fu | (rn) << 16 | (rt) << 12)
#define A_STREX(rd, rt, rn)      (0xE1800F90u | (rn) << 16 | (rd) << 12 | (rt))
#define A_LDREXB(rt, rn)         (0xE1D00F9Fu | (rn) << 16 | (rt) << 12)
#define A_STREXB(rd, rt, rn)     (0xE1C00F90u | (rn) << 16 | (rd) << 12 | (rt))
#define A_CLREX                  0xF57FF01Fu
#define A_LDRD(rt, rn)           (0xE1C000D0u | (rn) << 16 | (rt) << 12)   /* ldrd rt, rt+1, [rn] (aligned host data only) */
#define A_NOP                    0xE320F000u

/* VFP double: d = 0..31 */
#define VD(d) (((uint32_t)(d) >> 4) << 22 | ((uint32_t)(d) & 15) << 12)
#define VN(n) (((uint32_t)(n) & 15) << 16 | ((uint32_t)(n) >> 4) << 7)
#define VM(m) (((uint32_t)(m) >> 4) << 5 | ((uint32_t)(m) & 15))
/* VFP single: s = 0..31 */
#define SD(s) (((uint32_t)(s) & 1) << 22 | ((uint32_t)(s) >> 1) << 12)
#define SN(s) (((uint32_t)(s) >> 1) << 16 | ((uint32_t)(s) & 1) << 7)
#define SM(s) (((uint32_t)(s) & 1) << 5 | ((uint32_t)(s) >> 1))

static inline uint32_t a_vldr(int d, int rn, int off)   /* off: multiple of 4, -1020..1020 */
{
    uint32_t u = off >= 0, imm = (uint32_t)(off >= 0 ? off : -off) >> 2;
    return 0xED100B00u | u << 23 | VD(d) | (uint32_t)rn << 16 | imm;
}
static inline uint32_t a_vstr(int d, int rn, int off)
{
    uint32_t u = off >= 0, imm = (uint32_t)(off >= 0 ? off : -off) >> 2;
    return 0xED000B00u | u << 23 | VD(d) | (uint32_t)rn << 16 | imm;
}
static inline uint32_t a_vldr_s(int s, int rn, int off)
{
    uint32_t u = off >= 0, imm = (uint32_t)(off >= 0 ? off : -off) >> 2;
    return 0xED100A00u | u << 23 | SD(s) | (uint32_t)rn << 16 | imm;
}
static inline uint32_t a_vstr_s(int s, int rn, int off)
{
    uint32_t u = off >= 0, imm = (uint32_t)(off >= 0 ? off : -off) >> 2;
    return 0xED000A00u | u << 23 | SD(s) | (uint32_t)rn << 16 | imm;
}
#define A_VMOV_DRR(d, lo, hi)  (0xEC400B10u | (hi) << 16 | (lo) << 12 | VM(d))   /* d = lo:hi */
#define A_VMOV_RRD(lo, hi, d)  (0xEC500B10u | (hi) << 16 | (lo) << 12 | VM(d))
#define A_VMOV_SR(s, r)        (0xEE000A10u | SN(s) | (r) << 12)
#define A_VMOV_RS(r, s)        (0xEE100A10u | SN(s) | (r) << 12)
#define A_VADD(d, n, m)        (0xEE300B00u | VD(d) | VN(n) | VM(m))
#define A_VSUB(d, n, m)        (0xEE300B40u | VD(d) | VN(n) | VM(m))
#define A_VMUL(d, n, m)        (0xEE200B00u | VD(d) | VN(n) | VM(m))
#define A_VDIV(d, n, m)        (0xEE800B00u | VD(d) | VN(n) | VM(m))
#define A_VNEG(d, m)           (0xEEB10B40u | VD(d) | VM(m))
#define A_VABS(d, m)           (0xEEB00BC0u | VD(d) | VM(m))
#define A_VSQRT(d, m)          (0xEEB10BC0u | VD(d) | VM(m))
#define A_VMOV(d, m)           (0xEEB00B40u | VD(d) | VM(m))
#define A_VMOV_IMM(d, imm8)    (0xEEB00B00u | ((uint32_t)(imm8) >> 4) << 16 | VD(d) | ((uint32_t)(imm8) & 15))
#define A_VMOVc(c, d, m)       ((uint32_t)(c) << 28 | 0x0EB00B40u | VD(d) | VM(m))
#define A_VCMP(d, m)           (0xEEB40B40u | VD(d) | VM(m))
#define A_VCMP0(d)             (0xEEB50B40u | VD(d))
#define A_VPUSH(d, n)          (0xED2D0B00u | VD(d) | 2u * (n))   /* vpush {d..d+n-1} */
#define A_VPOP(d, n)           (0xECBD0B00u | VD(d) | 2u * (n))
#define A_VMRS_APSR            0xEEF1FA10u
#define A_VMRS(r)              (0xEEF10A10u | (r) << 12)    /* vmrs r, fpscr */
#define A_VMSR(r)              (0xEEE10A10u | (r) << 12)    /* vmsr fpscr, r */
#define A_VCVT_DS(d, s)        (0xEEB70AC0u | VD(d) | SM(s))   /* f64 <- f32 */
#define A_VCVT_SD(s, d)        (0xEEB70BC0u | SD(s) | VM(d))   /* f32 <- f64 */
#define A_VCVT_D_S32(d, s)     (0xEEB80BC0u | VD(d) | SM(s))   /* f64 <- s32 */
#define A_VCVT_D_U32(d, s)     (0xEEB80B40u | VD(d) | SM(s))   /* f64 <- u32 */
#define A_VCVT_S32_D_Z(s, d)   (0xEEBD0BC0u | SD(s) | VM(d))   /* s32 <- f64, toward zero */
#define A_VCVT_S32_D_R(s, d)   (0xEEBD0B40u | SD(s) | VM(d))   /* s32 <- f64, FPSCR rounding */

#endif
