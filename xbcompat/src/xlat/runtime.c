/*
 * cpu.h on xlat: host functions the guest calls, guest functions the host
 * calls, and the per-thread guest CPUs.
 *
 * Host functions are reached through stubs, as in the Unicorn backend: a
 * 64 KB page in guest memory whose even offsets index the stub table.  When
 * a thread's eip lands on one, the function's adapter reads the arguments
 * where the x86 calling convention put them, the result goes to eax/edx (or
 * st0), the callee's arguments are popped and execution resumes at the
 * return address.  cpu_call() pushes the arguments and a return address
 * just past the stubs and runs the guest until it gets there; host code
 * the guest called may call the guest again (callbacks), which nests.
 *
 * Every host thread running guest code has its own guest CPU, and they all
 * run at once, like the native build's threads: nothing here serializes
 * them, so cpu_block() has nothing to do.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xbcompat.h"
#include "cpu.h"
#include "xlat/xlat.h"

#define STUB_MAX   0xF000u            /* stub page: offsets 0..STUB_MAX-1 hold stubs */
#define STUB_COUNT (STUB_MAX / 2)
#define STUB_PAGE  0x10000u
#define RET_OFFSET 0xFF00u            /* cpu_call's return address */
#define GUEST_STACK_DEFAULT (256u << 10)

/* ---- registry of host functions -------------------------------------- */

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static const struct xa_entry **entries;
static size_t nentries, entries_cap;

void xa_register(const struct xa_entry *tbl, size_t n)
{
    pthread_mutex_lock(&reg_lock);
    for (size_t i = 0; i < n; i++) {
        if (nentries == entries_cap) {
            entries_cap = entries_cap ? entries_cap * 2 : 1024;
            entries = realloc(entries, entries_cap * sizeof(*entries));
        }
        entries[nentries++] = &tbl[i];
    }
    pthread_mutex_unlock(&reg_lock);
}

static const struct xa_entry *find_entry(const void *host)
{
    for (size_t i = 0; i < nentries; i++)
        if (entries[i]->host == host) return entries[i];
    return NULL;
}

/* ---- stubs ----------------------------------------------------------- */

struct stub {
    const struct xa_entry *e;
    int nspec;                 /* > 0: arguments per a regs= spec */
    int8_t spec[16];           /* register index (XR_*) or -1 for the next stack word */
    int spec_stack;            /* stack words the spec consumes */
    void *ctx;
};

static uint8_t *stub_page;
static uint32_t stub_base;
static struct stub stubs[STUB_COUNT];
static unsigned nstubs;
static pthread_mutex_t stub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ret_addr;

static unsigned stub_new(const struct xa_entry *e, void *ctx)
{
    pthread_mutex_lock(&stub_lock);
    if (nstubs == STUB_COUNT) fatal("out of guest stubs");
    unsigned i = nstubs;
    if (getenv("XBCOMPAT_LOG_STUBS")) xlog("stub %u %08x %s", i, stub_base + 2 * i, e->name ? e->name : "?");
    stubs[i].e = e;
    stubs[i].ctx = ctx;
    __atomic_store_n(&nstubs, i + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&stub_lock);
    return i;
}

static uint32_t stub_addr(unsigned i) { return stub_base + 2 * i; }

static bool is_stub(uint32_t a, unsigned *idx)
{
    if (!stub_page || a - stub_base >= 2 * nstubs || (a & 1)) return false;
    if (idx) *idx = (a - stub_base) / 2;
    return true;
}

uint32_t cpu_guest_entry(void *fn)
{
    if (!fn) return 0;
    if (is_stub((uint32_t)(uintptr_t)fn, NULL)) return (uint32_t)(uintptr_t)fn;
    const struct xa_entry *e = find_entry(fn);
    if (!e) return (uint32_t)(uintptr_t)fn;   /* guest code (or data), already callable */
    pthread_mutex_lock(&stub_lock);
    for (unsigned i = 0; i < nstubs; i++)
        if (stubs[i].e == e && !stubs[i].nspec && !stubs[i].ctx) {
            pthread_mutex_unlock(&stub_lock);
            return stub_addr(i);
        }
    pthread_mutex_unlock(&stub_lock);
    return stub_addr(stub_new(e, NULL));
}

/* Register order for regs= specs, as hle.c names them. */
static const char *const spec_names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

