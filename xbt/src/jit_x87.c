/*
 * jit_x87.c - x87 FPU translation.
 *
 * The eight stack registers live as doubles in cpu->fpr[] (physical order) and
 * the TOP index stays in host register w27, so ST(0) is a single
 * `ldr d, [x28, w27, uxtw #3]`. Arithmetic is done in IEEE double precision
 * (like box86); the guest rounding mode is mirrored into the host FPCR, which
 * makes FIST/FISTP and FRNDINT honour FCW.RC without helper calls.
 * Transcendentals, BCD and state save/restore run in C (fpu.c).
 */
#include <string.h>
#include "jit.h"

/* keep in sync with fpu.c */
enum {
    X87H_F2XM1 = 1, X87H_FYL2X, X87H_FPTAN, X87H_FPATAN, X87H_FXTRACT, X87H_FPREM1,
    X87H_FPREM, X87H_FYL2XP1, X87H_FSINCOS, X87H_FSCALE, X87H_FSIN, X87H_FCOS,
    X87H_FXAM, X87H_FNINIT, X87H_FLDCW_SYNC,
    /* memory forms (H_X87_MEM, x1 = op, x2 = address) */
    X87M_FLD80 = 32, X87M_FSTP80, X87M_FBLD, X87M_FBSTP, X87M_FLDENV, X87M_FNSTENV,
    X87M_FRSTOR, X87M_FNSAVE,
};

#define FSW_C_MASK 0x4700u

/* ---- register cache ------------------------------------------------ */

/* physical-register index (w27 + slot) & 7 into a W register */
static int phys_idx(jit_t *j, int slot, int tmp)
{
    if ((slot & 7) == 0)
        return RTOP;
    a64buf *b = JB(j);
    w_addi(b, tmp, RTOP, (uint32_t)(slot & 7));
    w_andi(b, tmp, tmp, 7, tmp);
    return tmp;
}

static int alloc_dreg(jit_t *j)
{
    for (int r = 8; r < 16; r++) {
        int used = 0;
        for (int s = 0; s < 8; s++)
            if (j->x87_reg[s] == r)
                used = 1;
        if (!used)
            return r;
    }
    return -1;   /* cannot happen: 8 slots, 8 registers */
}

/* host D register holding ST(i), loaded on first use */
static int st_get(jit_t *j, int i)
{
    int slot = (j->x87_delta + i) & 7;
    if (j->x87_reg[slot] < 0) {
        int r = alloc_dreg(j);
        int idx = phys_idx(j, slot, 9);
        a64_put(JB(j), enc_fldst_reg(FSZ_D, 1, r, RCPU, idx, EX_UXTW, 1));
        j->x87_reg[slot] = (int8_t)r;
        j->x87_dirty[slot] = 0;
    }
    return j->x87_reg[slot];
}

/* host D register that will receive a new value of ST(i) */
static int st_def(jit_t *j, int i)
{
    int slot = (j->x87_delta + i) & 7;
    if (j->x87_reg[slot] < 0)
        j->x87_reg[slot] = (int8_t)alloc_dreg(j);
    j->x87_dirty[slot] = 1;
    return j->x87_reg[slot];
}

static void ld_st(jit_t *j, int vd, int i)
{
    int r = st_get(j, i);
    if (r != vd)
        f_mov(JB(j), 1, vd, r);
}

static void x87_push(jit_t *j)
{
    j->x87_delta = (j->x87_delta - 1) & 7;
    int slot = j->x87_delta;
    j->x87_reg[slot] = -1;          /* old content of the new top is dead */
    j->x87_dirty[slot] = 0;
}

static void x87_pop(jit_t *j)
{
    int slot = j->x87_delta;
    j->x87_reg[slot] = -1;          /* a popped value is dead */
    j->x87_dirty[slot] = 0;
    j->x87_delta = (j->x87_delta + 1) & 7;
}

/* Write dirty slots back and make w27 the real TOP. */
void jit_x87_flush(jit_t *j, int invalidate)
{
    a64buf *b = JB(j);
    for (int s = 0; s < 8; s++) {
        if (j->x87_reg[s] >= 0 && j->x87_dirty[s]) {
            int idx = phys_idx(j, s, 9);
            a64_put(b, enc_fldst_reg(FSZ_D, 0, j->x87_reg[s], RCPU, idx, EX_UXTW, 1));
            j->x87_dirty[s] = 0;
        }
    }
    if (j->x87_delta) {
        w_addi(b, RTOP, RTOP, (uint32_t)j->x87_delta);
        w_andi(b, RTOP, RTOP, 7, RTOP);
        j->x87_delta = 0;
        invalidate = 1;             /* slots are relative to the old TOP */
    }
    if (invalidate)
        for (int s = 0; s < 8; s++)
            j->x87_reg[s] = -1;
}

