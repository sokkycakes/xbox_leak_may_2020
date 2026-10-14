/*
 * xlat's code generator for 32-bit ARM (the Raspberry Pi 3's Cortex-A53 in
 * AArch32): translates x86 basic blocks into ARM code and runs them.
 *
 * Guest registers live in host registers: eax..edi in r4..r11, r0 points at
 * the thread's xcpu (also kept at [sp]), r1-r3, r12 and lr are scratch.
 * x86 SF ZF OF are the CPSR's N Z V, and CF is C, inverted (C = NOT CF,
 * ARM's borrow convention) between blocks and wherever cf_inv says so within
 * one.  PF and AF stay in xcpu (pf_res, af), stored only where a later
 * instruction may read them: the translator knows which flags each
 * instruction reads and writes, and looks ahead into the next blocks.
 *
 * Instructions without a translation of their own call the interpreter
 * (jit_fallback), so every instruction runs; native translations are added
 * class by class, and difftest checks them against the real CPU.
 *
 * Blocks end at a branch.  A direct exit is "bl link_tramp; .word eip;
 * .word alt": the first time it runs, the dispatcher translates the target
 * and patches the bl (and the conditional branch at alt, when there is one)
 * into a branch to it, so linked blocks run without coming back to C.
 * Indirect exits look the target up in a direct-mapped table of {eip, code}
 * pairs and come back to C only when it isn't there.  Translations are
 * never freed: changed code has its blocks' first instruction patched into
 * a jump back to the dispatcher, so threads still holding them are safe.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "xlat.h"
#include "arm.h"

#define CACHE_BYTES  (32u << 20)        /* one region, so every branch reaches */
#define BLOCK_WORDS  (16u << 10)        /* the most one block can take */
#define MAX_INSNS    48
#define ITAB_BITS    15
#define ITAB_SIZE    (1u << ITAB_BITS)
#define ITAB_SHIFT   13                 /* slot: (eip ^ eip >> 13) mod size */
#define HTAB_BITS    16
#define LOOK_INSNS   64                 /* instructions the flag lookahead reads per piece */
#define LOOK_NODES   24                 /* straight-line pieces it reads */

enum { FL_C = 1, FL_P = 2, FL_A = 4, FL_Z = 8, FL_S = 16, FL_O = 32 };
#define FL_ALL  63
#define FL_NZCV (FL_C | FL_Z | FL_S | FL_O)

enum { EXIT_LINK = 1, EXIT_INDIRECT = 2 };

/* host registers */
#define HR(r) (R4 + (r))
#define T0 R1
#define T1 R2
#define T2 R3
#define T3 R12
#define T4 LR

#define O_EIP  ((int)offsetof(xcpu, eip))
#define O_NZCV ((int)offsetof(xcpu, nzcv))
#define O_PF   ((int)offsetof(xcpu, pf_res))
#define O_AF   ((int)offsetof(xcpu, af))
#define O_DF   ((int)offsetof(xcpu, df))
#define O_FS   ((int)offsetof(xcpu, fs_base))
#define O_SITE ((int)offsetof(xcpu, jit_site))
#define O_TOP  ((int)offsetof(xcpu, top))
#define O_ST   ((int)offsetof(xcpu, st))
#define O_FPCW ((int)offsetof(xcpu, fpcw))
#define O_FPSW ((int)offsetof(xcpu, fpsw))
#define O_FTAG ((int)offsetof(xcpu, ftag))
#define O_XMM  ((int)offsetof(xcpu, xmm))

struct block {
    uint32_t eip, end;           /* the guest bytes [eip, end) */
    uint32_t *code;
    struct block *next;          /* hash chain */
    uint32_t count;              /* XLAT_COUNT: times run */
    uint16_t ninsns, nwords;     /* guest instructions, host words */
};

struct plist {                   /* blocks with code in one guest page */
    struct block **v;
    unsigned n, cap;
};

typedef struct { uint32_t eip, code; } itab_entry;

static uint32_t *cache, *cache_cur, *cache_end, *cache_blocks;
static int (*enter)(xcpu *c, const void *code);
static uint32_t *exit_common, *link_tramp, *indirect, *indirect_nf, *fb_tramp, *fb_tramp_c;
static itab_entry *itab;
static struct block *htab[1u << HTAB_BITS];
static struct plist **pages;
static unsigned live_blocks;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t stop_lo, stop_hi;
static unsigned stop_live, unknown_live, abi_live;

static bool stats;
static const char *count_dir;   /* XLAT_COUNT=DIR: count block runs, dump the hottest there */
static FILE *perf_map;   /* XLAT_PERF_MAP: names blocks for perf */
static uint64_t fb_count[0x200], fb_x87[8][16], n_blocks, n_insns, n_native, n_links, n_lookups;

static inline bool in_stop(uint32_t a) { return a - stop_lo < stop_hi - stop_lo; }
static inline unsigned islot(uint32_t eip) { return (eip ^ (eip >> ITAB_SHIFT)) & (ITAB_SIZE - 1); }
static inline unsigned hslot(uint32_t eip) { return (eip * 0x9E3779B1u) >> (32 - HTAB_BITS); }

static void die(const char *msg)
{
    fprintf(stderr, "xlat: %s\n", msg);
    abort();
}

static void flush_icache(const void *start, const void *end)
{
    __builtin___clear_cache((char *)start, (char *)end);
}

/* ---- what instructions do to the flags -------------------------------- */

static const uint8_t cc_flags[8] = { FL_O, FL_C, FL_Z, FL_C | FL_Z, FL_S, FL_P, FL_S | FL_O, FL_Z | FL_S | FL_O };

/* Flags in may read, and flags it always writes (or leaves undefined). */
static void flag_info(const xinsn *in, unsigned *rd, unsigned *wr)
{
    unsigned op = in->op, reg = in->reg, r = 0, w = 0;
    if (!in->len) { *rd = FL_ALL; *wr = 0; return; }
    if (op < 0x40 && (op & 7) < 6) {
        w = FL_ALL;
        if ((op >> 3) == 2 || (op >> 3) == 3) r = FL_C;
    } else if (op >= 0x40 && op < 0x50) {
        w = FL_ALL & ~FL_C;
    } else if ((op >= 0x70 && op < 0x80) || (op >= 0x180 && op < 0x1A0) || (op >= 0x140 && op < 0x150)) {
        r = cc_flags[(op & 15) >> 1];
    } else switch (op) {
    case 0x27: case 0x2F: r = FL_C | FL_A; w = FL_ALL; break;
    case 0x37: case 0x3F: r = FL_A; w = FL_ALL; break;
    case 0x80: case 0x81: case 0x82: case 0x83:
        w = FL_ALL;
        if (reg == 2 || reg == 3) r = FL_C;
        break;
    case 0x84: case 0x85: case 0xA8: case 0xA9: case 0x9D: case 0xCF: case 0xD4: case 0xD5:
    case 0x12E: case 0x12F: case 0x1AF: case 0x1B0: case 0x1B1: case 0x1BC: case 0x1BD: case 0x1C0: case 0x1C1:
        w = FL_ALL;
        break;
    case 0x9C: r = FL_ALL; break;
    case 0x9E: w = FL_ALL & ~FL_O; break;
    case 0x9F: r = FL_ALL & ~FL_O; break;
    case 0xA6: case 0xA7: case 0xAE: case 0xAF:
        if (!(in->pfx & (XP_REP | XP_REPNE))) w = FL_ALL;   /* rep with ecx = 0 writes nothing */
        break;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        unsigned n = (op == 0xD0 || op == 0xD1) ? 1 : (op == 0xD2 || op == 0xD3) ? 0 : in->imm & 31;
        bool dword = (op & 1) && !(in->pfx & XP_OPSZ);
        if (reg == 2 || reg == 3) r = FL_C;
        if (n && reg >= 4) w = FL_ALL;
        else if (n && dword) w = FL_C | FL_O;   /* rotates; narrower ones may rotate by 0 */
        break;
    }
    case 0xCE: r = FL_O; break;
    case 0xD6: r = FL_C; break;
    case 0xDA: case 0xDB:
        if (in->mod == 3 && reg < 4) r = reg == 0 ? FL_C : reg == 1 ? FL_Z : reg == 2 ? FL_C | FL_Z : FL_P;
        else if (in->mod == 3 && op == 0xDB && (reg == 5 || reg == 6)) w = FL_ALL;
        break;
    case 0xDF: if (in->mod == 3 && (reg == 5 || reg == 6)) w = FL_ALL; break;
    case 0xE0: case 0xE1: r = FL_Z; break;
    case 0xF5: r = FL_C; w = FL_C; break;
    case 0xF8: case 0xF9: w = FL_C; break;
    case 0xF6: case 0xF7: if (reg != 2) w = FL_ALL; break;
    case 0xFE: case 0xFF: if (reg < 2) w = FL_ALL & ~FL_C; break;
    case 0x1A3: case 0x1AB: case 0x1B3: case 0x1BB: case 0x1BA: w = FL_ALL & ~FL_Z; break;
    case 0x1A4: case 0x1AC: if (in->imm & 31) w = FL_ALL; break;
    case 0x1C7: w = FL_Z; break;
    }
    *rd = r;
    *wr = w;
}

enum { BR_NONE, BR_JCC, BR_JMP, BR_CALL, BR_RET, BR_IND, BR_OTHER };

/* How in transfers control; *target for the direct kinds. */
static int branch_kind(const xinsn *in, uint32_t *target)
{
    uint32_t next = in->addr + in->len;
    unsigned op = in->op;
    bool o16 = in->pfx & XP_OPSZ;
    if ((op >= 0x70 && op < 0x80) || (op >= 0xE0 && op <= 0xE3) || op == 0xEB) {
        *target = next + (uint32_t)(int8_t)in->imm;
        return op == 0xEB ? BR_JMP : BR_JCC;
    }
    if ((op >= 0x180 && op < 0x190) || op == 0xE8 || op == 0xE9) {
        if (o16) return BR_OTHER;
        *target = next + in->imm;
        return op == 0xE8 ? BR_CALL : op == 0xE9 ? BR_JMP : BR_JCC;
    }
    switch (op) {
    case 0xC2: case 0xC3: case 0xCA: case 0xCB: case 0xCF: return BR_RET;
    case 0x9A: case 0xEA: case 0xCC: case 0xCD: case 0xCE: case 0xF4: return BR_OTHER;
    case 0xFF: if (in->reg >= 2 && in->reg <= 5) return BR_IND; break;
    }
    return BR_NONE;
}

/* Flags the code at eip may read before writing them: liveness over the
   code reachable from eip (up to LOOK_NODES straight-line pieces of up to
   LOOK_INSNS instructions each), solved to a fixed point so loops don't
   end the search.  Flags are dead at calls, rets and indirect jumps (the
   MSVC ABI never passes them; XLAT_SAFE_FLAGS keeps them), and the ones a
   piece past the limit might read count as live. */
static unsigned flags_needed(uint32_t eip)
{
    struct { uint32_t addr; uint8_t need, done, end, in; int16_t succ[2]; } nd[LOOK_NODES];
    int n = 1, work = 0;
    nd[0].addr = eip;
    for (; work < n; work++) {
        uint32_t a = nd[work].addr;
        unsigned need = 0, done = 0, end = unknown_live;
        int16_t succ[2] = { -1, -1 };
        uint32_t to[2];
        int nto = 0;
        for (int k = 0; k < LOOK_INSNS; k++) {
            if (in_stop(a)) { end = stop_live; break; }
            xinsn in;
            if (!x86_decode(&in, a)) { end = FL_ALL; break; }
            unsigned r, w;
            flag_info(&in, &r, &w);
            need |= r & ~done;
            done |= w;
            if (done == FL_ALL) { end = 0; break; }
            uint32_t target = 0;
            int kind = branch_kind(&in, &target);
            if (kind == BR_NONE) { a += in.len; continue; }
            if (kind == BR_JMP) { a = target; continue; }
            if (kind == BR_JCC) { to[nto++] = target; to[nto++] = a + in.len; end = 0; }
            else if (kind == BR_CALL || kind == BR_RET || kind == BR_IND) end = abi_live;
            else end = FL_ALL;
            break;
        }
        for (int j = 0; j < nto; j++) {
            int m = 0;
            while (m < n && nd[m].addr != to[j]) m++;
            if (m == n) {
                if (n == LOOK_NODES) { end |= unknown_live; continue; }
                nd[n++].addr = to[j];
            }
            succ[j] = (int16_t)m;
        }
        nd[work].need = (uint8_t)need;
        nd[work].done = (uint8_t)done;
        nd[work].end = (uint8_t)end;
        nd[work].succ[0] = succ[0];
        nd[work].succ[1] = succ[1];
        nd[work].in = (uint8_t)(need | (end & ~done));
    }
    for (bool changed = true; changed; ) {
        changed = false;
        for (int i = n - 1; i >= 0; i--) {
            unsigned out = nd[i].end;
            for (int j = 0; j < 2; j++)
                if (nd[i].succ[j] >= 0) out |= nd[nd[i].succ[j]].in;
            unsigned in = nd[i].need | (out & ~nd[i].done);
            if (in != nd[i].in) { nd[i].in = (uint8_t)in; changed = true; }
        }
    }
    return nd[0].in;
}

/* ---- translation context and emitters --------------------------------- */

typedef struct {
    asm_t a;
    xinsn in[MAX_INSNS];
    uint8_t live[MAX_INSNS + 1];   /* flags live before in[i]; live[n] after the block */
    int n;
    bool cf_inv;                   /* CPSR C holds NOT CF */
    /* x87 registers cached in VFP registers (see x87_sync) */
    int x_delta;                   /* net pushes */
    uint8_t x_loaded, x_dirty;     /* by slot */
    uint8_t x_tset, x_tempty;      /* slots whose tag changed, and those now empty */
    uint8_t x_tknown, x_tentry;    /* slots whose tag at entry is known, and those empty then */
    uint8_t x_first;               /* slots loaded before the first sync */
    bool x_synced;                 /* a sync has happened */
    uint8_t xreg[8];               /* each slot's VFP register */
} tctx;

#define E(w) emit(&t->a, (w))