uint32_t cpu_guest_entry_regs(void *fn, const char *spec)
{
    const struct xa_entry *e = find_entry(fn);
    if (!e) return 0;
    struct stub s = { .e = e };
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char *save, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        int words = !strcmp(t, "s") ? 1 : !strcmp(t, "s2") ? 2 : 0;
        if (s.nspec + (words ? words : 1) > 16) return 0;
        if (words) {
            for (int k = 0; k < words; k++) s.spec[s.nspec++] = -1;
            s.spec_stack += words;
            continue;
        }
        int r = -1;
        for (int k = 0; k < 8; k++) if (!strcmp(t, spec_names[k]) && k != XR_ESP) r = k;
        if (r < 0) return 0;
        s.spec[s.nspec++] = (int8_t)r;
    }
    if (!s.nspec) s.nspec = -1;   /* "no arguments": still a spec stub */
    unsigned i = stub_new(e, NULL);
    stubs[i] = s;
    return stub_addr(i);
}

static uint64_t trap_adapter(struct xa_frame *f)
{
    fatal("guest called %s from %#x", (const char *)f->ctx, f->ret);
}

static const struct xa_entry trap_entry = { NULL, trap_adapter, CONV_CDECL, "trap" };

uint32_t cpu_guest_trap(const char *msg)
{
    return stub_addr(stub_new(&trap_entry, (void *)msg));
}

void cpu_guest_table(void **tbl, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!find_entry(tbl[i])) xlog("cpu: guest table entry %p is not a known host function", tbl[i]);
        tbl[i] = (void *)(uintptr_t)cpu_guest_entry(tbl[i]);
    }
}

/* ---- per-thread guest CPUs ------------------------------------------- */

struct xlat_thread {
    xcpu cpu;
    int depth;                 /* cpu_call levels running guest code */
    uint32_t stack_top;        /* guest stack for calls at depth 0 */
    uint32_t fs_base;
};

static __thread struct xlat_thread *cur;

static struct xlat_thread *thread_cpu(void)
{
    if (cur) return cur;
    struct xlat_thread *t = aligned_alloc(64, (sizeof(*t) + 63) & ~(size_t)63);
    memset(t, 0, sizeof(*t));
    xi_reset(&t->cpu);
    cur = t;
    return t;
}

void cpu_thread_attach(void *pcr, void *stack_limit, void *stack_base)
{
    struct xlat_thread *t = thread_cpu();
    (void)stack_limit;
    t->stack_top = (uint32_t)(uintptr_t)stack_base;
    t->fs_base = (uint32_t)(uintptr_t)pcr;
    t->cpu.fs_base = t->fs_base;
}

void cpu_thread_detach(void)
{
    /* The thread may be mid-call (PsTerminateSystemThread longjmps out of a
       host function); it is ending, so its CPU is just let go. */
    cur = NULL;
}

void cpu_code_changed(uint32_t addr, uint32_t len)
{
#ifdef XLAT_JIT
    jit_invalidate(addr, len);
#else
    (void)addr; (void)len;
#endif
}

void cpu_block(void) {}

int cpu_fix_fault(int sig, void *ucontext)
{
#if defined(XLAT_JIT) && defined(__arm__)
    return sig == SIGBUS && jit_fix_unaligned(ucontext);
#else
    (void)sig;
    (void)ucontext;
    return 0;
#endif
}

/* ---- services --------------------------------------------------------- */

uint64_t xrt_rdtsc(xcpu *c)
{
    (void)c;
    g_guest_traps++;
    return ke_guest_tsc();
}

void xrt_cpuid(xcpu *c)
{
    /* The Xbox's CPU: a 733 MHz Pentium III (Coppermine), MMX and SSE. */
    uint32_t leaf = c->r[XR_EAX], a = 0, b = 0, cx = 0, d = 0;
    switch (leaf) {
    case 0: a = 2; b = 0x756E6547; d = 0x49656E69; cx = 0x6C65746E; break;   /* GenuineIntel */
    case 1: a = 0x683; d = 0x0383F9FF; break;
    case 2: a = 0x03020101; d = 0x0C040882; break;
    }
    c->r[XR_EAX] = a;
    c->r[XR_EBX] = b;
    c->r[XR_ECX] = cx;
    c->r[XR_EDX] = d;
}

