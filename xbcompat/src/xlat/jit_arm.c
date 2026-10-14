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
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
#define LOOK_INSNS   32                 /* instructions the flag lookahead reads per path */
#define LOOK_DEPTH   2                  /* conditional branches it follows */

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

struct block {
    uint32_t eip, end;           /* the guest bytes [eip, end) */
    uint32_t *code;
    struct block *next;          /* hash chain */
};

struct plist {                   /* blocks with code in one guest page */
    struct block **v;
    unsigned n, cap;
};

typedef struct { uint32_t eip, code; } itab_entry;

static uint32_t *cache, *cache_cur, *cache_end, *cache_blocks;
static int (*enter)(xcpu *c, const void *code);
static uint32_t *exit_common, *link_tramp, *indirect, *fb_tramp, *fb_tramp_c;
static itab_entry *itab;
static struct block *htab[1u << HTAB_BITS];
static struct plist **pages;
static unsigned live_blocks;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t stop_lo, stop_hi;
static unsigned stop_live, unknown_live;

static bool stats;
static FILE *perf_map;   /* XLAT_PERF_MAP: names blocks for perf */
static uint64_t fb_count[0x200], n_blocks, n_insns, n_native, n_links, n_lookups;

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

/* Flags the code at eip may read before writing them. */
static unsigned flags_needed(uint32_t eip, int depth)
{
    unsigned need = 0, done = 0;
    for (int k = 0; k < LOOK_INSNS; k++) {
        if (in_stop(eip)) return need | (stop_live & ~done);
        xinsn in;
        if (!x86_decode(&in, eip)) return need | (FL_ALL & ~done);
        unsigned r, w;
        flag_info(&in, &r, &w);
        need |= r & ~done;
        done |= w;
        if (done == FL_ALL) return need;
        uint32_t target = 0;
        switch (branch_kind(&in, &target)) {
        case BR_NONE:
            eip += in.len;
            continue;
        case BR_JMP: case BR_CALL:
            eip = target;
            continue;
        case BR_JCC:
            if (depth <= 0) break;
            return need | ((flags_needed(target, depth - 1) | flags_needed(eip + in.len, depth - 1)) & ~done);
        default:
            break;
        }
        break;
    }
    return need | (unknown_live & ~done);
}

/* ---- translation context and emitters --------------------------------- */

