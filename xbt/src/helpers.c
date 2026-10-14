/*
 * helpers.c - rarely executed instructions implemented in C. Translated code
 * calls these through cpu->helpers[]; guest registers are spilled to cpu->r
 * around calls that need them.
 */
#include <string.h>
#include <time.h>
#include "xbt.h"

static void h_sync(xbt_cpu *cpu) { xbt_flags_sync(cpu); }
static int h_cond(xbt_cpu *cpu, int cc) { return xbt_cond(cpu, cc); }
static uint32_t h_get_flags(xbt_cpu *cpu) { return xbt_flags(cpu) | 2; }

static void h_set_flags(xbt_cpu *cpu, uint32_t v, uint32_t mask)
{
    xbt_set_flags(cpu, v, mask);
    cpu->eflags |= 2;
}

static void h_cpuid(xbt_cpu *cpu)
{
    uint32_t leaf = cpu->r[R_EAX];
    uint32_t a = 0, b = 0, c = 0, d = 0;
    switch (leaf) {
    case 0:
        a = 2;
        memcpy(&b, "Genu", 4);
        memcpy(&d, "ineI", 4);
        memcpy(&c, "ntel", 4);
        break;
    case 1:
        /* Pentium III (Coppermine), as in the Xbox: FPU TSC CX8 CMOV MMX FXSR SSE ... */
        a = 0x0000068a;
        d = 0x0383f9ffu;
        break;
    case 2:
        a = 0x03020101;
        d = 0x0c040843;
        break;
    }
    cpu->r[R_EAX] = a;
    cpu->r[R_EBX] = b;
    cpu->r[R_ECX] = c;
    cpu->r[R_EDX] = d;
}

static void h_rdtsc(xbt_cpu *cpu)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* the Xbox CPU runs at 733.33 MHz */
    unsigned __int128 ns = (unsigned __int128)ts.tv_sec * 1000000000u + (unsigned)ts.tv_nsec;
    uint64_t t = (uint64_t)(ns * 733333333u / 1000000000u);
    if (t <= cpu->tsc)
        t = cpu->tsc + 1;
    cpu->tsc = t;
    cpu->r[R_EAX] = (uint32_t)t;
    cpu->r[R_EDX] = (uint32_t)(t >> 32);
}

static uint32_t szf_pf(uint32_t v8)
{
    uint32_t f = 0;
    if ((v8 & 0xff) == 0) f |= XF_ZF;
    if (v8 & 0x80) f |= XF_SF;
    if (!(__builtin_popcount(v8 & 0xff) & 1)) f |= XF_PF;
    return f;
}

static xbt_pio_fn pio_handler;
void xbt_set_pio_handler(xbt_pio_fn fn) { pio_handler = fn; }

/* in/out: op = opcode | opsize << 8, imm = immediate port (E4-E7) */
static void h_pio(xbt_cpu *cpu, uint32_t op, uint32_t imm)
{
    int opc = op & 0xff, osz = (op >> 8) & 7;
    uint16_t port = (opc & 8) ? (uint16_t)cpu->r[R_EDX] : (uint16_t)imm;
    int size = (opc & 1) ? osz : 1;
    uint32_t mask = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
    if (opc & 2) {
        if (pio_handler)
            pio_handler(port, size, 1, cpu->r[R_EAX] & mask);
    } else {
        uint32_t v = pio_handler ? pio_handler(port, size, 0, 0) : mask;
        cpu->r[R_EAX] = (cpu->r[R_EAX] & ~mask) | (v & mask);
    }
}

/* daa das aaa aas aam aad (and port i/o) */
static void h_misc(xbt_cpu *cpu, uint32_t op, uint32_t imm)
{
    if ((op & 0xf0) == 0xe0) {
        h_pio(cpu, op, imm);
        return;
    }
    uint32_t fl = xbt_flags(cpu);
    uint32_t ax = cpu->r[R_EAX] & 0xffff;
    uint32_t al = ax & 0xff, ah = ax >> 8;
    int cf = fl & XF_CF ? 1 : 0, af = fl & XF_AF ? 1 : 0;
    uint32_t nf = 0;
    switch (op) {
    case 0x27: { /* daa */
        uint32_t old_al = al;
        int old_cf = cf;
        cf = 0;
        if ((al & 0xf) > 9 || af) {
            cf = old_cf || (al + 6 > 0xff);
            al = (al + 6) & 0xff;
            af = 1;
        } else {
            af = 0;
        }
        if (old_al > 0x99 || old_cf) {
            al = (al + 0x60) & 0xff;
            cf = 1;
        } else {
            cf = 0;
        }
        nf = szf_pf(al) | (cf ? XF_CF : 0) | (af ? XF_AF : 0);
        ax = (ax & 0xff00) | al;
        break;
    }
    case 0x2f: { /* das */
        uint32_t old_al = al;
        int old_cf = cf;
        cf = 0;
        if ((al & 0xf) > 9 || af) {
            cf = old_cf || (al < 6);
            al = (al - 6) & 0xff;
            af = 1;
        } else {
            af = 0;
        }
        if (old_al > 0x99 || old_cf) {
            al = (al - 0x60) & 0xff;
            cf = 1;
        }
        nf = szf_pf(al) | (cf ? XF_CF : 0) | (af ? XF_AF : 0);
        ax = (ax & 0xff00) | al;
        break;
    }
    case 0x37: /* aaa */
        if ((al & 0xf) > 9 || af) {
            ax = (ax + 0x106) & 0xffff;
            af = cf = 1;
        } else {
            af = cf = 0;
        }
        ax &= 0xff0f;
        nf = szf_pf(ax & 0xff) | (cf ? XF_CF : 0) | (af ? XF_AF : 0);
        break;
    case 0x3f: /* aas */
        if ((al & 0xf) > 9 || af) {
            ax = (ax - 6) & 0xffff;
            ax = (ax - 0x100) & 0xffff;
            af = cf = 1;
        } else {
            af = cf = 0;
        }
        ax &= 0xff0f;
        nf = szf_pf(ax & 0xff) | (cf ? XF_CF : 0) | (af ? XF_AF : 0);
        break;
    case 0xd4: /* aam */
        if (imm == 0)
            imm = 10;   /* #DE on hardware; avoid a host crash */
        ah = al / imm;
        al = al % imm;
        ax = ah << 8 | al;
        nf = szf_pf(al);
        break;
    case 0xd5: /* aad */
        al = (al + ah * imm) & 0xff;
        ax = al;
        nf = szf_pf(al);
        break;
    }
    cpu->r[R_EAX] = (cpu->r[R_EAX] & 0xffff0000u) | ax;
    cpu->eflags = (fl & ~XF_STATUS) | nf;
    cpu->lf_op = 0;
}

