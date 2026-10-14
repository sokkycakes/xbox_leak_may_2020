/*
 * translate.c - block translator core: decoding, flag analysis, operand
 * access, control flow, exits and block linking.
 *
 * A block is a run of guest instructions ending at a control transfer. It is
 * translated in two passes:
 *   1. analysis: decode, then a backward liveness pass over the six status
 *      flags and a forward pass that pairs every flag consumer with its
 *      producer. Producers whose flags are never read emit no flag code.
 *      Consumers next to their producer read the host NZCV register directly
 *      (cmp + jl becomes cmp + b.lt). Only flags that escape the block are
 *      stored as a lazy record.
 *   2. code generation, one guest instruction at a time.
 */
#include <string.h>
#include "jit.h"

xbt_options xbt_opt = {
    .abi_flags = 1,
    .link_blocks = 1,
    .flag_fusion = 1,
    .flag_elim = 1,
    .max_insns = 200,
};
xbt_stats xbt_st;
xbt_stubs xbt_stub;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

int jit_szidx(int size) { return size == 1 ? 0 : size == 2 ? 1 : 2; }

static uint8_t cc_flags(int cc)
{
    switch (cc >> 1) {
    case 0: return FOF;
    case 1: return FCF;
    case 2: return FZF;
    case 3: return FCF | FZF;
    case 4: return FSF;
    case 5: return FPF;
    case 6: return FSF | FOF;
    default: return FZF | FSF | FOF;
    }
}

