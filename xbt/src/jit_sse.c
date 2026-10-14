/*
 * jit_sse.c - MMX and SSE (Pentium III) translation onto AArch64 NEON.
 *
 * XMM registers live in cpu->xmm[], MMX registers in cpu->mm[] (the x87
 * aliasing of MMX is not modelled: titles execute EMMS before x87 code).
 * Each instruction loads its operands into v0/v1, computes and stores back;
 * scalar (ss) forms only store the low lane, which gives the x86
 * "upper lanes unchanged" semantics for free.
 * SSE2 (0x66/0xF2 prefixed) does not exist on the Xbox CPU and is rejected.
 */
#include <string.h>
#include "jit.h"

#define XOFF(r) (CPUOFF(xmm) + 16 * (r))
#define MOFF(r) (CPUOFF(mm) + 8 * (r))

enum { SSEH_FXSAVE = 1, SSEH_FXRSTOR, SSEH_MASKMOVQ };

static void xmm_ld(a64buf *b, int sz, int v, int r, int off) { f_ldr(b, sz, v, RCPU, XOFF(r) + off); }
static void xmm_st(a64buf *b, int sz, int v, int r, int off) { f_str(b, sz, v, RCPU, XOFF(r) + off); }
static void mm_ld(a64buf *b, int v, int r) { f_ldr(b, FSZ_D, v, RCPU, MOFF(r)); }
static void mm_st(a64buf *b, int v, int r) { f_str(b, FSZ_D, v, RCPU, MOFF(r)); }

/* Load the xmm r/m operand (sz bytes as FSZ_*) into v. */
static void ld_xrm(jit_t *j, const x86_insn *in, int sz, int v)
{
    if (in->is_mem) {
        amode am;
        jit_ea(j, in, &am);
        jit_fld(j, sz, v, am);
    } else {
        xmm_ld(JB(j), sz, v, in->rm, 0);
    }
}

static void ld_mrm(jit_t *j, const x86_insn *in, int v)
{
    if (in->is_mem) {
        amode am;
        jit_ea(j, in, &am);
        jit_fld(j, FSZ_D, v, am);
    } else {
        mm_ld(JB(j), v, in->rm);
    }
}

static void st_xrm(jit_t *j, const x86_insn *in, int sz, int v)
{
    if (in->is_mem) {
        amode am;
        jit_ea(j, in, &am);
        jit_fst(j, sz, v, am);
    } else {
        xmm_st(JB(j), sz, v, in->rm, 0);
    }
}

/* x86 minps/maxps: result = (d < s) ? d : s, i.e. the source on NaN or equality */
static void minmax(a64buf *b, int is_max, int q)
{
    if (is_max)
        V3(b, VF_CMGT, q, 0, 2, 0, 1);      /* d > s */
    else
        V3(b, VF_CMGT, q, 0, 2, 1, 0);      /* s > d */
    V3(b, V_BSL, q, 0, 2, 0, 1);            /* v2 = mask ? d : s */
}

/* cmpps predicate into v2 (v0 = d, v1 = s) */
static void cmp_pred(a64buf *b, int pred)
{
    switch (pred & 7) {
    case 0: V3(b, VF_CMEQ, 1, 0, 2, 0, 1); break;
    case 1: V3(b, VF_CMGT, 1, 0, 2, 1, 0); break;
    case 2: V3(b, VF_CMGE, 1, 0, 2, 1, 0); break;
    case 3: case 7:
        V3(b, VF_CMEQ, 1, 0, 2, 0, 0);
        V3(b, VF_CMEQ, 1, 0, 3, 1, 1);
        V3(b, V_AND, 1, 0, 2, 2, 3);
        if ((pred & 7) == 3)
            V2(b, V_NOT, 1, 0, 2, 2);
        break;
    case 4: V3(b, VF_CMEQ, 1, 0, 2, 0, 1); V2(b, V_NOT, 1, 0, 2, 2); break;
    case 5: V3(b, VF_CMGT, 1, 0, 2, 1, 0); V2(b, V_NOT, 1, 0, 2, 2); break;
    case 6: V3(b, VF_CMGE, 1, 0, 2, 1, 0); V2(b, V_NOT, 1, 0, 2, 2); break;
    }
}

