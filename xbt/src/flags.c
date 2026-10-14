/*
 * flags.c - lazy x86 flag evaluation.
 *
 * Translated code never computes EFLAGS eagerly. A flag-producing instruction
 * whose flags may be read later stores a small record (op, result, operands);
 * the flags are rebuilt here only when something actually needs them
 * (PUSHF, a condition in another block, a partial flag update, ...). Most
 * producers do not even store the record: the translator proves that their
 * flags are overwritten before being read, or hands them to the consumer
 * through the host NZCV register.
 */
#include "xbt.h"

static inline uint32_t parity8(uint32_t v)
{
    return (__builtin_popcount(v & 0xff) & 1) ? 0 : XF_PF;
}

uint32_t xbt_flags(xbt_cpu *cpu)
{
    uint32_t op = cpu->lf_op;
    uint32_t kind = op >> 2, sz = op & 3;
    uint32_t base = cpu->eflags;
    if (kind == LFK_NONE)
        return base;

    unsigned bits = 8u << sz;
    uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    uint32_t sign = 1u << (bits - 1);
    uint32_t res = cpu->lf_res & mask;
    uint32_t a = cpu->lf_a & mask;
    uint32_t b = cpu->lf_b & mask;
    uint32_t f = 0, defined = XF_STATUS;

    switch (kind) {
    case LFK_ADD:
        a = (res - b) & mask;
        if (res < a) f |= XF_CF;
        f |= ((a ^ b ^ res) & 0x10) ? XF_AF : 0;
        f |= ((a ^ res) & (b ^ res) & sign) ? XF_OF : 0;
        break;
    case LFK_ADC: {
        uint32_t cin = (res - a - b) & mask;
        if (cin ? res <= a : res < a) f |= XF_CF;
        f |= ((a ^ b ^ res) & 0x10) ? XF_AF : 0;
        f |= ((a ^ res) & (b ^ res) & sign) ? XF_OF : 0;
        break;
    }
    case LFK_SUB:
        a = (res + b) & mask;
        if (a < b) f |= XF_CF;
        f |= ((a ^ b ^ res) & 0x10) ? XF_AF : 0;
        f |= ((a ^ b) & (a ^ res) & sign) ? XF_OF : 0;
        break;
    case LFK_SBB: {
        uint32_t cin = (a - b - res) & mask;
        if (cin ? a <= b : a < b) f |= XF_CF;
        f |= ((a ^ b ^ res) & 0x10) ? XF_AF : 0;
        f |= ((a ^ b) & (a ^ res) & sign) ? XF_OF : 0;
        break;
    }
    case LFK_LOGIC:
        break;
    case LFK_INC:
        defined &= ~XF_CF;
        f |= (res & 0xf) == 0 ? XF_AF : 0;
        f |= res == sign ? XF_OF : 0;
        break;
    case LFK_DEC:
        defined &= ~XF_CF;
        f |= (res & 0xf) == 0xf ? XF_AF : 0;
        f |= res == sign - 1 ? XF_OF : 0;
        break;
    case LFK_SHL: {
        uint32_t cnt = cpu->lf_b;
        uint32_t cf = cnt <= bits ? (a >> (bits - cnt)) & 1 : 0;
        f |= cf ? XF_CF : 0;
        f |= (((res & sign) != 0) ^ cf) ? XF_OF : 0;
        break;
    }
    case LFK_SHR: {
        uint32_t cnt = cpu->lf_b;
        uint32_t cf = cnt <= bits ? (a >> (cnt - 1)) & 1 : 0;
        f |= cf ? XF_CF : 0;
        f |= (a & sign) ? XF_OF : 0;
        break;
    }
    case LFK_SHRD: {
        uint32_t cnt = cpu->lf_b;
        uint32_t cf = cnt <= bits ? (a >> (cnt - 1)) & 1 : 0;
        f |= cf ? XF_CF : 0;
        f |= ((a ^ res) & sign) ? XF_OF : 0;
        break;
    }
    case LFK_SAR: {
        uint32_t cnt = cpu->lf_b;
        int32_t sa = (int32_t)(a << (32 - bits)) >> (32 - bits);
        uint32_t cf = cnt < bits ? ((uint32_t)(sa >> (cnt - 1)) & 1) : ((a & sign) != 0);
        f |= cf ? XF_CF : 0;
        break;
    }
    case LFK_ROL: {
        defined = XF_CF | XF_OF;
        uint32_t cf = res & 1;
        f |= cf ? XF_CF : 0;
        f |= (((res & sign) != 0) ^ cf) ? XF_OF : 0;
        break;
    }
    case LFK_ROR: {
        defined = XF_CF | XF_OF;
        uint32_t cf = (res & sign) != 0;
        f |= cf ? XF_CF : 0;
        f |= (cf ^ ((res >> (bits - 2)) & 1)) ? XF_OF : 0;
        break;
    }
    case LFK_MUL:
        f |= b ? (XF_CF | XF_OF) : 0;
        break;
    case LFK_BT:
        defined = XF_CF;
        f |= (cpu->lf_res & 1) ? XF_CF : 0;
        break;
    default:
        break;
    }
    if (defined & XF_ZF)
        f |= res == 0 ? XF_ZF : 0;
    if (defined & XF_SF)
        f |= (res & sign) ? XF_SF : 0;
    if (defined & XF_PF)
        f |= parity8(res);
    return (base & ~defined) | (f & defined);
}

void xbt_flags_sync(xbt_cpu *cpu)
{
    cpu->eflags = xbt_flags(cpu);
    cpu->lf_op = LF_OP(LFK_NONE, 0);
}

void xbt_set_flags(xbt_cpu *cpu, uint32_t value, uint32_t mask)
{
    uint32_t f = xbt_flags(cpu);
    cpu->eflags = (f & ~mask) | (value & mask);
    cpu->lf_op = LF_OP(LFK_NONE, 0);
}

int xbt_cond(xbt_cpu *cpu, int cc)
{
    uint32_t f = xbt_flags(cpu);
    int cf = !!(f & XF_CF), zf = !!(f & XF_ZF), sf = !!(f & XF_SF);
    int of = !!(f & XF_OF), pf = !!(f & XF_PF);
    int r;
    switch (cc >> 1) {
    case 0: r = of; break;
    case 1: r = cf; break;
    case 2: r = zf; break;
    case 3: r = cf | zf; break;
    case 4: r = sf; break;
    case 5: r = pf; break;
    case 6: r = sf != of; break;
    default: r = zf | (sf != of); break;
    }
    return (cc & 1) ? !r : r;
}