static void invert_c(tctx *t)
{
    E(A_MRS(T3));
    E(dp_i(C_AL, OP_EOR, 0, T3, T3, arm_imm(XF_C)));
    E(A_MSR(T3));
    t->cf_inv = !t->cf_inv;
}

static void mov_imm(tctx *t, int rd, uint32_t v)
{
    int e;
    if ((e = arm_imm(v)) >= 0) E(dp_i(C_AL, OP_MOV, 0, rd, 0, e));
    else if ((e = arm_imm(~v)) >= 0) E(dp_i(C_AL, OP_MVN, 0, rd, 0, e));
    else {
        E(a_movw(C_AL, rd, v & 0xFFFF));
        if (v >> 16) E(a_movt(C_AL, rd, v >> 16));
    }
}

/* rd = rn + v, flags untouched; scratch may be used (it may be rd, not rn). */
static void add_imm(tctx *t, int rd, int rn, uint32_t v, int scratch)
{
    int e;
    if (!v) {
        if (rd != rn) E(A_MOV(rd, rn));
    } else if ((e = arm_imm(v)) >= 0) {
        E(dp_i(C_AL, OP_ADD, 0, rd, rn, e));
    } else if ((e = arm_imm(-v)) >= 0) {
        E(dp_i(C_AL, OP_SUB, 0, rd, rn, e));
    } else if (scratch != rn) {
        mov_imm(t, scratch, v);
        E(dp_r(C_AL, OP_ADD, 0, rd, rn, scratch, SH_LSL, 0));
    } else {
        int op = OP_ADD, src = rn;
        if ((int32_t)v < 0) { v = -v; op = OP_SUB; }
        while (v) {
            int p = __builtin_ctz(v) & ~1;
            uint32_t chunk = v & (0xFFu << p);
            E(dp_i(C_AL, op, 0, rd, src, arm_imm(chunk)));
            src = rd;
            v &= ~chunk;
        }
    }
}

/* in's memory operand: emits what it needs and returns the base register
   for [base, #off] with |off| <= maxoff (scratch may become the base). */
static int mem_addr(tctx *t, const xinsn *in, int scratch, int maxoff, int32_t *off)
{
    int b = in->base >= 0 ? HR(in->base) : -1, x = in->index >= 0 ? HR(in->index) : -1, base = -1;
    int32_t d = in->disp;
    if (in->seg == XSEG_FS) {
        E(A_LDR(scratch, R0, O_FS));
        base = scratch;
    }
    if (b >= 0) {
        if (base >= 0) E(dp_r(C_AL, OP_ADD, 0, scratch, scratch, b, SH_LSL, 0));
        else base = b;
    }
    if (x >= 0) {
        if (base >= 0) E(dp_r(C_AL, OP_ADD, 0, scratch, base, x, SH_LSL, in->scale));
        else E(dp_r(C_AL, OP_MOV, 0, scratch, 0, x, SH_LSL, in->scale));
        base = scratch;
    }
    if (base < 0) {
        mov_imm(t, scratch, (uint32_t)d);
        *off = 0;
        return scratch;
    }
    if (d >= -maxoff && d <= maxoff) {
        *off = d;
        return base;
    }
    add_imm(t, scratch, base, (uint32_t)d, scratch);
    *off = 0;
    return scratch;
}

static void ld(tctx *t, int sz, int rt, int base, int32_t off)
{
    if (sz == 4) E(A_LDR(rt, base, off));
    else if (sz == 1) E(A_LDRB(rt, base, off));
    else E(a_ldsth(C_AL, 1, 0xB, rt, base, off));
}

static void st(tctx *t, int sz, int rt, int base, int32_t off)
{
    if (sz == 4) E(A_STR(rt, base, off));
    else if (sz == 1) E(A_STRB(rt, base, off));
    else E(a_ldsth(C_AL, 0, 0xB, rt, base, off));
}

/* rd = byte or word register r (numbered for its size), zero-extended */
static void get_sub(tctx *t, int rd, int r, int sz)
{
    if (sz == 2) E(A_UXTH(rd, HR(r)));
    else if (r < 4) E(A_UXTB(rd, HR(r)));
    else E(A_UBFX(rd, HR(r - 4), 8, 8));
}

/* byte or word register r = the low bits of rs */
static void set_sub(tctx *t, int r, int sz, int rs)
{
    if (sz == 2) E(A_BFI(HR(r), rs, 0, 16));
    else if (r < 4) E(A_BFI(HR(r), rs, 0, 8));
    else E(A_BFI(HR(r - 4), rs, 8, 8));
}

static void store_pf(tctx *t, int r, int sh)
{
    if (sh) {
        E(dp_r(C_AL, OP_MOV, 0, T4, 0, r, SH_LSR, sh));
        E(A_STR(T4, R0, O_PF));
    } else {
        E(A_STR(r, R0, O_PF));
    }
}

/* ARM condition for x86 condition cc, or -1 (parity, which isn't in the
   CPSR).  "below or equal" and "above" need C = NOT CF. */
static int cond_for(tctx *t, unsigned cc)
{
    int c;
    switch (cc >> 1) {
    case 0: c = C_VS; break;
    case 1: c = t->cf_inv ? C_CC : C_CS; break;
    case 2: c = C_EQ; break;
    case 3: if (!t->cf_inv) invert_c(t); c = C_LS; break;
    case 4: c = C_MI; break;
    case 5: return -1;
    case 6: c = C_LT; break;
    default: c = C_LE; break;
    }
    return (cc & 1) ? c ^ 1 : c;
}

/* ---- integer ALU ------------------------------------------------------ */

typedef struct { bool imm; uint32_t val; int rm, sh; } op2_t;

static op2_t o_reg(int rm, int sh) { return (op2_t){ false, 0, rm, sh }; }
static op2_t o_imm(uint32_t v) { return (op2_t){ true, v, 0, 0 }; }

/* data processing with o (an immediate must be encodable) */
static uint32_t dpo(int op, int s, int rd, int rn, op2_t o)
{
    return o.imm ? dp_i(C_AL, op, s, rd, rn, arm_imm(o.val)) : dp_r(C_AL, op, s, rd, rn, o.rm, SH_LSL, o.sh);
}

/* Whether aop has another encoding for the immediate v (negated or inverted). */
static bool alt_imm(int aop, uint32_t v)
{
    switch (aop) {
    case 0: case 5: case 7: return v && arm_imm(-v) >= 0;
    case 2: case 3: case 4: case 8: return arm_imm(~v) >= 0;
    default: return false;
    }
}

/* x86 ALU operation aop (0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor,
   7 cmp, 8 test) on rn and b, the result to rd (none for cmp and test).
   Narrower operands come shifted left by sh, their top bit at bit 31, and
   the low bits clear.  Produces the flags in live. */
static void alu_core(tctx *t, int aop, int rd, int rn, op2_t b, unsigned live, int sh)
{
    bool nores = aop == 7 || aop == 8, logic = aop == 1 || aop == 4 || aop == 6 || aop == 8;
    bool pf = live & FL_P, af = (live & FL_A) && !logic, co = live & (FL_C | FL_O), any = live & FL_ALL;
    if (nores && !any) return;
    if (aop == 8 && !b.imm && b.rm == rn && !b.sh) {   /* test r, r */
        E(dp_i(C_AL, OP_CMP, 1, 0, rn, 0));
        t->cf_inv = true;
        if (pf) store_pf(t, rn, sh);
        return;
    }
    int res = nores ? (pf || af || (aop == 8 && co) ? T3 : -1) : rd;
    if (aop == 2 && t->cf_inv) invert_c(t);
    if (aop == 3 && !t->cf_inv) invert_c(t);
    if (b.imm && arm_imm(b.val) < 0 && (af || !alt_imm(aop, b.val))) {
        mov_imm(t, T2, b.val);
        b = o_reg(T2, 0);
    }
    bool alt = b.imm && arm_imm(b.val) < 0;
    uint32_t v = b.val;
    int op, s = any ? 1 : 0;
    if (af) E(dpo(OP_EOR, 0, T4, rn, b));
    switch (aop) {
    case 0: case 5: case 7:
        op = aop == 0 ? OP_ADD : OP_SUB;
        if (alt) {
            /* add x, v is sub x, -v with the same flags (C still the carry),
               sub x, v is add x, -v (C still NOT borrow); v isn't 0 */
            op = op == OP_ADD ? OP_SUB : OP_ADD;
            b.val = -v;
        }
        if (res < 0) E(dpo(op == OP_SUB ? OP_CMP : OP_CMN, 1, 0, rn, b));
        else E(dpo(op, s, res, rn, b));
        if (s) t->cf_inv = aop != 0;
        break;
    case 2: case 3:
        /* adc x, v is sbc x, ~v and the other way round: the same sum */
        op = aop == 2 ? OP_ADC : OP_SBC;
        if (alt) {
            op = op == OP_ADC ? OP_SBC : OP_ADC;
            b.val = ~v;
        }
        E(dpo(op, s, res, rn, b));
        if (s) t->cf_inv = aop == 3;
        break;
    default:
        op = aop == 1 ? OP_ORR : aop == 6 ? OP_EOR : OP_AND;
        if (alt) {
            op = OP_BIC;
            b.val = ~v;
        }
        if (co) {
            /* CF and OF clear: cmp with 0 gives C = 1 (CF = 0), V = 0 */
            int r = res < 0 ? T3 : res;
            E(dpo(op, 0, r, rn, b));
            E(dp_i(C_AL, OP_CMP, 1, 0, r, 0));
            t->cf_inv = true;
        } else if (any) {
            if (res < 0 && op == OP_AND) E(dpo(OP_TST, 1, 0, rn, b));
            else E(dpo(op, 1, res < 0 ? T3 : res, rn, b));
        } else if (!nores) {
            E(dpo(op, 0, res, rn, b));
        }
        break;
    }
    if (logic && (live & FL_A)) {
        /* undefined, but the CPU clears it (and so does the interpreter) */
        E(dp_i(C_AL, OP_MOV, 0, T4, 0, 0));
        E(A_STR(T4, R0, O_AF));
    }
    if (af) {
        E(dp_r(C_AL, OP_EOR, 0, T4, T4, res, SH_LSL, 0));
        if (sh) E(dp_r(C_AL, OP_MOV, 0, T4, 0, T4, SH_LSR, sh));
        E(A_STR(T4, R0, O_AF));
    }
    if (pf) store_pf(t, res, sh);
}

/* operands: a guest register (numbered for its size), memory, an immediate */
enum { K_REG, K_MEM, K_IMM };
typedef struct { int k, r; uint32_t imm; } xop;

static xop k_reg(int r) { return (xop){ K_REG, r, 0 }; }
static xop k_imm(uint32_t v) { return (xop){ K_IMM, 0, v }; }
static xop k_e(const xinsn *in) { return in->mod == 3 ? k_reg(in->rm) : (xop){ K_MEM, 0, 0 }; }

/* T0 = the address of in's memory operand (exclusive loads take no offset) */
static void addr_t0(tctx *t, const xinsn *in)
{
    int32_t off;
    int base = mem_addr(t, in, T0, 4095, &off);
    if (off || base != T0) add_imm(t, T0, base, (uint32_t)off, T0);
}

/* Atomic read-modify-write on the address in T0: an ldrex/strex loop
   between excl_begin (returns the loop's start) and excl_end.  Exclusive
   accesses fault when unaligned, so for a dword excl_begin branches to
   *plain (patched by excl_plain) for a plain load and store instead: the
   CPU makes those atomic too, ARM can't.  tst and teq change N and Z only. */
static uint32_t *excl_begin(tctx *t, bool word, uint32_t **plain)
{
    *plain = NULL;
    if (word) {
        E(dp_i(C_AL, OP_TST, 1, 0, T0, 3));
        *plain = t->a.p;
        E(0);
    }
    E(A_DMB);
    return t->a.p;
}

/* strex's status is in T3; returns the branch to patch past the plain code */
static uint32_t *excl_end(tctx *t, uint32_t *retry, uint32_t *plain)
{
    E(dp_i(C_AL, OP_TEQ, 1, 0, T3, 0));
    E(a_b(C_NE, t->a.p, retry));
    E(A_DMB);
    if (!plain) return NULL;
    uint32_t *join = t->a.p;
    E(0);
    if (plain < t->a.end) *plain = a_b(C_NE, plain, t->a.p);
    return join;
}

static void excl_join(tctx *t, uint32_t *join)
{
    if (join && join < t->a.end) *join = a_b(C_AL, join, t->a.p);
}

/* lock add/or/and/sub/xor [mem], r or imm on a dword: an ldrex/strex loop
   without flags, then the flags from the value it read */
static bool emit_lock_alu(tctx *t, const xinsn *in, int aop, xop s, unsigned live)
{
    op2_t b;
    if (s.k == K_REG) {
        b = o_reg(HR(s.r), 0);
    } else {
        b = o_imm(s.imm);
        if (arm_imm(s.imm) < 0 && !alt_imm(aop, s.imm)) {
            if (live & FL_A) return false;
            mov_imm(t, T4, s.imm);
            b = o_reg(T4, 0);
        }
    }
    addr_t0(t, in);
    uint32_t *plain, *retry = excl_begin(t, true, &plain);
    E(A_LDREX(T1, T0));
    alu_core(t, aop, T2, T1, b, 0, 0);
    E(A_STREX(T3, T2, T0));
    uint32_t *join = excl_end(t, retry, plain);
    E(A_LDR(T1, T0, 0));
    alu_core(t, aop, T2, T1, b, 0, 0);
    E(A_STR(T2, T0, 0));
    excl_join(t, join);
    if (live & FL_ALL) alu_core(t, aop, T2, T1, b, live, 0);
    return true;
}

