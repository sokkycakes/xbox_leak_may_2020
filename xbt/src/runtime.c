/*
 * runtime.c - code cache, dispatcher, block linking, SMC detection.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "jit.h"

#define HASH_BITS 16
static xbt_block *hash_tab[1u << HASH_BITS];
static uint32_t *code_base, *code_ptr, *code_end, *code_blocks;
static uint64_t generation;
static xbt_hle_fn hle_handler;
static xbt_cpu *volatile current_cpu;
volatile int xbt_preempt;   /* set (with cpu->pending) by the OS layer to regain control */

typedef uint32_t (*enter_fn)(xbt_cpu *, void *);

static inline uint32_t hash_eip(uint32_t eip)
{
    return (eip * 0x9E3779B1u) >> (32 - HASH_BITS);
}

static xbt_block *lookup(uint32_t eip)
{
    for (xbt_block *b = hash_tab[hash_eip(eip)]; b; b = b->next)
        if (b->guest_start == eip && !b->dead)
            return b;
    return NULL;
}

/* ---- per physical page lists of blocks (for precise invalidation) ---- */
#define MAX_PAGES (XBOX_RAM_SIZE_DEVKIT >> 12)
typedef struct plist { xbt_block **v; uint16_t n, cap; } plist;
static plist pages[MAX_PAGES];
static uint16_t page_data_faults[MAX_PAGES];
#define VOLATILE_THRESHOLD 256

static void page_add(uint32_t pg, xbt_block *b)
{
    plist *l = &pages[pg];
    for (int i = 0; i < l->n; i++)
        if (l->v[i] == b)
            return;
    if (l->n == l->cap) {
        l->cap = l->cap ? (uint16_t)(l->cap * 2) : 8;
        l->v = realloc(l->v, sizeof(*l->v) * l->cap);
    }
    l->v[l->n++] = b;
}

/* Invalidate a block: its entry now branches to a stub that re-dispatches. */
static void tombstone(xbt_block *b)
{
    if (b->dead)
        return;
    b->dead = 1;
    *b->host = enc_b((intptr_t)b->retrans - (intptr_t)b->host);
    __builtin___clear_cache((char *)b->host, (char *)(b->host + 1));
    if (b->guest_start < XBT_JTAB_RANGE)
        ((uint32_t *)(uintptr_t)XBT_JTAB_BASE)[b->guest_start] = 0;
    xbt_st.smc_invalidated++;
}

static void page_invalidate_all(uint32_t pg)
{
    plist *l = &pages[pg];
    for (int i = 0; i < l->n; i++)
        tombstone(l->v[i]);
    l->n = 0;
}

uint32_t *xbt_lookup_host(uint32_t eip)
{
    xbt_block *b = lookup(eip);
    return b ? b->host : NULL;
}

void xbt_set_hle_handler(xbt_hle_fn fn) { hle_handler = fn; }

/* ------------------------------------------------------------------ */
/* Shared stubs, generated once at the start of the code cache         */
/* ------------------------------------------------------------------ */