/* Map an x86 condition to a host condition given how NZCV encodes the flags. */
static int map_cond(int mode, int cc)
{
    static const int8_t sub[16] = { CC_VS, CC_VC, CC_LO, CC_HS, CC_EQ, CC_NE, CC_LS, CC_HI,
                                    CC_MI, CC_PL, -1, -1, CC_LT, CC_GE, CC_LE, CC_GT };
    static const int8_t add[16] = { CC_VS, CC_VC, CC_HS, CC_LO, CC_EQ, CC_NE, -1, -1,
                                    CC_MI, CC_PL, -1, -1, CC_LT, CC_GE, CC_LE, CC_GT };
    static const int8_t logic[16] = { CC_VS, CC_VC, CC_HS, CC_LO, CC_EQ, CC_NE, CC_EQ, CC_NE,
                                      CC_MI, CC_PL, -1, -1, CC_LT, CC_GE, CC_LE, CC_GT };
    static const int8_t inc[16] = { CC_VS, CC_VC, -1, -1, CC_EQ, CC_NE, -1, -1,
                                    CC_MI, CC_PL, -1, -1, CC_LT, CC_GE, CC_LE, CC_GT };
    static const int8_t sahf[16] = { -1, -1, CC_LO, CC_HS, CC_EQ, CC_NE, CC_LS, CC_HI,
                                     CC_MI, CC_PL, CC_VS, CC_VC, -1, -1, -1, -1 };
    static const int8_t fcmp[16] = { -1, -1, CC_LT, CC_GE, HC_EQ_OR_VS, HC_NE_AND_VC, CC_LE, CC_GT,
                                     -1, -1, CC_VS, CC_VC, -1, -1, -1, -1 };
    switch (mode) {
    case NM_SUB: return sub[cc];
    case NM_ADD: return add[cc];
    case NM_LOGIC: return logic[cc];
    case NM_INC: return inc[cc];
    case NM_SAHF: return sahf[cc];
    case NM_FCMP: return fcmp[cc];
    default: return HC_INVALID;
    }
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

/* Value of guest register r (size 1/2/4), zero-extended. 8-bit r: AL..BH. */
int jit_get_reg(jit_t *j, int size, int r, int tmp)
{
    a64buf *b = JB(j);
    if (size == 4)
        return GREG(r);
    if (size == 2) {
        w_uxth(b, tmp, GREG(r));
        return tmp;
    }
    if (r < 4) {
        w_uxtb(b, tmp, GREG(r));
    } else {
        w_ubfx(b, tmp, GREG(r - 4), 8, 8);
    }
    return tmp;
}

/* Like jit_get_reg but the bits above `size` may be garbage. */
int jit_get_reg_dirty(jit_t *j, int size, int r, int tmp)
{
    if (size == 4 || size == 2 || r < 4)
        return GREG(size == 1 && r >= 4 ? r - 4 : r);
    w_lsri(JB(j), tmp, GREG(r - 4), 8);
    return tmp;
}

void jit_set_reg(jit_t *j, int size, int r, int src)
{
    a64buf *b = JB(j);
    if (size == 4) {
        if (src != GREG(r))
            w_mov(b, GREG(r), src);
    } else if (size == 2) {
        w_bfi(b, GREG(r), src, 0, 16);
    } else if (r < 4) {
        w_bfi(b, GREG(r), src, 0, 8);
    } else {
        w_bfi(b, GREG(r - 4), src, 8, 8);
    }
}

/* ------------------------------------------------------------------ */
/* Effective addresses and memory access                               */
/* ------------------------------------------------------------------ */

static int adrp_ok(jit_t *j, uint32_t target)
{
    intptr_t pc_page = (intptr_t)j->b.p & ~0xfffl;
    intptr_t t_page = (intptr_t)target & ~0xfffl;
    intptr_t d = t_page - pc_page;
    return d >= -(1l << 32) && d < (1l << 32);
}

void jit_ea(jit_t *j, const x86_insn *in, amode *am)
{
    a64buf *b = JB(j);
    const x86_mem *m = &in->mem;
    uint32_t disp = (uint32_t)m->disp;

    if (m->seg == SEG_FS) {
        /* fs:[...] -> fs_base + ea, always 32-bit wrapped */
        w_ldr(b, RB, RCPU, CPUOFF(fs_base));
        if (m->base >= 0 && m->index >= 0) {
            w_add_sh(b, RA, GREG(m->base), GREG(m->index), SH_LSL, m->scale);
            w_add(b, RA, RA, RB);
        } else if (m->base >= 0) {
            w_add(b, RA, RB, GREG(m->base));
        } else if (m->index >= 0) {
            w_add_sh(b, RA, RB, GREG(m->index), SH_LSL, m->scale);
        } else {
            am->base = RB;
            am->off = (int32_t)disp;
            if ((int32_t)disp < 0 || disp > 4095) {
                w_addimm(b, RA, RB, disp, RA);
                am->base = RA;
                am->off = 0;
            }
            return;
        }
        w_addimm(b, RA, RA, disp, RB);
        am->base = RA;
        am->off = 0;
        return;
    }

    if (m->base >= 0 && m->index < 0) {
        /* [base + disp]: the guest register is the host base */
        am->base = GREG(m->base);
        am->off = m->disp;
        if (m->disp < -256 || m->disp > 16380) {
            w_addimm(b, RA, GREG(m->base), disp, RB);
            am->base = RA;
            am->off = 0;
        }
        return;
    }
    if (m->index >= 0) {
        if (m->base >= 0) {
            /* small displacements stay in the load: base + index*s is already
               a wrapped 32-bit pointer; the sum is reused while it is valid */
            if (m->disp >= -256 && m->disp <= 4095) {
                if (!(j->idx_valid && j->idx_base == m->base && j->idx_index == m->index &&
                      j->idx_scale == (int8_t)m->scale)) {
                    w_add_sh(b, RIDX, GREG(m->base), GREG(m->index), SH_LSL, m->scale);
                    j->idx_valid = 1;
                    j->idx_base = m->base;
                    j->idx_index = m->index;
                    j->idx_scale = (int8_t)m->scale;
                }
                am->base = RIDX;
                am->off = m->disp;
                return;
            }
            w_add_sh(b, RA, GREG(m->base), GREG(m->index), SH_LSL, m->scale);
            w_addimm(b, RA, RA, disp, RB);
        } else {
            if (disp) {
                w_movi(b, RA, disp);
                w_add_sh(b, RA, RA, GREG(m->index), SH_LSL, m->scale);
            } else {
                w_lsli(b, RA, GREG(m->index), m->scale);
            }
        }
        am->base = RA;
        am->off = 0;
        return;
    }
    /* absolute address: one adrp per 4K page while it stays valid */
    if (j->abs_valid && j->abs_page == (disp & ~0xfffu)) {
        am->base = RPAGE;
        am->off = (int32_t)(disp & 0xfff);
    } else if (adrp_ok(j, disp)) {
        intptr_t pc_page = (intptr_t)j->b.p & ~0xfffl;
        intptr_t d = ((intptr_t)disp & ~0xfffl) - pc_page;
        a64_put(b, enc_adrp(RPAGE, d >> 12));
        j->abs_valid = 1;
        j->abs_page = disp & ~0xfffu;
        am->base = RPAGE;
        am->off = (int32_t)(disp & 0xfff);
    } else {
        w_movi(b, RA, disp);
        am->base = RA;
        am->off = 0;
    }
}

/* Make sure the address does not depend on a guest register (it may change). */
void jit_ea_pin(jit_t *j, amode *am)
{
    if ((am->base >= GREG(0) && am->base <= GREG(7)) || am->base == RIDX) {
        w_addimm(JB(j), RA, am->base, (uint32_t)am->off, RB);
        am->base = RA;
        am->off = 0;
    }
}

static void fix_amode(jit_t *j, amode *am, int size_log2)
{
    int32_t off = am->off;
    int ok = (off >= 0 && (off & ((1 << size_log2) - 1)) == 0 && (off >> size_log2) < 4096) ||
             (off >= -256 && off < 256);
    if (!ok) {
        w_addimm(JB(j), RB, am->base, (uint32_t)off, RB);
        am->base = RB;
        am->off = 0;
    }
}

static int log2sz(int size) { return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : size == 8 ? 3 : 4; }

void jit_ld(jit_t *j, int size, int rt, amode am)
{
    int l = log2sz(size);
    fix_amode(j, &am, l);
    a64_ldst_off(JB(j), l, LDST_LDR, rt, am.base, am.off);
}

void jit_ld_sx(jit_t *j, int size, int rt, amode am)
{
    int l = log2sz(size);
    fix_amode(j, &am, l);
    a64_ldst_off(JB(j), l, size == 4 ? LDST_LDR : LDST_LDRSW, rt, am.base, am.off);
}

void jit_st(jit_t *j, int size, int rt, amode am)
{
    int l = log2sz(size);
    fix_amode(j, &am, l);
    a64_ldst_off(JB(j), l, LDST_STR, rt, am.base, am.off);
}

void jit_fld(jit_t *j, int fsz, int vt, amode am)
{
    fix_amode(j, &am, fsz);
    a64_fldst_off(JB(j), fsz, 1, vt, am.base, am.off);
}

void jit_fst(jit_t *j, int fsz, int vt, amode am)
{
    fix_amode(j, &am, fsz);
    a64_fldst_off(JB(j), fsz, 0, vt, am.base, am.off);
}

int jit_ld_rm(jit_t *j, const x86_insn *in, int size, int tmp, amode *am)
{
    if (!in->is_mem)
        return jit_get_reg(j, size, in->rm, tmp);
    jit_ea(j, in, am);
    jit_ld(j, size, tmp, *am);
    return tmp;
}

int jit_ld_rm_dirty(jit_t *j, const x86_insn *in, int size, int tmp, amode *am)
{
    if (!in->is_mem)
        return jit_get_reg_dirty(j, size, in->rm, tmp);
    jit_ea(j, in, am);
    jit_ld(j, size, tmp, *am);
    return tmp;
}

void jit_st_rm(jit_t *j, const x86_insn *in, int size, int src, const amode *am)
{
    if (!in->is_mem)
        jit_set_reg(j, size, in->rm, src);
    else
        jit_st(j, size, src, *am);
}

/* ------------------------------------------------------------------ */
/* Helper calls and exits                                              */
/* ------------------------------------------------------------------ */

static void spill_regs(a64buf *b)
{
    for (int r = 0; r < 8; r += 2)
        w_stp(b, GREG(r), GREG(r + 1), RCPU, CPUOFF(r) + 4 * r);
    w_str(b, RTOP, RCPU, CPUOFF(ftop));
}

static void reload_regs(a64buf *b)
{
    for (int r = 0; r < 8; r += 2)
        w_ldp(b, GREG(r), GREG(r + 1), RCPU, CPUOFF(r) + 4 * r);
    w_ldr(b, RTOP, RCPU, CPUOFF(ftop));
}

void jit_nzcv_clobber(jit_t *j) { j->nzcv_owner = -1; }

void jit_caches_flush(jit_t *j)
{
    jit_x87_flush(j, 1);
    jit_xmm_flush(j, 1);
}

void jit_caches_reset(jit_t *j)
{
    for (int i = 0; i < 8; i++) {
        j->x87_reg[i] = -1;
        j->x87_dirty[i] = 0;
    }
    j->x87_delta = 0;
    j->xmm_cached = j->xmm_dirty = 0;
    j->abs_valid = j->idx_valid = 0;
}

/* Instructions that never write a general register (x87 except fnstsw ax,
   SSE/MMX arithmetic and moves): the cached base + index stays valid. */
static int keeps_gprs(const x86_insn *in)
{
    int op = in->op;
    if (op >= 0xd8 && op <= 0xdf)
        return !(op == 0xdf && !in->is_mem && in->reg == 4);
    if (op < 0x100)
        return 0;
    op &= 0xff;
    if ((op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2b) || op == 0x2e || op == 0x2f ||
        (op >= 0x51 && op <= 0x5f) || op == 0xc2 || op == 0xc6)
        return 1;
    if (op == 0x2c || op == 0x2d)
        return !(in->rep == REP_Z);      /* cvt(t)ss2si writes a GPR */
    if ((op >= 0x60 && op <= 0x6f) || (op >= 0x70 && op <= 0x77) || op == 0x7f ||
        (op >= 0xd1 && op <= 0xfe && op != 0xd7))
        return 1;
    return 0;
}

/* Call cpu->helpers[h](cpu, x1..x7). Arguments other than x0 must already be set. */
void jit_call_helper(jit_t *j, int h, int spill)
{
    a64buf *b = JB(j);
    /* v16+ are caller-saved: the xmm cache never survives a call; the x87
       cache (d8..d15) does, unless the helper works on the x87 state */
    jit_xmm_flush(j, 1);
    if (h == H_X87 || h == H_X87_MEM || h == H_SSE)
        jit_x87_flush(j, 1);
    j->abs_valid = j->idx_valid = 0;     /* x12/x13 are caller-saved */
    if (spill)
        spill_regs(b);
    x_mov(b, 0, RCPU);
    x_ldr(b, 9, RCPU, CPUOFF(helpers) + 8 * h);
    a64_blr(b, 9);
    if (spill)
        reload_regs(b);
    jit_nzcv_clobber(j);
}

void jit_stub(jit_t *j, uint32_t *site, int type, uint32_t eip, int reason, uint32_t info)
{
    if (j->nst >= JIT_MAX_STUBS) {
        j->b.overflow = 1;
        return;
    }
    jstub *s = &j->st[j->nst++];
    s->site = site;
    s->type = (uint8_t)type;
    s->eip = eip;
    s->reason = (uint8_t)reason;
    s->info = info;
}

/* Unconditionally leave the block with a special exit reason. */
void jit_exit_special(jit_t *j, uint32_t eip, int reason, uint32_t info)
{
    jit_caches_flush(j);
    uint32_t *site = a64_here(JB(j));
    a64_put(JB(j), enc_b(0));
    jit_stub(j, site, SITE_B, eip, reason, info);
}

static void add_exit(jit_t *j, uint32_t target)
{
    if (j->nex >= JIT_MAX_EXITS) {
        j->b.overflow = 1;
        return;
    }
    j->ex[j->nex].site = a64_here(JB(j));
    j->ex[j->nex].target = target;
    j->nex++;
    a64_put(JB(j), enc_b(0));
}

/* Direct branch to a guest address (linked later). Backward branches poll
   cpu->pending so that loops can be interrupted. */
static void emit_direct(jit_t *j, uint32_t target, uint32_t from)
{
    a64buf *b = JB(j);
    if (target <= from) {
        w_ldrb(b, RA, RCPU, CPUOFF(pending));
        uint32_t *site = a64_here(b);
        a64_put(b, enc_cbz(0, 1, RA, 0));
        jit_stub(j, site, SITE_CBZ, target, EXIT_PENDING, 0);
    }
    add_exit(j, target);
}

/* Jump to the guest address held in w0 through the lookup table. */
static void emit_indirect(jit_t *j)
{
    a64buf *b = JB(j);
    w_lsri(b, RA, 0, 27);                 /* XBT_JTAB_RANGE == 1 << 27 */
    uint32_t *c1 = a64_here(b);
    a64_put(b, enc_cbz(0, 1, RA, 0));
    x_movi(b, RB, XBT_JTAB_BASE);
    ldst_reg(b, 2, LDST_LDR, RA, RB, 0, EX_UXTW, 1);
    uint32_t *c2 = a64_here(b);
    a64_put(b, enc_cbz(0, 0, RA, 0));
    a64_br(b, RA);
    uint32_t *miss = a64_here(b);
    a64_patch_cbz(c1, miss);
    a64_patch_cbz(c2, miss);
    a64_put(b, enc_b((intptr_t)xbt_stub.indirect_miss - (intptr_t)miss));
}

/* ------------------------------------------------------------------ */
/* Flags framework                                                     */
/* ------------------------------------------------------------------ */

void jit_flags_record(jit_t *j, jinsn *ji, int kind, int size, int r_res, int r_a, int r_b)
{
    if (!ji->store)
        return;
    a64buf *b = JB(j);
    ji->rec_kind = (uint8_t)kind;
    ji->rec_size = (uint8_t)size;
    w_movi(b, 15, LF_OP(kind, jit_szidx(size)));
    w_stp(b, 15, r_res, RCPU, CPUOFF(lf_op));
    if (r_a >= 0 && r_b >= 0)
        w_stp(b, r_a, r_b, RCPU, CPUOFF(lf_a));
    else if (r_b >= 0)
        w_str(b, r_b, RCPU, CPUOFF(lf_b));
    else if (r_a >= 0)
        w_str(b, r_a, RCPU, CPUOFF(lf_a));
}

/* Called by a producer right after the host instruction that set NZCV. */
void jit_flags_produced(jit_t *j, jinsn *ji)
{
    if (ji->hostfl)
        j->nzcv_owner = j->cur;
}

void jit_sync(jit_t *j)
{
    jit_call_helper(j, H_FLAGS_SYNC, 0);
}

static int replayable(int kind)
{
    return kind == LFK_ADD || kind == LFK_SUB || kind == LFK_LOGIC ||
           kind == LFK_INC || kind == LFK_DEC;
}

/* Recreate NZCV from the stored record of a known producer. Returns the NZCV
   mode, or NM_NONE if not possible. For cc P/NP returns -2 with the parity
   flag tested (Z clear = PF set). */
static int replay(jit_t *j, jinsn *p, int cc)
{
    a64buf *b = JB(j);
    int kind = p->rec_kind, size = p->rec_size;
    if (!replayable(kind))
        return NM_NONE;
    int sh = 32 - 8 * size;
    if ((cc >> 1) == 5) {
        w_ldrb(b, 0, RCPU, CPUOFF(lf_res));
        x_addi(b, 1, RCPU, CPUOFF(parity));
        ldst_reg(b, 0, LDST_LDR, 0, 1, 0, EX_UXTW, 0);
        w_cmpi(b, 0, 0);
        return -2;
    }
    w_ldr(b, 0, RCPU, CPUOFF(lf_res));
    switch (kind) {
    case LFK_SUB:
        w_ldr(b, 1, RCPU, CPUOFF(lf_b));
        w_add(b, 2, 0, 1);
        if (sh) {
            w_lsli(b, 2, 2, sh);
            w_cmp_sh(b, 2, 1, SH_LSL, sh);
        } else {
            w_cmp(b, 2, 1);
        }
        return NM_SUB;
    case LFK_ADD:
        w_ldr(b, 1, RCPU, CPUOFF(lf_b));
        w_sub(b, 2, 0, 1);
        if (sh) {
            w_lsli(b, 2, 2, sh);
            a64_put(b, enc_addsub_sreg(0, 0, 1, ZR, 2, 1, SH_LSL, sh));
        } else {
            w_cmn(b, 2, 1);
        }
        return NM_ADD;
    case LFK_LOGIC:
        a64_put(b, enc_addsub_sreg(0, 0, 1, ZR, ZR, 0, SH_LSL, sh));
        return NM_LOGIC;
    case LFK_INC:
    case LFK_DEC:
        if (kind == LFK_INC)
            w_subi(b, 2, 0, 1);
        else
            w_addi(b, 2, 0, 1);
        w_lsli(b, 2, 2, sh);
        w_movi(b, 3, 1u << sh);
        if (kind == LFK_INC)
            w_cmn(b, 2, 3);
        else
            w_cmp(b, 2, 3);
        return NM_INC;
    }
    return NM_NONE;
}

/* Evaluate condition cc for consumer ji. Returns a host condition valid in
   NZCV right now (HC_* compound conditions included). May call a helper, so
   call it before loading operands into scratch registers. */
int jit_cond(jit_t *j, jinsn *ji, int cc)
{
    a64buf *b = JB(j);
    xbt_st.consumers++;
    if (ji->fuse >= 0) {
        if (j->nzcv_owner != ji->fuse) {
            XBT_LOG("internal error: flag fusion broken at %08x (owner %d, producer %d)\n",
                    ji->in.addr, j->nzcv_owner, ji->fuse);
            abort();
        }
        int hc = map_cond(j->ji[ji->fuse].mode, cc);
        if (hc != HC_INVALID) {
            xbt_st.consumers_fused++;
            return hc;
        }
    }
    if (ji->replay && ji->single >= 0) {
        int m = replay(j, &j->ji[ji->single], cc);
        jit_nzcv_clobber(j);
        if (m == -2)
            return (cc & 1) ? CC_EQ : CC_NE;
        if (m != NM_NONE) {
            int hc = map_cond(m, cc);
            if (hc != HC_INVALID)
                return hc;
        }
    }
    xbt_st.consumers_helper++;
    w_movi(b, 1, (uint32_t)cc);
    jit_call_helper(j, H_COND, 0);
    w_cmpi(b, 0, 0);
    return CC_NE;
}

void jit_cond_reg(jit_t *j, jinsn *ji, int cc, int rd)
{
    a64buf *b = JB(j);
    int hc = jit_cond(j, ji, cc);
    if (hc == HC_EQ_OR_VS) {
        w_cset(b, rd, CC_EQ);
        w_csinc(b, rd, rd, ZR, CC_VC);
    } else if (hc == HC_NE_AND_VC) {
        w_cset(b, rd, CC_NE);
        w_csel(b, rd, rd, ZR, CC_VC);
    } else {
        w_cset(b, rd, hc);
    }
}

/* Put x86 CF into host C for adc (sbc = 0) or !CF for sbc (sbc = 1). */
void jit_carry_in(jit_t *j, jinsn *ji, int sbc)
{
    a64buf *b = JB(j);
    if (ji->fuse >= 0 && j->nzcv_owner == ji->fuse) {
        int hc = map_cond(j->ji[ji->fuse].mode, 2);
        xbt_st.consumers++;
        xbt_st.consumers_fused++;
        if (!sbc && hc == CC_HS)
            return;
        if (sbc && hc == CC_LO)
            return;
        w_cset(b, 3, hc);
    } else {
        jit_cond_reg(j, ji, 2, 3);
    }
    if (sbc)
        w_cmp(b, ZR, 3);
    else
        w_cmpi(b, 3, 1);
    jit_nzcv_clobber(j);
}

/* Emit a two-way branch on host condition hc. *taken and *fall receive the
   addresses of `b` instructions for the two outcomes (to be patched). */
void jit_bcond(jit_t *j, int hc, uint32_t **taken, uint32_t **fall)
{
    a64buf *b = JB(j);
    uint32_t *s1, *s2 = NULL;
    if (hc == HC_EQ_OR_VS) {
        s1 = a64_here(b); a64_put(b, enc_bcond(0, CC_EQ));
        s2 = a64_here(b); a64_put(b, enc_bcond(0, CC_VS));
        *fall = a64_here(b); a64_put(b, enc_b(0));
        *taken = a64_here(b); a64_put(b, enc_b(0));
        a64_patch_bcond(s1, *taken);
        a64_patch_bcond(s2, *taken);
    } else if (hc == HC_NE_AND_VC) {
        s1 = a64_here(b); a64_put(b, enc_bcond(0, CC_EQ));
        s2 = a64_here(b); a64_put(b, enc_bcond(0, CC_VS));
        *taken = a64_here(b); a64_put(b, enc_b(0));
        *fall = a64_here(b); a64_put(b, enc_b(0));
        a64_patch_bcond(s1, *fall);
        a64_patch_bcond(s2, *fall);
    } else {
        s1 = a64_here(b); a64_put(b, enc_bcond(0, cc_inv(hc)));
        *taken = a64_here(b); a64_put(b, enc_b(0));
        *fall = a64_here(b); a64_put(b, enc_b(0));
        a64_patch_bcond(s1, *fall);
    }
}

/* ------------------------------------------------------------------ */
/* Instruction classification for the flag analysis                    */
/* ------------------------------------------------------------------ */

static void alu_class(jinsn *ji, int alu, int size)
{
    ji->wr = FALL;
    ji->clobber = 0;
    switch (alu) {
    case 0: ji->mode = NM_ADD; break;
    case 5: case 7: ji->mode = NM_SUB; break;
    case 1: case 4: case 6: ji->mode = NM_LOGIC; break;
    case 2: ji->rd = FCF; ji->consumer = CK_CARRY; ji->cc = 2;
            ji->mode = size == 4 ? NM_ADD : NM_NONE; break;
    case 3: ji->rd = FCF; ji->consumer = CK_CARRY; ji->cc = 2;
            ji->mode = size == 4 ? NM_SUB : NM_NONE; break;
    }
}

static int is_terminator(const x86_insn *in)
{
    int op = in->op;
    if (in->invalid)
        return 1;
    if ((op >= 0x70 && op <= 0x7f) || (op >= 0x180 && op <= 0x18f))
        return 1;
    switch (op) {
    case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0x9a:
    case 0xc2: case 0xc3: case 0xca: case 0xcb: case 0xcf:
    case 0xe0: case 0xe1: case 0xe2: case 0xe3:
    case 0xcc: case 0xcd: case 0xce: case 0xf4: case 0x10b:
    case 0xf1:
        return 1;
    case 0xff:
        return in->reg >= 2 && in->reg <= 5;
    }
    return 0;
}

static void classify(jinsn *ji)
{
    x86_insn *in = &ji->in;
    int op = in->op, reg = in->reg;
    int osz = in->opsize;
    ji->rd = ji->wr = 0;
    ji->mode = NM_NONE;
    ji->clobber = 1;
    ji->consumer = CK_NONE;
    ji->cond_wr = 0;
    ji->cc = 0;
    if (in->invalid)
        return;

    if (op < 0x40 && (op & 7) < 6) {
        alu_class(ji, op >> 3, (op & 1) ? osz : 1);
        return;
    }
    if (op >= 0x40 && op <= 0x4f) {
        ji->wr = FALL & ~FCF;
        ji->mode = NM_INC;
        ji->clobber = 0;
        return;
    }
    if ((op >= 0x70 && op <= 0x7f) || (op >= 0x180 && op <= 0x18f) ||
        (op >= 0x190 && op <= 0x19f) || (op >= 0x140 && op <= 0x14f)) {
        ji->cc = op & 15;
        ji->rd = cc_flags(ji->cc);
        ji->consumer = CK_COND;
        ji->clobber = 0;
        return;
    }
    if (op >= 0xd8 && op <= 0xdf) {
        jit_x87_classify(ji);
        return;
    }
    switch (op) {
    case 0x80: case 0x81: case 0x82: case 0x83:
        alu_class(ji, reg, (op & 1) ? osz : 1);
        return;
    case 0x84: case 0x85: case 0xa8: case 0xa9:
        alu_class(ji, 4, 0);
        return;
    case 0xf6: case 0xf7:
        switch (reg) {
        case 0: case 1: alu_class(ji, 4, 0); return;
        case 2: ji->clobber = 0; return;                        /* not */
        case 3: ji->wr = FALL; ji->mode = NM_SUB; ji->clobber = 0; return; /* neg */
        default: ji->wr = FALL; ji->clobber = 0; return;        /* mul/imul/div/idiv */
        }
    case 0xfe: case 0xff:
        if (reg <= 1) {
            ji->wr = FALL & ~FCF;
            ji->mode = NM_INC;
        }
        ji->clobber = 0;
        return;
    case 0x69: case 0x6b: case 0x1af:
        ji->wr = FALL;
        ji->clobber = 0;
        return;
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        int bycl = op == 0xd2 || op == 0xd3;
        unsigned cnt = (op == 0xd0 || op == 0xd1) ? 1 : (in->imm & 31);
        ji->clobber = 0;
        if (reg == 2 || reg == 3) {        /* rcl/rcr: helper */
            ji->clobber = 1;
            ji->rd = FALL;
            ji->wr = FALL;
            ji->consumer = CK_OTHER;
            return;
        }
        if (!bycl && cnt == 0)
            return;
        ji->wr = (reg == 0 || reg == 1) ? (FCF | FOF) : FALL;
        if (bycl) {
            ji->cond_wr = 1;
            ji->rd = ji->wr;
        }
        return;
    }
    case 0x1a4: case 0x1ac:
        ji->clobber = 0;
        if ((in->imm & 31) == 0)
            return;
        ji->wr = FALL;
        return;
    case 0x1a5: case 0x1ad:
        ji->clobber = 0;
        ji->wr = FALL;
        ji->rd = FALL;
        ji->cond_wr = 1;
        return;
    case 0x1a3: case 0x1ab: case 0x1b3: case 0x1bb: case 0x1ba:
        ji->wr = FALL & ~FZF;
        ji->clobber = 0;
        return;
    case 0x1bc: case 0x1bd:
        ji->wr = FALL;
        ji->clobber = 0;
        return;
    case 0xe0: case 0xe1:
        ji->cc = op == 0xe1 ? 4 : 5;
        ji->rd = FZF;
        ji->consumer = CK_COND;
        ji->clobber = 0;
        return;
    case 0x1b0: case 0x1b1:
        ji->wr = FALL;
        ji->mode = NM_SUB;
        ji->clobber = 1;
        return;
    case 0x1c0: case 0x1c1:
        ji->wr = FALL;
        ji->mode = NM_ADD;
        ji->clobber = 0;
        return;
    case 0xa6: case 0xa7: case 0xae: case 0xaf:
        ji->wr = FALL;
        if (in->rep) {
            ji->rd = FALL;
            ji->cond_wr = 1;
        }
        ji->clobber = 1;
        return;
    case 0x9c:
        ji->rd = FALL;
        ji->consumer = CK_OTHER;
        return;
    case 0x9d:
        ji->wr = FALL;
        return;
    case 0x9e:
        ji->wr = FALL & ~FOF;
        ji->mode = NM_SAHF;
        ji->clobber = 0;
        return;
    case 0x9f:
        ji->rd = FALL;
        ji->consumer = CK_OTHER;
        return;
    case 0xf5:
        ji->rd = FCF;
        ji->wr = FCF;
        ji->consumer = CK_OTHER;
        return;
    case 0xf8: case 0xf9:
        ji->wr = FCF;
        return;
    case 0xd6:
        ji->cc = 2;
        ji->rd = FCF;
        ji->consumer = CK_COND;
        ji->clobber = 0;
        return;
    case 0x27: case 0x2f: case 0x37: case 0x3f:
        ji->rd = FALL;
        ji->wr = FALL;
        ji->consumer = CK_OTHER;
        return;
    case 0xd4: case 0xd5:
        ji->wr = FALL;
        return;
    case 0xce: case 0xcc: case 0xcd: case 0xf4: case 0xcf:
        ji->rd = FALL;
        ji->consumer = CK_OTHER;
        return;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
    case 0x60: case 0x61: case 0x68: case 0x6a: case 0x8f:
    case 0x86: case 0x87: case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9b:
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5:
    case 0xaa: case 0xab: case 0xac: case 0xad:
    case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
    case 0xc2: case 0xc3: case 0xc6: case 0xc7: case 0xc8: case 0xc9:
    case 0xd7: case 0xe2: case 0xe3: case 0xe8: case 0xe9: case 0xeb:
    case 0xfa: case 0xfb: case 0xfc: case 0xfd:
    case 0x1b6: case 0x1b7: case 0x1be: case 0x1bf:
    case 0x1c8: case 0x1c9: case 0x1ca: case 0x1cb: case 0x1cc: case 0x1cd: case 0x1ce: case 0x1cf:
    case 0x10d: case 0x118: case 0x119: case 0x11a: case 0x11b: case 0x11c: case 0x11d:
    case 0x11e: case 0x11f: case 0x1a0: case 0x1a1:
        ji->clobber = 0;
        return;
    }
    if (op >= 0x100)
        jit_sse_classify(ji);
}