/* float -> int32 with x86 out-of-range behaviour (0x80000000). v0 lane 0 */
static void cvt_s2i(jit_t *j, int trunc, int rd)
{
    a64buf *b = JB(j);
    if (trunc)
        f_fcvtzs(b, 1, 0, 1, 0);
    else
        f_fcvtns(b, 1, 0, 1, 0);
    w_movi(b, 2, 0x80000000u);
    a64_put(b, enc_addsub_ereg(1, 1, 1, ZR, 1, 1, EX_SXTW, 0));     /* fits in int32? */
    w_csel(b, rd, 1, 2, CC_EQ);
    f_cmp(b, 0, 0, 0);
    w_csel(b, rd, 2, rd, CC_VS);
    jit_nzcv_clobber(j);
}

int jit_sse_classify(jinsn *ji)
{
    x86_insn *in = &ji->in;
    int op = in->op;
    ji->clobber = 0;
    switch (op) {
    case 0x12e: case 0x12f:
        ji->wr = FALL;
        ji->mode = NM_FCMP;
        ji->clobber = 1;
        return 1;
    case 0x12c: case 0x12d: case 0x1d1: case 0x1d2: case 0x1d3: case 0x1e1: case 0x1e2:
    case 0x1f1: case 0x1f2: case 0x1f3: case 0x1ae:
        ji->clobber = 1;
        return 1;
    case 0x1c7:
        ji->wr = FZF;
        ji->clobber = 1;
        return 1;
    }
    if (op == 0x131 || op == 0x1a2)
        ji->clobber = 1;
    return 0;
}