static void gen_stubs(void)
{
    a64buf bb = { code_base, code_base, code_base + 1024, 0 };
    a64buf *b = &bb;
    const int r0 = CPUOFF(r);

    /* frame: x29,x30 | x19..x28 | host fpcr */
    xbt_stub.enter = b->p;
    x_stp_pre(b, 29, 30, SP, -112);
    x_mov_sp(b, 29, SP);
    x_stp(b, 19, 20, SP, 16);
    x_stp(b, 21, 22, SP, 32);
    x_stp(b, 23, 24, SP, 48);
    x_stp(b, 25, 26, SP, 64);
    x_stp(b, 27, 28, SP, 80);
    x_mov(b, RCPU, 0);
    x_mrs_fpcr(b, 9);
    x_str(b, 9, SP, 96);
    w_ldr(b, 9, RCPU, CPUOFF(fpcr));
    x_msr_fpcr(b, 9);
    for (int r = 0; r < 8; r += 2)
        w_ldp(b, GREG(r), GREG(r + 1), RCPU, r0 + 4 * r);
    w_ldr(b, RTOP, RCPU, CPUOFF(ftop));
    a64_br(b, 1);

    xbt_stub.exit_eip = b->p;
    w_str(b, 0, RCPU, CPUOFF(eip));
    for (int r = 0; r < 8; r += 2)
        w_stp(b, GREG(r), GREG(r + 1), RCPU, r0 + 4 * r);
    w_str(b, RTOP, RCPU, CPUOFF(ftop));
    x_mrs_fpcr(b, 9);
    w_str(b, 9, RCPU, CPUOFF(fpcr));
    x_ldr(b, 9, SP, 96);
    x_msr_fpcr(b, 9);
    x_ldp(b, 19, 20, SP, 16);
    x_ldp(b, 21, 22, SP, 32);
    x_ldp(b, 23, 24, SP, 48);
    x_ldp(b, 25, 26, SP, 64);
    x_ldp(b, 27, 28, SP, 80);
    x_ldp_post(b, 29, 30, SP, 112);
    a64_ret(b);

    /* reached with `bl` from a per-exit stub: [x30] = target, [x30+4] = site */
    xbt_stub.link_handler = b->p;
    w_ldr(b, 0, 30, 0);
    w_ldr(b, 1, 30, 4);
    w_str(b, 1, RCPU, CPUOFF(link_site));
    w_movi(b, 2, EXIT_LINK);
    w_str(b, 2, RCPU, CPUOFF(exit_reason));
    a64_put(b, enc_b((intptr_t)xbt_stub.exit_eip - (intptr_t)b->p));

    xbt_stub.indirect_miss = b->p;
    w_movi(b, 2, EXIT_INDIRECT);
    w_str(b, 2, RCPU, CPUOFF(exit_reason));
    a64_put(b, enc_b((intptr_t)xbt_stub.exit_eip - (intptr_t)b->p));

    xbt_stub.special_exit = b->p;
    w_str(b, 1, RCPU, CPUOFF(exit_reason));
    w_str(b, 2, RCPU, CPUOFF(exit_info));
    a64_put(b, enc_b((intptr_t)xbt_stub.exit_eip - (intptr_t)b->p));

    code_blocks = b->p + 16;
    __builtin___clear_cache((char *)code_base, (char *)b->p);
}

/* ------------------------------------------------------------------ */
/* Cache management                                                    */
/* ------------------------------------------------------------------ */

static void free_blocks(void)
{
    for (unsigned i = 0; i < (1u << HASH_BITS); i++) {
        xbt_block *b = hash_tab[i];
        while (b) {
            xbt_block *n = b->next;
            free(b->insn_off);
            free(b->insn_eip);
            free(b);
            b = n;
        }
        hash_tab[i] = NULL;
    }
}

void xbt_flush(const char *why)
{
    if (xbt_opt.trace)
        XBT_LOG("flushing code cache (%s)\n", why);
    free_blocks();
    for (uint32_t i = 0; i < MAX_PAGES; i++)
        pages[i].n = 0;
    code_ptr = code_blocks;
    madvise((void *)(uintptr_t)XBT_JTAB_BASE, (size_t)XBT_JTAB_RANGE * 4, MADV_DONTNEED);
    mem_unprotect_all_code();
    generation++;
    xbt_st.flushes++;
}

static void protect_block(xbt_block *blk)
{
    for (uint32_t pg = blk->guest_start & ~0xfffu; pg < blk->guest_end; pg += 0x1000) {
        int64_t phys = mem_guest_to_phys(pg);
        if (phys < 0)
            continue;
        page_add((uint32_t)phys >> 12, blk);
        if (!blk->selfcheck)
            mem_protect_code_page(pg);
    }
}

static xbt_block *get_block(uint32_t eip)
{
    xbt_block *blk = lookup(eip);
    if (blk)
        return blk;
    blk = xbt_translate(eip, &code_ptr, code_end);
    if (!blk) {
        if (!mem_readable(eip, 1))
            return NULL;
        xbt_flush("code cache full");
        blk = xbt_translate(eip, &code_ptr, code_end);
        if (!blk)
            return NULL;
    }
    uint32_t h = hash_eip(eip);
    blk->next = hash_tab[h];
    hash_tab[h] = blk;
    if (eip < XBT_JTAB_RANGE)
        ((uint32_t *)(uintptr_t)XBT_JTAB_BASE)[eip] = (uint32_t)(uintptr_t)blk->host;
    protect_block(blk);
    return blk;
}

static xbt_block *find_block_by_host(uintptr_t pc)
{
    for (unsigned i = 0; i < (1u << HASH_BITS); i++)
        for (xbt_block *b = hash_tab[i]; b; b = b->next)
            if (pc >= (uintptr_t)b->host && pc < (uintptr_t)(b->host + b->host_words))
                return b;
    return NULL;
}

static xbt_mmio_fn mmio_handler;
static uint32_t mmio_start, mmio_end;