static bool emit_alu(tctx *t, const xinsn *in, int aop, int sz, xop d, xop s, unsigned live)
{
    bool mem = d.k == K_MEM || s.k == K_MEM, wb = aop != 7 && aop != 8;
    if (mem && (in->pfx & XP_LOCK)) {
        if (in->ad16 || sz != 4 || d.k != K_MEM || aop == 2 || aop == 3 || !wb) return false;
        return emit_lock_alu(t, in, aop, s, live);
    }
    if (mem && in->ad16) return false;
    if (sz != 4 && (aop == 2 || aop == 3)) return false;
    int base = 0, sh = 32 - 8 * sz;
    int32_t off = 0;
    op2_t b;
    if (mem) base = mem_addr(t, in, T0, sz == 2 ? 255 : 4095, &off);
    if (sz == 4) {
        int ra;
        if (d.k == K_REG) ra = HR(d.r);
        else { E(A_LDR(T1, base, off)); ra = T1; }
        if (s.k == K_REG) b = o_reg(HR(s.r), 0);
        else if (s.k == K_MEM) { E(A_LDR(T2, base, off)); b = o_reg(T2, 0); }
        else b = o_imm(s.imm);
        alu_core(t, aop, ra, ra, b, live, 0);
        if (wb && d.k == K_MEM) E(A_STR(T1, base, off));
        return true;
    }
    /* bytes and words, shifted to the top of the register */
    if (d.k == K_REG) {
        if (sz == 2 || d.r < 4) {
            E(dp_r(C_AL, OP_MOV, 0, T1, 0, HR(d.r), SH_LSL, sh));
        } else {
            E(dp_i(C_AL, OP_AND, 0, T1, HR(d.r - 4), arm_imm(0xFF00)));
            E(dp_r(C_AL, OP_MOV, 0, T1, 0, T1, SH_LSL, 16));
        }
    } else {
        ld(t, sz, T1, base, off);
        E(dp_r(C_AL, OP_MOV, 0, T1, 0, T1, SH_LSL, sh));
    }
    if (s.k == K_REG && d.k == K_REG && s.r == d.r) {
        b = o_reg(T1, 0);
    } else if (s.k == K_REG) {
        if (sz == 2 || s.r < 4) {
            b = o_reg(HR(s.r), sh);
        } else {
            E(dp_i(C_AL, OP_AND, 0, T2, HR(s.r - 4), arm_imm(0xFF00)));
            b = o_reg(T2, 16);
        }
    } else if (s.k == K_MEM) {
        ld(t, sz, T2, base, off);
        b = o_reg(T2, sh);
    } else {
        b = o_imm((s.imm & (sz == 2 ? 0xFFFFu : 0xFFu)) << sh);
    }
    alu_core(t, aop, T1, T1, b, live, sh);
    if (wb) {
        E(dp_r(C_AL, OP_MOV, 0, T1, 0, T1, SH_LSR, sh));
        if (d.k == K_REG) set_sub(t, d.r, sz, T1);
        else st(t, sz, T1, base, off);
    }
    return true;
}

/* shl shr sar rol ror on a dword, count an immediate (or -1: cl) */
static bool emit_shift(tctx *t, const xinsn *in, int count, unsigned live)
{
    int sop = in->reg;
    if (sop == 2 || sop == 3) return false;
    if (in->mod != 3 && (in->ad16 || (in->pfx & XP_LOCK))) return false;
    bool rot = sop < 2;
    if (count < 0) {
        if (live & FL_ALL) return false;   /* a count of 0 leaves the flags */
    } else {
        if (!count) return true;
        if (rot && (live & (FL_C | FL_O))) return false;
        if (!rot && (live & FL_O) && count == 1) return false;
    }
    int base = 0, r;
    int32_t off = 0;
    if (in->mod == 3) {
        r = HR(in->rm);
    } else {
        base = mem_addr(t, in, T0, 4095, &off);
        E(A_LDR(T1, base, off));
        r = T1;
    }
    int kind = sop == 5 ? SH_LSR : sop == 7 ? SH_ASR : rot ? SH_ROR : SH_LSL;
    bool s = !rot && count > 0 && (live & (FL_C | FL_Z | FL_S | FL_P));
    if (count > 0) {
        int amt = sop == 0 ? 32 - count : count;
        E(dp_r(C_AL, OP_MOV, s, r, 0, r, kind, amt));
        if (s) t->cf_inv = false;
        if (!rot && (live & FL_P)) store_pf(t, r, 0);
    } else {
        if (sop == 0) E(dp_i(C_AL, OP_RSB, 0, T2, HR(XR_ECX), 0));
        else if (sop == 1) E(A_MOV(T2, HR(XR_ECX)));
        else E(dp_i(C_AL, OP_AND, 0, T2, HR(XR_ECX), 31));
        E(dp_rr(C_AL, OP_MOV, 0, r, 0, r, kind, T2));
    }
    if (in->mod != 3) E(A_STR(T1, base, off));
    return true;
}

/* ---- x87 ---------------------------------------------------------------- */

/* A block keeps the x87 registers it uses in VFP registers d16-d23, loaded
   when first read, and writes them back (with TOP and the tags) at its
   exits and before the interpreter runs anything.  Slot q is the register
   that was st(q) when the block started (or last synced): after delta net
   pushes st(i) is slot (i - delta) & 7, whatever TOP is, so the translation
   reads TOP only to find a slot's memory.  Popped values are dropped: an
   empty register's contents don't matter. */

#define XD_TMP 1    /* d1: a memory operand */
#define XS_TMP 0    /* s0: single precision and integer conversions */

static int xq(const tctx *t, int i) { return (i - t->x_delta) & 7; }

static void x_reset(tctx *t)
{
    t->x_delta = 0;
    t->x_loaded = t->x_dirty = t->x_tset = t->x_tempty = t->x_tknown = t->x_tentry = 0;
    for (int q = 0; q < 8; q++) t->xreg[q] = (uint8_t)(16 + q);
}

/* T3 = xcpu + 8 * TOP: slot q is then at [T3, #O_ST + 8 * q] */
static void x_base(tctx *t)
{
    E(A_LDR(T3, R0, O_TOP));
    E(dp_r(C_AL, OP_ADD, 0, T3, R0, T3, SH_LSL, 3));
}

/* the VFP register holding st(i), loaded (uses T3) */
static int x_get(tctx *t, int i)
{
    int q = xq(t, i);
    if (!(t->x_loaded >> q & 1)) {
        x_base(t);
        E(a_vldr(t->xreg[q], T3, O_ST + 8 * q));
        t->x_loaded |= (uint8_t)(1u << q);
        if (!t->x_synced) t->x_first |= (uint8_t)(1u << q);
    }
    return t->xreg[q];
}

/* Loads the slots in m at the block's start, sharing one TOP read. */
static void x_preload(tctx *t, unsigned m)
{
    x_base(t);
    for (int q = 0; q < 8; q++)
        if (m >> q & 1) E(a_vldr(t->xreg[q], T3, O_ST + 8 * q));
    t->x_loaded = (uint8_t)m;
}

/* the VFP register for a new value of st(i) */
static int x_put(tctx *t, int i)
{
    int q = xq(t, i);
    t->x_loaded |= (uint8_t)(1u << q);
    t->x_dirty |= (uint8_t)(1u << q);
    return t->xreg[q];
}

/* Slot q's tag becomes empty or valid.  entry is what its first change
   says the tag was when the block started: compiled code never pushes onto
   a full register or pops an empty one, so a push means it was empty and a
   pop that it was valid (-1: unknown).  A slot that ends as it started
   needs no tag write: a push and its pop leave the tags alone. */
static void x_tag(tctx *t, int q, bool empty, int entry)
{
    uint8_t bit = (uint8_t)(1u << q);
    if (!(t->x_tset & bit) && entry >= 0) {
        t->x_tknown |= bit;
        if (entry) t->x_tentry |= bit;
        else t->x_tentry &= (uint8_t)~bit;
    }
    t->x_tset |= bit;
    if (empty) t->x_tempty |= bit;
    else t->x_tempty &= (uint8_t)~bit;
}

/* push: the VFP register for the new st(0) */
static int x_push(tctx *t)
{
    t->x_delta++;
    x_tag(t, xq(t, 0), false, 1);
    return x_put(t, 0);
}

static void x_pop(tctx *t)
{
    uint8_t bit = (uint8_t)(1u << xq(t, 0));
    x_tag(t, xq(t, 0), true, 0);
    t->x_loaded &= (uint8_t)~bit;
    t->x_dirty &= (uint8_t)~bit;
    t->x_delta--;
}

/* rd = slot mask m as physical registers: rotated left by TOP (in tr) */
static void x_rot(tctx *t, int rd, unsigned m, int tr)
{
    mov_imm(t, rd, m | m << 8);
    E(dp_rr(C_AL, OP_MOV, 0, rd, 0, rd, SH_LSL, tr));
    E(A_UBFX(rd, rd, 8, 8));
}

/* Writes the cache back to xcpu (flags kept; uses T1-T3) and empties it. */
static void x87_sync(tctx *t)
{
    int delta = t->x_delta & 7;
    uint8_t same = t->x_tknown & (uint8_t)~(t->x_tentry ^ t->x_tempty);
    uint8_t tset = t->x_tset & (uint8_t)~same, tempty = t->x_tempty & tset;
    if (t->x_dirty || tset || delta) {
        E(A_LDR(T3, R0, O_TOP));
        if (t->x_dirty) E(dp_r(C_AL, OP_ADD, 0, T2, R0, T3, SH_LSL, 3));
        /* st[TOP + q] is register (TOP + q) & 7 or its copy 8 above: the
           three stores write both */
        for (int q = 0; q < 8; q++)
            if (t->x_dirty >> q & 1)
                for (int k = -1; k <= 1; k++) E(a_vstr(t->xreg[q], T2, O_ST + 64 * k + 8 * q));
        if (tset) {
            E(A_LDR(T2, R0, O_FTAG));
            if (tset != tempty) {
                x_rot(t, T1, tset, T3);
                E(dp_r(C_AL, OP_BIC, 0, T2, T2, T1, SH_LSL, 0));
            }
            if (tempty) {
                x_rot(t, T1, tempty, T3);
                E(dp_r(C_AL, OP_ORR, 0, T2, T2, T1, SH_LSL, 0));
            }
            E(A_STR(T2, R0, O_FTAG));
        }
        if (delta) {
            E(dp_i(C_AL, OP_SUB, 0, T3, T3, delta));
            E(dp_i(C_AL, OP_AND, 0, T3, T3, 7));
            E(A_STR(T3, R0, O_TOP));
        }
    }
    x_reset(t);
    t->x_synced = true;
}

/* VFP's 8-bit immediate for the double with bits v, or -1 */
static int vfp_imm(uint64_t v)
{
    if (v & 0xFFFFFFFFFFFFull) return -1;
    unsigned top = (unsigned)(v >> 48), b = top >> 6 & 1, rep = top >> 6 & 0xFF;
    if (rep != (b ? 0xFFu : 0) || (top >> 14 & 1) == b) return -1;
    return (int)((top >> 15) << 7 | b << 6 | (top & 0x3F));
}

static void x_const(tctx *t, int d, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    uint32_t lo = (uint32_t)bits, hi = (uint32_t)(bits >> 32);
    int imm = vfp_imm(bits);
    if (imm >= 0) {
        E(A_VMOV_IMM(d, imm));
        return;
    }
    mov_imm(t, T1, lo);
    if (hi != lo) mov_imm(t, T2, hi);
    E(A_VMOV_DRR(d, T1, hi != lo ? T2 : T1));
}

/* in's memory operand for vldr/vstr: base register, *off a multiple of 4
   (uses T0).  Guest data needn't be aligned: an unaligned access faults
   and jit_fix_unaligned finishes it. */
static int vfp_addr(tctx *t, const xinsn *in, int32_t *off)
{
    int base = mem_addr(t, in, T0, 1020, off);
    if (*off & 3) {
        add_imm(t, T0, base, (uint32_t)*off, T0);
        *off = 0;
        base = T0;
    }
    return base;
}

/* in's memory operand as a double in d: kind 0 m32 float, 1 m32 integer,
   2 m64 float, 3 m16 integer (uses T0-T1, s0) */
static void x_load(tctx *t, const xinsn *in, int kind, int d)
{
    int32_t off;
    if (kind == 3) {
        int base = mem_addr(t, in, T0, 255, &off);
        E(a_ldsth(C_AL, 1, 0xF, T1, base, off));
        E(A_VMOV_SR(XS_TMP, T1));
        E(A_VCVT_D_S32(d, XS_TMP));
        return;
    }
    int base = vfp_addr(t, in, &off);
    if (kind == 2) {
        E(a_vldr(d, base, off));
        return;
    }
    E(a_vldr_s(XS_TMP, base, off));
    E(kind == 0 ? A_VCVT_DS(d, XS_TMP) : A_VCVT_D_S32(d, XS_TMP));
}

/* st(0) (in s) to in's memory operand: kind 0 m32, 2 m64 */
static void x_store(tctx *t, const xinsn *in, int kind, int s)
{
    int32_t off;
    if (kind != 2) E(A_VCVT_SD(XS_TMP, s));
    int base = vfp_addr(t, in, &off);
    E(kind == 2 ? a_vstr(s, base, off) : a_vstr_s(XS_TMP, base, off));
}

/* SIGBUS in translated code: an unaligned vldr or vstr (guest x87 data
   can be anywhere).  Does the access through the signal frame and steps
   past it. */
bool jit_fix_unaligned(void *ucontext)
{
    ucontext_t *uc = ucontext;
    unsigned long *r = &uc->uc_mcontext.arm_r0;
    const uint32_t *pc = (const uint32_t *)uc->uc_mcontext.arm_pc;
    if (!cache || pc < cache || pc >= cache_end) return false;
    uint32_t w = *pc;
    if ((w & 0xFF200E00u) != 0xED000A00u || (w >> 16 & 15) == 15) return false;
    uint32_t off = (w & 0xFF) * 4;
    uint32_t addr = (uint32_t)r[w >> 16 & 15] + ((w >> 23 & 1) ? off : -off);
    unsigned long *p = uc->uc_regspace, *end = p + sizeof(uc->uc_regspace) / sizeof(*p);
    while (p + 2 < end && p[0] != 0x56465001ul) {   /* VFP_MAGIC */
        if (!p[0] || p[1] < 8) return false;
        p += p[1] / sizeof(*p);
    }
    if (p + 2 + 64 > end) return false;
    uint64_t *d = (uint64_t *)(void *)(p + 2);       /* struct user_vfp's fpregs */
    void *mem = (void *)(uintptr_t)addr, *reg;
    size_t len;
    unsigned vd = w >> 12 & 15, dbit = w >> 22 & 1;
    if (w >> 8 & 1) {
        reg = &d[dbit << 4 | vd];
        len = 8;
    } else {
        unsigned n = vd << 1 | dbit;
        reg = (uint32_t *)(void *)&d[n >> 1] + (n & 1);
        len = 4;
    }
    if (w >> 20 & 1) memcpy(reg, mem, len);
    else memcpy(mem, reg, len);
    uc->uc_mcontext.arm_pc += 4;
    return true;
}