static int gen_mmx(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op & 0xff;
    int d = in->reg;
    amode am;

    switch (op) {
    case 0x6e:   /* movd mm, r/m32 */
        if (in->is_mem) {
            jit_ea(j, in, &am);
            jit_fld(j, FSZ_S, 0, am);
        } else {
            f_fmov_sw(b, 0, GREG(in->rm));
        }
        mm_st(b, 0, d);
        return 1;
    case 0x7e:   /* movd r/m32, mm */
        if (in->is_mem) {
            f_ldr(b, FSZ_S, 0, RCPU, MOFF(d));
            jit_ea(j, in, &am);
            jit_fst(j, FSZ_S, 0, am);
        } else {
            w_ldr(b, GREG(in->rm), RCPU, MOFF(d));
        }
        return 1;
    case 0x6f:   /* movq mm, mm/m64 */
        ld_mrm(j, in, 0);
        mm_st(b, 0, d);
        return 1;
    case 0x7f: case 0xe7:   /* movq / movntq mm/m64, mm */
        mm_ld(b, 0, d);
        if (in->is_mem) {
            jit_ea(j, in, &am);
            jit_fst(j, FSZ_D, 0, am);
        } else {
            mm_st(b, 0, in->rm);
        }
        return 1;
    case 0x77:   /* emms */
        w_movi(b, 0, 0xff);
        w_strh(b, 0, RCPU, CPUOFF(ftw));
        return 1;
    case 0x71: case 0x72: case 0x73: {   /* shift by immediate */
        int esize = op == 0x71 ? 16 : op == 0x72 ? 32 : 64;
        int sh = (int)(in->imm & 0xff);
        int kind = in->reg;   /* 2 srl, 4 sra, 6 sll */
        mm_ld(b, 0, in->rm);
        if (kind == 2 || kind == 6) {
            if (sh >= esize) {
                f_fmov_dx(b, 0, ZR);
            } else if (sh) {
                if (kind == 2)
                    v_ushr(b, 0, esize, 0, 0, sh);
                else
                    v_shl(b, 0, esize, 0, 0, sh);
            }
        } else if (kind == 4 && esize != 64) {
            if (sh >= esize)
                sh = esize - 1;
            if (sh)
                v_sshr(b, 0, esize, 0, 0, sh);
        } else {
            return -1;
        }
        mm_st(b, 0, in->rm);
        return 1;
    }
    case 0x70: {   /* pshufw */
        ld_mrm(j, in, 1);
        for (int k = 0; k < 4; k++)
            v_ins_elem(b, 1, 0, k, 1, (int)(in->imm >> (2 * k)) & 3);
        mm_st(b, 0, d);
        return 1;
    }
    case 0xc4:   /* pinsrw mm, r32/m16, imm */
        mm_ld(b, 0, d);
        if (in->is_mem) {
            jit_ea(j, in, &am);
            jit_ld(j, 2, 1, am);
            v_ins_gen(b, 1, 0, (int)in->imm & 3, 1);
        } else {
            v_ins_gen(b, 1, 0, (int)in->imm & 3, GREG(in->rm));
        }
        mm_st(b, 0, d);
        return 1;
    case 0xc5:   /* pextrw r32, mm, imm */
        if (in->is_mem)
            return -1;
        mm_ld(b, 0, in->rm);
        v_umov(b, 1, GREG(d), 0, (int)in->imm & 3);
        return 1;
    case 0xd7:   /* pmovmskb r32, mm */
        if (in->is_mem)
            return -1;
        mm_ld(b, 0, in->rm);
        V2(b, VI_CMLT0, 0, 0, 0, 0);
        x_movi(b, 9, 0x8040201008040201ull);
        f_fmov_dx(b, 1, 9);
        V3(b, V_AND, 0, 0, 0, 0, 1);
        V2(b, VI_ADDV, 0, 0, 0, 0);
        v_umov(b, 0, GREG(d), 0, 0);
        return 1;
    case 0xf7: { /* maskmovq mm1, mm2: store bytes of mm1 where mm2 byte msb set, to [edi] */
        if (in->is_mem)
            return -1;
        mm_ld(b, 0, d);
        mm_ld(b, 1, in->rm);
        f_ldr(b, FSZ_D, 2, GREG(R_EDI), 0);
        V2(b, VI_CMLT0, 0, 0, 1, 1);
        V3(b, V_BIT, 0, 0, 2, 0, 1);       /* insert v0 where mask */
        f_str(b, FSZ_D, 2, GREG(R_EDI), 0);
        return 1;
    }
    }

    /* two-operand forms: v0 = mm(d), v1 = mm/m64 */
    mm_ld(b, 0, d);
    int shift_esize = 0, shift_kind = 0;
    switch (op) {
    case 0xd1: shift_esize = 16; shift_kind = 2; break;
    case 0xd2: shift_esize = 32; shift_kind = 2; break;
    case 0xd3: shift_esize = 64; shift_kind = 2; break;
    case 0xe1: shift_esize = 16; shift_kind = 4; break;
    case 0xe2: shift_esize = 32; shift_kind = 4; break;
    case 0xf1: shift_esize = 16; shift_kind = 6; break;
    case 0xf2: shift_esize = 32; shift_kind = 6; break;
    case 0xf3: shift_esize = 64; shift_kind = 6; break;
    }
    if (shift_esize) {
        /* count is the whole 64-bit operand; clamp to 64 then use (u|s)shl */
        if (in->is_mem) {
            jit_ea(j, in, &am);
            jit_ld(j, 8, 1, am);
        } else {
            x_ldr(b, 1, RCPU, MOFF(in->rm));
        }
        x_movi(b, 2, 64);
        a64_put(b, enc_addsub_sreg(1, 1, 1, ZR, 1, 2, SH_LSL, 0));   /* cmp x1, x2 */
        x_csel(b, 1, 1, 2, CC_LO);
        jit_nzcv_clobber(j);
        if (shift_kind != 6)
            w_neg(b, 1, 1);
        int esl = shift_esize == 16 ? 1 : shift_esize == 32 ? 2 : 3;
        if (esl == 3) {
            f_fmov_dx(b, 1, 1);
            a64_put(b, (shift_kind == 4 ? VS_SSHL_D : VS_USHL_D) | 1 << 16 | 0 << 5 | 0);
        } else {
            v_dup_gen(b, 0, 0, 1, 1);    /* shift count read from the low byte of each lane */
            V3(b, shift_kind == 4 ? VI_SSHL : VI_USHL, 0, esl, 0, 0, 1);
        }
        mm_st(b, 0, d);
        return 1;
    }
    ld_mrm(j, in, 1);
    switch (op) {
    case 0x60: V3(b, VI_ZIP1, 0, 0, 0, 0, 1); break;
    case 0x61: V3(b, VI_ZIP1, 0, 1, 0, 0, 1); break;
    case 0x62: V3(b, VI_ZIP1, 0, 2, 0, 0, 1); break;
    case 0x68: V3(b, VI_ZIP2, 0, 0, 0, 0, 1); break;
    case 0x69: V3(b, VI_ZIP2, 0, 1, 0, 0, 1); break;
    case 0x6a: V3(b, VI_ZIP2, 0, 2, 0, 0, 1); break;
    case 0x63: v_ins_elem(b, 3, 0, 1, 1, 0); V2(b, VI_SQXTN, 0, 0, 0, 0); break;
    case 0x6b: v_ins_elem(b, 3, 0, 1, 1, 0); V2(b, VI_SQXTN, 0, 1, 0, 0); break;
    case 0x67: v_ins_elem(b, 3, 0, 1, 1, 0); V2(b, VI_SQXTUN, 0, 0, 0, 0); break;
    case 0x64: V3(b, VI_CMGT, 0, 0, 0, 0, 1); break;
    case 0x65: V3(b, VI_CMGT, 0, 1, 0, 0, 1); break;
    case 0x66: V3(b, VI_CMGT, 0, 2, 0, 0, 1); break;
    case 0x74: V3(b, VI_CMEQ, 0, 0, 0, 0, 1); break;
    case 0x75: V3(b, VI_CMEQ, 0, 1, 0, 0, 1); break;
    case 0x76: V3(b, VI_CMEQ, 0, 2, 0, 0, 1); break;
    case 0xd5: V3(b, VI_MUL, 0, 1, 0, 0, 1); break;
    case 0xe5: V3(b, VI_SMULL, 0, 1, 2, 0, 1); v_shrn(b, 16, 0, 2, 16); break;
    case 0xe4: V3(b, VI_UMULL, 0, 1, 2, 0, 1); v_shrn(b, 16, 0, 2, 16); break;
    case 0xf5: V3(b, VI_SMULL, 0, 1, 2, 0, 1); V3(b, VI_ADDP, 1, 2, 0, 2, 2); break;
    case 0xd8: V3(b, VI_UQSUB, 0, 0, 0, 0, 1); break;
    case 0xd9: V3(b, VI_UQSUB, 0, 1, 0, 0, 1); break;
    case 0xdc: V3(b, VI_UQADD, 0, 0, 0, 0, 1); break;
    case 0xdd: V3(b, VI_UQADD, 0, 1, 0, 0, 1); break;
    case 0xe8: V3(b, VI_SQSUB, 0, 0, 0, 0, 1); break;
    case 0xe9: V3(b, VI_SQSUB, 0, 1, 0, 0, 1); break;
    case 0xec: V3(b, VI_SQADD, 0, 0, 0, 0, 1); break;
    case 0xed: V3(b, VI_SQADD, 0, 1, 0, 0, 1); break;
    case 0xf8: V3(b, VI_SUB, 0, 0, 0, 0, 1); break;
    case 0xf9: V3(b, VI_SUB, 0, 1, 0, 0, 1); break;
    case 0xfa: V3(b, VI_SUB, 0, 2, 0, 0, 1); break;
    case 0xfc: V3(b, VI_ADD, 0, 0, 0, 0, 1); break;
    case 0xfd: V3(b, VI_ADD, 0, 1, 0, 0, 1); break;
    case 0xfe: V3(b, VI_ADD, 0, 2, 0, 0, 1); break;
    case 0xdb: V3(b, V_AND, 0, 0, 0, 0, 1); break;
    case 0xdf: V3(b, V_BIC, 0, 0, 0, 1, 0); break;
    case 0xeb: V3(b, V_ORR, 0, 0, 0, 0, 1); break;
    case 0xef: V3(b, V_EOR, 0, 0, 0, 0, 1); break;
    case 0xda: V3(b, VI_UMIN, 0, 0, 0, 0, 1); break;
    case 0xde: V3(b, VI_UMAX, 0, 0, 0, 0, 1); break;
    case 0xea: V3(b, VI_SMIN, 0, 1, 0, 0, 1); break;
    case 0xee: V3(b, VI_SMAX, 0, 1, 0, 0, 1); break;
    case 0xe0: V3(b, VI_URHADD, 0, 0, 0, 0, 1); break;
    case 0xe3: V3(b, VI_URHADD, 0, 1, 0, 0, 1); break;
    case 0xf6: V3(b, VI_UABD, 0, 0, 0, 0, 1); V2(b, VI_UADDLV, 0, 0, 0, 0); break;
    default: return 0;
    }
    mm_st(b, 0, d);
    return 1;
}