void xbt_set_mmio_handler(xbt_mmio_fn fn, uint32_t start, uint32_t end)
{
    mmio_handler = fn;
    mmio_start = start;
    mmio_end = end;
}

/* Emulate the AArch64 load/store at pc that touched a trapped MMIO page. */
static int emulate_mmio(ucontext_t *uc, uint32_t addr)
{
    uint32_t ins = *(uint32_t *)(uintptr_t)uc->uc_mcontext.pc;
    if ((ins & 0x3b000000u) != 0x39000000u && (ins & 0x3b200c00u) != 0x38000000u &&
        (ins & 0x3b200c00u) != 0x38200800u)
        return 0;                       /* not a plain GP load/store */
    int size = 1 << (ins >> 30);
    int opc = (ins >> 22) & 3;
    int rt = ins & 31;
    uint64_t v = 0;
    if (opc == 0) {
        v = rt == 31 ? 0 : uc->uc_mcontext.regs[rt];
        if (size < 8)
            v &= (1ull << (8 * size)) - 1;
        if (!mmio_handler(addr, size, 1, &v))
            return 0;
    } else {
        if (!mmio_handler(addr, size, 0, &v))
            return 0;
        if (opc == 2 || opc == 3) {
            int sh = 64 - 8 * size;
            v = (uint64_t)((int64_t)(v << sh) >> sh);
            if (opc == 3)
                v &= 0xffffffffu;
        }
        if (rt != 31)
            uc->uc_mcontext.regs[rt] = v;
    }
    uc->uc_mcontext.pc += 4;
    return 1;
}

/*
 * A write hit a write-protected page holding translated code. Decode the
 * store, invalidate only the blocks whose guest bytes it overlaps, perform
 * the write through the RAM alias and skip the instruction. Pages that keep
 * receiving plain data writes (code and data sharing a page, common in XDK
 * library sections) become "volatile": unprotected, with self-checking
 * blocks. Returns 1 if handled (instruction completed or retry is safe).
 */