static void x_arith(tctx *t, int op, int d, int x, int y)
{
    switch (op) {
    case 0: E(A_VADD(d, x, y)); break;
    case 1: E(A_VMUL(d, x, y)); break;
    case 4: E(A_VSUB(d, x, y)); break;
    case 5: E(A_VSUB(d, y, x)); break;
    case 6: E(A_VDIV(d, x, y)); break;
    default: E(A_VDIV(d, y, x)); break;
    }
}

/* fcom a, b (b < 0: with 0.0): C0 C2 C3 from the compare, C1 clear (uses T1, T4) */
static void x_fcom(tctx *t, int a, int b, unsigned live)
{
    bool keep = live & FL_NZCV;
    E(b < 0 ? A_VCMP0(a) : A_VCMP(a, b));
    if (keep) E(A_MRS(T4));
    E(A_VMRS_APSR);
    E(A_LDR(T1, R0, O_FPSW));
    E(dp_i(C_AL, OP_BIC, 0, T1, T1, arm_imm(0x4700)));
    E(dp_i(C_MI, OP_ORR, 0, T1, T1, arm_imm(0x0100)));   /* less: C0 */
    E(dp_i(C_EQ, OP_ORR, 0, T1, T1, arm_imm(0x4000)));   /* equal: C3 */
    E(dp_i(C_VS, OP_ORR, 0, T1, T1, arm_imm(0x4500)));   /* unordered: all three */
    E(A_STR(T1, R0, O_FPSW));
    if (keep) E(A_MSR(T4));
}

/* fcomi a, b: ZF PF CF from the compare, OF SF AF clear, C1 clear */
static void x_fcomi(tctx *t, int a, int b, unsigned live)
{
    E(A_VCMP(a, b));
    E(A_VMRS(T1));
    if (live & FL_NZCV) {
        /* ARM NZCV: less 1000, equal 0110, greater 0010, unordered 0011.
           x86 wants Z = ZF and C = NOT CF: unordered sets Z and clears C. */
        E(dp_i(C_AL, OP_AND, 0, T2, T1, arm_imm(0x60000000)));
        E(dp_i(C_AL, OP_AND, 0, T3, T1, arm_imm(0x10000000)));
        E(dp_r(C_AL, OP_ORR, 0, T3, T3, T3, SH_LSL, 1));
        E(dp_r(C_AL, OP_EOR, 0, T2, T2, T3, SH_LSL, 1));
        E(A_MSR(T2));
        t->cf_inv = true;
    }
    if (live & FL_P) {   /* pf_res 0 (PF set) when unordered, else 1 */
        E(dp_i(C_AL, OP_MOV, 0, T3, 0, 1));
        E(dp_r(C_AL, OP_BIC, 0, T3, T3, T1, SH_LSR, 28));
        E(A_STR(T3, R0, O_PF));
    }
    if (live & FL_A) {
        E(dp_i(C_AL, OP_MOV, 0, T3, 0, 0));
        E(A_STR(T3, R0, O_AF));
    }
    E(A_LDR(T3, R0, O_FPSW));
    E(dp_i(C_AL, OP_BIC, 0, T3, T3, arm_imm(0x0200)));
    E(A_STR(T3, R0, O_FPSW));
}

/* T1 = s rounded to a 32-bit integer as the control word says (ARM rounds
   as FPSCR says, so FPSCR gets the x87 mode for the conversion); T3 bit 0
   set if it didn't fit (or s is a NaN). */
static void x_cvt(tctx *t, int s)
{
    E(A_LDR(T1, R0, O_FPCW));
    E(A_UBFX(T1, T1, 10, 2));                              /* x87: nearest, down, up, zero */
    E(dp_r(C_AL, OP_ORR, 0, T1, T1, T1, SH_LSL, 2));
    E(A_UBFX(T1, T1, 1, 2));                               /* ARM: nearest, up, down, zero */
    E(A_VMRS(T2));
    E(dp_i(C_AL, OP_BIC, 0, T3, T2, arm_imm(0x00C00000)));
    E(dp_i(C_AL, OP_BIC, 0, T3, T3, 1));                   /* IOC */
    E(dp_r(C_AL, OP_ORR, 0, T3, T3, T1, SH_LSL, 22));
    E(A_VMSR(T3));
    E(A_VCVT_S32_D_R(XS_TMP, s));
    E(A_VMRS(T3));
    E(A_VMSR(T2));
    E(A_VMOV_RS(T1, XS_TMP));
}

/* fist m16/m32: T1 = s as a bits-wide integer, or the integer indefinite
   value (uses T1-T4) */
static void x_fist(tctx *t, int s, int bits)
{
    x_cvt(t, s);
    E(dp_i(C_AL, OP_AND, 0, T3, T3, 1));
    E(dp_i(C_AL, OP_RSB, 0, T3, T3, 0));                   /* all ones if it didn't fit */
    if (bits == 16) {
        E(A_SXTH(T2, T1));
        E(dp_r(C_AL, OP_EOR, 0, T2, T2, T1, SH_LSL, 0));   /* nonzero if past 16 bits */
        E(dp_i(C_AL, OP_RSB, 0, T4, T2, 0));
        E(dp_r(C_AL, OP_ORR, 0, T2, T2, T4, SH_LSL, 0));
        E(dp_r(C_AL, OP_ORR, 0, T3, T3, T2, SH_ASR, 31));
    }
    E(dp_r(C_AL, OP_BIC, 0, T1, T1, T3, SH_LSL, 0));
    E(dp_r(C_AL, OP_ORR, 0, T1, T1, T3, SH_LSL, bits - 1));
}

/* fistp m64 outside 32 bits: v rounded as the control word fpcw says */
uint64_t jit_fist64(double v, uint32_t fpcw);
uint64_t jit_fist64(double v, uint32_t fpcw)
{
    double r;
    switch ((fpcw >> 10) & 3) {
    case 0: r = nearbyint(v); break;
    case 1: r = floor(v); break;
    case 2: r = ceil(v); break;
    default: r = trunc(v); break;
    }
    if (!(r >= -0x1p63 && r < 0x1p63)) return 1ull << 63;
    return (uint64_t)(int64_t)r;
}

/* fistp m64: values that fit in 32 bits here, the rest in jit_fist64 */
static void x_fistp64(tctx *t, const xinsn *in, int s, unsigned live)
{
    bool keep = live & FL_NZCV;
    x_cvt(t, s);
    if (keep) E(A_MRS(T4));
    E(dp_i(C_AL, OP_TST, 1, 0, T3, 1));
    uint32_t *slow = t->a.p;
    E(0);
    E(dp_r(C_AL, OP_MOV, 0, T2, 0, T1, SH_ASR, 31));
    uint32_t *join = t->a.p;
    E(0);
    if (slow < t->a.end) *slow = a_b(C_NE, slow, t->a.p);
    if (keep) E(A_STR(T4, SP, 4));
    E(A_VPUSH(16, 8));
    E(A_VMOV(0, s));
    E(A_LDR(R0, R0, O_FPCW));
    mov_imm(t, R12, (uint32_t)(uintptr_t)jit_fist64);
    E(A_BLX(R12));
    E(A_VPOP(16, 8));
    E(A_MOV(T1, R0));
    E(A_MOV(T2, R1));
    E(A_LDR(R0, SP, 0));
    if (keep) E(A_LDR(T4, SP, 4));
    if (join < t->a.end) *join = a_b(C_AL, join, t->a.p);
    if (keep) E(A_MSR(T4));
    int32_t off;
    int base = mem_addr(t, in, T0, 4091, &off);
    E(A_STR(T1, base, off));
    E(A_STR(T2, base, off + 4));
}

/* fild m64: high word * 2^32 + low word, rounded once */
static void x_fild64(tctx *t, const xinsn *in)
{
    int32_t off;
    int base = mem_addr(t, in, T0, 4091, &off);
    E(A_LDR(T1, base, off));
    E(A_LDR(T2, base, off + 4));
    E(A_VMOV_SR(0, T1));
    E(A_VMOV_SR(1, T2));
    E(A_VCVT_D_U32(2, 0));
    E(A_VCVT_D_S32(3, 1));
    x_const(t, 1, 0x1p32);
    E(A_VMUL(3, 3, 1));
    int d = x_push(t);
    E(A_VADD(d, 3, 2));
}

/* rd = the status word with TOP (uses T3) */
static void x_fsw(tctx *t, int rd)
{
    int delta = t->x_delta & 7;
    E(A_LDR(T3, R0, O_TOP));
    if (delta) {
        E(dp_i(C_AL, OP_SUB, 0, T3, T3, delta));
        E(dp_i(C_AL, OP_AND, 0, T3, T3, 7));
    }
    E(A_LDR(rd, R0, O_FPSW));
    E(dp_i(C_AL, OP_BIC, 0, rd, rd, arm_imm(0x3800)));
    E(dp_r(C_AL, OP_ORR, 0, rd, rd, T3, SH_LSL, 11));
}

static void x_fxch(tctx *t, int i)
{
    if (!i) return;
    x_get(t, 0);
    x_get(t, i);
    int q0 = xq(t, 0), qi = xq(t, i);
    uint8_t r = t->xreg[q0];
    t->xreg[q0] = t->xreg[qi];
    t->xreg[qi] = r;
    t->x_dirty |= (uint8_t)(1u << q0 | 1u << qi);
}

/* st(i) = st(0) (st(i) is no longer empty), then pop if pop */
static void x_fst_reg(tctx *t, int i, bool pop)
{
    if (i) {
        int a = x_get(t, 0);
        int d = x_put(t, i);
        E(A_VMOV(d, a));
        x_tag(t, xq(t, i), false, -1);
    }
    if (pop) x_pop(t);
}

static const double x87_consts[7] = {
    1.0, 3.32192809488736234787, 1.44269504088896340736, 3.14159265358979323846,
    0.30102999566398119521, 0.69314718055994530942, 0.0,
};

/* D8-DF; false (nothing emitted) for the ones left to the interpreter */
static bool x87(tctx *t, const xinsn *in, unsigned live)
{
    unsigned grp = in->op - 0xD8, r = in->reg, i = in->rm;
    int a, b, d;
    int32_t off;
    if (in->mod != 3) {
        int base;
        switch (grp) {
        case 0: case 2: case 4: case 6:   /* st0 op= m32, m32 int, m64, m16 int */
            x_load(t, in, grp == 0 ? 0 : grp == 2 ? 1 : grp == 4 ? 2 : 3, XD_TMP);
            a = x_get(t, 0);
            if (r == 2 || r == 3) {
                x_fcom(t, a, XD_TMP, live);
                if (r == 3) x_pop(t);
            } else {
                x_arith(t, (int)r, x_put(t, 0), a, XD_TMP);
            }
            return true;
        case 1:
            switch (r) {
            case 0: x_load(t, in, 0, x_push(t)); return true;
            case 2: case 3:
                x_store(t, in, 0, x_get(t, 0));
                if (r == 3) x_pop(t);
                return true;
            case 5:   /* fldcw */
                base = mem_addr(t, in, T0, 255, &off);
                E(a_ldsth(C_AL, 1, 0xB, T1, base, off));
                E(A_STR(T1, R0, O_FPCW));
                return true;
            case 7:   /* fnstcw */
                E(A_LDR(T1, R0, O_FPCW));
                base = mem_addr(t, in, T0, 255, &off);
                E(a_ldsth(C_AL, 0, 0xB, T1, base, off));
                return true;
            }
            return false;
        case 3:
            switch (r) {
            case 0: x_load(t, in, 1, x_push(t)); return true;
            case 2: case 3:
                x_fist(t, x_get(t, 0), 32);
                base = mem_addr(t, in, T0, 4095, &off);
                E(A_STR(T1, base, off));
                if (r == 3) x_pop(t);
                return true;
            }
            return false;
        case 5:
            switch (r) {
            case 0: x_load(t, in, 2, x_push(t)); return true;
            case 2: case 3:
                x_store(t, in, 2, x_get(t, 0));
                if (r == 3) x_pop(t);
                return true;
            case 7:   /* fnstsw m16 */
                x_fsw(t, T1);
                base = mem_addr(t, in, T0, 255, &off);
                E(a_ldsth(C_AL, 0, 0xB, T1, base, off));
                return true;
            }
            return false;
        default:
            switch (r) {
            case 0: x_load(t, in, 3, x_push(t)); return true;
            case 2: case 3:
                x_fist(t, x_get(t, 0), 16);
                base = mem_addr(t, in, T0, 255, &off);
                E(a_ldsth(C_AL, 0, 0xB, T1, base, off));
                if (r == 3) x_pop(t);
                return true;
            case 5: x_fild64(t, in); return true;
            case 7:
                x_fistp64(t, in, x_get(t, 0), live);
                x_pop(t);
                return true;
            }
            return false;
        }
    }
    switch (grp) {
    case 0: case 4: case 6:   /* D8 st0 op= st(i); DC st(i) op= st0; DE the same, then pop */
        if (grp == 6 && r == 3) {   /* fcompp */
            if (i != 1) return false;
            a = x_get(t, 0);
            b = x_get(t, 1);
            x_fcom(t, a, b, live);
            x_pop(t);
            x_pop(t);
            return true;
        }
        a = x_get(t, 0);
        b = x_get(t, (int)i);
        if (r == 2 || r == 3) {
            x_fcom(t, a, b, live);
            if (r == 3 || grp == 6) x_pop(t);
            return true;
        }
        if (grp == 0) {
            x_arith(t, (int)r, x_put(t, 0), a, b);
        } else {
            x_arith(t, r >= 4 ? (int)(r ^ 1) : (int)r, x_put(t, (int)i), b, a);
            if (grp == 6) x_pop(t);
        }
        return true;
    case 1:
        switch (r) {
        case 0:   /* fld st(i) */
            a = x_get(t, (int)i);
            d = x_push(t);
            if (d != a) E(A_VMOV(d, a));
            return true;
        case 1: x_fxch(t, (int)i); return true;
        case 2: return i == 0;   /* fnop */
        case 4:
            if (i == 0 || i == 1) {   /* fchs, fabs */
                a = x_get(t, 0);
                d = x_put(t, 0);
                E(i ? A_VABS(d, a) : A_VNEG(d, a));
                return true;
            }
            if (i == 4) {   /* ftst */
                x_fcom(t, x_get(t, 0), -1, live);
                return true;
            }
            return false;
        case 5:
            if (i < 7) {
                x_const(t, x_push(t), x87_consts[i]);
                return true;
            }
            return false;
        case 7:
            if (i == 2) {   /* fsqrt */
                a = x_get(t, 0);
                d = x_put(t, 0);
                E(A_VSQRT(d, a));
                return true;
            }
            return false;
        }
        return false;
    case 2: case 3:
        if (r < 4) {   /* fcmovcc */
            static const uint8_t cc[3] = { 2, 4, 6 };   /* b, e, be (DB: not) */
            if (r == 3) return false;   /* parity */
            int c = cond_for(t, cc[r] | (grp - 2));
            a = x_get(t, 0);
            b = x_get(t, (int)i);
            x_put(t, 0);
            E(A_VMOVc(c, a, b));
            return true;
        }
        if (grp == 2) {   /* fucompp */
            if (r != 5 || i != 1) return false;
            a = x_get(t, 0);
            b = x_get(t, 1);
            x_fcom(t, a, b, live);
            x_pop(t);
            x_pop(t);
            return true;
        }
        if (r == 5 || r == 6) {   /* fucomi, fcomi */
            a = x_get(t, 0);
            b = x_get(t, (int)i);
            x_fcomi(t, a, b, live);
            return true;
        }
        return false;
    case 5:
        switch (r) {
        case 0: {   /* ffree: st(i) empty, its value dropped */
            uint8_t bit = (uint8_t)(1u << xq(t, (int)i));
            x_tag(t, xq(t, (int)i), true, -1);
            t->x_loaded &= (uint8_t)~bit;
            t->x_dirty &= (uint8_t)~bit;
            return true;
        }
        case 1: x_fxch(t, (int)i); return true;
        case 2: case 3: x_fst_reg(t, (int)i, r == 3); return true;
        case 4: case 5:   /* fucom, fucomp */
            a = x_get(t, 0);
            b = x_get(t, (int)i);
            x_fcom(t, a, b, live);
            if (r == 5) x_pop(t);
            return true;
        }
        return false;
    default:
        switch (r) {
        case 1: x_fxch(t, (int)i); return true;
        case 2: case 3: x_fst_reg(t, (int)i, true); return true;
        case 4:   /* fnstsw ax */
            if (i) return false;
            x_fsw(t, T1);
            E(A_BFI(R4, T1, 0, 16));
            return true;
        case 5: case 6:   /* fucomip, fcomip */
            a = x_get(t, 0);
            b = x_get(t, (int)i);
            x_fcomi(t, a, b, live);
            x_pop(t);
            return true;
        }
        return false;
    }
}