int jit_sse(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op & 0xff;
    int d = in->reg;
    int scalar = in->rep == REP_Z;
    amode am;

    if (in->opsz_pfx || in->rep == REP_NZ)
        return -1;                     /* SSE2: not on the Pentium III */
    if (in->op < 0x100)
        return 0;

    if ((op >= 0x60 && op <= 0x7f) || (op >= 0xd1 && op <= 0xfe) ||
        op == 0xc4 || op == 0xc5 || op == 0xd7 || op == 0xe7 || op == 0xf7) {
        if (scalar)
            return -1;
        return gen_mmx(j, ji);
    }

    switch (op) {
    case 0x10:   /* movups / movss load */
        if (scalar) {
            if (in->is_mem) {
                jit_ea(j, in, &am);
                jit_fld(j, FSZ_S, 0, am);    /* upper lanes zeroed */
                xmm_st(b, FSZ_Q, 0, d, 0);
            } else {
                xmm_ld(b, FSZ_S, 0, in->rm, 0);
                xmm_st(b, FSZ_S, 0, d, 0);
            }
        } else {
            ld_xrm(j, in, FSZ_Q, 0);
            xmm_st(b, FSZ_Q, 0, d, 0);
        }
        return 1;
    case 0x11:   /* movups / movss store */
        xmm_ld(b, scalar ? FSZ_S : FSZ_Q, 0, d, 0);
        st_xrm(j, in, scalar ? FSZ_S : FSZ_Q, 0);
        return 1;
    case 0x28:
        ld_xrm(j, in, FSZ_Q, 0);
        xmm_st(b, FSZ_Q, 0, d, 0);
        return 1;
    case 0x29: case 0x2b:
        xmm_ld(b, FSZ_Q, 0, d, 0);
        st_xrm(j, in, FSZ_Q, 0);
        return 1;
    case 0x12:   /* movlps xmm, m64 / movhlps */
        if (in->is_mem)
            ld_xrm(j, in, FSZ_D, 0);
        else
            xmm_ld(b, FSZ_D, 0, in->rm, 8);
        xmm_st(b, FSZ_D, 0, d, 0);
        return 1;
    case 0x13:
        if (!in->is_mem)
            return -1;
        xmm_ld(b, FSZ_D, 0, d, 0);
        st_xrm(j, in, FSZ_D, 0);
        return 1;
    case 0x16:   /* movhps xmm, m64 / movlhps */
        if (in->is_mem)
            ld_xrm(j, in, FSZ_D, 0);
        else
            xmm_ld(b, FSZ_D, 0, in->rm, 0);
        xmm_st(b, FSZ_D, 0, d, 8);
        return 1;
    case 0x17:
        if (!in->is_mem)
            return -1;
        xmm_ld(b, FSZ_D, 0, d, 8);
        st_xrm(j, in, FSZ_D, 0);
        return 1;
    case 0x14: case 0x15:
        xmm_ld(b, FSZ_Q, 0, d, 0);
        ld_xrm(j, in, FSZ_Q, 1);
        V3(b, op == 0x14 ? VI_ZIP1 : VI_ZIP2, 1, 2, 0, 0, 1);
        xmm_st(b, FSZ_Q, 0, d, 0);
        return 1;
    case 0xc6: { /* shufps */
        xmm_ld(b, FSZ_Q, 0, d, 0);
        ld_xrm(j, in, FSZ_Q, 1);
        uint32_t imm = in->imm;
        v_ins_elem(b, 2, 2, 0, 0, (int)imm & 3);
        v_ins_elem(b, 2, 2, 1, 0, (int)(imm >> 2) & 3);
        v_ins_elem(b, 2, 2, 2, 1, (int)(imm >> 4) & 3);
        v_ins_elem(b, 2, 2, 3, 1, (int)(imm >> 6) & 3);
        xmm_st(b, FSZ_Q, 2, d, 0);
        return 1;
    }
    case 0x51: case 0x52: case 0x53:
    case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
    case 0x54: case 0x55: case 0x56: case 0x57: {
        int bitwise = op >= 0x54 && op <= 0x57;
        if (bitwise && scalar)
            return -1;
        if (op == 0x57 && !in->is_mem && in->rm == d) {   /* xorps x, x */
            x_stp(b, ZR, ZR, RCPU, XOFF(d));
            return 1;
        }
        int sz = scalar ? FSZ_S : FSZ_Q;
        int q = 1;
        xmm_ld(b, FSZ_Q, 0, d, 0);
        ld_xrm(j, in, sz, 1);
        int res = 0;
        switch (op) {
        case 0x58: V3(b, VF_ADD, q, 0, 0, 0, 1); break;
        case 0x59: V3(b, VF_MUL, q, 0, 0, 0, 1); break;
        case 0x5c: V3(b, VF_SUB, q, 0, 0, 0, 1); break;
        case 0x5e: V3(b, VF_DIV, q, 0, 0, 0, 1); break;
        case 0x5d: minmax(b, 0, q); res = 2; break;
        case 0x5f: minmax(b, 1, q); res = 2; break;
        case 0x51: V2(b, VF_SQRT, q, 0, 0, 1); break;
        case 0x53:   /* rcp: estimate refined by one Newton step (~16 bits; x86 gives 12) */
            V2(b, VF_RECPE, q, 0, 2, 1);
            V3(b, VF_RECPS, q, 0, 3, 1, 2);
            V3(b, VF_MUL, q, 0, 0, 2, 3);
            break;
        case 0x52:   /* rsqrt */
            V2(b, VF_RSQRTE, q, 0, 2, 1);
            V3(b, VF_MUL, q, 0, 3, 1, 2);
            V3(b, VF_RSQRTS, q, 0, 3, 3, 2);
            V3(b, VF_MUL, q, 0, 0, 2, 3);
            break;
        case 0x54: V3(b, V_AND, 1, 0, 0, 0, 1); break;
        case 0x55: V3(b, V_BIC, 1, 0, 0, 1, 0); break;
        case 0x56: V3(b, V_ORR, 1, 0, 0, 0, 1); break;
        case 0x57: V3(b, V_EOR, 1, 0, 0, 0, 1); break;
        }
        xmm_st(b, sz, res, d, 0);
        return 1;
    }
    case 0xc2:   /* cmpps / cmpss */
        xmm_ld(b, FSZ_Q, 0, d, 0);
        ld_xrm(j, in, scalar ? FSZ_S : FSZ_Q, 1);
        cmp_pred(b, (int)in->imm);
        xmm_st(b, scalar ? FSZ_S : FSZ_Q, 2, d, 0);
        return 1;
    case 0x2e: case 0x2f:   /* ucomiss / comiss */
        xmm_ld(b, FSZ_S, 0, d, 0);
        ld_xrm(j, in, FSZ_S, 1);
        f_cmp(b, 0, 0, 1);
        if (ji->hostfl)
            jit_flags_produced(j, ji);
        else
            jit_nzcv_clobber(j);
        if (ji->store) {
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
        }
        return 1;
    case 0x2a:
        if (scalar) {   /* cvtsi2ss xmm, r/m32 */
            int r;
            if (in->is_mem) {
                jit_ea(j, in, &am);
                jit_ld(j, 4, 1, am);
                r = 1;
            } else {
                r = GREG(in->rm);
            }
            f_scvtf(b, 0, 0, 0, r);
            xmm_st(b, FSZ_S, 0, d, 0);
        } else {        /* cvtpi2ps xmm, mm/m64 */
            ld_mrm(j, in, 0);
            V2(b, VF_SCVTF, 0, 0, 0, 0);
            xmm_st(b, FSZ_D, 0, d, 0);
        }
        return 1;
    case 0x2c: case 0x2d:
        if (scalar) {   /* cvt(t)ss2si r32, xmm/m32 */
            ld_xrm(j, in, FSZ_S, 0);
            cvt_s2i(j, op == 0x2c, GREG(d));
        } else {        /* cvt(t)ps2pi mm, xmm/m64 */
            ld_xrm(j, in, FSZ_D, 0);
            V2(b, op == 0x2c ? VF_FCVTZS : VF_FCVTNS, 0, 0, 0, 0);
            mm_st(b, 0, d);
        }
        return 1;
    case 0x50:   /* movmskps */
        if (in->is_mem)
            return -1;
        x_ldp(b, 1, 2, RCPU, XOFF(in->rm));      /* lanes 0,1 in x1; 2,3 in x2 */
        x_ubfx(b, 0, 1, 31, 1);
        x_lsri(b, 3, 1, 63);
        w_orr_sh(b, 0, 0, 3, SH_LSL, 1);
        x_ubfx(b, 3, 2, 31, 1);
        w_orr_sh(b, 0, 0, 3, SH_LSL, 2);
        x_lsri(b, 3, 2, 63);
        w_orr_sh(b, GREG(d), 0, 3, SH_LSL, 3);
        return 1;
    case 0xae:
        if (!in->is_mem)
            return 1;                  /* sfence / lfence / mfence: single core, nothing to order */
        jit_ea(j, in, &am);
        switch (in->reg) {
        case 0: case 1:
            w_addimm(b, 2, am.base, (uint32_t)am.off, 3);
            w_movi(b, 1, in->reg == 0 ? SSEH_FXSAVE : SSEH_FXRSTOR);
            jit_call_helper(j, H_SSE, 1);
            return 1;
        case 2:
            jit_ld(j, 4, 0, am);
            w_str(b, 0, RCPU, CPUOFF(mxcsr));
            return 1;
        case 3:
            w_ldr(b, 0, RCPU, CPUOFF(mxcsr));
            jit_st(j, 4, 0, am);
            return 1;
        }
        return -1;
    }
    return 0;
}

static void sse_helper(xbt_cpu *cpu, uint32_t op, uint32_t addr)
{
    switch (op) {
    case SSEH_FXSAVE: xbt_fxsave(cpu, (uint8_t *)(uintptr_t)addr); break;
    case SSEH_FXRSTOR: xbt_fxrstor(cpu, (const uint8_t *)(uintptr_t)addr); break;
    }
}

void *xbt_sse_helper = (void *)sse_helper;