void xrt_interrupt(xcpu *c, unsigned n, uint32_t next)
{
    if (n == 0x2D) {
        /* The kernel debugger service (DebugService in the NT CRT): eax =
           service, ecx/edx = arguments, followed by an int 3 the kernel
           skips.  xapilib's OutputDebugString uses it. */
        uint32_t service = c->r[XR_EAX];
        if (service == 1 /* BREAKPOINT_PRINT */) {
            const ANSI_STRING *s = (const ANSI_STRING *)(uintptr_t)c->r[XR_ECX];
            if (s && s->Buffer) {
                int len = s->Length;
                while (len && (s->Buffer[len - 1] == '\n' || s->Buffer[len - 1] == '\r')) len--;
                xlog("[guest] %.*s", len, s->Buffer);
            }
        } else if (service == 2 /* BREAKPOINT_PROMPT */) {
            c->r[XR_EAX] = 0;
        }
        if (rd8(next) == 0xCC) next++;
        c->eip = next;
        return;
    }
    if (n == 3) {
        xlog("breakpoint at eip=%08x ignored", c->eip);
        c->eip = next;
        return;
    }
    char what[32];
    snprintf(what, sizeof(what), "guest interrupt %#x", n);
    xrt_fault(c, what);
}

void xrt_hlt(xcpu *c)
{
    (void)c;
    usleep(1000);   /* waiting for an interrupt */
}

void xrt_fault(xcpu *c, const char *what)
{
    xinsn in;
    char text[96];
    x86_decode(&in, c->eip);
    x86_format(&in, text, sizeof(text));
    xlog("cpu: %s at %s", what, text);
    cpu_dump_guest();
    fatal("guest fault: %s at eip=%08x", what, c->eip);
}

void cpu_dump_guest(void)
{
    struct xlat_thread *t = cur;
    if (!t || !t->depth) return;
    const xcpu *c = &t->cpu;
    uint32_t esp = c->r[XR_ESP];
    xlog("  guest eip=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x efl=%08x",
         c->eip, c->r[XR_EAX], c->r[XR_EBX], c->r[XR_ECX], c->r[XR_EDX], c->r[XR_ESI], c->r[XR_EDI],
         c->r[XR_EBP], esp, xi_get_eflags(c));
    unsigned si;
    if (is_stub(c->eip, &si)) xlog("  in host function %s", stubs[si].e->name ? stubs[si].e->name : "?");
    if (esp > 0x10000) {
        const uint32_t *sp = (const uint32_t *)(uintptr_t)esp;
        xlog("  guest stack: %08x %08x %08x %08x %08x %08x %08x %08x", sp[0], sp[1], sp[2], sp[3], sp[4],
             sp[5], sp[6], sp[7]);
    }
    uint32_t ebp = c->r[XR_EBP];
    char chain[160];
    int n = 0;
    for (int i = 0; i < 8 && ebp > 0x10000 && ebp < 0xF0000000u && !(ebp & 3) && n < (int)sizeof(chain) - 10; i++) {
        const uint32_t *fp = (const uint32_t *)(uintptr_t)ebp;
        n += snprintf(chain + n, sizeof(chain) - n, " %08x", fp[1]);
        if (fp[0] <= ebp) break;
        ebp = fp[0];
    }
    if (n) xlog("  guest callers:%s", chain);
}

/* ---- guest -> host ----------------------------------------------------- */

static void run_stub(struct xlat_thread *t, unsigned idx)
{
    xcpu *c = &t->cpu;
    const struct stub *s = &stubs[idx];
    uint32_t esp = c->r[XR_ESP];
    const uint32_t *sp = (const uint32_t *)(uintptr_t)esp;
    struct xa_frame f = { .stack = sp + 1, .ret = sp[0], .ctx = s->ctx };
    uint32_t pop;
    uint64_t r;
    if (s->nspec) {
        uint32_t args[16];
        int sw = 0;
        for (int i = 0; i < s->nspec; i++) args[i] = s->spec[i] < 0 ? f.stack[sw++] : c->r[s->spec[i]];
        f.stack = args;
        r = s->e->adapter(&f);
        pop = s->spec_stack * 4;
    } else {
        if (s->e->conv == CONV_FAST) {
            f.fast = 1;
            f.ecx = c->r[XR_ECX];
            f.edx = c->r[XR_EDX];
        }
        r = s->e->adapter(&f);
        pop = s->e->conv == CONV_CDECL ? 0 : f.used * 4;
    }
    if (f.fp) {
        xi_fpu_push(c, f.fret);
    } else {
        c->r[XR_EAX] = (uint32_t)r;
        c->r[XR_EDX] = (uint32_t)(r >> 32);
    }
    c->r[XR_ESP] = esp + 4 + pop;
    c->eip = f.ret;
}