static int smc_fault(ucontext_t *uc, uint32_t fault)
{
    uint32_t ins = *(uint32_t *)(uintptr_t)uc->uc_mcontext.pc;
    uint64_t *x = (uint64_t *)uc->uc_mcontext.regs;
    uint64_t sp = uc->uc_mcontext.sp;
    int64_t fphys = mem_guest_to_phys(fault);
    if (fphys < 0)
        return 0;
    uint32_t fpg = (uint32_t)fphys >> 12;
#define XREG(r) ((r) == 31 ? 0 : x[r])
#define XBASE(r) ((r) == 31 ? sp : x[r])
    uint8_t data[16];
    int size = 0, wb = 0, rn = 0;
    uint64_t ea = 0, newbase = 0;
    int ok = 1;
    if ((ins & 0x3fc00000u) == 0x39000000u) {               /* STR (unsigned imm) */
        size = 1 << (ins >> 30);
        rn = (ins >> 5) & 31;
        ea = XBASE(rn) + (((ins >> 10) & 0xfff) << (ins >> 30));
        uint64_t v = XREG(ins & 31);
        memcpy(data, &v, 8);
    } else if ((ins & 0x3fe00000u) == 0x38000000u && ((ins >> 10) & 3) != 2) {  /* STUR / pre / post */
        size = 1 << (ins >> 30);
        rn = (ins >> 5) & 31;
        int64_t imm = (int64_t)((int32_t)(ins << 11) >> 23);
        int mode = (ins >> 10) & 3;
        uint64_t base = XBASE(rn);
        ea = mode == 1 ? base : base + (uint64_t)imm;
        if (mode == 1 || mode == 3) {
            wb = 1;
            newbase = base + (uint64_t)imm;
        }
        uint64_t v = XREG(ins & 31);
        memcpy(data, &v, 8);
    } else if ((ins & 0x3fe00c00u) == 0x38200800u) {          /* STR (register) */
        size = 1 << (ins >> 30);
        rn = (ins >> 5) & 31;
        int rm = (ins >> 16) & 31, opt = (ins >> 13) & 7, sh = (ins >> 12) & 1;
        uint64_t off = XREG(rm);
        if (opt == 2)
            off &= 0xffffffffu;
        else if (opt == 6)
            off = (uint64_t)(int64_t)(int32_t)off;
        ea = XBASE(rn) + (sh ? off << (ins >> 30) : off);
        uint64_t v = XREG(ins & 31);
        memcpy(data, &v, 8);
    } else if ((ins & 0x7e400000u) == 0x28000000u) {           /* STP (32/64) */
        int is64 = ins >> 31;
        int esz = is64 ? 8 : 4;
        size = 2 * esz;
        rn = (ins >> 5) & 31;
        int64_t imm = (int64_t)((int32_t)(ins << 10) >> 25) * esz;
        int mode = (ins >> 23) & 3;
        uint64_t base = XBASE(rn);
        ea = mode == 1 ? base : base + (uint64_t)imm;
        if (mode == 1 || mode == 3) {
            wb = 1;
            newbase = base + (uint64_t)imm;
        }
        uint64_t a = XREG(ins & 31), b2 = XREG((ins >> 10) & 31);
        memcpy(data, &a, (size_t)esz);
        memcpy(data + esz, &b2, (size_t)esz);
    } else {
        ok = 0;
    }
#undef XREG
#undef XBASE
    int64_t phys = ok ? mem_guest_to_phys((uint32_t)ea) : -1;
    if (!ok || phys < 0 || ea >= 0x100000000ull || mem_guest_to_phys((uint32_t)ea + (uint32_t)size - 1) < 0 ||
        (uint32_t)phys + (uint32_t)size > mem_ram_size()) {
        /* unknown store (host C code, SIMD...): invalidate the page and retry */
        page_invalidate_all(fpg);
        mem_unprotect_page(fpg);
        return 1;
    }
    /* invalidate blocks whose guest bytes overlap the write */
    uint32_t p0 = (uint32_t)phys, p1 = p0 + (uint32_t)size;
    int overlap = 0;
    for (uint32_t pg = p0 >> 12; pg <= (p1 - 1) >> 12; pg++) {
        plist *l = &pages[pg];
        for (int i = 0; i < l->n; i++) {
            xbt_block *b = l->v[i];
            int64_t bs = mem_guest_to_phys(b->guest_start);
            if (bs < 0)
                continue;
            uint32_t s0 = (uint32_t)bs, s1 = s0 + (b->guest_end - b->guest_start);
            if (p0 < s1 && s0 < p1 && !b->dead) {
                tombstone(b);
                overlap = 1;
            }
        }
    }
    memcpy(mem_alias((uint32_t)phys), data, (size_t)size);
    if (wb)
        uc->uc_mcontext.regs[rn] = newbase;   /* rn == 31 (sp) never targets guest code */
    uc->uc_mcontext.pc += 4;
    if (!overlap) {
        xbt_st.smc_data_writes++;
        if (++page_data_faults[fpg] >= VOLATILE_THRESHOLD) {
            /* code and hot data share this page: stop trapping, self-check instead */
            page_invalidate_all(fpg);
            mem_set_page_volatile(fpg);
            mem_unprotect_page(fpg);
            xbt_st.smc_volatile_pages++;
        }
    }
    return 1;
}