/* Flags read by a successor block before it overwrites them. */
static uint8_t peek_live(uint32_t addr, int depth)
{
    uint8_t live = 0, killed = 0;
    /* long enough to get through x87/SSE loop bodies, which write no flags */
    for (int i = 0; i < 96; i++) {
        if (!mem_readable(addr, 1) || (addr >= XBOX_HLE_BASE && addr < XBOX_HLE_BASE + XBOX_HLE_SIZE))
            return live | (FALL & ~killed);
        int avail = mem_readable(addr, 15) ? 15 : 1;
        jinsn tmp;
        if (!x86_decode(&tmp.in, addr, (const uint8_t *)(uintptr_t)addr, avail))
            return live | (FALL & ~killed);
        classify(&tmp);
        live |= tmp.rd & ~killed;
        if (!tmp.cond_wr)
            killed |= tmp.wr;
        if (killed == FALL)
            return live;
        if (is_terminator(&tmp.in)) {
            int op = tmp.in.op;
            if (xbt_opt.abi_flags && (op == 0xe8 || op == 0xc3 || op == 0xc2 ||
                                      (op == 0xff && (tmp.in.reg == 2 || tmp.in.reg == 3))))
                return live;
            if (depth > 0 && (op == 0xe9 || op == 0xeb))
                return live | (peek_live(tmp.in.target, depth - 1) & ~killed);
            return live | (FALL & ~killed);
        }
        addr += tmp.in.len;
    }
    return live | (FALL & ~killed);
}

