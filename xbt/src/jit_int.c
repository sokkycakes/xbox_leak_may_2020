/*
 * jit_int.c - code generation for the integer instruction set.
 *
 * Conventions: w0..w5 hold operand values, w15 is used by flag records,
 * x16/x17 hold addresses. A producer only computes flags when the analysis
 * says they are needed (ji->need); it leaves them in NZCV when a consumer in
 * the same block reads them from there (ji->hostfl) and stores a lazy record
 * only when they may be read through the context (ji->store).
 */
#include <string.h>
#include "jit.h"

enum { OPD_REG, OPD_RM, OPD_IMM };
typedef struct opd {
    int kind;
    int reg;
    uint32_t imm;
} opd;

static inline int byteop(const x86_insn *in) { return !(in->op & 1); }
static inline uint32_t szmask(int size) { return size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1; }

/* Load a register or r/m operand (dirty upper bits allowed). */
static int ld_opd(jit_t *j, const x86_insn *in, opd o, int size, int tmp, amode *am, int *am_done)
{
    if (o.kind == OPD_REG)
        return jit_get_reg_dirty(j, size, o.reg, tmp);
    if (!in->is_mem)
        return jit_get_reg_dirty(j, size, in->rm, tmp);
    if (!*am_done) {
        jit_ea(j, in, am);
        *am_done = 1;
    }
    jit_ld(j, size, tmp, *am);
    return tmp;
}

static void emit_shifted_flags(jit_t *j, int kind, int size, int ra, int rb, uint32_t imm, int use_imm, int rres)
{
    a64buf *b = JB(j);
    int sh = 32 - 8 * size;
    if (kind == 0 || kind == 5 || kind == 7) {
        w_lsli(b, 3, ra, sh);
        if (use_imm) {
            w_movi(b, 4, imm << sh);
            if (kind == 0)
                w_cmn(b, 3, 4);
            else
                w_cmp(b, 3, 4);
        } else {
            a64_put(b, enc_addsub_sreg(0, kind == 0 ? 0 : 1, 1, ZR, 3, rb, SH_LSL, sh));
        }
    } else {
        /* logic: N,Z from the result, C = V = 0 */
        a64_put(b, enc_addsub_sreg(0, 0, 1, ZR, ZR, rres, SH_LSL, sh));
    }
}

/* alu: 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp 8 test */
static int gen_alu(jit_t *j, jinsn *ji, int alu, int size, opd dst, opd src)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int is_cmp = alu == 7 || alu == 8;
    int kind = alu == 8 ? 4 : alu;
    int logic = kind == 1 || kind == 4 || kind == 6;
    int carry = kind == 2 || kind == 3;
    int need = ji->need != 0;
    int setfl = ji->hostfl && size == 4;
    int store = ji->store;
    amode am = { 0, 0 };
    int am_done = 0;

    if (is_cmp && !store && !ji->hostfl)
        return 1;   /* flags not needed: cmp/test have no other effect */
    (void)need;
    if (carry)
        jit_carry_in(j, ji, kind == 3);

    int dst_mem = dst.kind == OPD_RM && in->is_mem;
    int dst_regnum = dst.kind == OPD_RM ? in->rm : dst.reg;
    int ra = ld_opd(j, in, dst, size, 0, &am, &am_done);

    int rb = -1, use_imm = 0;
    uint32_t imm = 0;
    if (src.kind == OPD_IMM) {
        imm = src.imm & szmask(size);
        use_imm = 1;
    } else {
        rb = ld_opd(j, in, src, size, 1, &am, &am_done);
    }

    /* decide whether the immediate can be encoded directly */
    if (use_imm) {
        int ok;
        if (logic)
            ok = a64_bitmask(imm, 0) != 0;          /* logic records do not need b */
        else if (kind == 0 || kind == 5 || kind == 7)
            ok = a64_addimm_ok(imm) && !store;
        else
            ok = 0;
        if (!ok) {
            w_movi(b, 1, imm);
            rb = 1;
            use_imm = 0;
        }
    }

    int dst_greg = (!dst_mem && size == 4) ? GREG(dst_regnum) : -1;
    /* result register; -1 = the result itself is not needed */
    int rr;
    if (is_cmp) {
        if (store)
            rr = 2;
        else if (size == 4)
            rr = ZR;                    /* need => hostfl: cmp/tst only */
        else
            rr = logic ? 2 : -1;        /* 8/16 logic needs the result for NZCV */
    } else if (dst_greg >= 0 && !carry && !(store && rb == dst_greg)) {
        rr = dst_greg;
    } else {
        rr = 2;
    }

    switch (kind) {
    case 0:
        if (use_imm)
            a64_put(b, enc_addsub_imm(0, 0, setfl, rr, ra, imm >= 4096 ? imm >> 12 : imm, imm >= 4096));
        else
            a64_put(b, enc_addsub_sreg(0, 0, setfl, rr, ra, rb, SH_LSL, 0));
        break;
    case 5: case 7:
        if (rr < 0)
            break;
        if (use_imm)
            a64_put(b, enc_addsub_imm(0, 1, setfl, rr, ra, imm >= 4096 ? imm >> 12 : imm, imm >= 4096));
        else
            a64_put(b, enc_addsub_sreg(0, 1, setfl, rr, ra, rb, SH_LSL, 0));
        break;
    case 2:
        a64_put(b, enc_adc(0, 0, setfl, rr, ra, rb));
        break;
    case 3:
        a64_put(b, enc_adc(0, 1, setfl, rr, ra, rb));
        break;
    case 4:
        if (use_imm)
            a64_put(b, enc_log_imm(0, setfl ? LOG_ANDS : LOG_AND, rr, ra, a64_bitmask(imm, 0)));
        else
            a64_put(b, enc_log_sreg(0, setfl ? LOG_ANDS : LOG_AND, 0, rr, ra, rb, 0, 0));
        break;
    case 1: case 6:
        if (use_imm)
            a64_put(b, enc_log_imm(0, kind == 1 ? LOG_ORR : LOG_EOR, rr, ra, a64_bitmask(imm, 0)));
        else
            a64_put(b, enc_log_sreg(0, kind == 1 ? LOG_ORR : LOG_EOR, 0, rr, ra, rb, 0, 0));
        if (setfl)
            w_tst(b, rr, rr);
        break;
    }
    if (ji->hostfl && size != 4) {
        /* 8/16-bit: recompute NZCV on operands shifted to the top */
        emit_shifted_flags(j, kind, size, ra, rb, imm, use_imm, rr);
    }
    if (ji->hostfl)
        jit_flags_produced(j, ji);

    /* record before write-back: operands may alias the destination */
    if (store) {
        if (logic)
            jit_flags_record(j, ji, LFK_LOGIC, size, rr, -1, -1);
        else if (kind == 0)
            jit_flags_record(j, ji, LFK_ADD, size, rr, -1, rb);
        else if (kind == 5 || kind == 7)
            jit_flags_record(j, ji, LFK_SUB, size, rr, -1, rb);
        else
            jit_flags_record(j, ji, kind == 2 ? LFK_ADC : LFK_SBB, size, rr, ra, rb);
    }
    if (!is_cmp) {
        if (dst_mem)
            jit_st(j, size, rr, am);
        else if (size == 4)
            jit_set_reg(j, 4, dst_regnum, rr);
        else
            jit_set_reg(j, size, dst_regnum, rr);
    }
    return 1;
}