/* T2 bit 0 = PF clear: the low byte of pf_res has odd parity (uses T1, T2) */
static void parity_odd(tctx *t)
{
    E(A_LDRB(T1, R0, O_PF));
    E(dp_r(C_AL, OP_EOR, 0, T1, T1, T1, SH_LSR, 4));
    E(dp_i(C_AL, OP_AND, 0, T1, T1, 15));
    mov_imm(t, T2, 0x6996);   /* bit n: n has odd parity */
    E(dp_rr(C_AL, OP_MOV, 0, T2, 0, T2, SH_LSR, T1));
}

/* inc or dec r (a host register), CF kept */
static void emit_incdec(tctx *t, int r, bool dec, unsigned live)
{
    int op = dec ? OP_SUB : OP_ADD;
    if (live & FL_A) E(A_MOV(T2, r));
    if (!(live & FL_C)) {
        E(dp_i(C_AL, op, (live & FL_ALL) ? 1 : 0, r, r, 1));
    } else if (live & FL_O) {
        /* N Z V from the add, C from before */
        E(A_MRS(T3));
        E(dp_i(C_AL, op, 1, r, r, 1));
        E(A_MRS(T4));
        E(dp_i(C_AL, OP_AND, 0, T3, T3, arm_imm(XF_C)));
        E(dp_i(C_AL, OP_BIC, 0, T4, T4, arm_imm(XF_C)));
        E(dp_r(C_AL, OP_ORR, 0, T4, T4, T3, SH_LSL, 0));
        E(A_MSR(T4));
    } else {
        E(dp_i(C_AL, op, 0, r, r, 1));
        if (live & (FL_Z | FL_S)) E(dp_r(C_AL, OP_TST, 1, 0, r, r, SH_LSL, 0));   /* C and V untouched */
    }
    if (live & FL_A) {
        E(dp_r(C_AL, OP_EOR, 0, T2, T2, r, SH_LSL, 0));
        E(A_STR(T2, R0, O_AF));
    }
    if (live & FL_P) store_pf(t, r, 0);
}

/* movs, stos (and their rep forms) on bytes or dwords */
static bool emit_string(tctx *t, const xinsn *in, unsigned live)
{
    bool movs = (in->op & ~1) == 0xA4, rep = in->pfx & XP_REP;
    int sz = (in->op & 1) ? ((in->pfx & XP_OPSZ) ? 2 : 4) : 1;
    if (sz == 2 || (in->pfx & (XP_ADSZ | XP_REPNE)) || (movs && in->seg == XSEG_FS)) return false;
    bool keep = rep && (live & FL_NZCV);
    uint32_t *skip = NULL, *loop = NULL;
    /* T3 = the step: sz, or -sz when DF is set */
    E(A_LDR(T2, R0, O_DF));
    E(dp_i(C_AL, OP_MOV, 0, T3, 0, sz));
    E(dp_r(C_AL, OP_SUB, 0, T3, T3, T2, SH_LSL, sz == 4 ? 3 : 1));
    if (rep) {
        if (keep) E(A_MRS(T4));
        E(dp_i(C_AL, OP_CMP, 1, 0, HR(XR_ECX), 0));
        skip = t->a.p;
        E(0);
        loop = t->a.p;
    }
    if (movs) {
        E(a_ldst_post(C_AL, 1, sz == 1, T1, HR(XR_ESI), T3));
        E(a_ldst_post(C_AL, 0, sz == 1, T1, HR(XR_EDI), T3));
    } else {
        E(a_ldst_post(C_AL, 0, sz == 1, R4, HR(XR_EDI), T3));
    }
    if (rep) {
        E(dp_i(C_AL, OP_SUB, 1, HR(XR_ECX), HR(XR_ECX), 1));
        E(a_b(C_NE, t->a.p, loop));
        if (skip < t->a.end) *skip = a_b(C_EQ, skip, t->a.p);
        if (keep) E(A_MSR(T4));
    }
    return true;
}

/* n dwords from [sb, #so] to [db, #do_] through T1-T4 (all loaded first) */
static void copy_words(tctx *t, int n, int sb, int32_t so, int db, int32_t do_)
{
    static const int tr[4] = { T1, T2, T3, T4 };
    for (int i = 0; i < n; i++) E(A_LDR(tr[i], sb, so + 4 * i));
    for (int i = 0; i < n; i++) E(A_STR(tr[i], db, do_ + 4 * i));
}

/* the Pentium III's SSE moves (no 66 or F2 forms: those are SSE2) */
static bool emit_sse_move(tctx *t, const xinsn *in)
{
    unsigned op = in->op;
    bool ss = in->pfx & XP_REP, m = in->mod != 3;
    if (in->pfx & (XP_OPSZ | XP_REPNE | XP_LOCK)) return false;
    if (ss && op != 0x110 && op != 0x111) return false;
    int x = O_XMM + 16 * in->reg, y = O_XMM + 16 * in->rm, base = R0;
    int32_t off = y;
    if (m) base = mem_addr(t, in, T0, 4083, &off);
    switch (op) {
    case 0x110: case 0x128:   /* movups, movaps, movss xmm, xmm/m */
        if (!ss) {
            copy_words(t, 4, base, off, R0, x);
        } else if (m) {
            copy_words(t, 1, base, off, R0, x);
            E(dp_i(C_AL, OP_MOV, 0, T1, 0, 0));
            for (int i = 1; i < 4; i++) E(A_STR(T1, R0, x + 4 * i));
        } else {
            copy_words(t, 1, R0, y, R0, x);
        }
        return true;
    case 0x111: case 0x129: case 0x12B:   /* movups, movaps, movss, movntps xmm/m, xmm */
        if (op == 0x12B && !m) return false;
        copy_words(t, ss ? 1 : 4, R0, x, base, off);
        return true;
    case 0x112:   /* movlps xmm, m64; movhlps xmm, xmm */
        copy_words(t, 2, base, m ? off : y + 8, R0, x);
        return true;
    case 0x116:   /* movhps xmm, m64; movlhps xmm, xmm */
        copy_words(t, 2, base, off, R0, x + 8);
        return true;
    case 0x113: case 0x117:   /* movlps, movhps m64, xmm */
        if (!m) return false;
        copy_words(t, 2, R0, op == 0x113 ? x : x + 8, base, off);
        return true;
    }
    return false;
}

/* ---- native translations ---------------------------------------------- */

static void emit_fallback(tctx *t, const xinsn *in);

/* Emits in natively, with the flags in live correct after it; false (and
   nothing emitted) to leave it to the interpreter. */
