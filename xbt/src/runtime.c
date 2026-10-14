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
static volatile sig_atomic_t flush_pending;
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
        if (b->guest_start == eip)
            return b;
    return NULL;
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

    xbt_stub.enter = b->p;
    x_stp_pre(b, 29, 30, SP, -96);
    x_mov_sp(b, 29, SP);
    x_stp(b, 19, 20, SP, 16);
    x_stp(b, 21, 22, SP, 32);
    x_stp(b, 23, 24, SP, 48);
    x_stp(b, 25, 26, SP, 64);
    x_stp(b, 27, 28, SP, 80);
    x_mov(b, RCPU, 0);
    for (int r = 0; r < 8; r += 2)
        w_ldp(b, GREG(r), GREG(r + 1), RCPU, r0 + 4 * r);
    w_ldr(b, RTOP, RCPU, CPUOFF(ftop));
    a64_br(b, 1);

    xbt_stub.exit_eip = b->p;
    w_str(b, 0, RCPU, CPUOFF(eip));
    for (int r = 0; r < 8; r += 2)
        w_stp(b, GREG(r), GREG(r + 1), RCPU, r0 + 4 * r);
    w_str(b, RTOP, RCPU, CPUOFF(ftop));
    x_ldp(b, 19, 20, SP, 16);
    x_ldp(b, 21, 22, SP, 32);
    x_ldp(b, 23, 24, SP, 48);
    x_ldp(b, 25, 26, SP, 64);
    x_ldp(b, 27, 28, SP, 80);
    x_ldp_post(b, 29, 30, SP, 96);
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
    code_ptr = code_blocks;
    madvise((void *)(uintptr_t)XBT_JTAB_BASE, (size_t)XBT_JTAB_RANGE * 4, MADV_DONTNEED);
    mem_unprotect_all_code();
    generation++;
    xbt_st.flushes++;
}

static void protect_block(xbt_block *blk)
{
    for (uint32_t pg = blk->guest_start & ~0xfffu; pg < blk->guest_end; pg += 0x1000)
        mem_protect_code_page(pg);
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

static void segv_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t addr = (uintptr_t)si->si_addr;
    extern int mem_is_code_page_fault(uint32_t addr);
    if (addr < 0x100000000ull && mem_is_code_page_fault((uint32_t)addr)) {
        /* write to a page holding translated code: drop all translations */
        xbt_st.smc_faults++;
        mem_unprotect_all_code();
        flush_pending = 1;
        if (current_cpu)
            current_cpu->pending = 1;
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
        if (flush_pending) {
            flush_pending = 0;
            xbt_flush("self-modifying code");
        }
        uint32_t eip = cpu->eip;
        if (eip == cpu->stop_eip)
            return EXIT_STOP;
        if (eip >= XBOX_HLE_BASE && eip < XBOX_HLE_THUNK_END) {
            if (!hle_handler || hle_handler(cpu, eip) < 0)
                return EXIT_UD;
            cpu->exit_reason = EXIT_NONE;
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
    fprintf(stderr, "blocks linked         %llu, cache flushes %llu, SMC faults %llu\n",
            (unsigned long long)s->links, (unsigned long long)s->flushes,
            (unsigned long long)s->smc_faults);
}