typedef struct {
    asm_t a;
    xinsn in[MAX_INSNS];
    uint8_t live[MAX_INSNS + 1];   /* flags live before in[i]; live[n] after the block */
    int n;
    bool cf_inv;                   /* CPSR C holds NOT CF */
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

static bool emit_alu(tctx *t, const xinsn *in, int aop, int sz, xop d, xop s, unsigned live)
{
    bool mem = d.k == K_MEM || s.k == K_MEM, wb = aop != 7 && aop != 8;
    if (mem && (in->ad16 || (in->pfx & XP_LOCK))) return false;
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

/* ---- native translations ---------------------------------------------- */

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
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        if (osz != 4 || (live & (FL_C | FL_A))) return false;
        int r = HR(op & 7);
        E(dp_i(C_AL, op < 0x48 ? OP_ADD : OP_SUB, (live & FL_ALL) ? 1 : 0, r, r, 1));
        if (live & FL_P) store_pf(t, r, 0);
        return true;
    }
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
    case 0x87:
        if (osz != 4 || m) return false;
        E(A_MOV(T0, HR(in->rm)));
        E(A_MOV(HR(in->rm), HR(in->reg)));
        E(A_MOV(HR(in->reg), T0));
        return true;
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
    case 0x90:
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
        }
        return false;
    }
    case 0xFF: {
        if (osz != 4) return false;
        if (in->reg < 2) {
            if (live & (FL_C | FL_A)) return false;
            if (m && (in->pfx & XP_LOCK)) return false;
            int r = HR(in->rm);
            if (m) {
                base = mem_addr(t, in, T0, 4095, &off);
                E(A_LDR(T1, base, off));
                r = T1;
            }
            E(dp_i(C_AL, in->reg ? OP_SUB : OP_ADD, (live & FL_ALL) ? 1 : 0, r, r, 1));
            if (live & FL_P) store_pf(t, r, 0);
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
        if (((op & 15) >> 1) == 5) return false;
        c = cond_for(t, op & 15);
        E(dp_i(C_AL, OP_MOV, 0, T1, 0, 0));
        E(dp_i(c, OP_MOV, 0, T1, 0, 1));
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
    case 0x1B6: case 0x1B7: case 0x1BE: case 0x1BF: {
        if (osz != 4) return false;
        bool word = op & 1, sx = op >= 0x1BE;
        int rd = HR(in->reg);
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
    if (!t->cf_inv && (unknown_live & FL_C)) invert_c(t);
    E(a_b(C_AL, t->a.p, indirect));
}

static bool emit_branch(tctx *t, const xinsn *in, int kind, uint32_t target, unsigned lt, unsigned lf)
{
    uint32_t next = in->addr + in->len;
    if (in->pfx & XP_OPSZ) return false;
    switch (kind) {
    case BR_JCC: {
        if (in->op >= 0xE0 && in->op <= 0xE3) return false;
        unsigned cc = in->op & 15;
        if ((cc >> 1) == 5) return false;
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
    case BR_NONE: lf = flags_needed(next, LOOK_DEPTH); break;
    case BR_JCC: lt = flags_needed(target, LOOK_DEPTH); lf = flags_needed(next, LOOK_DEPTH); break;
    case BR_JMP: case BR_CALL: lt = flags_needed(target, LOOK_DEPTH); break;
    case BR_RET: case BR_IND: lt = unknown_live; break;
    default: lt = lf = FL_ALL; break;
    }
    t->live[n] = (uint8_t)(lt | lf);
    for (int i = n - 1; i >= 0; i--) t->live[i] = (uint8_t)((t->live[i + 1] & ~wr[i]) | rd[i]);

    t->a = (asm_t){ cache_cur, cache_cur, cache_end };
    t->cf_inv = true;
    bool ended = false;
    for (int i = 0; i < n && !ended; i++) {
        const xinsn *in = &t->in[i];
        if (i == n - 1 && kind != BR_NONE && kind != BR_OTHER && emit_branch(t, in, kind, target, lt, lf)) {
            n_native++;
            ended = true;
            break;
        }
        if (in->len && native(t, in, t->live[i + 1])) n_native++;
        else emit_fallback(t, in);
        if (!in->len) ended = true;
    }
    if (!ended) emit_exit(t, next, kind == BR_NONE || kind == BR_OTHER ? lf : lt | lf, NULL);
    if (t->a.p > t->a.end) die("a block outgrew the code cache");
    flush_icache(t->a.start, t->a.p);

    struct block *b = malloc(sizeof(*b));
    b->eip = eip;
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

void jit_run(xcpu *c)
{
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
    if (stats) fb_count[in->op & 0x1FF]++;
    if (!in->len || xi_exec(c, in) != XI_OK) xrt_fault(c, "invalid instruction");
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

    /* indirect exit: r1 = the target eip; flags kept */
    indirect = a.p;
    emit(&a, A_MRS(LR));
    emit(&a, dp_r(C_AL, OP_EOR, 0, R2, R1, R1, SH_LSR, ITAB_SHIFT));
    emit(&a, a_movw(C_AL, R3, (uint32_t)(uintptr_t)itab & 0xFFFF));
    emit(&a, a_movt(C_AL, R3, (uint32_t)(uintptr_t)itab >> 16));
    emit(&a, A_UBFX(R2, R2, 0, ITAB_BITS));
    emit(&a, dp_r(C_AL, OP_ADD, 0, R3, R3, R2, SH_LSL, 3));
    emit(&a, A_LDRD(R2, R3));                         /* r2 = eip, r3 = code */
    emit(&a, dp_r(C_AL, OP_CMP, 1, 0, R2, R1, SH_LSL, 0));
    uint32_t *miss = a.p;
    emit(&a, 0);
    emit(&a, A_MSR(LR));
    emit(&a, A_BX(R3));
    *miss = a_b(C_NE, miss, a.p);
    emit(&a, A_MSR(LR));
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

void jit_print_stats(void)
{
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
}

void jit_init(uint32_t lo, uint32_t hi, bool flags_at_stop)
{
    stop_lo = lo;
    stop_hi = hi;
    stop_live = flags_at_stop ? FL_ALL : 0;
    if (cache) return;
    unknown_live = getenv("XLAT_SAFE_FLAGS") ? FL_ALL : FL_NZCV;
    stats = getenv("XLAT_STATS") != NULL;
    cache = mmap(NULL, CACHE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cache == MAP_FAILED) die("cannot map the code cache");
    cache_end = cache + CACHE_BYTES / 4;
    itab = aligned_alloc(64, ITAB_SIZE * sizeof(*itab));
    pages = calloc(1u << 20, sizeof(*pages));
    if (!itab || !pages) die("out of memory");
    for (unsigned i = 0; i < ITAB_SIZE; i++) itab[i] = (itab_entry){ 0xFFFFFFFFu, 0 };
    gen_trampolines();
    if (stats) atexit(jit_print_stats);
    if (getenv("XLAT_PERF_MAP")) {
        char path[64];
        snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int)getpid());
        perf_map = fopen(path, "w");
    }
}