static bool native(tctx *t, const xinsn *in, unsigned live)
{
    unsigned op = in->op;
    int osz = (in->pfx & XP_OPSZ) ? 2 : 4;
    bool m = in->has_modrm && in->mod != 3;
    int32_t off;
    int base = 0, c;

    if (m && in->ad16) return false;
    if (op >= 0xD8 && op <= 0xDF) return x87(t, in, live);
    if (op < 0x40 && (op & 7) < 6) {
        int aop = op >> 3, form = op & 7, sz = (form & 1) ? osz : 1;
        switch (form) {
        case 0: case 1: return emit_alu(t, in, aop, sz, k_e(in), k_reg(in->reg), live);
        case 2: case 3: return emit_alu(t, in, aop, sz, k_reg(in->reg), k_e(in), live);
        default: return emit_alu(t, in, aop, sz, k_reg(XR_EAX), k_imm(in->imm), live);
        }
    }
    switch (op) {
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        if (osz != 4) return false;
        emit_incdec(t, HR(op & 7), op >= 0x48, live);
        return true;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (osz != 4) return false;
        if (op == 0x54) {
            E(A_MOV(T0, R8));
            E(A_PUSH1(T0, R8));
        } else {
            E(A_PUSH1(HR(op & 7), R8));
        }
        return true;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5D: case 0x5E: case 0x5F:
        if (osz != 4) return false;
        E(A_POP1(HR(op & 7), R8));
        return true;
    case 0x68: case 0x6A:
        if (osz != 4) return false;
        mov_imm(t, T0, op == 0x6A ? (uint32_t)(int8_t)in->imm : in->imm);
        E(A_PUSH1(T0, R8));
        return true;
    case 0x69: case 0x6B: {
        if (osz != 4 || (live & (FL_C | FL_O))) return false;
        int src;
        if (m) {
            base = mem_addr(t, in, T0, 4095, &off);
            E(A_LDR(T1, base, off));
            src = T1;
        } else {
            src = HR(in->rm);
        }
        mov_imm(t, T2, op == 0x6B ? (uint32_t)(int8_t)in->imm : in->imm);
        E(A_MUL(HR(in->reg), src, T2));
        return true;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int sz = (op & 1) ? osz : 1;
        uint32_t imm = op == 0x83 ? (uint32_t)(int8_t)in->imm : in->imm;
        return emit_alu(t, in, in->reg, sz, k_e(in), k_imm(imm), live);
    }
    case 0x84: case 0x85:
        return emit_alu(t, in, 8, (op & 1) ? osz : 1, k_e(in), k_reg(in->reg), live);
    case 0x86:
        if (!m) {
            get_sub(t, T1, in->rm, 1);
            get_sub(t, T2, in->reg, 1);
            set_sub(t, in->rm, 1, T2);
            set_sub(t, in->reg, 1, T1);
            return true;
        }
        /* fall through */
    case 0x87: {
        if (op == 0x87 && osz != 4) return false;
        if (!m) {
            E(A_MOV(T0, HR(in->rm)));
            E(A_MOV(HR(in->rm), HR(in->reg)));
            E(A_MOV(HR(in->reg), T0));
            return true;
        }
        /* with memory: always atomic */
        bool keep = live & (FL_Z | FL_S), byte = op == 0x86;
        int src = HR(in->reg);
        addr_t0(t, in);
        if (byte && in->reg >= 4) {
            E(A_UBFX(T2, HR(in->reg - 4), 8, 8));
            src = T2;
        }
        if (keep) E(A_MRS(T4));
        uint32_t *plain, *retry = excl_begin(t, !byte, &plain);
        E(byte ? A_LDREXB(T1, T0) : A_LDREX(T1, T0));
        E(byte ? A_STREXB(T3, src, T0) : A_STREX(T3, src, T0));
        uint32_t *join = excl_end(t, retry, plain);
        if (plain) {
            E(A_LDR(T1, T0, 0));
            E(A_STR(src, T0, 0));
        }
        excl_join(t, join);
        if (keep) E(A_MSR(T4));
        if (byte) set_sub(t, in->reg, 1, T1);
        else E(A_MOV(HR(in->reg), T1));
        return true;
    }
    case 0x88: case 0x89: case 0x8A: case 0x8B: {
        int sz = (op & 1) ? osz : 1;
        bool to_e = !(op & 2);
        if (!m) {
            int dst = to_e ? in->rm : in->reg, src = to_e ? in->reg : in->rm;
            if (sz == 4) E(A_MOV(HR(dst), HR(src)));
            else if (sz == 2) E(A_BFI(HR(dst), HR(src), 0, 16));
            else if (src < 4) set_sub(t, dst, 1, HR(src));
            else { get_sub(t, T1, src, 1); set_sub(t, dst, 1, T1); }
            return true;
        }
        base = mem_addr(t, in, T0, sz == 2 ? 255 : 4095, &off);
        if (to_e) {
            if (sz == 1 && in->reg >= 4) {
                E(dp_r(C_AL, OP_MOV, 0, T1, 0, HR(in->reg - 4), SH_LSR, 8));
                st(t, 1, T1, base, off);
            } else {
                st(t, sz, HR(in->reg), base, off);
            }
        } else if (sz == 4) {
            E(A_LDR(HR(in->reg), base, off));
        } else {
            ld(t, sz, T1, base, off);
            set_sub(t, in->reg, sz, T1);
        }
        return true;
    }
    case 0x8D: {
        if (osz != 4 || !m) return false;
        int rd = HR(in->reg), b = in->base >= 0 ? HR(in->base) : -1, x = in->index >= 0 ? HR(in->index) : -1;
        if (b >= 0 && x >= 0) {
            E(dp_r(C_AL, OP_ADD, 0, rd, b, x, SH_LSL, in->scale));
            add_imm(t, rd, rd, (uint32_t)in->disp, T0);
        } else if (x >= 0) {
            E(dp_r(C_AL, OP_MOV, 0, rd, 0, x, SH_LSL, in->scale));
            add_imm(t, rd, rd, (uint32_t)in->disp, T0);
        } else if (b >= 0) {
            add_imm(t, rd, b, (uint32_t)in->disp, T0);
        } else {
            mov_imm(t, rd, (uint32_t)in->disp);
        }
        return true;
    }
    case 0x8F:
        if (osz != 4 || in->reg) return false;
        if (!m) {
            if (in->rm == XR_ESP) return false;
            E(A_POP1(HR(in->rm), R8));
            return true;
        }
        if (in->base == XR_ESP || in->index == XR_ESP) return false;   /* addressed after the pop */
        E(A_POP1(T1, R8));
        base = mem_addr(t, in, T0, 4095, &off);
        E(A_STR(T1, base, off));
        return true;
    case 0x90: case 0x9B:   /* nop, fwait (exceptions are masked) */
        return true;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        if (osz != 4) return false;
        E(A_MOV(T0, R4));
        E(A_MOV(R4, HR(op & 7)));
        E(A_MOV(HR(op & 7), T0));
        return true;
    case 0x98:
        if (osz != 4) return false;
        E(A_SXTH(R4, R4));
        return true;
    case 0x99:
        if (osz != 4) return false;
        E(dp_r(C_AL, OP_MOV, 0, HR(XR_EDX), 0, R4, SH_ASR, 31));
        return true;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        int sz = (op & 1) ? osz : 1;
        if (in->pfx & XP_ADSZ) return false;
        base = mem_addr(t, in, T0, sz == 2 ? 255 : 4095, &off);
        if (op >= 0xA2) {
            st(t, sz, R4, base, off);
        } else if (sz == 4) {
            E(A_LDR(R4, base, off));
        } else {
            ld(t, sz, T1, base, off);
            set_sub(t, XR_EAX, sz, T1);
        }
        return true;
    }
    case 0xA4: case 0xA5: case 0xAA: case 0xAB:
        return emit_string(t, in, live);
    case 0xA8: case 0xA9:
        return emit_alu(t, in, 8, (op & 1) ? osz : 1, k_reg(XR_EAX), k_imm(in->imm), live);
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        E(dp_i(C_AL, OP_MOV, 0, T1, 0, in->imm & 0xFF));
        set_sub(t, op & 7, 1, T1);
        return true;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        if (osz == 4) {
            mov_imm(t, HR(op & 7), in->imm);
        } else {
            mov_imm(t, T1, in->imm & 0xFFFF);
            set_sub(t, op & 7, 2, T1);
        }
        return true;
    case 0xC1: case 0xD1: case 0xD3:
        if (osz != 4) return false;
        return emit_shift(t, in, op == 0xC1 ? (int)(in->imm & 31) : op == 0xD1 ? 1 : -1, live);
    case 0xC6: case 0xC7: {
        int sz = (op & 1) ? osz : 1;
        if (in->reg) return false;
        uint32_t v = in->imm & (sz == 4 ? 0xFFFFFFFFu : sz == 2 ? 0xFFFFu : 0xFFu);
        if (!m) {
            if (sz == 4) { mov_imm(t, HR(in->rm), v); return true; }
            mov_imm(t, T1, v);
            set_sub(t, in->rm, sz, T1);
            return true;
        }
        base = mem_addr(t, in, T0, sz == 2 ? 255 : 4095, &off);
        mov_imm(t, T1, v);
        st(t, sz, T1, base, off);
        return true;
    }
    case 0xC9:
        if (osz != 4) return false;
        E(A_MOV(R8, R9));
        E(A_POP1(R9, R8));
        return true;
    case 0xF5:
        t->cf_inv = !t->cf_inv;   /* cmc: C now reads the other way */
        return true;
    case 0xF8: case 0xF9:
        /* C = NOT CF: clc sets it, stc clears it */
        E(A_MRS(T3));
        E(dp_i(C_AL, op == 0xF8 ? OP_ORR : OP_BIC, 0, T3, T3, arm_imm(XF_C)));
        E(A_MSR(T3));
        t->cf_inv = true;
        return true;
    case 0xFC: case 0xFD:
        E(dp_i(C_AL, OP_MOV, 0, T3, 0, op & 1));
        E(A_STR(T3, R0, O_DF));
        return true;
    case 0xF6: case 0xF7: {
        int sz = (op & 1) ? osz : 1;
        switch (in->reg) {
        case 0: case 1:
            return emit_alu(t, in, 8, sz, k_e(in), k_imm(in->imm), live);
        case 2:
            if (sz == 2) return false;
            if (!m) {
                int r = HR(sz == 1 && in->rm >= 4 ? in->rm - 4 : in->rm);
                if (sz == 4) E(dp_r(C_AL, OP_MVN, 0, r, 0, r, SH_LSL, 0));
                else E(dp_i(C_AL, OP_EOR, 0, r, r, arm_imm(in->rm >= 4 ? 0xFF00 : 0xFF)));
                return true;
            }
            if (in->pfx & XP_LOCK) return false;
            base = mem_addr(t, in, T0, 4095, &off);
            ld(t, sz, T1, base, off);
            E(dp_r(C_AL, OP_MVN, 0, T1, 0, T1, SH_LSL, 0));
            st(t, sz, T1, base, off);
            return true;
        case 3: {
            if (sz != 4 || (live & FL_A) || (in->pfx & XP_LOCK)) return false;
            int r = HR(in->rm), s = (live & FL_ALL) ? 1 : 0;
            if (m) {
                base = mem_addr(t, in, T0, 4095, &off);
                E(A_LDR(T1, base, off));
                r = T1;
            }
            E(dp_i(C_AL, OP_RSB, s, r, r, 0));
            if (s) t->cf_inv = true;
            if (live & FL_P) store_pf(t, r, 0);
            if (m) E(A_STR(T1, base, off));
            return true;
        }
        case 4: case 5: {
            if (sz != 4 || (live & (FL_C | FL_O))) return false;
            int src = HR(in->rm);
            if (m) {
                base = mem_addr(t, in, T0, 4095, &off);
                E(A_LDR(T1, base, off));
                src = T1;
            }
            E(in->reg == 4 ? A_UMULL(R4, HR(XR_EDX), R4, src) : A_SMULL(R4, HR(XR_EDX), R4, src));
            return true;
        }
        case 6: case 7: {
            /* div and idiv when the quotient fits in 32 bits the easy way
               (edx is 0, or eax's sign for idiv) with udiv/sdiv; the
               interpreter takes the rest and the divide errors.  The flags
               are undefined after either. */
            if (sz != 4 || (in->pfx & XP_LOCK)) return false;
            x87_sync(t);
            int src = HR(in->rm), eax = HR(XR_EAX), edx = HR(XR_EDX);
            if (m) {
                base = mem_addr(t, in, T0, 4095, &off);
                E(A_LDR(T1, base, off));
                src = T1;
            }
            uint32_t *slow[3];
            int ns = 0;
            E(dp_i(C_AL, OP_CMP, 1, 0, src, 0));
            slow[ns++] = t->a.p;
            E(0);
            if (in->reg == 6) {
                E(dp_i(C_AL, OP_CMP, 1, 0, edx, 0));
            } else {
                E(dp_r(C_AL, OP_CMP, 1, 0, edx, eax, SH_ASR, 31));
                slow[ns++] = t->a.p;
                E(0);
                E(dp_i(C_AL, OP_CMN, 1, 0, src, 1));
                E(dp_i(C_EQ, OP_CMP, 1, 0, eax, arm_imm(0x80000000u)));
            }
            slow[ns++] = t->a.p;
            E(0);
            E(in->reg == 6 ? A_UDIV(T2, eax, src) : A_SDIV(T2, eax, src));
            E(A_MLS(edx, T2, src, eax));
            E(A_MOV(eax, T2));
            uint32_t *done = t->a.p;
            E(0);
            for (int k = 0; k < ns; k++)
                if (slow[k] < t->a.end) *slow[k] = a_b(k == ns - 1 && in->reg == 7 ? C_EQ : k ? C_NE : C_EQ, slow[k], t->a.p);
            emit_fallback(t, in);
            if (done < t->a.end) *done = a_b(C_AL, done, t->a.p);
            t->cf_inv = true;
            return true;
        }
        }
        return false;
    }
    case 0xFF: {
        if (osz != 4) return false;
        if (in->reg < 2) {
            if (m && (in->pfx & XP_LOCK)) {
                addr_t0(t, in);
                uint32_t *plain, *retry = excl_begin(t, true, &plain);   /* C and V untouched, as inc/dec needs */
                E(A_LDREX(T1, T0));
                E(dp_i(C_AL, in->reg ? OP_SUB : OP_ADD, 0, T2, T1, 1));
                E(A_STREX(T3, T2, T0));
                uint32_t *join = excl_end(t, retry, plain);
                E(A_LDR(T1, T0, 0));
                E(dp_i(C_AL, in->reg ? OP_SUB : OP_ADD, 0, T2, T1, 1));
                E(A_STR(T2, T0, 0));
                excl_join(t, join);
                if (live & FL_ALL) emit_incdec(t, T1, in->reg, live);
                return true;
            }
            int r = HR(in->rm);
            if (m) {
                base = mem_addr(t, in, T0, 4095, &off);
                E(A_LDR(T1, base, off));
                r = T1;
            }
            emit_incdec(t, r, in->reg, live);
            if (m) E(A_STR(T1, base, off));
            return true;
        }
        if (in->reg != 6) return false;
        if (!m) {
            if (in->rm == XR_ESP) { E(A_MOV(T0, R8)); E(A_PUSH1(T0, R8)); }
            else E(A_PUSH1(HR(in->rm), R8));
            return true;
        }
        base = mem_addr(t, in, T0, 4095, &off);
        E(A_LDR(T1, base, off));
        E(A_PUSH1(T1, R8));
        return true;
    }
    case 0x110: case 0x111: case 0x112: case 0x113: case 0x116: case 0x117: case 0x128: case 0x129: case 0x12B:
        return emit_sse_move(t, in);
    case 0x118: case 0x119: case 0x11A: case 0x11B: case 0x11C: case 0x11D: case 0x11E: case 0x11F:
        return true;   /* prefetch and hint nops */
    case 0x140: case 0x141: case 0x142: case 0x143: case 0x144: case 0x145: case 0x146: case 0x147:
    case 0x148: case 0x149: case 0x14A: case 0x14B: case 0x14C: case 0x14D: case 0x14E: case 0x14F: {
        if (osz != 4 || ((op & 15) >> 1) == 5) return false;
        c = cond_for(t, op & 15);
        int src = HR(in->rm);
        if (m) {
            base = mem_addr(t, in, T0, 4095, &off);
            E(A_LDR(T1, base, off));
            src = T1;
        }
        E(A_MOVc(c, HR(in->reg), src));
        return true;
    }
    case 0x190: case 0x191: case 0x192: case 0x193: case 0x194: case 0x195: case 0x196: case 0x197:
    case 0x198: case 0x199: case 0x19A: case 0x19B: case 0x19C: case 0x19D: case 0x19E: case 0x19F:
        if (((op & 15) >> 1) == 5) {
            parity_odd(t);
            E(dp_i(C_AL, OP_AND, 0, T1, T2, 1));
            if (!(op & 1)) E(dp_i(C_AL, OP_EOR, 0, T1, T1, 1));
        } else {
            c = cond_for(t, op & 15);
            E(dp_i(C_AL, OP_MOV, 0, T1, 0, 0));
            E(dp_i(c, OP_MOV, 0, T1, 0, 1));
        }
        if (!m) {
            set_sub(t, in->rm, 1, T1);
        } else {
            base = mem_addr(t, in, T0, 4095, &off);
            st(t, 1, T1, base, off);
        }
        return true;
    case 0x1AF: {
        if (osz != 4 || (live & (FL_C | FL_O))) return false;
        int src = HR(in->rm);
        if (m) {
            base = mem_addr(t, in, T0, 4095, &off);
            E(A_LDR(T1, base, off));
            src = T1;
        }
        E(A_MUL(HR(in->reg), HR(in->reg), src));
        return true;
    }
    case 0x1B1: {   /* cmpxchg r/m32, r32 */
        if (osz != 4) return false;
        if (!m) {
            alu_core(t, 7, 0, R4, o_reg(HR(in->rm), 0), live | FL_Z, 0);
            E(A_MOVc(C_EQ, HR(in->rm), HR(in->reg)));
            E(A_MOVc(C_NE, R4, HR(in->rm)));
            return true;
        }
        addr_t0(t, in);
        uint32_t *plain, *retry = excl_begin(t, true, &plain);
        E(A_LDREX(T1, T0));
        E(dp_r(C_AL, OP_CMP, 1, 0, T1, R4, SH_LSL, 0));
        uint32_t *fail = t->a.p;
        E(0);
        E(A_STREX(T3, HR(in->reg), T0));
        E(dp_i(C_AL, OP_TEQ, 1, 0, T3, 0));
        E(a_b(C_NE, t->a.p, retry));
        if (fail < t->a.end) *fail = a_b(C_NE, fail, t->a.p);
        E(A_CLREX);
        E(A_DMB);
        uint32_t *join = t->a.p;
        E(0);
        if (plain < t->a.end) *plain = a_b(C_NE, plain, t->a.p);
        E(A_LDR(T1, T0, 0));
        E(dp_r(C_AL, OP_CMP, 1, 0, T1, R4, SH_LSL, 0));
        E(a_ldst(C_EQ, 0, 0, HR(in->reg), T0, 0, 1, 0));
        excl_join(t, join);
        alu_core(t, 7, 0, R4, o_reg(T1, 0), live, 0);
        E(A_MOV(R4, T1));   /* the value read: eax's own when they matched */
        return true;
    }
    case 0x1C1: {   /* xadd r/m32, r32 */
        if (osz != 4) return false;
        if (!m) {
            alu_core(t, 0, T2, HR(in->rm), o_reg(HR(in->reg), 0), live, 0);
            E(A_MOV(HR(in->reg), HR(in->rm)));
            E(A_MOV(HR(in->rm), T2));
            return true;
        }
        addr_t0(t, in);
        uint32_t *plain, *retry = excl_begin(t, true, &plain);
        E(A_LDREX(T1, T0));
        E(dp_r(C_AL, OP_ADD, 0, T2, T1, HR(in->reg), SH_LSL, 0));
        E(A_STREX(T3, T2, T0));
        uint32_t *join = excl_end(t, retry, plain);
        E(A_LDR(T1, T0, 0));
        E(dp_r(C_AL, OP_ADD, 0, T2, T1, HR(in->reg), SH_LSL, 0));
        E(A_STR(T2, T0, 0));
        excl_join(t, join);
        if (live & FL_ALL) alu_core(t, 0, T2, T1, o_reg(HR(in->reg), 0), live, 0);
        E(A_MOV(HR(in->reg), T1));
        return true;
    }
    case 0x1B6: case 0x1B7: case 0x1BE: case 0x1BF: {
        bool word = op & 1, sx = op >= 0x1BE;
        int rd = HR(in->reg);
        if (osz != 4) {
            /* movzx/movsx r16, r/m8: the low word only */
            if (word) return false;
            if (!m) get_sub(t, T1, in->rm, 1);
            else { base = mem_addr(t, in, T0, 4095, &off); E(A_LDRB(T1, base, off)); }
            if (sx) E(A_SXTB(T1, T1));
            E(A_BFI(rd, T1, 0, 16));
            return true;
        }
        if (!m) {
            int r = in->rm;
            if (word) E(sx ? A_SXTH(rd, HR(r)) : A_UXTH(rd, HR(r)));
            else if (r < 4) E(sx ? A_SXTB(rd, HR(r)) : A_UXTB(rd, HR(r)));
            else E(sx ? A_SBFX(rd, HR(r - 4), 8, 8) : A_UBFX(rd, HR(r - 4), 8, 8));
            return true;
        }
        base = mem_addr(t, in, T0, word || sx ? 255 : 4095, &off);
        if (!word && !sx) E(A_LDRB(rd, base, off));
        else E(a_ldsth(C_AL, 1, word ? (sx ? 0xF : 0xB) : 0xD, rd, base, off));
        return true;
    }
    }
    return false;
}