static uint32_t get_r8(xbt_cpu *cpu, int r)
{
    return r < 4 ? cpu->r[r] & 0xff : (cpu->r[r - 4] >> 8) & 0xff;
}

static void set_r8(xbt_cpu *cpu, int r, uint32_t v)
{
    if (r < 4)
        cpu->r[r] = (cpu->r[r] & ~0xffu) | (v & 0xff);
    else
        cpu->r[r - 4] = (cpu->r[r - 4] & ~0xff00u) | (v & 0xff) << 8;
}

/* rcl / rcr. info: bit0 rcr, bits1-3 size, bit4 memory, bits5-7 register */
static void h_rcx(xbt_cpu *cpu, uint32_t info, uint32_t addr, uint32_t count)
{
    int rcr = info & 1, size = (info >> 1) & 7, mem = (info >> 4) & 1, reg = (info >> 5) & 7;
    unsigned bits = 8u * (unsigned)size;
    uint32_t mask = bits == 32 ? 0xffffffffu : (1u << bits) - 1;
    count &= 31;
    if (count == 0)
        return;
    uint32_t v;
    if (mem) {
        v = 0;
        memcpy(&v, (void *)(uintptr_t)addr, (size_t)size);
    } else if (size == 1) {
        v = get_r8(cpu, reg);
    } else {
        v = cpu->r[reg] & mask;
    }
    uint32_t fl = xbt_flags(cpu);
    uint32_t cf = fl & 1, of = 0;
    unsigned n = bits == 32 ? count : count % (bits + 1);
    uint32_t msb = 1u << (bits - 1);
    if (rcr)
        of = ((v & msb) != 0) ^ cf;
    for (unsigned i = 0; i < n; i++) {
        if (rcr) {
            uint32_t ncf = v & 1;
            v = (v >> 1) | (cf ? msb : 0);
            cf = ncf;
        } else {
            uint32_t ncf = (v & msb) != 0;
            v = ((v << 1) | cf) & mask;
            cf = ncf;
        }
    }
    if (!rcr)
        of = ((v & msb) != 0) ^ cf;
    if (mem)
        memcpy((void *)(uintptr_t)addr, &v, (size_t)size);
    else if (size == 1)
        set_r8(cpu, reg, v);
    else if (size == 2)
        cpu->r[reg] = (cpu->r[reg] & 0xffff0000u) | v;
    else
        cpu->r[reg] = v;
    fl = (fl & ~(XF_CF | XF_OF)) | (cf ? XF_CF : 0) | (of ? XF_OF : 0);
    cpu->eflags = fl;
    cpu->lf_op = 0;
}

static void h_cmpxchg8b(xbt_cpu *cpu, uint32_t addr)
{
    uint64_t m;
    memcpy(&m, (void *)(uintptr_t)addr, 8);
    uint64_t cmp = (uint64_t)cpu->r[R_EDX] << 32 | cpu->r[R_EAX];
    int eq = m == cmp;
    if (eq) {
        uint64_t n = (uint64_t)cpu->r[R_ECX] << 32 | cpu->r[R_EBX];
        memcpy((void *)(uintptr_t)addr, &n, 8);
    } else {
        cpu->r[R_EAX] = (uint32_t)m;
        cpu->r[R_EDX] = (uint32_t)(m >> 32);
    }
    xbt_set_flags(cpu, eq ? XF_ZF : 0, XF_ZF);
}

extern void *xbt_x87_helper, *xbt_x87_mem_helper, *xbt_sse_helper;

void xbt_helpers_install(xbt_cpu *cpu)
{
    cpu->helpers[H_FLAGS_SYNC] = (void *)h_sync;
    cpu->helpers[H_COND] = (void *)h_cond;
    cpu->helpers[H_GET_FLAGS] = (void *)h_get_flags;
    cpu->helpers[H_SET_FLAGS] = (void *)h_set_flags;
    cpu->helpers[H_CPUID] = (void *)h_cpuid;
    cpu->helpers[H_RDTSC] = (void *)h_rdtsc;
    cpu->helpers[H_MISC] = (void *)h_misc;
    cpu->helpers[H_RCx] = (void *)h_rcx;
    cpu->helpers[H_CMPXCHG8B] = (void *)h_cmpxchg8b;
    cpu->helpers[H_X87] = xbt_x87_helper;
    cpu->helpers[H_X87_MEM] = xbt_x87_mem_helper;
    cpu->helpers[H_SSE] = xbt_sse_helper;
}