static int gen_incdec(jit_t *j, jinsn *ji, int dec, int size, opd dst)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    amode am = { 0, 0 };
    int am_done = 0;
    int dst_mem = dst.kind == OPD_RM && in->is_mem;
    int regnum = dst.kind == OPD_RM ? in->rm : dst.reg;
    int ra = ld_opd(j, in, dst, size, 0, &am, &am_done);
    int setfl = ji->hostfl && size == 4;
    int rr = (!dst_mem && size == 4) ? GREG(regnum) : 2;
    a64_put(b, enc_addsub_imm(0, dec, setfl, rr, ra, 1, 0));
    if (ji->hostfl && size != 4) {
        int sh = 32 - 8 * size;
        w_lsli(b, 3, ra, sh);
        w_movi(b, 4, 1u << sh);
        if (dec)
            w_cmp(b, 3, 4);
        else
            w_cmn(b, 3, 4);
    }
    if (ji->hostfl)
        jit_flags_produced(j, ji);
    if (ji->store)
        jit_flags_record(j, ji, dec ? LFK_DEC : LFK_INC, size, rr, -1, -1);
    if (dst_mem)
        jit_st(j, size, rr, am);
    else if (size != 4)
        jit_set_reg(j, size, regnum, rr);
    return 1;
}

/* ------------------------------------------------------------------ */
/* group 3: test/not/neg/mul/imul/div/idiv                             */
/* ------------------------------------------------------------------ */

static int gen_muldiv(jit_t *j, jinsn *ji, int size)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int reg = in->reg;
    amode am;
    int rs = jit_ld_rm(j, in, size, 1, &am);   /* zero-extended source */
    int eax = GREG(R_EAX), edx = GREG(R_EDX);

    if (reg == 4 || reg == 5) {
        int sgn = reg == 5;
        if (size == 4) {
            if (sgn)
                x_smull(b, 0, eax, rs);
            else
                x_umull(b, 0, eax, rs);
            x_lsri(b, 2, 0, 32);              /* hi */
            if (ji->store) {
                if (sgn)
                    w_eor_sh(b, 3, 2, 0, SH_ASR, 31);
                else
                    w_mov(b, 3, 2);
            }
            w_mov(b, eax, 0);
            w_mov(b, edx, 2);
            if (ji->store)
                jit_flags_record(j, ji, LFK_MUL, 4, 0, -1, 3);
        } else if (size == 2) {
            if (sgn) {
                w_sxth(b, 0, eax);
                w_sxth(b, 1, rs);
            } else {
                w_uxth(b, 0, eax);
            }
            w_mul(b, 2, 0, sgn ? 1 : rs);
            w_bfi(b, eax, 2, 0, 16);
            w_lsri(b, 3, 2, 16);
            w_bfi(b, edx, 3, 0, 16);
            if (ji->store) {
                if (sgn) {
                    w_sxth(b, 4, 2);
                    w_eor(b, 3, 2, 4);
                }
                jit_flags_record(j, ji, LFK_MUL, 4, 2, -1, 3);
            }
        } else {
            if (sgn) {
                w_sxtb(b, 0, eax);
                w_sxtb(b, 1, rs);
            } else {
                w_uxtb(b, 0, eax);
            }
            w_mul(b, 2, 0, sgn ? 1 : rs);
            w_bfi(b, eax, 2, 0, 16);
            if (ji->store) {
                if (sgn) {
                    w_sxtb(b, 4, 2);
                    w_eor(b, 3, 2, 4);
                    w_uxth(b, 3, 3);
                } else {
                    w_lsri(b, 3, 2, 8);
                    w_uxtb(b, 3, 3);
                }
                jit_flags_record(j, ji, LFK_MUL, 4, 2, -1, 3);
            }
        }
        return 1;
    }

    /* div / idiv */
    int sgn = reg == 7;
    uint32_t eip = in->addr;
    uint32_t *site = a64_here(b);
    a64_put(b, enc_cbz(0, 0, rs, 0));
    jit_stub(j, site, SITE_CBZ, eip, EXIT_DIVIDE, 0);
    if (size == 4) {
        x_orr_sh(b, 0, eax, edx, SH_LSL, 32);   /* x0 = edx:eax */
        if (sgn) {
            x_sxtw(b, 1, rs);
            x_sdiv(b, 2, 0, 1);
            /* quotient must fit in 32 bits (signed) */
            a64_put(b, enc_addsub_ereg(1, 1, 0, 3, 2, 2, EX_SXTW, 0));
        } else {
            x_udiv(b, 2, 0, rs);
            x_lsri(b, 3, 2, 32);
        }
        site = a64_here(b);
        a64_put(b, enc_cbz(1, 1, 3, 0));
        jit_stub(j, site, SITE_CBZ, eip, EXIT_DIVIDE, 0);
        x_msub(b, 4, 2, sgn ? 1 : rs, 0);
        w_mov(b, eax, 2);
        w_mov(b, edx, 4);
    } else {
        int bits = 8 * size;
        if (size == 2) {
            w_uxth(b, 0, eax);
            w_orr_sh(b, 0, 0, edx, SH_LSL, 16);   /* dx:ax */
        } else {
            w_uxth(b, 0, eax);                     /* ax */
        }
        if (sgn) {
            if (size == 2) {
                w_sxth(b, 1, rs);
            } else {
                w_sxth(b, 0, 0);
                w_sxtb(b, 1, rs);
            }
            w_sdiv(b, 2, 0, 1);
            a64_put(b, enc_addsub_ereg(0, 1, 0, 3, 2, 2, size == 2 ? EX_SXTH : EX_SXTB, 0));
        } else {
            w_udiv(b, 2, 0, rs);
            w_lsri(b, 3, 2, bits);
        }
        site = a64_here(b);
        a64_put(b, enc_cbz(0, 1, 3, 0));
        jit_stub(j, site, SITE_CBZ, eip, EXIT_DIVIDE, 0);
        w_msub(b, 4, 2, sgn ? 1 : rs, 0);
        if (size == 2) {
            w_bfi(b, eax, 2, 0, 16);
            w_bfi(b, edx, 4, 0, 16);
        } else {
            w_bfi(b, eax, 2, 0, 8);
            w_bfi(b, eax, 4, 8, 8);
        }
    }
    if (ji->store)
        jit_flags_record(j, ji, LFK_LOGIC, 4, 2, -1, -1);
    return 1;
}