/* Load a memory operand as a double into vd. kind: 0 m32fp 1 m64fp 2 m32int 3 m16int 4 m64int */
static void ld_mem(jit_t *j, const x86_insn *in, int kind, int vd)
{
    a64buf *b = JB(j);
    amode am;
    jit_ea(j, in, &am);
    switch (kind) {
    case 0:
        jit_fld(j, FSZ_S, vd, am);
        f_cvt_s2d(b, vd, vd);
        break;
    case 1:
        jit_fld(j, FSZ_D, vd, am);
        break;
    case 2:
        jit_ld(j, 4, 1, am);
        f_scvtf(b, 0, 1, vd, 1);
        break;
    case 3:
        jit_ld_sx(j, 2, 1, am);
        f_scvtf(b, 0, 1, vd, 1);
        break;
    case 4:
        jit_ld(j, 8, 1, am);
        f_scvtf(b, 1, 1, vd, 1);
        break;
    }
}

/* op: 0 add 1 mul 2 com 3 comp 4 sub 5 subr 6 div 7 divr ; d = d op s */
static void arith(jit_t *j, int op, int vd, int vs)
{
    a64buf *b = JB(j);
    switch (op) {
    case 0: f_add(b, 1, vd, vd, vs); break;
    case 1: f_mul(b, 1, vd, vd, vs); break;
    case 4: f_sub(b, 1, vd, vd, vs); break;
    case 5: f_sub(b, 1, vd, vs, vd); break;
    case 6: f_div(b, 1, vd, vd, vs); break;
    case 7: f_div(b, 1, vd, vs, vd); break;
    }
}

/* Compare d0 with d1 and set FSW C0/C2/C3. */
static void fcom_fsw(jit_t *j)
{
    a64buf *b = JB(j);
    f_cmp(b, 1, 0, 1);
    w_cset(b, 1, CC_LT);            /* C0: less or unordered */
    w_cset(b, 2, CC_VS);            /* C2: unordered */
    w_cset(b, 3, CC_EQ);
    w_orr(b, 3, 3, 2);              /* C3: equal or unordered */
    w_ldrh(b, 4, RCPU, CPUOFF(fsw));
    w_andi(b, 4, 4, ~FSW_C_MASK & 0xffffu, 5);
    w_orr_sh(b, 4, 4, 1, SH_LSL, 8);
    w_orr_sh(b, 4, 4, 2, SH_LSL, 10);
    w_orr_sh(b, 4, 4, 3, SH_LSL, 14);
    w_strh(b, 4, RCPU, CPUOFF(fsw));
    jit_nzcv_clobber(j);
}

/* fcomi family: NZCV holds the comparison (NM_FCMP), optionally stored */
static void fcomi(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    f_cmp(b, 1, 0, 1);
    if (ji->hostfl)
        jit_flags_produced(j, ji);
    else
        jit_nzcv_clobber(j);
    if (ji->store) {
        /* ZF = eq|un, PF = un, CF = lt|un, others clear */
        w_cset(b, 1, CC_VS);
        w_cset(b, 2, CC_EQ);
        w_orr(b, 2, 2, 1);
        w_cset(b, 3, CC_LT);
        w_ldr(b, 4, RCPU, CPUOFF(eflags));
        w_andi(b, 4, 4, ~XF_STATUS, 5);
        w_orr_sh(b, 4, 4, 1, SH_LSL, 2);
        w_orr_sh(b, 4, 4, 2, SH_LSL, 6);
        w_orr(b, 4, 4, 3);
        w_str(b, 4, RCPU, CPUOFF(eflags));
        w_str(b, ZR, RCPU, CPUOFF(lf_op));
        ji->rec_kind = 0;
    }
}