static void segv_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t addr = (uintptr_t)si->si_addr;
    if (mmio_handler && addr >= mmio_start && addr < mmio_end &&
        emulate_mmio((ucontext_t *)ucv, (uint32_t)addr))
        return;
    if (addr < 0x100000000ull && mem_is_code_page_fault((uint32_t)addr)) {
        xbt_st.smc_faults++;
        if (smc_fault((ucontext_t *)ucv, (uint32_t)addr))
            return;
    }
    ucontext_t *uc = ucv;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
    char msg[256];
    int n;
    if (pc >= (uintptr_t)code_base && pc < (uintptr_t)code_end) {
        xbt_block *b = find_block_by_host(pc);
        uint32_t geip = 0;
        if (b) {
            uint32_t off = (uint32_t)((pc - (uintptr_t)b->host) / 4);
            for (int i = 0; i < b->ninsn; i++)
                if (b->insn_off[i] <= off)
                    geip = b->insn_eip[i];
        }
        n = snprintf(msg, sizeof msg,
                     "[xbt] guest memory fault at 0x%08lx (guest eip 0x%08x)\n",
                     (unsigned long)addr, geip);
    } else {
        n = snprintf(msg, sizeof msg, "[xbt] host fault at 0x%lx (pc 0x%lx)\n",
                     (unsigned long)addr, (unsigned long)pc);
    }
    if (write(2, msg, (size_t)n) < 0) {
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int xbt_runtime_init(void)
{
    code_base = (uint32_t *)(uintptr_t)XBT_CODE_BASE;
    code_end = code_base + XBT_CODE_SIZE / 4;
    gen_stubs();
    code_ptr = code_blocks;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    return 0;
}

extern void xbt_helpers_install(xbt_cpu *cpu);

void xbt_cpu_init(xbt_cpu *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
    cpu->eflags = 0x202;
    cpu->fcw = 0x037f;
    cpu->ftw = 0xff;
    cpu->mxcsr = 0x1f80;
    cpu->stop_eip = 0xffffffffu;
    for (int i = 0; i < 256; i++)
        cpu->parity[i] = (__builtin_popcount((unsigned)i) & 1) ? 0 : 1;
    xbt_helpers_install(cpu);
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

int xbt_run(xbt_cpu *cpu)
{
    enter_fn enter = (enter_fn)(void *)xbt_stub.enter;
    uint64_t link_gen = generation;
    current_cpu = cpu;
    cpu->exit_reason = EXIT_NONE;
    cpu->link_site = 0;
    for (;;) {
        uint32_t eip = cpu->eip;
        if (eip == cpu->stop_eip)
            return EXIT_STOP;
        if (eip >= XBOX_HLE_BASE && eip < XBOX_HLE_THUNK_END) {
            int r = hle_handler ? hle_handler(cpu, eip) : -1;
            cpu->exit_reason = EXIT_NONE;
            if (r < 0)
                return EXIT_UD;
            if (r > 0)
                return EXIT_YIELD;
            continue;
        }
        xbt_block *blk = get_block(eip);
        if (!blk) {
            XBT_LOG("cannot execute at 0x%08x (unmapped)\n", eip);
            return EXIT_UD;
        }
        if (cpu->exit_reason == EXIT_LINK && cpu->link_site && link_gen == generation &&
            xbt_opt.link_blocks) {
            uint32_t *site = (uint32_t *)(uintptr_t)cpu->link_site;
            if (a64_b_in_range(site, blk->host)) {
                a64_patch_b(site, blk->host);
                __builtin___clear_cache((char *)site, (char *)(site + 1));
                xbt_st.links++;
            }
        }
        link_gen = generation;
        cpu->exit_reason = EXIT_NONE;
        cpu->link_site = 0;
        xbt_st.dispatches++;
        enter(cpu, blk->host);
        switch (cpu->exit_reason) {
        case EXIT_LINK:
            xbt_st.exits_link++;
            break;
        case EXIT_INDIRECT:
            xbt_st.exits_indirect++;
            break;
        case EXIT_SMC: {
            /* a self-checking block saw modified guest code */
            xbt_block *b = lookup(cpu->eip);
            if (b)
                tombstone(b);
            break;
        }
        case EXIT_PENDING:
            cpu->pending = 0;
            xbt_st.exits_other++;
            if (xbt_preempt) {
                xbt_preempt = 0;
                return EXIT_PENDING;
            }
            break;
        default:
            xbt_st.exits_other++;
            return (int)cpu->exit_reason;
        }
    }
}

void xbt_print_stats(void)
{
    xbt_stats *s = &xbt_st;
    fprintf(stderr, "---- xbt statistics ----\n");
    fprintf(stderr, "blocks translated     %llu (%llu guest insns -> %llu host insns, %.2f per guest insn)\n",
            (unsigned long long)s->blocks, (unsigned long long)s->guest_insns,
            (unsigned long long)s->host_insns,
            s->guest_insns ? (double)s->host_insns / (double)s->guest_insns : 0.0);
    fprintf(stderr, "flag producers        %llu: %llu eliminated, %llu via NZCV only, %llu stored\n",
            (unsigned long long)s->flag_producers, (unsigned long long)s->flag_elided,
            (unsigned long long)s->flag_fused_only, (unsigned long long)s->flag_stored);
    fprintf(stderr, "flag consumers        %llu: %llu read NZCV directly, %llu via helper\n",
            (unsigned long long)s->consumers, (unsigned long long)s->consumers_fused,
            (unsigned long long)s->consumers_helper);
    fprintf(stderr, "dispatcher entries    %llu (link exits %llu, indirect misses %llu, other %llu)\n",
            (unsigned long long)s->dispatches, (unsigned long long)s->exits_link,
            (unsigned long long)s->exits_indirect, (unsigned long long)s->exits_other);
    fprintf(stderr, "blocks linked         %llu, cache flushes %llu\n",
            (unsigned long long)s->links, (unsigned long long)s->flushes);
    fprintf(stderr, "code page writes      %llu faults: %llu data writes emulated, %llu blocks invalidated, "
            "%llu volatile pages\n", (unsigned long long)s->smc_faults,
            (unsigned long long)s->smc_data_writes, (unsigned long long)s->smc_invalidated,
            (unsigned long long)s->smc_volatile_pages);
}