/* ---- block exits -------------------------------------------------------- */

static void emit_exit(tctx *t, uint32_t target, unsigned live, uint32_t *alt)
{
    if (!t->cf_inv && (live & FL_C)) {
        E(A_MRS(T3));
        E(dp_i(C_AL, OP_EOR, 0, T3, T3, arm_imm(XF_C)));
        E(A_MSR(T3));
    }
    E(a_bl(C_AL, t->a.p, link_tramp));
    E(target);
    E((uint32_t)(uintptr_t)alt);
}

/* jump to the guest address in T0 */
static void emit_indirect(tctx *t)
{
    if (!t->cf_inv && (abi_live & FL_C)) invert_c(t);
    E(a_b(C_AL, t->a.p, abi_live ? indirect : indirect_nf));
}

static bool emit_branch(tctx *t, const xinsn *in, int kind, uint32_t target, unsigned lt, unsigned lf)
{
    uint32_t next = in->addr + in->len;
    if (in->pfx & XP_OPSZ) return false;
    x87_sync(t);
    switch (kind) {
    case BR_JCC: {
        if (in->op >= 0xE0 && in->op <= 0xE3) return false;
        unsigned cc = in->op & 15;
        if ((cc >> 1) == 5) {
            /* parity: tst sets Z from PF (C and V untouched), so N and Z are
               saved around it when either exit needs them */
            bool keep = (lt | lf) & (FL_Z | FL_S);
            parity_odd(t);
            if (keep) E(A_MRS(T4));
            E(dp_i(C_AL, OP_TST, 1, 0, T2, 1));
            uint32_t *j = t->a.p;
            E(0);
            if (keep) E(A_MSR(T4));
            emit_exit(t, next, lf, NULL);
            bool norm = !t->cf_inv && (lt & FL_C);
            if (j < t->a.end) *j = a_b((cc & 1) ? C_NE : C_EQ, j, t->a.p);
            if (keep) E(A_MSR(T4));
            emit_exit(t, target, lt, keep || norm ? NULL : j);
            return true;
        }
        int c = cond_for(t, cc);
        uint32_t *j = t->a.p;
        E(0);
        emit_exit(t, next, lf, NULL);
        bool norm = !t->cf_inv && (lt & FL_C);
        if (j < t->a.end) *j = a_b(c, j, t->a.p);
        emit_exit(t, target, lt, norm ? NULL : j);
        return true;
    }
    case BR_JMP:
        emit_exit(t, target, lt, NULL);
        return true;
    case BR_CALL:
        mov_imm(t, T0, next);
        E(A_PUSH1(T0, R8));
        emit_exit(t, target, lt, NULL);
        return true;
    case BR_RET:
        if (in->op != 0xC2 && in->op != 0xC3) return false;
        E(A_POP1(T0, R8));
        if (in->op == 0xC2) add_imm(t, R8, R8, in->imm, T1);
        emit_indirect(t);
        return true;
    case BR_IND:
        if (in->reg == 3 || in->reg == 5 || (in->has_modrm && in->mod != 3 && in->ad16)) return false;
        if (in->mod == 3) {
            E(A_MOV(T0, HR(in->rm)));
        } else {
            int32_t off;
            int base = mem_addr(t, in, T0, 4095, &off);
            E(A_LDR(T0, base, off));
        }
        if (in->reg == 2) {
            mov_imm(t, T1, next);
            E(A_PUSH1(T1, R8));
        }
        emit_indirect(t);
        return true;
    }
    return false;
}

static void emit_fallback(tctx *t, const xinsn *in)
{
    static xinsn *pool;
    static unsigned left;
    if (!left) {
        pool = malloc(4096 * sizeof(*pool));
        if (!pool) die("out of memory");
        left = 4096;
    }
    xinsn *copy = pool++;
    left--;
    *copy = *in;
    x87_sync(t);
    E(a_bl(C_AL, t->a.p, t->cf_inv ? fb_tramp : fb_tramp_c));
    E((uint32_t)(uintptr_t)copy);
    t->cf_inv = true;
}

/* ---- blocks ------------------------------------------------------------- */

static int decode_block(tctx *t, uint32_t eip)
{
    int n = 0;
    for (uint32_t a = eip; n < MAX_INSNS; ) {
        if (n && in_stop(a)) break;
        xinsn *in = &t->in[n++];
        if (!x86_decode(in, a)) {
            in->addr = a;
            in->len = 0;   /* faults when it runs */
            break;
        }
        uint32_t target;
        if (branch_kind(in, &target) != BR_NONE) break;
        a += in->len;
    }
    return n;
}

static void page_add(uint32_t page, struct block *b)
{
    struct plist *p = pages[page];
    if (!p) p = pages[page] = calloc(1, sizeof(*p));
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->v = realloc(p->v, p->cap * sizeof(*p->v));
    }
    p->v[p->n++] = b;
}

static void page_remove(uint32_t page, struct block *b)
{
    struct plist *p = pages[page];
    for (unsigned i = 0; p && i < p->n; i++)
        if (p->v[i] == b) { p->v[i] = p->v[--p->n]; return; }
}

/* Translates the block at eip (with the lock held). */
static struct block *translate(uint32_t eip)
{
    static tctx tc;
    tctx *t = &tc;
    if ((size_t)(cache_end - cache_cur) < BLOCK_WORDS) {
        if (live_blocks) die("the code cache is full");
        cache_cur = cache_blocks;   /* nothing refers to the old code */
    }
    int n = t->n = decode_block(t, eip);
    uint8_t rd[MAX_INSNS], wr[MAX_INSNS];
    for (int i = 0; i < n; i++) {
        unsigned r, w;
        flag_info(&t->in[i], &r, &w);
        rd[i] = (uint8_t)r;
        wr[i] = (uint8_t)w;
    }
    const xinsn *last = &t->in[n - 1];
    uint32_t next = last->addr + last->len, target = 0;
    int kind = last->len ? branch_kind(last, &target) : BR_OTHER;
    unsigned lt = 0, lf = 0;
    switch (kind) {
    case BR_NONE: lf = flags_needed(next); break;
    case BR_JCC: lt = flags_needed(target); lf = flags_needed(next); break;
    case BR_JMP: lt = flags_needed(target); break;
    case BR_CALL: case BR_RET: case BR_IND: lt = abi_live; break;
    default: lt = lf = FL_ALL; break;
    }
    t->live[n] = (uint8_t)(lt | lf);
    for (int i = n - 1; i >= 0; i--) t->live[i] = (uint8_t)((t->live[i + 1] & ~wr[i]) | rd[i]);

    struct block *b = calloc(1, sizeof(*b));
    if (!b) die("out of memory");
    /* When the first pass loads two or more x87 registers before its first
       sync, the second loads them all at the start with one TOP read. */
    unsigned pre = 0;
    for (int pass = 0; pass < 2; pass++) {
        t->a = (asm_t){ cache_cur, cache_cur, cache_end };
        t->cf_inv = true;
        x_reset(t);
        t->x_first = 0;
        t->x_synced = false;
        if (count_dir) {
            mov_imm(t, T3, (uint32_t)(uintptr_t)&b->count);
            E(A_LDR(T2, T3, 0));
            E(dp_i(C_AL, OP_ADD, 0, T2, T2, 1));
            E(A_STR(T2, T3, 0));
        }
        if (pre) x_preload(t, pre);
        bool ended = false;
        unsigned nat = 0;
        for (int i = 0; i < n && !ended; i++) {
            const xinsn *in = &t->in[i];
            if (i == n - 1 && kind != BR_NONE && kind != BR_OTHER && emit_branch(t, in, kind, target, lt, lf)) {
                nat++;
                ended = true;
                break;
            }
            if (in->len && native(t, in, t->live[i + 1])) nat++;
            else emit_fallback(t, in);
            if (!in->len) ended = true;
        }
        if (!ended) x87_sync(t);
        if (!ended) emit_exit(t, next, kind == BR_NONE || kind == BR_OTHER ? lf : lt | lf, NULL);
        if (pass) {
            n_native += nat;
        } else {
            pre = t->x_first;
            if (__builtin_popcount(pre) < 2) pre = 0;
            if (!pre) {
                n_native += nat;
                break;
            }
        }
    }
    if (t->a.p > t->a.end) die("a block outgrew the code cache");
    flush_icache(t->a.start, t->a.p);

    b->eip = eip;
    b->ninsns = (uint16_t)n;
    b->nwords = (uint16_t)(t->a.p - cache_cur);
    b->end = next > eip ? next : eip + 1;
    b->code = cache_cur;
    cache_cur = t->a.p;
    if (perf_map) {
        fprintf(perf_map, "%x %x x86_%08x\n", (unsigned)(uintptr_t)b->code, (unsigned)((t->a.p - b->code) * 4), eip);
        fflush(perf_map);
    }
    unsigned h = hslot(eip);
    b->next = htab[h];
    htab[h] = b;
    page_add(eip >> 12, b);
    if (((b->end - 1) >> 12) != (eip >> 12)) page_add((b->end - 1) >> 12, b);
    live_blocks++;
    n_blocks++;
    n_insns += n;
    return b;
}

static struct block *find_block(uint32_t eip)
{
    for (struct block *b = htab[hslot(eip)]; b; b = b->next)
        if (b->eip == eip) return b;
    return NULL;
}

static void itab_set(uint32_t eip, const uint32_t *code)
{
    uint64_t e = eip | (uint64_t)(uint32_t)(uintptr_t)code << 32;
    __atomic_store_n((uint64_t *)&itab[islot(eip)], e, __ATOMIC_RELEASE);
}

static const uint32_t *code_for(uint32_t eip)
{
    uint64_t e = __atomic_load_n((uint64_t *)&itab[islot(eip)], __ATOMIC_ACQUIRE);
    if ((uint32_t)e == eip) return (const uint32_t *)(uintptr_t)(uint32_t)(e >> 32);
    pthread_mutex_lock(&lock);
    n_lookups++;
    struct block *b = find_block(eip);
    if (!b) b = translate(eip);
    itab_set(eip, b->code);
    pthread_mutex_unlock(&lock);
    return b->code;
}

/* Patches a direct exit (its bl at site, the conditional branch at alt) to
   jump to code. */
static void link_exit(uint32_t *site, uint32_t *alt, const uint32_t *code)
{
    __atomic_store_n(site, a_b(C_AL, site, code), __ATOMIC_RELAXED);
    flush_icache(site, site + 1);
    if (alt) {
        uint32_t w = (*alt & 0xF0000000u) | (a_b(C_AL, alt, code) & 0x0FFFFFFFu);
        __atomic_store_n(alt, w, __ATOMIC_RELAXED);
        flush_icache(alt, alt + 1);
    }
    n_links++;
}

/* The x87 registers' copies the translated code reads past st[7] (C code
   only writes st[0..7]). */
static void st_mirror(xcpu *c)
{
    memcpy(c->st_hi, c->st, sizeof(c->st));
}