/* Store ST(0) as an integer of `size` bytes, rounding per FCW (via FPCR). */
static void fist(jit_t *j, const x86_insn *in, int size)
{
    a64buf *b = JB(j);
    amode am;
    ld_st(j, 0, 0);
    f_frinti(b, 1, 0, 0);
    f_fcvtzs(b, 1, 1, 1, 0);         /* x1 = (int64) d0, saturating */
    /* out of range or NaN -> "integer indefinite" (most negative value) */
    if (size == 8) {
        x_movi(b, 2, 0x8000000000000000ull);
        x_movi(b, 3, 0x7fffffffffffffffull);
        a64_put(b, enc_addsub_sreg(1, 1, 1, ZR, 1, 3, SH_LSL, 0));   /* cmp x1, x3 */
        x_csel(b, 1, 2, 1, CC_EQ);
        f_cmp(b, 1, 0, 0);
        x_csel(b, 1, 2, 1, CC_VS);
    } else {
        int bits = 8 * size;
        w_movi(b, 2, 1u << (bits - 1));
        /* fits iff sext(x1, bits) == x1 */
        if (size == 4)
            a64_put(b, enc_addsub_ereg(1, 1, 1, ZR, 1, 1, EX_SXTW, 0));
        else
            a64_put(b, enc_addsub_ereg(1, 1, 1, ZR, 1, 1, EX_SXTH, 0));
        w_csel(b, 1, 1, 2, CC_EQ);
        f_cmp(b, 1, 0, 0);
        w_csel(b, 1, 2, 1, CC_VS);
    }
    jit_nzcv_clobber(j);
    jit_ea(j, in, &am);
    jit_st(j, size, 1, am);
}

static void load_const(jit_t *j, int vd, double v)
{
    a64buf *b = JB(j);
    uint64_t bits;
    memcpy(&bits, &v, 8);
    if (v == 1.0) {
        f_fmovi(b, 1, vd, 0x70);
    } else if (bits == 0) {
        f_fmov_dx(b, vd, ZR);
    } else {
        x_movi(b, 9, bits);
        f_fmov_dx(b, vd, 9);
    }
}

static void helper(jit_t *j, int op, int mem, const x86_insn *in)
{
    a64buf *b = JB(j);
    if (mem) {
        amode am;
        jit_ea(j, in, &am);
        w_addimm(b, 2, am.base, (uint32_t)am.off, 3);
    }
    w_movi(b, 1, (uint32_t)op);
    jit_call_helper(j, mem ? H_X87_MEM : H_X87, 1);
}

/* fldcw: store the control word and mirror RC into FPCR.RMode */
static void fldcw_from(jit_t *j, int wr)
{
    a64buf *b = JB(j);
    w_strh(b, wr, RCPU, CPUOFF(fcw));
    w_ubfx(b, 1, wr, 10, 2);          /* RC: 0 near 1 down 2 up 3 zero */
    w_rbit(b, 1, 1);
    w_lsri(b, 1, 1, 30);              /* bit-reverse: ARM RMode 0 RN 1 RP 2 RM 3 RZ */
    x_mrs_fpcr(b, 2);
    x_bfi(b, 2, 1, 22, 2);
    x_msr_fpcr(b, 2);
}

static int cc_fcmov(int op, int rm_hi)
{
    /* DA: C0 B, C8 E, D0 BE, D8 U ; DB: C0 NB, C8 NE, D0 NBE, D8 NU */
    static const int base[4] = { 2, 4, 6, 10 };
    return base[rm_hi] | (op == 0xdb ? 1 : 0);
}