static uint8_t block_live_out(jit_t *j)
{
    if (!xbt_opt.flag_elim)
        return FALL;
    jinsn *last = &j->ji[j->n - 1];
    x86_insn *in = &last->in;
    int op = in->op;
    if (!j->ended)
        return peek_live(j->end_eip, 1);
    if (in->invalid)
        return FALL;
    if ((op >= 0x70 && op <= 0x7f) || (op >= 0x180 && op <= 0x18f) || (op >= 0xe0 && op <= 0xe3))
        return peek_live(in->target, 1) | peek_live(j->end_eip, 1);
    if (op == 0xe9 || op == 0xeb)
        return peek_live(in->target, 1);
    if (xbt_opt.abi_flags) {
        if (op == 0xe8 || op == 0xc2 || op == 0xc3)
            return 0;
        if (op == 0xff && (in->reg == 2 || in->reg == 3))
            return 0;
    }
    return FALL;
}

static void analyze(jit_t *j)
{
    int n = j->n;
    uint8_t live_out = block_live_out(j);
    uint8_t live = live_out;
    for (int i = n - 1; i >= 0; i--) {
        jinsn *ji = &j->ji[i];
        ji->live_after = xbt_opt.flag_elim ? live : FALL;
        live = (live & ~(ji->cond_wr ? 0 : ji->wr)) | ji->rd;
        ji->fuse = -1;
        ji->single = -1;
        ji->hostfl = ji->store = ji->sync = ji->replay = 0;
        ji->rec_kind = 0;
        ji->need = ji->wr & ji->live_after;
    }

    int16_t lw[6];
    for (int f = 0; f < 6; f++)
        lw[f] = -1;
    for (int i = 0; i < n; i++) {
        jinsn *ji = &j->ji[i];
        if (ji->need && ji->wr != FALL) {
            uint8_t pres = (uint8_t)(FALL & ~ji->wr & ji->live_after);
            if (pres) {
                ji->sync = 1;
                for (int f = 0; f < 6; f++)
                    if ((pres >> f & 1) && lw[f] >= 0)
                        j->ji[lw[f]].store = 1;
            }
        }
        if (ji->rd) {
            int p = -2, multi = 0;
            for (int f = 0; f < 6; f++) {
                if (!(ji->rd >> f & 1))
                    continue;
                if (p == -2)
                    p = lw[f];
                else if (p != lw[f])
                    multi = 1;
            }
            if (!multi)
                ji->single = (int16_t)p;
            int fusable = !multi && p >= 0 && xbt_opt.flag_fusion &&
                          (ji->consumer == CK_COND || ji->consumer == CK_CARRY) &&
                          map_cond(j->ji[p].mode, ji->cc) != HC_INVALID;
            if (fusable) {
                ji->fuse = (int16_t)p;
            } else {
                for (int f = 0; f < 6; f++)
                    if ((ji->rd >> f & 1) && lw[f] >= 0)
                        j->ji[lw[f]].store = 1;
            }
        }
        for (int f = 0; f < 6; f++)
            if (ji->wr >> f & 1)
                lw[f] = (int16_t)i;
    }
    for (int f = 0; f < 6; f++)
        if ((live_out >> f & 1) && lw[f] >= 0)
            j->ji[lw[f]].store = 1;
    if (!xbt_opt.flag_elim) {
        /* eager mode (for comparison): every producer stores its flags */
        for (int i = 0; i < n; i++)
            if (j->ji[i].wr)
                j->ji[i].store = 1;
    }

    /* Drop fusions whose NZCV would be clobbered in between. */
    int changed;
    do {
        changed = 0;
        for (int i = 0; i < n; i++)
            j->ji[i].hostfl = 0;
        for (int i = 0; i < n; i++)
            if (j->ji[i].fuse >= 0)
                j->ji[j->ji[i].fuse].hostfl = 1;
        for (int i = 0; i < n && !changed; i++) {
            jinsn *ji = &j->ji[i];
            if (ji->fuse < 0)
                continue;
            for (int k = ji->fuse + 1; k < i; k++) {
                jinsn *jk = &j->ji[k];
                int clob = jk->clobber || jk->hostfl || jk->sync ||
                           (jk->consumer && jk->fuse < 0) || jk->consumer == CK_CARRY;
                if (clob) {
                    j->ji[ji->fuse].store = 1;
                    ji->fuse = -1;
                    changed = 1;
                    break;
                }
            }
        }
    } while (changed);

    /* Replay: a non-fused consumer with one in-block producer and no flag
       activity in between can rebuild NZCV from that producer's record. */
    for (int i = 0; i < n; i++) {
        jinsn *ji = &j->ji[i];
        if (!ji->consumer || ji->fuse >= 0 || ji->single < 0)
            continue;
        /* a conditional writer (shift by CL, REP) may have skipped its store */
        int ok = !j->ji[ji->single].cond_wr;
        for (int k = ji->single + 1; k < i; k++)
            if (j->ji[k].need)
                ok = 0;
        ji->replay = (uint8_t)ok;
    }

    for (int i = 0; i < n; i++) {
        jinsn *ji = &j->ji[i];
        if (!ji->wr)
            continue;
        xbt_st.flag_producers++;
        if (!ji->need)
            xbt_st.flag_elided++;
        else if (!ji->store)
            xbt_st.flag_fused_only++;
        else
            xbt_st.flag_stored++;
    }
}