void jit_run(xcpu *c)
{
    st_mirror(c);
    for (;;) {
        uint32_t eip = c->eip;
        if (in_stop(eip)) return;
        const uint32_t *code = code_for(eip);
        if (enter(c, code) != EXIT_LINK) continue;   /* indirect: c->eip is the target */
        uint32_t *w = (uint32_t *)(uintptr_t)c->jit_site;
        uint32_t target = w[0];
        c->eip = target;
        if (in_stop(target)) continue;
        pthread_mutex_lock(&lock);
        struct block *b = find_block(target);
        if (!b) b = translate(target);
        link_exit(w - 1, (uint32_t *)(uintptr_t)w[1], b->code);
        pthread_mutex_unlock(&lock);
    }
}

void jit_invalidate(uint32_t addr, uint32_t len)
{
    if (!cache || !len) return;
    pthread_mutex_lock(&lock);
    for (uint32_t page = addr >> 12; page <= (addr + len - 1) >> 12; page++) {
        struct plist *p = pages[page];
        for (unsigned i = 0; p && i < p->n; ) {
            struct block *b = p->v[i];
            if (b->eip >= addr + len || b->end <= addr) { i++; continue; }
            /* unhash it, then turn its entry into a fresh exit to its eip, so
               whatever still jumps there is linked to the new translation */
            struct block **pp = &htab[hslot(b->eip)];
            while (*pp != b) pp = &(*pp)->next;
            *pp = b->next;
            page_remove(b->eip >> 12, b);
            if (((b->end - 1) >> 12) != (b->eip >> 12)) page_remove((b->end - 1) >> 12, b);
            itab_entry *e = &itab[islot(b->eip)];
            if (e->code == (uint32_t)(uintptr_t)b->code) __atomic_store_n((uint64_t *)e, 0xFFFFFFFFu, __ATOMIC_RELEASE);
            if (cache_end - cache_cur < 4) die("the code cache is full");
            uint32_t *s = cache_cur;
            s[0] = a_bl(C_AL, s, link_tramp);
            s[1] = b->eip;
            s[2] = (uint32_t)(uintptr_t)b->code;
            cache_cur += 3;
            flush_icache(s, cache_cur);
            __atomic_store_n(b->code, a_b(C_AL, b->code, s), __ATOMIC_RELAXED);
            flush_icache(b->code, b->code + 1);
            free(b);
            live_blocks--;
        }
        if (page == 0xFFFFF) break;
    }
    pthread_mutex_unlock(&lock);
}

/* The interpreter runs the instruction whose copy site[0] points at; the
   result is where the trampoline continues: past the call, or the indirect
   lookup when the instruction jumped. */
uintptr_t jit_fallback(xcpu *c, uint32_t *site);
uintptr_t jit_fallback(xcpu *c, uint32_t *site)
{
    const xinsn *in = (const xinsn *)(uintptr_t)site[0];
    c->eip = in->addr;
    if (stats) {
        fb_count[in->op & 0x1FF]++;
        if (in->op >= 0xD8 && in->op <= 0xDF) fb_x87[in->op - 0xD8][in->reg + (in->mod == 3 ? 8 : 0)]++;
    }
    if (!in->len || xi_exec(c, in) != XI_OK) xrt_fault(c, "invalid instruction");
    if ((in->op >= 0xD8 && in->op <= 0xDF) || in->op == 0x1AE) st_mirror(c);
    if (c->eip == in->addr + in->len) return (uintptr_t)(site + 1);
    return (uintptr_t)indirect;
}

/* ---- trampolines -------------------------------------------------------- */

/* Guest registers and flags to xcpu (inv: C holds NOT CF). */
static void save_state(asm_t *a, bool inv)
{
    emit(a, a_stm(R0, 0x0FF0));
    emit(a, A_MRS(R2));
    if (inv) emit(a, dp_i(C_AL, OP_EOR, 0, R2, R2, arm_imm(XF_C)));
    emit(a, dp_i(C_AL, OP_AND, 0, R2, R2, arm_imm(0xF0000000u)));
    emit(a, A_STR(R2, R0, O_NZCV));
}

static void gen_trampolines(void)
{
    asm_t a = { cache, cache, cache_end };

    /* int enter(xcpu *c, const void *code) */
    enter = (int (*)(xcpu *, const void *))(uintptr_t)a.p;
    emit(&a, A_PUSH(0x4FF0));                         /* r4-r11, lr */
    emit(&a, dp_i(C_AL, OP_SUB, 0, SP, SP, 12));      /* [sp] = c, sp 8-aligned */
    emit(&a, A_STR(R0, SP, 0));
    emit(&a, A_LDR(R2, R0, O_NZCV));
    emit(&a, dp_i(C_AL, OP_EOR, 0, R2, R2, arm_imm(XF_C)));
    emit(&a, A_MSR(R2));
    emit(&a, a_ldm(R0, 0x0FF0));
    emit(&a, A_BX(R1));
    exit_common = a.p;                                /* r0 = why */
    emit(&a, dp_i(C_AL, OP_ADD, 0, SP, SP, 12));
    emit(&a, A_POP(0x8FF0));                          /* r4-r11, pc */

    /* direct exit: lr points at the target eip */
    link_tramp = a.p;
    save_state(&a, true);
    emit(&a, A_STR(LR, R0, O_SITE));
    emit(&a, dp_i(C_AL, OP_MOV, 0, R0, 0, EXIT_LINK));
    emit(&a, a_b(C_AL, a.p, exit_common));

    /* indirect exit: r1 = the target eip; flags kept (indirect_nf: not) */
    uint32_t *miss[2];
    for (int keep = 1; keep >= 0; keep--) {
        if (keep) indirect = a.p;
        else indirect_nf = a.p;
        if (keep) emit(&a, A_MRS(LR));
        emit(&a, dp_r(C_AL, OP_EOR, 0, R2, R1, R1, SH_LSR, ITAB_SHIFT));
        emit(&a, a_movw(C_AL, R3, (uint32_t)(uintptr_t)itab & 0xFFFF));
        emit(&a, a_movt(C_AL, R3, (uint32_t)(uintptr_t)itab >> 16));
        emit(&a, A_UBFX(R2, R2, 0, ITAB_BITS));
        emit(&a, dp_r(C_AL, OP_ADD, 0, R3, R3, R2, SH_LSL, 3));
        emit(&a, A_LDRD(R2, R3));                     /* r2 = eip, r3 = code */
        emit(&a, dp_r(C_AL, OP_CMP, 1, 0, R2, R1, SH_LSL, 0));
        miss[keep] = a.p;
        emit(&a, 0);
        if (keep) emit(&a, A_MSR(LR));
        emit(&a, A_BX(R3));
    }
    *miss[1] = a_b(C_NE, miss[1], a.p);
    emit(&a, A_MSR(LR));
    *miss[0] = a_b(C_NE, miss[0], a.p);
    emit(&a, A_STR(R1, R0, O_EIP));
    save_state(&a, true);
    emit(&a, dp_i(C_AL, OP_MOV, 0, R0, 0, EXIT_INDIRECT));
    emit(&a, a_b(C_AL, a.p, exit_common));

    /* interpreter fallback: lr points at the xinsn; fb_tramp_c when C is CF */
    fb_tramp_c = a.p;
    save_state(&a, false);
    uint32_t *to_common = a.p;
    emit(&a, 0);
    fb_tramp = a.p;
    save_state(&a, true);
    *to_common = a_b(C_AL, to_common, a.p);
    emit(&a, A_MOV(R1, LR));
    emit(&a, a_movw(C_AL, R12, (uint32_t)(uintptr_t)jit_fallback & 0xFFFF));
    emit(&a, a_movt(C_AL, R12, (uint32_t)(uintptr_t)jit_fallback >> 16));
    emit(&a, A_BLX(R12));
    emit(&a, A_MOV(R12, R0));
    emit(&a, A_LDR(R0, SP, 0));
    emit(&a, A_LDR(R2, R0, O_NZCV));
    emit(&a, dp_i(C_AL, OP_EOR, 0, R2, R2, arm_imm(XF_C)));
    emit(&a, A_MSR(R2));
    emit(&a, a_ldm(R0, 0x0FF0));
    emit(&a, A_LDR(R1, R0, O_EIP));
    emit(&a, A_BX(R12));

    cache_blocks = cache_cur = a.p;
    flush_icache(cache, cache_cur);
}

static int by_work(const void *x, const void *y)
{
    const struct block *a = *(struct block *const *)x, *b = *(struct block *const *)y;
    uint64_t wa = (uint64_t)a->count * a->nwords, wb = (uint64_t)b->count * b->nwords;
    return wa < wb ? 1 : wa > wb ? -1 : 0;
}

/* XLAT_COUNT: the blocks that ran the most host code since the last print,
   listed in DIR/hot.txt with their x86 bytes and ARM words (tools/xlat/
   hotdis.sh disassembles both). */
static void print_hot_blocks(void)
{
    size_t n = 0, k = 0;
    for (unsigned h = 0; h < (1u << HTAB_BITS); h++)
        for (struct block *b = htab[h]; b; b = b->next) n++;
    struct block **v = malloc((n ? n : 1) * sizeof(*v));
    uint64_t total = 0;
    for (unsigned h = 0; h < (1u << HTAB_BITS); h++)
        for (struct block *b = htab[h]; b; b = b->next) {
            v[k++] = b;
            total += (uint64_t)b->count * b->nwords;
        }
    qsort(v, n, sizeof(*v), by_work);
    char path[512];
    snprintf(path, sizeof(path), "%s/hot.txt", count_dir);
    FILE *f = fopen(path, "w");
    fprintf(stderr, "xlat: hottest blocks (host words run, of %llu):\n", (unsigned long long)total);
    for (size_t i = 0; i < n && i < 40; i++) {
        struct block *b = v[i];
        uint64_t w = (uint64_t)b->count * b->nwords;
        char line[160];
        snprintf(line, sizeof(line), "x86_%08x %5.2f%% %10u runs %3u insns %4u words", b->eip,
                 total ? 100.0 * (double)w / (double)total : 0.0, b->count, b->ninsns, b->nwords);
        fprintf(stderr, "xlat:   %s\n", line);
        if (!f) continue;
        fprintf(f, "%s\nx86:", line);
        for (uint32_t a = b->eip; a < b->end; a++) fprintf(f, " %02x", *(const uint8_t *)(uintptr_t)a);
        fprintf(f, "\narm:");
        for (unsigned j = 0; j < b->nwords; j++) fprintf(f, " %08x", b->code[j]);
        fprintf(f, "\n");
    }
    if (f) fclose(f);
    for (size_t i = 0; i < n; i++) v[i]->count = 0;
    free(v);
}

void jit_print_stats(void)
{
    if (count_dir) print_hot_blocks();
    fprintf(stderr, "xlat: %llu blocks, %llu instructions (%llu native), %u KB of code, %llu links, %llu C lookups\n",
            (unsigned long long)n_blocks, (unsigned long long)n_insns, (unsigned long long)n_native,
            (unsigned)((cache_cur - cache) * 4 / 1024), (unsigned long long)n_links, (unsigned long long)n_lookups);
    uint64_t total = 0;
    for (int i = 0; i < 0x200; i++) total += fb_count[i];
    fprintf(stderr, "xlat: %llu interpreted instructions; most frequent:\n", (unsigned long long)total);
    for (int k = 0; k < 40; k++) {
        int best = -1;
        for (int i = 0; i < 0x200; i++)
            if (fb_count[i] && (best < 0 || fb_count[i] > fb_count[best])) best = i;
        if (best < 0) break;
        fprintf(stderr, "xlat:   %s%02x %12llu\n", best >= 0x100 ? "0f " : "", best & 0xFF,
                (unsigned long long)fb_count[best]);
        fb_count[best] = 0;
    }
    for (int k = 0; k < 16; k++) {
        int best = -1;
        for (int i = 0; i < 128; i++)
            if (fb_x87[i >> 4][i & 15] && (best < 0 || fb_x87[i >> 4][i & 15] > fb_x87[best >> 4][best & 15])) best = i;
        if (best < 0) break;
        fprintf(stderr, "xlat:   %02x /%d%s %12llu\n", 0xD8 + (best >> 4), best & 7, best & 8 ? " st(i)" : "",
                (unsigned long long)fb_x87[best >> 4][best & 15]);
        fb_x87[best >> 4][best & 15] = 0;
    }
    memset(fb_count, 0, sizeof(fb_count));
    memset(fb_x87, 0, sizeof(fb_x87));
}

/* XLAT_STATS=N with N > 1: also print every N seconds, counting from the
   last print, for runs that never exit cleanly. */
static void *stats_thread(void *arg)
{
    unsigned secs = (unsigned)(uintptr_t)arg;
    for (;;) {
        sleep(secs);
        fprintf(stderr, "xlat: last %u s:\n", secs);
        jit_print_stats();
    }
    return NULL;
}

void jit_init(uint32_t lo, uint32_t hi, bool flags_at_stop)
{
    stop_lo = lo;
    stop_hi = hi;
    stop_live = flags_at_stop ? FL_ALL : 0;
    if (cache) return;
    /* Flags are dead across ret and indirect jumps (MSVC never passes them
       there); where the lookahead gives up, SF ZF CF OF are kept.
       XLAT_SAFE_FLAGS keeps every flag everywhere. */
    bool safe = getenv("XLAT_SAFE_FLAGS") != NULL;
    unknown_live = safe ? FL_ALL : FL_NZCV;
    abi_live = safe ? FL_ALL : 0;
    stats = getenv("XLAT_STATS") != NULL;
    count_dir = getenv("XLAT_COUNT");
    cache = mmap(NULL, CACHE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cache == MAP_FAILED) die("cannot map the code cache");
    cache_end = cache + CACHE_BYTES / 4;
    itab = aligned_alloc(64, ITAB_SIZE * sizeof(*itab));
    pages = calloc(1u << 20, sizeof(*pages));
    if (!itab || !pages) die("out of memory");
    for (unsigned i = 0; i < ITAB_SIZE; i++) itab[i] = (itab_entry){ 0xFFFFFFFFu, 0 };
    gen_trampolines();
    if (stats) {
        unsigned secs = (unsigned)atoi(getenv("XLAT_STATS"));
        pthread_t th;
        if (secs > 1 && !pthread_create(&th, NULL, stats_thread, (void *)(uintptr_t)secs)) pthread_detach(th);
        else atexit(jit_print_stats);
    }
    if (getenv("XLAT_PERF_MAP")) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int)getpid());
        perf_map = fopen(path, "w");
    }
}