void jit_x87_classify(jinsn *ji)
{
    x86_insn *in = &ji->in;
    int op = in->op, reg = in->reg;
    ji->clobber = 0;
    if (in->is_mem) {
        if ((op == 0xd8 || op == 0xdc || op == 0xda || op == 0xde) && (reg == 2 || reg == 3))
            ji->clobber = 1;                    /* fcom / ficom */
        if (op == 0xd9 && (reg == 4 || reg == 6))
            ji->clobber = 1;                    /* fldenv / fnstenv */
        if ((op == 0xdb && (reg == 2 || reg == 3 || reg == 5 || reg == 7)) ||
            (op == 0xdd && (reg == 4 || reg == 6)) ||
            (op == 0xdf && (reg == 2 || reg == 3 || reg == 4 || reg == 6 || reg == 7)))
            ji->clobber = 1;                    /* fist, m80, bcd, save/restore */
        return;
    }
    uint8_t modrm = (uint8_t)(0xc0 | reg << 3 | in->rm);
    if ((op == 0xdb && modrm >= 0xe8 && modrm <= 0xf7) || (op == 0xdf && modrm >= 0xe8 && modrm <= 0xf7)) {
        ji->wr = FALL;
        ji->mode = NM_FCMP;
        ji->clobber = 1;
        return;
    }
    if ((op == 0xda || op == 0xdb) && modrm < 0xe0) {
        ji->cc = (uint8_t)cc_fcmov(op, (modrm >> 3) & 3);
        static const uint8_t fl[4] = { FCF, FZF, FCF | FZF, FPF };
        ji->rd = fl[(modrm >> 3) & 3];
        ji->consumer = CK_COND;
        return;
    }
    if (op == 0xd8 && (reg == 2 || reg == 3))
        ji->clobber = 1;
    if (op == 0xdc && (reg == 2 || reg == 3))
        ji->clobber = 1;
    if (op == 0xde && modrm == 0xd9)
        ji->clobber = 1;
    if (op == 0xdd && (reg == 4 || reg == 5))
        ji->clobber = 1;
    if (op == 0xda && modrm == 0xe9)
        ji->clobber = 1;
    if (op == 0xd9 && (modrm == 0xe4 || modrm == 0xe5 || modrm >= 0xf0) &&
        modrm != 0xf6 && modrm != 0xf7 && modrm != 0xfa && modrm != 0xfc)
        ji->clobber = 1;                        /* ftst fxam transcendental ...; not the
                                                   constants, fsqrt, frndint, fincstp/fdecstp */
    if (op == 0xdb && (modrm == 0xe2 || modrm == 0xe3))
        ji->clobber = 1;
}