/* ------------------------------------------------------------------ */
/* Control flow                                                        */
/* ------------------------------------------------------------------ */

static void push_imm(jit_t *j, uint32_t v)
{
    a64buf *b = JB(j);
    w_movi(b, 0, v);
    w_str_pre(b, 0, GREG(R_ESP), -4);
}

static int gen_control(jit_t *j, jinsn *ji)
{
    a64buf *b = JB(j);
    x86_insn *in = &ji->in;
    int op = in->op;
    uint32_t next = in->addr + in->len;

    if ((op >= 0x70 && op <= 0x7f) || (op >= 0x180 && op <= 0x18f)) {
        int hc = jit_cond(j, ji, op & 15);
        uint32_t *s1 = a64_here(b), *s2 = NULL;
        if (hc == HC_NE_AND_VC) {
            /* taken only if !(eq || vs) */
            a64_put(b, enc_bcond(0, CC_EQ));
            s2 = a64_here(b);
            a64_put(b, enc_bcond(0, CC_VS));
            emit_direct(j, in->target, in->addr);
            uint32_t *lf = a64_here(b);
            a64_patch_bcond(s1, lf);
            a64_patch_bcond(s2, lf);
            add_exit(j, next);
        } else {
            if (hc == HC_EQ_OR_VS) {
                a64_put(b, enc_bcond(0, CC_EQ));
                s2 = a64_here(b);
                a64_put(b, enc_bcond(0, CC_VS));
            } else {
                a64_put(b, enc_bcond(0, hc));
            }
            add_exit(j, next);
            uint32_t *lt = a64_here(b);
            a64_patch_bcond(s1, lt);
            if (s2)
                a64_patch_bcond(s2, lt);
            emit_direct(j, in->target, in->addr);
        }
        return 1;
    }
    switch (op) {
    case 0xeb: case 0xe9:
        emit_direct(j, in->target, in->addr);
        return 1;
    case 0xe8:
        push_imm(j, next);
        emit_direct(j, in->target, in->addr);
        return 1;
    case 0xc3: case 0xc2:
        w_ldr_post(b, 0, GREG(R_ESP), 4);
        if (op == 0xc2 && in->imm)
            w_addimm(b, GREG(R_ESP), GREG(R_ESP), in->imm, RA);
        emit_indirect(j);
        return 1;
    case 0xe2: case 0xe1: case 0xe0: case 0xe3: {
        uint32_t *s1 = NULL, *s2 = NULL;
        if (op == 0xe3) {
            int r = in->opsize == 2 ? 0 : GREG(R_ECX);
            if (in->opsize == 2)
                w_uxth(b, 0, GREG(R_ECX));
            s1 = a64_here(b);
            a64_put(b, enc_cbz(0, 1, r, 0));       /* ecx != 0 -> fall */
        } else {
            if (op != 0xe2)
                jit_cond_reg(j, ji, ji->cc, 1);    /* w1 = ZF condition */
            w_subi(b, GREG(R_ECX), GREG(R_ECX), 1);
            s1 = a64_here(b);
            a64_put(b, enc_cbz(0, 0, GREG(R_ECX), 0)); /* ecx == 0 -> fall */
            if (op != 0xe2) {
                s2 = a64_here(b);
                a64_put(b, enc_cbz(0, 0, 1, 0));   /* condition false -> fall */
            }
        }
        emit_direct(j, in->target, in->addr);
        uint32_t *f = a64_here(b);
        a64_patch_cbz(s1, f);
        if (s2)
            a64_patch_cbz(s2, f);
        add_exit(j, next);
        return 1;
    }
    case 0xff:
        if (in->reg == 2 || in->reg == 4) {
            amode am;
            int r = jit_ld_rm(j, in, 4, 0, &am);
            if (r != 0)
                w_mov(b, 0, r);
            if (in->reg == 2) {
                w_movi(b, 1, next);
                w_str_pre(b, 1, GREG(R_ESP), -4);
            }
            emit_indirect(j);
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Block construction                                                  */
/* ------------------------------------------------------------------ */

static int readable_bytes(uint32_t addr)
{
    int n = 0;
    while (n < 15 && mem_readable(addr + (uint32_t)n, 1))
        n++;
    return n;
}

static jit_t jit_state;

static void emit_stubs(jit_t *j)
{
    a64buf *b = JB(j);
    for (int i = 0; i < j->nst; i++) {
        jstub *s = &j->st[i];
        uint32_t *here = a64_here(b);
        switch (s->type) {
        case SITE_B: a64_patch_b(s->site, here); break;
        case SITE_CBZ: a64_patch_cbz(s->site, here); break;
        case SITE_BCOND: a64_patch_bcond(s->site, here); break;
        case SITE_TBZ: a64_patch_tbz(s->site, here); break;
        }
        w_movi(b, 0, s->eip);
        w_movi(b, 1, s->reason);
        w_movi(b, 2, s->info);
        uint32_t *p = a64_here(b);
        a64_put(b, enc_b((intptr_t)xbt_stub.special_exit - (intptr_t)p));
    }
    for (int i = 0; i < j->nex; i++) {
        jexit *e = &j->ex[i];
        uint32_t *target = NULL;
        if (xbt_opt.link_blocks) {
            if (e->target == j->start_eip)
                target = j->host_start;
            else
                target = xbt_lookup_host(e->target);
        }
        if (target && a64_b_in_range(e->site, target)) {
            a64_patch_b(e->site, target);
            xbt_st.links++;
            continue;
        }
        uint32_t *stub = a64_here(b);
        a64_patch_b(e->site, stub);
        a64_put(b, enc_bl((intptr_t)xbt_stub.link_handler - (intptr_t)stub));
        a64_put(b, e->target);
        a64_put(b, (uint32_t)(uintptr_t)e->site);
    }
}

xbt_block *xbt_translate(uint32_t eip, uint32_t **cache_ptr, uint32_t *cache_end)
{
    jit_t *j = &jit_state;
    j->n = 0;
    j->nex = 0;
    j->nst = 0;
    j->ended = 0;
    j->x87_used = 0;
    j->start_eip = eip;
    j->nzcv_owner = -1;

    /* pass 0: decode */
    uint32_t a = eip;
    int max = xbt_opt.max_insns < JIT_MAX_INSNS ? xbt_opt.max_insns : JIT_MAX_INSNS;
    while (j->n < max) {
        int avail = readable_bytes(a);
        if (avail == 0)
            break;
        if (a >= XBOX_HLE_BASE && a < XBOX_HLE_BASE + XBOX_HLE_SIZE && j->n > 0)
            break;
        jinsn *ji = &j->ji[j->n];
        memset(ji, 0, sizeof(*ji));
        x86_decode(&ji->in, a, (const uint8_t *)(uintptr_t)a, avail);
        classify(ji);
        j->n++;
        a += ji->in.len;
        if (is_terminator(&ji->in)) {
            j->ended = 1;
            break;
        }
    }
    j->end_eip = a;
    if (j->n == 0)
        return NULL;

    analyze(j);

    j->b.start = j->b.p = *cache_ptr;
    j->b.end = cache_end - 64;
    j->b.overflow = 0;
    j->host_start = *cache_ptr;

    /* blocks on pages that also hold hot data are not write-protected;
       they verify their guest bytes on entry instead */
    int selfcheck = 0;
    for (uint32_t pg = eip & ~0xfffu; pg < j->end_eip; pg += 0x1000)
        if (mem_page_volatile(pg))
            selfcheck = 1;
    uint32_t *lit_adr = NULL;
    if (selfcheck) {
        a64buf *b = JB(j);
        uint32_t len = j->end_eip - eip, off = 0;
        w_movi(b, 0, eip);
        lit_adr = a64_here(b);
        a64_put(b, enc_adr(1, 0));
        while (len - off >= 8) {
            x_ldr(b, 2, 0, (int32_t)off);
            x_ldr(b, 3, 1, (int32_t)off);
            a64_put(b, enc_addsub_sreg(1, 1, 1, ZR, 2, 3, SH_LSL, 0));
            jit_stub(j, a64_here(b), SITE_BCOND, eip, EXIT_SMC, 0);
            a64_put(b, enc_bcond(0, CC_NE));
            off += 8;
        }
        for (int sz = 4; sz >= 1; sz >>= 1) {
            if (len - off >= (uint32_t)sz) {
                int l2 = sz == 4 ? 2 : sz == 2 ? 1 : 0;
                a64_ldst_off(b, l2, LDST_LDR, 2, 0, (int32_t)off);
                a64_ldst_off(b, l2, LDST_LDR, 3, 1, (int32_t)off);
                w_cmp(b, 2, 3);
                jit_stub(j, a64_here(b), SITE_BCOND, eip, EXIT_SMC, 0);
                a64_put(b, enc_bcond(0, CC_NE));
                off += (uint32_t)sz;
            }
        }
    }

    jit_caches_reset(j);
    for (int i = 0; i < j->n && !j->b.overflow; i++) {
        jinsn *ji = &j->ji[i];
        j->cur = i;
        j->insn_off[i] = (uint16_t)(j->b.p - j->host_start);
        if (ji->sync && ji->need)
            jit_sync(j);
        if (is_terminator(&ji->in))
            jit_caches_flush(j);       /* no NZCV clobber: fused flags survive */
        int ok = 0;
        if (!ji->in.invalid) {
            ok = gen_control(j, ji);
            if (!ok)
                ok = jit_int(j, ji);
            if (!ok && ji->in.op >= 0xd8 && ji->in.op <= 0xdf)
                ok = jit_x87(j, ji);
            if (!ok && ji->in.op >= 0x100)
                ok = jit_sse(j, ji);
        }
        if (!keeps_gprs(&ji->in))
            j->idx_valid = 0;
        if (ok <= 0) {
            if (xbt_opt.trace || ok == 0) {
                char buf[160];
                x86_format(&ji->in, buf, sizeof buf);
                XBT_LOG("unsupported instruction %s\n", buf);
            }
            jit_exit_special(j, ji->in.addr, EXIT_UD, 0);
            j->n = i + 1;
            j->ended = 1;
            break;
        }
    }
    if (!j->ended && !j->b.overflow) {
        jit_caches_flush(j);
        add_exit(j, j->end_eip);
    }
    if (!j->b.overflow)
        emit_stubs(j);
    /* re-dispatch stub used when the block is invalidated */
    uint32_t *retrans = a64_here(JB(j));
    w_movi(JB(j), 0, eip);
    a64_put(JB(j), enc_b((intptr_t)xbt_stub.indirect_miss - (intptr_t)a64_here(JB(j))));
    if (lit_adr && !j->b.overflow) {
        /* copy of the guest bytes the prologue compares against */
        if (((uintptr_t)j->b.p & 7) != 0)
            a64_nop(JB(j));
        uint32_t *lit = a64_here(JB(j));
        *lit_adr = enc_adr(1, (intptr_t)lit - (intptr_t)lit_adr);
        uint32_t len = j->end_eip - eip;
        for (uint32_t o = 0; o < len; o += 4) {
            uint32_t w = 0;
            memcpy(&w, (const uint8_t *)(uintptr_t)(eip + o), len - o < 4 ? len - o : 4);
            a64_put(JB(j), w);
        }
    }
    if (j->b.overflow)
        return NULL;

    xbt_block *blk = calloc(1, sizeof(*blk));
    blk->guest_start = eip;
    blk->guest_end = j->ji[j->n - 1].in.addr + j->ji[j->n - 1].in.len;
    blk->host = j->host_start;
    blk->host_words = (uint32_t)(j->b.p - j->host_start);
    blk->ninsn = (uint16_t)j->n;
    blk->retrans = retrans;
    blk->selfcheck = (uint8_t)selfcheck;
    blk->insn_off = malloc(sizeof(uint16_t) * (size_t)j->n);
    blk->insn_eip = malloc(sizeof(uint32_t) * (size_t)j->n);
    for (int i = 0; i < j->n; i++) {
        blk->insn_off[i] = j->insn_off[i];
        blk->insn_eip[i] = j->ji[i].in.addr;
    }
    *cache_ptr = j->b.p;
    __builtin___clear_cache((char *)blk->host, (char *)j->b.p);

    xbt_st.blocks++;
    xbt_st.guest_insns += (uint64_t)j->n;
    xbt_st.host_insns += blk->host_words;
    if (xbt_opt.trace)
        XBT_LOG("block %08x-%08x: %d insns -> %u host words\n", eip, blk->guest_end, j->n,
                blk->host_words);
    if (xbt_opt.dump_dir) {
        char path[512];
        snprintf(path, sizeof path, "%s/%08x_%p.bin", xbt_opt.dump_dir, eip, (void *)blk->host);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(blk->host, 4, blk->host_words, f);
            fclose(f);
        }
    }
    return blk;
}

void xbt_translate_init(void) {}