static int gen_grp3(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int size = byteop(in) ? 1 : in->opsize;
    opd rm = { OPD_RM, 0, 0 };
    switch (in->reg) {
    case 0: case 1: {
        opd im = { OPD_IMM, 0, in->imm };
        return gen_alu(j, ji, 8, size, rm, im);
    }
    case 2: {
        amode am;
        int r = jit_ld_rm_dirty(j, in, size, 0, &am);
        if (!in->is_mem && size == 4) {
            w_mvn(b, GREG(in->rm), GREG(in->rm));
        } else {
            w_mvn(b, 2, r);
            jit_st_rm(j, in, size, 2, &am);
        }
        return 1;
    }
    case 3: {
        amode am;
        int r = jit_ld_rm_dirty(j, in, size, 0, &am);
        int setfl = ji->hostfl && size == 4;
        int rr = (!in->is_mem && size == 4 && !ji->store) ? GREG(in->rm) : 2;
        a64_put(b, enc_addsub_sreg(0, 1, setfl, rr, ZR, r, SH_LSL, 0));
        if (ji->hostfl && size != 4) {
            w_lsli(b, 3, r, 32 - 8 * size);
            w_cmp(b, ZR, 3);
        }
        if (ji->hostfl)
            jit_flags_produced(j, ji);
        if (ji->store)
            jit_flags_record(j, ji, LFK_SUB, size, rr, -1, r);
        jit_st_rm(j, in, size, rr, &am);
        return 1;
    }
    default:
        return gen_muldiv(j, ji, size);
    }
}