int jit_x87(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op, reg = in->reg;
    j->x87_used = 1;

    if (in->is_mem) {
        amode am;
        switch (op) {
        case 0xd8: case 0xdc: case 0xda: case 0xde: {
            int kind = op == 0xd8 ? 0 : op == 0xdc ? 1 : op == 0xda ? 2 : 3;
            ld_mem(j, in, kind, 1);
            if (reg == 2 || reg == 3) {
                ld_st(j, 0, 0);
                fcom_fsw(j);
                if (reg == 3)
                    x87_pop(j);
            } else {
                st_get(j, 0);
                int r = st_def(j, 0);
                arith(j, reg, r, 1);
            }
            return 1;
        }
        case 0xd9:
            switch (reg) {
            case 0:
                x87_push(j);
                ld_mem(j, in, 0, st_def(j, 0));
                return 1;
            case 2: case 3:
                f_cvt_d2s(b, 0, st_get(j, 0));
                jit_ea(j, in, &am);
                jit_fst(j, FSZ_S, 0, am);
                if (reg == 3)
                    x87_pop(j);
                return 1;
            case 4:
                helper(j, X87M_FLDENV, 1, in);
                return 1;
            case 5:
                jit_ea(j, in, &am);
                jit_ld(j, 2, 0, am);
                fldcw_from(j, 0);
                return 1;
            case 6:
                helper(j, X87M_FNSTENV, 1, in);
                return 1;
            case 7:
                w_ldrh(b, 0, RCPU, CPUOFF(fcw));
                jit_ea(j, in, &am);
                jit_st(j, 2, 0, am);
                return 1;
            }
            return 0;
        case 0xdb:
            switch (reg) {
            case 0:
                x87_push(j);
                ld_mem(j, in, 2, st_def(j, 0));
                return 1;
            case 2: case 3:
                fist(j, in, 4);
                if (reg == 3)
                    x87_pop(j);
                return 1;
            case 5:
                helper(j, X87M_FLD80, 1, in);
                return 1;
            case 7:
                helper(j, X87M_FSTP80, 1, in);
                return 1;
            }
            return 0;
        case 0xdd:
            switch (reg) {
            case 0:
                x87_push(j);
                ld_mem(j, in, 1, st_def(j, 0));
                return 1;
            case 2: case 3: {
                int r = st_get(j, 0);
                jit_ea(j, in, &am);
                jit_fst(j, FSZ_D, r, am);
                if (reg == 3)
                    x87_pop(j);
                return 1;
            }
            case 4:
                helper(j, X87M_FRSTOR, 1, in);
                return 1;
            case 6:
                helper(j, X87M_FNSAVE, 1, in);
                return 1;
            case 7: {
                int t = phys_idx(j, j->x87_delta, 2);
                w_ldrh(b, 0, RCPU, CPUOFF(fsw));
                w_andi(b, 0, 0, ~0x3800u & 0xffffu, 1);
                w_orr_sh(b, 0, 0, t, SH_LSL, 11);
                jit_ea(j, in, &am);
                jit_st(j, 2, 0, am);
                return 1;
            }
            }
            return 0;
        case 0xdf:
            switch (reg) {
            case 0:
                x87_push(j);
                ld_mem(j, in, 3, st_def(j, 0));
                return 1;
            case 2: case 3:
                fist(j, in, 2);
                if (reg == 3)
                    x87_pop(j);
                return 1;
            case 4:
                helper(j, X87M_FBLD, 1, in);
                return 1;
            case 5:
                x87_push(j);
                ld_mem(j, in, 4, st_def(j, 0));
                return 1;
            case 6:
                helper(j, X87M_FBSTP, 1, in);
                return 1;
            case 7:
                fist(j, in, 8);
                x87_pop(j);
                return 1;
            }
            return 0;
        }
        return 0;
    }

    /* register forms */
    int i = in->rm;
    uint8_t modrm = (uint8_t)(0xc0 | reg << 3 | i);
    switch (op) {
    case 0xd8:
        if (reg == 2 || reg == 3) {
            ld_st(j, 0, 0);
            ld_st(j, 1, i);
            fcom_fsw(j);
            if (reg == 3)
                x87_pop(j);
        } else {
            int rs = st_get(j, i);
            st_get(j, 0);
            int rd = st_def(j, 0);
            arith(j, reg, rd, rs);
        }
        return 1;
    case 0xdc: case 0xde: {
        if (op == 0xde && modrm == 0xd9) {          /* fcompp */
            ld_st(j, 0, 0);
            ld_st(j, 1, 1);
            fcom_fsw(j);
            x87_pop(j);
            x87_pop(j);
            return 1;
        }
        if (reg == 2 || reg == 3) {                 /* fcom/fcomp aliases */
            ld_st(j, 0, 0);
            ld_st(j, 1, i);
            fcom_fsw(j);
            if (reg == 3 || op == 0xde)
                x87_pop(j);
            return 1;
        }
        /* ST(i) = ST(i) op ST(0); sub/subr and div/divr are swapped here */
        static const int map[8] = { 0, 1, -1, -1, 5, 4, 7, 6 };
        int rs = st_get(j, 0);
        st_get(j, i);
        int rd = st_def(j, i);
        arith(j, map[reg], rd, rs);
        if (op == 0xde)
            x87_pop(j);
        return 1;
    }
    case 0xd9:
        if (reg == 0) {                             /* fld st(i) */
            int rs = st_get(j, i);
            x87_push(j);
            int rd = st_def(j, 0);
            f_mov(b, 1, rd, rs);
            return 1;
        }
        if (reg == 1) {                             /* fxch: renaming, no code */
            int s0 = j->x87_delta & 7, si = (j->x87_delta + i) & 7;
            st_get(j, 0);
            st_get(j, i);
            int8_t t = j->x87_reg[s0];
            j->x87_reg[s0] = j->x87_reg[si];
            j->x87_reg[si] = t;
            j->x87_dirty[s0] = j->x87_dirty[si] = 1;
            return 1;
        }
        switch (modrm) {
        case 0xd0: return 1;                        /* fnop */
        case 0xe0: { int r = st_get(j, 0); st_def(j, 0); f_neg(b, 1, r, r); return 1; }
        case 0xe1: { int r = st_get(j, 0); st_def(j, 0); f_abs(b, 1, r, r); return 1; }
        case 0xe4:                                  /* ftst */
            ld_st(j, 0, 0);
            f_fmov_dx(b, 1, ZR);
            fcom_fsw(j);
            return 1;
        case 0xe5: helper(j, X87H_FXAM, 0, in); return 1;
        case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec: case 0xed: case 0xee: {
            static const double k[7] = {
                1.0, 3.32192809488736234787, 1.44269504088896340736, 3.14159265358979323846,
                0.30102999566398119521, 0.69314718055994530942, 0.0
            };
            x87_push(j);
            load_const(j, st_def(j, 0), k[modrm - 0xe8]);
            return 1;
        }
        case 0xf0: helper(j, X87H_F2XM1, 0, in); return 1;
        case 0xf1: helper(j, X87H_FYL2X, 0, in); return 1;
        case 0xf2: helper(j, X87H_FPTAN, 0, in); return 1;
        case 0xf3: helper(j, X87H_FPATAN, 0, in); return 1;
        case 0xf4: helper(j, X87H_FXTRACT, 0, in); return 1;
        case 0xf5: helper(j, X87H_FPREM1, 0, in); return 1;
        case 0xf6: j->x87_delta = (j->x87_delta - 1) & 7; return 1;   /* fdecstp: values stay */
        case 0xf7: j->x87_delta = (j->x87_delta + 1) & 7; return 1;   /* fincstp */
        case 0xf8: helper(j, X87H_FPREM, 0, in); return 1;
        case 0xf9: helper(j, X87H_FYL2XP1, 0, in); return 1;
        case 0xfa: { int r = st_get(j, 0); st_def(j, 0); f_sqrt(b, 1, r, r); return 1; }
        case 0xfb: helper(j, X87H_FSINCOS, 0, in); return 1;
        case 0xfc: { int r = st_get(j, 0); st_def(j, 0); f_frinti(b, 1, r, r); return 1; }
        case 0xfd: helper(j, X87H_FSCALE, 0, in); return 1;
        case 0xfe: helper(j, X87H_FSIN, 0, in); return 1;
        case 0xff: helper(j, X87H_FCOS, 0, in); return 1;
        }
        return 0;
    case 0xda: case 0xdb:
        if (modrm < 0xe0) {                         /* fcmovcc */
            int hc = jit_cond(j, ji, ji->cc);
            int rs = st_get(j, i);
            st_get(j, 0);
            int rd = st_def(j, 0);
            if (hc == HC_EQ_OR_VS) {
                f_csel(b, 1, rd, rs, rd, CC_EQ);
                f_csel(b, 1, rd, rs, rd, CC_VS);
            } else if (hc == HC_NE_AND_VC) {
                f_csel(b, 1, 2, rs, rd, CC_NE);
                f_csel(b, 1, rd, 2, rd, CC_VC);
            } else {
                f_csel(b, 1, rd, rs, rd, hc);
            }
            return 1;
        }
        if (op == 0xda && modrm == 0xe9) {          /* fucompp */
            ld_st(j, 0, 0);
            ld_st(j, 1, 1);
            fcom_fsw(j);
            x87_pop(j);
            x87_pop(j);
            return 1;
        }
        if (op == 0xdb) {
            if (modrm == 0xe2) {                    /* fnclex */
                w_ldrh(b, 0, RCPU, CPUOFF(fsw));
                w_andi(b, 0, 0, 0x7f00u, 1);
                w_strh(b, 0, RCPU, CPUOFF(fsw));
                return 1;
            }
            if (modrm == 0xe3) {                    /* fninit */
                helper(j, X87H_FNINIT, 0, in);
                return 1;
            }
            if (modrm >= 0xe8 && modrm <= 0xf7) {   /* fucomi / fcomi */
                ld_st(j, 0, 0);
                ld_st(j, 1, i);
                fcomi(j, ji);
                return 1;
            }
            if (modrm == 0xe0 || modrm == 0xe1 || modrm == 0xe4)
                return 1;                           /* feni/fdisi/fsetpm: no-ops */
        }
        return 0;
    case 0xdd:
        switch (reg) {
        case 0: return 1;                           /* ffree */
        case 2: case 3: {                           /* fst / fstp st(i) */
            int rs = st_get(j, 0);
            if (i != 0) {
                int rd = st_def(j, i);
                f_mov(b, 1, rd, rs);
            }
            if (reg == 3)
                x87_pop(j);
            return 1;
        }
        case 4: case 5:                             /* fucom / fucomp */
            ld_st(j, 0, 0);
            ld_st(j, 1, i);
            fcom_fsw(j);
            if (reg == 5)
                x87_pop(j);
            return 1;
        }
        return 0;
    case 0xdf:
        if (modrm == 0xe0) {                        /* fnstsw ax */
            int t = phys_idx(j, j->x87_delta, 2);
            w_ldrh(b, 0, RCPU, CPUOFF(fsw));
            w_andi(b, 0, 0, ~0x3800u & 0xffffu, 1);
            w_orr_sh(b, 0, 0, t, SH_LSL, 11);
            w_bfi(b, GREG(R_EAX), 0, 0, 16);
            return 1;
        }
        if (modrm >= 0xe8 && modrm <= 0xf7) {       /* fucomip / fcomip */
            ld_st(j, 0, 0);
            ld_st(j, 1, i);
            fcomi(j, ji);
            x87_pop(j);
            return 1;
        }
        if (reg == 0) {                             /* ffreep */
            x87_pop(j);
            return 1;
        }
        return 0;
    }
    return 0;
}

void jit_x87_block_end(jit_t *j) { (void)j; }