static void run_guest(struct xlat_thread *t)
{
    xcpu *c = &t->cpu;
    for (;;) {
        uint32_t eip = c->eip;
        if (eip - stub_base < STUB_PAGE) {
            if (eip == ret_addr) return;
            unsigned idx;
            if (!is_stub(eip, &idx)) xrt_fault(c, "jump into the stub page");
            run_stub(t, idx);
            continue;
        }
#ifdef XLAT_JIT
        jit_run(c);   /* until it reaches the stub page */
#else
        if (xi_step(c) != XI_OK) xrt_fault(c, "invalid instruction");
#endif
    }
}

/* ---- host -> guest ----------------------------------------------------- */

uint64_t cpu_call(uint32_t fn, int conv, int n, const uint32_t *args)
{
    /* A host function (or its stub): call its adapter directly. */
    unsigned idx;
    const struct xa_entry *e = NULL;
    if (is_stub(fn, &idx) && !stubs[idx].nspec) e = stubs[idx].e;
    else if (!is_stub(fn, NULL)) e = find_entry((void *)(uintptr_t)fn);
    if (e) {
        struct xa_frame f = { .stack = args, .ctx = is_stub(fn, &idx) ? stubs[idx].ctx : NULL };
        if (e->conv == CONV_FAST) {
            f.fast = 1;
            f.ecx = n > 0 ? args[0] : 0;
            f.edx = n > 1 ? args[1] : 0;
            f.stack = args + (n > 2 ? 2 : n);
        }
        return e->adapter(&f);
    }

    struct xlat_thread *t = thread_cpu();
    xcpu *c = &t->cpu;
    uint32_t saved_r[8], saved_eip = 0, saved_eflags = 0;
    if (t->depth) {
        memcpy(saved_r, c->r, sizeof(saved_r));
        saved_eip = c->eip;
        saved_eflags = xi_get_eflags(c);
    }
    if (!t->stack_top) t->stack_top = (uint32_t)(uintptr_t)arena_alloc(GUEST_STACK_DEFAULT, 1) + GUEST_STACK_DEFAULT;
    /* Below the interrupted guest code's stack, or at the top of this
       thread's own guest stack. */
    uint32_t esp = t->depth ? (c->r[XR_ESP] - 64) & ~15u : t->stack_top - 16;
    int sargs = n;
    if (conv == CONV_FAST) {
        c->r[XR_ECX] = n > 0 ? args[0] : 0;
        c->r[XR_EDX] = n > 1 ? args[1] : 0;
        sargs = n > 2 ? n - 2 : 0;
        args += n - sargs;
    }
    esp -= 4 * (sargs + 1);
    uint32_t *sp = (uint32_t *)(uintptr_t)esp;
    sp[0] = ret_addr;
    for (int i = 0; i < sargs; i++) sp[1 + i] = args[i];
    c->r[XR_ESP] = esp;
    if (!t->depth) xi_set_eflags(c, 0x202);   /* IF, reserved bit; DF clear */
    c->fs_base = t->fs_base;
    c->eip = fn;
    t->depth++;
    run_guest(t);
    t->depth--;
    uint64_t r = c->r[XR_EAX] | (uint64_t)c->r[XR_EDX] << 32;
    if (t->depth) {
        memcpy(c->r, saved_r, sizeof(saved_r));
        c->eip = saved_eip;
        xi_set_eflags(c, saved_eflags);
    }
    return r;
}

void cpu_init(void)
{
    stub_page = arena_alloc(STUB_PAGE, 1);
    stub_base = (uint32_t)(uintptr_t)stub_page;
    /* Each stub is "jmp $" (EB FE), the rest hlt, as in the Unicorn backend:
       nothing runs them, but guest code that looks finds code there. */
    memset(stub_page, 0xF4, STUB_PAGE);
    for (unsigned i = 0; i < STUB_MAX; i += 2) { stub_page[i] = 0xEB; stub_page[i + 1] = 0xFE; }
    ret_addr = stub_base + RET_OFFSET;
#ifdef XLAT_JIT
    /* Host functions don't read the guest's flags. */
    jit_init(stub_base, stub_base + STUB_PAGE, false);
    if (getenv("XLAT_STATS")) xbc_at_exit(jit_print_stats);
    xlog("cpu: x86 guest code runs in xlat, translated to ARM (%zu host functions)", nentries);
#else
    xlog("cpu: x86 guest code runs in xlat, the interpreter (%zu host functions)", nentries);
#endif
}