/* imul r, r/m [, imm] */
static int gen_imul2(jit_t *j, jinsn *ji, int has_imm)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int size = in->opsize;
    amode am;
    int rs;
    if (size == 4) {
        rs = jit_ld_rm(j, in, 4, 1, &am);
    } else {
        int r = jit_ld_rm(j, in, 2, 1, &am);
        w_sxth(b, 1, r);
        rs = 1;
    }
    int rm2;
    if (has_imm) {
        w_movi(b, 2, in->imm);
        rm2 = 2;
    } else if (size == 4) {
        rm2 = GREG(in->reg);
    } else {
        w_sxth(b, 2, GREG(in->reg));
        rm2 = 2;
    }
    if (size == 4) {
        x_smull(b, 0, rs, rm2);
        if (ji->store) {
            x_lsri(b, 3, 0, 32);
            w_eor_sh(b, 3, 3, 0, SH_ASR, 31);
        }
        if (ji->store)
            jit_flags_record(j, ji, LFK_MUL, 4, 0, -1, 3);
        w_mov(b, GREG(in->reg), 0);
    } else {
        w_mul(b, 0, rs, rm2);
        if (ji->store) {
            w_sxth(b, 3, 0);
            w_eor(b, 3, 3, 0);
            jit_flags_record(j, ji, LFK_MUL, 4, 0, -1, 3);
        }
        w_bfi(b, GREG(in->reg), 0, 0, 16);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* shifts and rotates                                                  */
/* ------------------------------------------------------------------ */

static void replicate(a64buf *b, int rd, int rs, int size)
{
    if (size == 1) {
        w_uxtb(b, rd, rs);
        w_orr_sh(b, rd, rd, rd, SH_LSL, 8);
        w_orr_sh(b, rd, rd, rd, SH_LSL, 16);
    } else {
        w_uxth(b, rd, rs);
        w_orr_sh(b, rd, rd, rd, SH_LSL, 16);
    }
}

static int gen_shift(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op, reg = in->reg;
    int size = byteop(in) ? 1 : in->opsize;
    int bycl = op == 0xd2 || op == 0xd3;
    unsigned cnt = (op == 0xd0 || op == 0xd1) ? 1 : (in->imm & 31);
    amode am;

    if (reg == 2 || reg == 3) {
        /* rcl / rcr: rare, done in C on the materialized state */
        if (in->is_mem) {
            jit_ea(j, in, &am);
            w_addimm(b, 2, am.base, (uint32_t)am.off, 3);
        }
        w_movi(b, 1, (uint32_t)(reg == 3) | (uint32_t)size << 1 | (uint32_t)in->is_mem << 4 |
                     (uint32_t)(in->is_mem ? 0 : in->rm) << 5);
        if (bycl)
            w_andi(b, 3, GREG(R_ECX), 31, 4);
        else
            w_movi(b, 3, cnt);
        jit_call_helper(j, H_RCx, 1);
        return 1;
    }
    if (!bycl && cnt == 0)
        return 1;
    if (reg == 6)
        reg = 4;

    int ra = jit_ld_rm(j, in, size, 0, &am);
    if (in->is_mem)
        jit_ea_pin(j, &am);
    int dst_greg = (!in->is_mem && size == 4) ? GREG(in->rm) : -1;
    int rr = (dst_greg >= 0 && !ji->store) ? dst_greg : 2;
    uint32_t *skip = NULL;

    if (bycl) {
        w_andi(b, 5, GREG(R_ECX), 31, 4);
        skip = a64_here(b);
        a64_put(b, enc_cbz(0, 0, 5, 0));
    }
    int kind = 0;
    switch (reg) {
    case 0: case 1: {   /* rol / ror */
        int src = ra;
        if (size != 4) {
            replicate(b, 1, ra, size);
            src = 1;
        }
        if (bycl) {
            if (reg == 0) {
                w_neg(b, 4, 5);
                w_rorv(b, rr, src, 4);
            } else {
                w_rorv(b, rr, src, 5);
            }
        } else {
            unsigned c = cnt & 31;
            unsigned rot = reg == 0 ? (32 - c) & 31 : c;
            if (rot)
                w_rori(b, rr, src, rot);
            else if (rr != src)
                w_mov(b, rr, src);
        }
        kind = reg == 0 ? LFK_ROL : LFK_ROR;
        break;
    }
    case 4:
        if (bycl)
            w_lslv(b, rr, ra, 5);
        else
            w_lsli(b, rr, ra, cnt);
        kind = LFK_SHL;
        break;
    case 5:
        if (bycl)
            w_lsrv(b, rr, ra, 5);
        else
            w_lsri(b, rr, ra, cnt);
        kind = LFK_SHR;
        break;
    case 7: {
        int src = ra;
        if (size == 1) {
            w_sxtb(b, 1, ra);
            src = 1;
        } else if (size == 2) {
            w_sxth(b, 1, ra);
            src = 1;
        }
        if (bycl)
            w_asrv(b, rr, src, 5);
        else
            w_asri(b, rr, src, cnt);
        kind = LFK_SAR;
        break;
    }
    }
    if (ji->store) {
        if (!bycl)
            w_movi(b, 5, cnt);
        if (kind == LFK_ROL || kind == LFK_ROR)
            jit_flags_record(j, ji, kind, size, rr, -1, -1);
        else
            jit_flags_record(j, ji, kind, size, rr, ra, 5);
    }
    if (in->is_mem)
        jit_st(j, size, rr, am);
    else
        jit_set_reg(j, size, in->rm, rr);
    if (skip)
        a64_patch_cbz(skip, a64_here(b));
    return 1;
}

static int gen_shxd(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int right = in->op == 0x1ac || in->op == 0x1ad;
    int bycl = in->op == 0x1a5 || in->op == 0x1ad;
    int size = in->opsize;
    unsigned cnt = in->imm & 31;
    amode am;
    if (!bycl && cnt == 0)
        return 1;
    int ra = jit_ld_rm(j, in, size, 0, &am);
    if (in->is_mem)
        jit_ea_pin(j, &am);
    int rs = jit_get_reg(j, size, in->reg, 1);
    uint32_t *skip = NULL;
    if (bycl) {
        w_andi(b, 5, GREG(R_ECX), 31, 4);
        skip = a64_here(b);
        a64_put(b, enc_cbz(0, 0, 5, 0));
    } else {
        w_movi(b, 5, cnt);
    }
    if (size == 4) {
        if (!bycl) {
            if (right)
                w_extr(b, 2, rs, ra, cnt);
            else
                w_extr(b, 2, ra, rs, 32 - cnt);
        } else if (right) {
            x_orr_sh(b, 3, ra, rs, SH_LSL, 32);
            x_lsrv(b, 3, 3, 5);
            w_mov(b, 2, 3);
        } else {
            x_orr_sh(b, 3, rs, ra, SH_LSL, 32);
            x_lslv(b, 3, 3, 5);
            x_lsri(b, 2, 3, 32);
        }
    } else {
        if (right) {
            w_orr_sh(b, 3, ra, rs, SH_LSL, 16);
            w_lsrv(b, 2, 3, 5);
        } else {
            w_orr_sh(b, 3, rs, ra, SH_LSL, 16);
            w_lslv(b, 2, 3, 5);
            w_lsri(b, 2, 2, 16);
        }
    }
    if (ji->store)
        jit_flags_record(j, ji, right ? LFK_SHRD : LFK_SHL, size, 2, ra, 5);
    if (in->is_mem)
        jit_st(j, size, 2, am);
    else
        jit_set_reg(j, size, in->rm, 2);
    if (skip)
        a64_patch_cbz(skip, a64_here(b));
    return 1;
}

/* ------------------------------------------------------------------ */
/* bit test / scan                                                     */
/* ------------------------------------------------------------------ */

static int gen_bt(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op;
    int size = in->opsize;
    int act;   /* 0 bt, 1 bts, 2 btr, 3 btc */
    int use_imm = op == 0x1ba;
    if (use_imm) {
        if (in->reg < 4)
            return -1;
        act = in->reg - 4;
    } else {
        act = op == 0x1a3 ? 0 : op == 0x1ab ? 1 : op == 0x1b3 ? 2 : 3;
    }
    int bits = 8 * size;
    amode am = { 0, 0 };
    int rv;
    /* w5 = bit index within the operand */
    if (use_imm) {
        w_movi(b, 5, in->imm & (uint32_t)(bits - 1));
    } else {
        int ro = GREG(in->reg);
        if (in->is_mem) {
            /* bit offset is a signed displacement in bits */
            jit_ea(j, in, &am);
            if (am.base != RA || am.off)
                w_addimm(b, RA, am.base, (uint32_t)am.off, RB);
            if (size == 4) {
                w_asri(b, 4, ro, 5);
                w_add_sh(b, RA, RA, 4, SH_LSL, 2);
            } else {
                w_sxth(b, 4, ro);
                w_asri(b, 4, 4, 4);
                w_add_sh(b, RA, RA, 4, SH_LSL, 1);
            }
            am.base = RA;
            am.off = 0;
        }
        w_andi(b, 5, ro, (uint32_t)(bits - 1), 4);
    }
    if (in->is_mem) {
        if (use_imm)
            jit_ea(j, in, &am);
        jit_ea_pin(j, &am);
        jit_ld(j, size, 0, am);
        rv = 0;
    } else {
        rv = jit_get_reg(j, size, in->rm, 0);
    }
    if (ji->store) {
        w_lsrv(b, 2, rv, 5);
        jit_flags_record(j, ji, LFK_BT, 4, 2, -1, -1);
    }
    if (act) {
        w_movi(b, 3, 1);
        w_lslv(b, 3, 3, 5);
        if (act == 1)
            w_orr(b, 2, rv, 3);
        else if (act == 2)
            w_bic(b, 2, rv, 3);
        else
            w_eor(b, 2, rv, 3);
        if (in->is_mem)
            jit_st(j, size, 2, am);
        else
            jit_set_reg(j, size, in->rm, 2);
    }
    return 1;
}

static int gen_bsx(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int size = in->opsize;
    amode am;
    int rs = jit_ld_rm(j, in, size, 0, &am);
    if (ji->store)
        jit_flags_record(j, ji, LFK_LOGIC, size, rs, -1, -1);
    uint32_t *skip = a64_here(b);
    a64_put(b, enc_cbz(0, 0, rs, 0));
    if (in->op == 0x1bc) {
        w_rbit(b, 1, rs);
        w_clz(b, 1, 1);
    } else {
        w_clz(b, 1, rs);
        w_eori(b, 1, 1, 31, 2);
    }
    jit_set_reg(j, size, in->reg, 1);
    a64_patch_cbz(skip, a64_here(b));
    return 1;
}

/* ------------------------------------------------------------------ */
/* string instructions                                                 */
/* ------------------------------------------------------------------ */

static int gen_string(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op;
    int size = byteop(in) ? 1 : in->opsize;
    int l = size == 1 ? 0 : size == 2 ? 1 : 2;
    int rep = in->rep != REP_NONE;
    int esi = GREG(R_ESI), edi = GREG(R_EDI), ecx = GREG(R_ECX), eax = GREG(R_EAX);
    amode as = { esi, 0 }, ad = { edi, 0 };

    /* w4 = +size or -size depending on DF */
    w_ldr(b, 4, RCPU, CPUOFF(eflags));
    w_ubfx(b, 4, 4, 10, 1);
    w_movi(b, 6, (uint32_t)size);
    w_sub_sh(b, 4, 6, 4, SH_LSL, l + 1);

    uint32_t *skip = NULL;
    if (rep) {
        skip = a64_here(b);
        a64_put(b, enc_cbz(0, 0, ecx, 0));
    }
    uint32_t *loop = a64_here(b);
    uint32_t *brk = NULL;
    switch (op) {
    case 0xa4: case 0xa5:
        jit_ld(j, size, 0, as);
        jit_st(j, size, 0, ad);
        w_add(b, esi, esi, 4);
        w_add(b, edi, edi, 4);
        break;
    case 0xaa: case 0xab:
        jit_st(j, size, eax, ad);
        w_add(b, edi, edi, 4);
        break;
    case 0xac: case 0xad:
        jit_ld(j, size, 0, as);
        jit_set_reg(j, size, R_EAX, 0);
        w_add(b, esi, esi, 4);
        break;
    case 0xa6: case 0xa7:
    case 0xae: case 0xaf:
        if (op <= 0xa7) {
            jit_ld(j, size, 0, as);
            jit_ld(j, size, 1, ad);
            w_add(b, esi, esi, 4);
        } else {
            if (size == 4)
                w_mov(b, 0, eax);
            else if (size == 2)
                w_uxth(b, 0, eax);
            else
                w_uxtb(b, 0, eax);
            jit_ld(j, size, 1, ad);
        }
        w_add(b, edi, edi, 4);
        w_sub(b, 2, 0, 1);
        if (rep) {
            w_subi(b, ecx, ecx, 1);
            int t = 2;
            if (size != 4) {
                w_lsli(b, 3, 2, 32 - 8 * size);
                t = 3;
            }
            brk = a64_here(b);
            /* REPE: stop when different; REPNE: stop when equal */
            a64_put(b, enc_cbz(0, in->rep == REP_Z ? 1 : 0, t, 0));
            a64_put(b, enc_cbz(0, 1, ecx, (intptr_t)loop - (intptr_t)a64_here(b)));
            a64_patch_cbz(brk, a64_here(b));
        }
        if (ji->need)
            jit_flags_record(j, ji, LFK_SUB, size, 2, -1, 1);
        if (skip)
            a64_patch_cbz(skip, a64_here(b));
        return 1;
    default:
        return 0;
    }
    if (rep) {
        w_subi(b, ecx, ecx, 1);
        a64_put(b, enc_cbz(0, 1, ecx, (intptr_t)loop - (intptr_t)a64_here(b)));
        a64_patch_cbz(skip, a64_here(b));
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

static void eflags_bit(jit_t *j, uint32_t bit, int set)
{
    a64buf *b = JB(j);
    w_ldr(b, 0, RCPU, CPUOFF(eflags));
    if (set)
        w_orri(b, 0, 0, bit, 1);
    else
        w_andi(b, 0, 0, ~bit, 1);
    w_str(b, 0, RCPU, CPUOFF(eflags));
}

static int gen_cmpxchg(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int size = byteop(in) ? 1 : in->opsize;
    amode am;
    int rd = jit_ld_rm(j, in, size, 0, &am);    /* destination value */
    if (in->is_mem)
        jit_ea_pin(j, &am);
    int rsrc = jit_get_reg(j, size, in->reg, 1);
    int racc = jit_get_reg(j, size, R_EAX, 3);
    if (rd != 0) {
        w_mov(b, 0, rd);
        rd = 0;
    }
    w_sub(b, 2, racc, rd);                        /* record: acc - dst */
    if (size == 4) {
        w_cmp(b, racc, rd);
    } else {
        int sh = 32 - 8 * size;
        w_lsli(b, 4, racc, sh);
        w_cmp_sh(b, 4, rd, SH_LSL, sh);
    }
    w_csel(b, 4, rsrc, rd, CC_EQ);                /* new destination */
    if (ji->hostfl)
        jit_flags_produced(j, ji);
    else
        jit_nzcv_clobber(j);
    if (ji->store)
        jit_flags_record(j, ji, LFK_SUB, size, 2, -1, 0);
    if (in->is_mem) {
        jit_st(j, size, 4, am);
        jit_set_reg(j, size, R_EAX, 0);           /* eax = old value (equal: unchanged) */
    } else {
        jit_set_reg(j, size, R_EAX, 0);
        jit_set_reg(j, size, in->rm, 4);
    }
    return 1;
}

static int gen_xadd(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int size = byteop(in) ? 1 : in->opsize;
    amode am;
    int rd = jit_ld_rm(j, in, size, 0, &am);
    if (in->is_mem)
        jit_ea_pin(j, &am);
    if (rd != 0) {
        w_mov(b, 0, rd);
        rd = 0;
    }
    int rs = jit_get_reg(j, size, in->reg, 1);
    if (rs != 1) {
        w_mov(b, 1, rs);
        rs = 1;
    }
    int setfl = ji->hostfl && size == 4;
    a64_put(b, enc_addsub_sreg(0, 0, setfl, 2, 0, 1, SH_LSL, 0));
    if (ji->hostfl && size != 4)
        emit_shifted_flags(j, 0, size, 0, 1, 0, 0, 2);
    if (ji->hostfl)
        jit_flags_produced(j, ji);
    if (ji->store)
        jit_flags_record(j, ji, LFK_ADD, size, 2, -1, 1);
    if (in->is_mem) {
        jit_st(j, size, 2, am);
        jit_set_reg(j, size, in->reg, 0);
    } else {
        jit_set_reg(j, size, in->reg, 0);
        jit_set_reg(j, size, in->rm, 2);
    }
    return 1;
}

static int gen_lea(jit_t *j, const x86_insn *in)
{
    a64buf *b = JB(j);
    const x86_mem *m = &in->mem;
    int rd = in->opsize == 4 ? GREG(in->reg) : 0;
    uint32_t disp = (uint32_t)m->disp;
    if (!in->is_mem)
        return -1;
    if (m->base >= 0 && m->index >= 0) {
        w_add_sh(b, rd, GREG(m->base), GREG(m->index), SH_LSL, m->scale);
        w_addimm(b, rd, rd, disp, RB);
    } else if (m->base >= 0) {
        w_addimm(b, rd, GREG(m->base), disp, RB);
    } else if (m->index >= 0) {
        if (disp) {
            w_movi(b, RA, disp);
            w_add_sh(b, rd, RA, GREG(m->index), SH_LSL, m->scale);
        } else {
            w_lsli(b, rd, GREG(m->index), m->scale);
        }
    } else {
        w_movi(b, rd, disp);
    }
    if (in->opsize == 2)
        w_bfi(b, GREG(in->reg), 0, 0, 16);
    return 1;
}

static void push_reg(jit_t *j, int size, int r)
{
    a64buf *b = JB(j);
    if (size == 4)
        w_str_pre(b, r, GREG(R_ESP), -4);
    else
        a64_put(b, enc_ldst_simm9(1, LDST_STR, r, GREG(R_ESP), -2, 3));
}

static int gen_push_pop(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op;
    int size = in->opsize;
    int esp = GREG(R_ESP);
    amode am;

    if (op >= 0x50 && op <= 0x57) {
        int r = op - 0x50;
        if (r == R_ESP) {
            w_mov(b, 0, esp);
            push_reg(j, size, 0);
        } else {
            push_reg(j, size, GREG(r));
        }
        return 1;
    }
    if (op >= 0x58 && op <= 0x5f) {
        int r = op - 0x58;
        if (size == 4) {
            if (r == R_ESP)
                w_ldr(b, esp, esp, 0);
            else
                w_ldr_post(b, GREG(r), esp, 4);
        } else {
            a64_put(b, enc_ldst_simm9(1, LDST_LDR, 0, esp, 2, 1));
            jit_set_reg(j, 2, r, 0);
        }
        return 1;
    }
    switch (op) {
    case 0x68: case 0x6a:
        w_movi(b, 0, in->imm);
        push_reg(j, size, 0);
        return 1;
    case 0x8f:
        if (in->reg != 0)
            return -1;
        if (size == 4)
            w_ldr_post(b, 1, esp, 4);
        else
            a64_put(b, enc_ldst_simm9(1, LDST_LDR, 1, esp, 2, 1));
        if (in->is_mem) {
            jit_ea(j, in, &am);   /* uses the incremented ESP, as on x86 */
            jit_st(j, size, 1, am);
        } else {
            jit_set_reg(j, size, in->rm, 1);
        }
        return 1;
    case 0x60:
        w_mov(b, 0, esp);
        w_stp(b, GREG(R_EDI), GREG(R_ESI), esp, -32);
        w_stp(b, GREG(R_EBP), 0, esp, -24);
        w_stp(b, GREG(R_EBX), GREG(R_EDX), esp, -16);
        w_stp(b, GREG(R_ECX), GREG(R_EAX), esp, -8);
        w_subi(b, esp, esp, 32);
        return 1;
    case 0x61:
        w_ldp(b, GREG(R_EDI), GREG(R_ESI), esp, 0);
        w_ldp(b, GREG(R_EBP), 0, esp, 8);
        w_ldp(b, GREG(R_EBX), GREG(R_EDX), esp, 16);
        w_ldp(b, GREG(R_ECX), GREG(R_EAX), esp, 24);
        w_addi(b, esp, esp, 32);
        return 1;
    case 0xff:   /* push r/m */
        if (in->reg != 6)
            return -1;
        jit_ld_rm(j, in, size, 0, &am);
        if (!in->is_mem && size == 4 && in->rm != R_ESP) {
            push_reg(j, 4, GREG(in->rm));
            return 1;
        }
        if (!in->is_mem && size == 4)
            w_mov(b, 0, esp);
        push_reg(j, size, 0);
        return 1;
    }
    return 0;
}

static int gen_movx(jit_t *j, const x86_insn *in)
{
    a64buf *b = JB(j);
    int op = in->op;
    int ssz = (op == 0x1b6 || op == 0x1be) ? 1 : 2;
    int sx = op >= 0x1be;
    int dsz = in->opsize;
    int rd = dsz == 4 ? GREG(in->reg) : 0;
    if (in->is_mem) {
        amode am;
        jit_ea(j, in, &am);
        if (sx)
            jit_ld_sx(j, ssz, rd, am);
        else
            jit_ld(j, ssz, rd, am);
    } else {
        int r = in->rm;
        if (ssz == 1) {
            int src = r < 4 ? GREG(r) : GREG(r - 4);
            int lsb = r < 4 ? 0 : 8;
            if (sx)
                w_sbfx(b, rd, src, lsb, 8);
            else
                w_ubfx(b, rd, src, lsb, 8);
        } else {
            if (sx)
                w_sxth(b, rd, GREG(r));
            else
                w_uxth(b, rd, GREG(r));
        }
    }
    if (dsz == 2)
        w_bfi(b, GREG(in->reg), 0, 0, 16);
    return 1;
}

static int gen_mov(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op;
    int size = byteop(in) ? 1 : in->opsize;
    amode am;
    switch (op) {
    case 0x88: case 0x89: {
        int r = jit_get_reg_dirty(j, size, in->reg, 0);
        if (!in->is_mem && size == 4) {
            w_mov(b, GREG(in->rm), r);
        } else if (!in->is_mem) {
            jit_set_reg(j, size, in->rm, r);
        } else {
            jit_ea(j, in, &am);
            jit_st(j, size, r, am);
        }
        return 1;
    }
    case 0x8a: case 0x8b:
        if (in->is_mem) {
            jit_ea(j, in, &am);
            if (size == 4) {
                jit_ld(j, 4, GREG(in->reg), am);
            } else {
                jit_ld(j, size, 0, am);
                jit_set_reg(j, size, in->reg, 0);
            }
        } else {
            int r = jit_get_reg_dirty(j, size, in->rm, 0);
            jit_set_reg(j, size, in->reg, r);
        }
        return 1;
    case 0xc6: case 0xc7:
        if (in->reg != 0)
            return -1;
        if (!in->is_mem && size == 4) {
            w_movi(b, GREG(in->rm), in->imm);
            return 1;
        }
        if (in->is_mem)
            jit_ea(j, in, &am);
        if ((in->imm & szmask(size)) == 0) {
            if (in->is_mem)
                jit_st(j, size, ZR, am);
            else
                jit_set_reg(j, size, in->rm, ZR);
        } else {
            w_movi(b, 0, in->imm & szmask(size));
            jit_st_rm(j, in, size, 0, &am);
        }
        return 1;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3:
        jit_ea(j, in, &am);
        if (op <= 0xa1) {
            if (size == 4) {
                jit_ld(j, 4, GREG(R_EAX), am);
            } else {
                jit_ld(j, size, 0, am);
                jit_set_reg(j, size, R_EAX, 0);
            }
        } else {
            jit_st(j, size, GREG(R_EAX), am);
        }
        return 1;
    }
    if (op >= 0xb0 && op <= 0xb7) {
        w_movi(b, 0, in->imm & 0xff);
        jit_set_reg(j, 1, op - 0xb0, 0);
        return 1;
    }
    if (op >= 0xb8 && op <= 0xbf) {
        if (in->opsize == 4) {
            w_movi(b, GREG(op - 0xb8), in->imm);
        } else {
            w_movi(b, 0, in->imm & 0xffff);
            jit_set_reg(j, 2, op - 0xb8, 0);
        }
        return 1;
    }
    return 0;
}

static int gen_xchg(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op;
    if (op >= 0x91 && op <= 0x97) {
        int r = op - 0x90;
        if (in->opsize == 4) {
            w_mov(b, 0, GREG(R_EAX));
            w_mov(b, GREG(R_EAX), GREG(r));
            w_mov(b, GREG(r), 0);
        } else {
            w_mov(b, 0, GREG(R_EAX));
            w_bfi(b, GREG(R_EAX), GREG(r), 0, 16);
            w_bfi(b, GREG(r), 0, 0, 16);
        }
        return 1;
    }
    int size = byteop(in) ? 1 : in->opsize;
    amode am;
    if (in->is_mem) {
        jit_ea(j, in, &am);
        jit_ea_pin(j, &am);
        jit_ld(j, size, 0, am);
        int r = jit_get_reg_dirty(j, size, in->reg, 1);
        jit_st(j, size, r, am);
        jit_set_reg(j, size, in->reg, 0);
    } else {
        int ra = jit_get_reg(j, size, in->rm, 0);
        if (ra != 0)
            w_mov(b, 0, ra);
        int rb = jit_get_reg(j, size, in->reg, 1);
        if (rb != 1)
            w_mov(b, 1, rb);
        jit_set_reg(j, size, in->rm, 1);
        jit_set_reg(j, size, in->reg, 0);
    }
    return 1;
}

static int gen_flagops(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    switch (in->op) {
    case 0x9c:   /* pushf */
        jit_call_helper(j, H_GET_FLAGS, 0);
        w_andi(b, 0, 0, 0x00fcffffu, 1);   /* VM and RF read as zero */
        push_reg(j, in->opsize, 0);
        return 1;
    case 0x9d:   /* popf */
        if (in->opsize == 4)
            w_ldr_post(b, 1, GREG(R_ESP), 4);
        else
            a64_put(b, enc_ldst_simm9(1, LDST_LDR, 1, GREG(R_ESP), 2, 1));
        w_movi(b, 2, 0x0cd5u);   /* status flags and DF; IF/TF have no effect here */
        jit_call_helper(j, H_SET_FLAGS, 0);
        return 1;
    case 0x9f:   /* lahf */
        jit_call_helper(j, H_GET_FLAGS, 0);
        w_bfi(b, GREG(R_EAX), 0, 8, 8);
        return 1;
    case 0x9e:   /* sahf */
        if (!ji->need)
            return 1;
        w_ubfx(b, 0, GREG(R_EAX), 8, 8);
        if (ji->hostfl) {
            /* N=SF(7) Z=ZF(6) C=!CF(0) V=PF(2) */
            w_lsli(b, 1, 0, 24);
            w_andi(b, 1, 1, 0xc0000000u, 2);
            w_eori(b, 2, 0, 1, 3);
            w_bfi(b, 1, 2, 29, 1);
            w_ubfx(b, 2, 0, 2, 1);
            w_bfi(b, 1, 2, 28, 1);
            x_msr_nzcv(b, 1);
            jit_flags_produced(j, ji);
        }
        if (ji->store) {
            w_ldr(b, 1, RCPU, CPUOFF(eflags));
            w_andi(b, 2, 0, 0xd5, 3);
            w_orri(b, 2, 2, 2, 3);
            w_bfi(b, 1, 2, 0, 8);
            w_str(b, 1, RCPU, CPUOFF(eflags));
            w_str(b, ZR, RCPU, CPUOFF(lf_op));
        }
        return 1;
    case 0xf8: case 0xf9:   /* clc / stc */
        if (!ji->need)
            return 1;
        /* after a sync (other flags live) lf_op is already NONE; otherwise
           the other flags are dead and eflags simply becomes authoritative */
        if (!ji->sync)
            w_str(b, ZR, RCPU, CPUOFF(lf_op));
        eflags_bit(j, XF_CF, in->op == 0xf9);
        return 1;
    case 0xf5:   /* cmc */
        jit_sync(j);
        w_ldr(b, 0, RCPU, CPUOFF(eflags));
        w_eori(b, 0, 0, XF_CF, 1);
        w_str(b, 0, RCPU, CPUOFF(eflags));
        return 1;
    case 0xfc: case 0xfd:
        eflags_bit(j, XF_DF, in->op == 0xfd);
        return 1;
    case 0xfa: case 0xfb:
        eflags_bit(j, XF_IF, in->op == 0xfb);
        return 1;
    case 0xd6: {   /* salc */
        jit_cond_reg(j, ji, 2, 0);
        w_neg(b, 0, 0);
        w_bfi(b, GREG(R_EAX), 0, 0, 8);
        return 1;
    }
    }
    return 0;
}

int jit_int(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op;
    int size = byteop(in) ? 1 : in->opsize;
    amode am;

    if (op < 0x40 && (op & 7) < 6) {
        int alu = op >> 3;
        opd rm = { OPD_RM, 0, 0 }, rg = { OPD_REG, in->reg, 0 };
        switch (op & 7) {
        case 0: case 1: return gen_alu(j, ji, alu, size, rm, rg);
        case 2: case 3: return gen_alu(j, ji, alu, size, rg, rm);
        default: {
            opd acc = { OPD_REG, R_EAX, 0 }, im = { OPD_IMM, 0, in->imm };
            return gen_alu(j, ji, alu, (op & 1) ? in->opsize : 1, acc, im);
        }
        }
    }
    if (op >= 0x40 && op <= 0x4f) {
        opd r = { OPD_REG, op & 7, 0 };
        return gen_incdec(j, ji, op >= 0x48, in->opsize, r);
    }
    if ((op >= 0x50 && op <= 0x61) || op == 0x68 || op == 0x6a || op == 0x8f)
        return gen_push_pop(j, ji);
    if (op >= 0x190 && op <= 0x19f) {
        jit_cond_reg(j, ji, op & 15, 0);
        if (in->is_mem) {
            jit_ea(j, in, &am);
            jit_st(j, 1, 0, am);
        } else {
            jit_set_reg(j, 1, in->rm, 0);
        }
        return 1;
    }
    if (op >= 0x140 && op <= 0x14f) {
        int hc = jit_cond(j, ji, op & 15);
        int rs = jit_ld_rm(j, in, in->opsize, 0, &am);
        int rd = in->opsize == 4 ? GREG(in->reg) : 1;
        if (in->opsize == 2)
            w_mov(b, 1, GREG(in->reg));
        if (hc == HC_EQ_OR_VS) {
            w_csel(b, rd, rs, rd, CC_EQ);
            w_csel(b, rd, rs, rd, CC_VS);
        } else if (hc == HC_NE_AND_VC) {
            w_csel(b, 2, rs, rd, CC_NE);
            w_csel(b, rd, 2, rd, CC_VC);
        } else {
            w_csel(b, rd, rs, rd, hc);
        }
        if (in->opsize == 2)
            jit_set_reg(j, 2, in->reg, 1);
        return 1;
    }
    if (op >= 0x1c8 && op <= 0x1cf) {
        w_rev(b, GREG(op - 0x1c8), GREG(op - 0x1c8));
        return 1;
    }
    if ((op >= 0x88 && op <= 0x8b) || op == 0xc6 || op == 0xc7 || (op >= 0xa0 && op <= 0xa3) ||
        (op >= 0xb0 && op <= 0xbf))
        return gen_mov(j, ji);
    if ((op >= 0x91 && op <= 0x97) || op == 0x86 || op == 0x87)
        return gen_xchg(j, ji);
    if (op == 0x1b6 || op == 0x1b7 || op == 0x1be || op == 0x1bf)
        return gen_movx(j, in);
    if (op >= 0xa4 && op <= 0xaf && op != 0xa8 && op != 0xa9)
        return gen_string(j, ji);

    switch (op) {
    case 0x80: case 0x81: case 0x82: case 0x83: {
        opd rm = { OPD_RM, 0, 0 }, im = { OPD_IMM, 0, in->imm };
        return gen_alu(j, ji, in->reg, size, rm, im);
    }
    case 0x84: case 0x85: {
        opd rm = { OPD_RM, 0, 0 }, rg = { OPD_REG, in->reg, 0 };
        return gen_alu(j, ji, 8, size, rm, rg);
    }
    case 0xa8: case 0xa9: {
        opd acc = { OPD_REG, R_EAX, 0 }, im = { OPD_IMM, 0, in->imm };
        return gen_alu(j, ji, 8, size, acc, im);
    }
    case 0xf6: case 0xf7:
        return gen_grp3(j, ji);
    case 0xfe:
        if (in->reg > 1)
            return -1;
        /* fallthrough */
    case 0xff:
        if (in->reg <= 1) {
            opd rm = { OPD_RM, 0, 0 };
            return gen_incdec(j, ji, in->reg == 1, size, rm);
        }
        if (in->reg == 6)
            return gen_push_pop(j, ji);
        return -1;
    case 0x69: case 0x6b: case 0x1af:
        return gen_imul2(j, ji, op != 0x1af);
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3:
        return gen_shift(j, ji);
    case 0x1a4: case 0x1a5: case 0x1ac: case 0x1ad:
        return gen_shxd(j, ji);
    case 0x1a3: case 0x1ab: case 0x1b3: case 0x1bb: case 0x1ba:
        return gen_bt(j, ji);
    case 0x1bc: case 0x1bd:
        return gen_bsx(j, ji);
    case 0x1b0: case 0x1b1:
        return gen_cmpxchg(j, ji);
    case 0x1c0: case 0x1c1:
        return gen_xadd(j, ji);
    case 0x8d:
        return gen_lea(j, in);
    case 0x90: case 0x9b: case 0x10d: case 0x118: case 0x119: case 0x11a: case 0x11b:
    case 0x11c: case 0x11d: case 0x11e: case 0x11f:
        return 1;
    case 0x98:
        if (in->opsize == 4) {
            w_sxth(b, GREG(R_EAX), GREG(R_EAX));
        } else {
            w_sxtb(b, 0, GREG(R_EAX));
            w_bfi(b, GREG(R_EAX), 0, 0, 16);
        }
        return 1;
    case 0x99:
        if (in->opsize == 4) {
            w_asri(b, GREG(R_EDX), GREG(R_EAX), 31);
        } else {
            w_sbfx(b, 0, GREG(R_EAX), 15, 1);
            w_bfi(b, GREG(R_EDX), 0, 0, 16);
        }
        return 1;
    case 0x9c: case 0x9d: case 0x9e: case 0x9f:
    case 0xf5: case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xd6:
        return gen_flagops(j, ji);
    case 0xc8:   /* enter */
        if (in->imm2 != 0)
            return -1;
        w_str_pre(b, GREG(R_EBP), GREG(R_ESP), -4);
        w_mov(b, GREG(R_EBP), GREG(R_ESP));
        if (in->imm)
            w_addimm(b, GREG(R_ESP), GREG(R_ESP), (uint32_t)-(int32_t)in->imm, RA);
        return 1;
    case 0xc9:   /* leave */
        w_mov(b, GREG(R_ESP), GREG(R_EBP));
        w_ldr_post(b, GREG(R_EBP), GREG(R_ESP), 4);
        return 1;
    case 0xd7:   /* xlat */
        w_uxtb(b, 0, GREG(R_EAX));
        w_add(b, RA, GREG(R_EBX), 0);
        w_ldrb(b, 0, RA, 0);
        w_bfi(b, GREG(R_EAX), 0, 0, 8);
        return 1;
    case 0x8c:   /* mov r/m, sreg */
        w_movi(b, 0, in->reg == 1 ? 0x08 : in->reg == 4 ? 0x20 : 0x10);
        if (in->is_mem) {
            jit_ea(j, in, &am);
            jit_st(j, 2, 0, am);
        } else {
            jit_set_reg(j, 4, in->rm, 0);
        }
        return 1;
    case 0x8e:   /* mov sreg, r/m: flat segments, ignored */
        return 1;
    case 0x06: case 0x0e: case 0x16: case 0x1e: case 0x1a0:
        w_movi(b, 0, op == 0x0e ? 0x08 : op == 0x1a0 ? 0x20 : 0x10);
        push_reg(j, 4, 0);
        return 1;
    case 0x07: case 0x17: case 0x1f: case 0x1a1:
        w_addi(b, GREG(R_ESP), GREG(R_ESP), 4);
        return 1;
    case 0x108: case 0x109:   /* invd / wbinvd: caches are coherent here */
        return 1;
    case 0x101:               /* invlpg (other system forms are not used by titles) */
        return in->reg == 7 ? 1 : -1;
    case 0x130:               /* wrmsr: MTRR/PAT setup, ignored */
        return 1;
    case 0x132:               /* rdmsr */
        w_movi(b, GREG(R_EAX), 0);
        w_movi(b, GREG(R_EDX), 0);
        return 1;
    case 0x120: {             /* mov r32, crN */
        static const uint32_t cr[8] = { 0x80010033u, 0, 0, 0x0000f000u, 0x00000600u, 0, 0, 0 };
        w_movi(b, GREG(in->rm), cr[in->reg]);
        return 1;
    }
    case 0x121:               /* mov r32, drN */
        w_movi(b, GREG(in->rm), 0);
        return 1;
    case 0x122: case 0x123:   /* mov crN/drN, r32: ignored */
        return 1;
    case 0x1a2:
        jit_call_helper(j, H_CPUID, 1);
        return 1;
    case 0x131:
        jit_call_helper(j, H_RDTSC, 1);
        return 1;
    case 0xe4: case 0xe5: case 0xe6: case 0xe7: case 0xec: case 0xed: case 0xee: case 0xef:
        w_movi(b, 1, (uint32_t)op | (uint32_t)in->opsize << 8);
        w_movi(b, 2, in->imm & 0xff);
        jit_call_helper(j, H_MISC, 1);
        return 1;
    case 0x27: case 0x2f: case 0x37: case 0x3f: case 0xd4: case 0xd5:
        w_movi(b, 1, (uint32_t)op);
        w_movi(b, 2, in->imm & 0xff);
        jit_call_helper(j, H_MISC, 1);
        return 1;
    case 0x1c7:
        if (in->reg != 1 || !in->is_mem)
            return -1;
        jit_ea(j, in, &am);
        w_addimm(b, 1, am.base, (uint32_t)am.off, 2);
        jit_call_helper(j, H_CMPXCHG8B, 1);
        return 1;
    case 0xcc:
        jit_exit_special(j, in->addr + in->len, EXIT_INT, 3);
        return 1;
    case 0xcd:
        jit_exit_special(j, in->addr + in->len, EXIT_INT, in->imm & 0xff);
        return 1;
    case 0xce: {
        int hc = jit_cond(j, ji, 0);
        uint32_t *site = a64_here(b);
        a64_put(b, enc_bcond(0, hc));
        jit_stub(j, site, SITE_BCOND, in->addr + in->len, EXIT_INT, 4);
        return 1;
    }
    case 0xf4:
        jit_exit_special(j, in->addr + in->len, EXIT_HLT, 0);
        return 1;
    case 0x10b:
        jit_exit_special(j, in->addr, EXIT_UD, 0);
        return 1;
    }
    return 0;
}
