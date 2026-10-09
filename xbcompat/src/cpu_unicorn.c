/*
 * Guest x86 code on a non-x86 host, through Unicorn (see cpu.h).
 *
 * Every host thread that runs guest code has its own Unicorn engine.  Each
 * engine maps the whole 32-bit address space onto the process's own memory,
 * one to one, so guest memory, the XBE image and host data the guest is
 * handed (kernel data exports, D3D state) are all where the guest expects.
 *
 * Host functions are reached through stubs: a 64 KB page of hlt bytes whose
 * offsets index the stub table.  A code hook over that page performs the call
 * (arguments read by the function's adapter), sets eax/edx, pops the
 * arguments as the callee would, and resumes at the return address.  Calls
 * the other way push the arguments and a return address just past the stub
 * page, and run the engine until it gets there.  Host code may call the guest
 * from inside a host function the guest called; Unicorn allows the nesting.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <unicorn/unicorn.h>

#include "xbcompat.h"
#include "cpu.h"

#define STUB_MAX   0xF000u            /* stub page: offsets 0..STUB_MAX-1 are stubs */
#define STUB_PAGE  0x10000u
#define RET_OFFSET 0xFF00u            /* cpu_call's return address, outside the hook range */
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
    int8_t spec[16];           /* register index (UC order below) or -1 for the next stack word */
    int spec_stack;            /* stack words the spec consumes */
    void *ctx;
};

static uint8_t *stub_page;
static struct stub stubs[STUB_MAX];
static unsigned nstubs;
static pthread_mutex_t stub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ret_addr;

static unsigned stub_new(const struct xa_entry *e, void *ctx)
{
    pthread_mutex_lock(&stub_lock);
    if (nstubs == STUB_MAX) fatal("out of guest stubs");
    unsigned i = nstubs++;
    stubs[i].e = e;
    stubs[i].ctx = ctx;
    pthread_mutex_unlock(&stub_lock);
    return i;
}

static uint32_t stub_addr(unsigned i) { return (uint32_t)(uintptr_t)stub_page + i; }

static bool is_stub(uint32_t a, unsigned *idx)
{
    uint32_t base = (uint32_t)(uintptr_t)stub_page;
    if (!stub_page || a < base || a >= base + nstubs) return false;
    if (idx) *idx = a - base;
    return true;
}

uint32_t cpu_guest_entry(void *fn)
{
    if (!fn) return 0;
    if (is_stub((uint32_t)(uintptr_t)fn, NULL)) return (uint32_t)(uintptr_t)fn;
    const struct xa_entry *e = find_entry(fn);
    if (!e) {
        /* Not a host function we know: guest code (or data), already callable. */
        return (uint32_t)(uintptr_t)fn;
    }
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
static const int spec_uc[] = { UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                               UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI };

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
        for (int k = 0; k < 8; k++) if (!strcmp(t, spec_names[k]) && k != 4) r = k;
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

/* ---- per-thread engines ---------------------------------------------- */

struct cpu_thread {
    uc_engine *uc;
    int depth;                 /* nested uc_emu_start calls in progress */
    uint32_t stack_top;        /* guest stack for calls at depth 0 */
    unsigned gen;              /* code generation the engine's translations match */
    bool stop_pending;
};

static __thread struct cpu_thread *cur;
static volatile unsigned code_gen;

static uint32_t rd(uc_engine *uc, int reg);
static void wr(uc_engine *uc, int reg, uint32_t v);
static void hook_stub(uc_engine *uc, uint64_t address, uint32_t size, void *user);
static void hook_intr(uc_engine *uc, uint32_t intno, void *user);
static int hook_rdtsc(uc_engine *uc, void *user);
static bool hook_invalid(uc_engine *uc, void *user);

static void check(uc_err e, const char *what)
{
    if (e != UC_ERR_OK) fatal("unicorn: %s: %s", what, uc_strerror(e));
}

static void set_fs_base(uc_engine *uc, uint32_t base)
{
    uc_x86_msr msr = { 0xC0000100 /* MSR_FS_BASE */, base };
    check(uc_reg_write(uc, UC_X86_REG_MSR, &msr), "fs base");
}

static struct cpu_thread *thread_engine(void)
{
    if (cur) return cur;
    struct cpu_thread *t = calloc(1, sizeof(*t));
    check(uc_open(UC_ARCH_X86, UC_MODE_32, &t->uc), "open");
    /* The Xbox CPU is a Pentium III (Coppermine): MMX and SSE, no SSE2. */
    uc_ctl_set_cpu_model(t->uc, UC_CPU_X86_PENTIUM3);
    uc_ctl_set_tcg_buffer_size(t->uc, 16u << 20);
    check(uc_mem_map_ptr(t->uc, 0x1000, 0xFFFFE000ull, UC_PROT_ALL, (void *)0x1000), "map");
    /* SSE on, as the Xbox kernel leaves it (CR4.OSFXSR|OSXMMEXCPT; CR0.MP, no EM). */
    uint32_t cr0 = rd(t->uc, UC_X86_REG_CR0), cr4 = rd(t->uc, UC_X86_REG_CR4);
    wr(t->uc, UC_X86_REG_CR0, (cr0 & ~4u) | 2u);
    wr(t->uc, UC_X86_REG_CR4, cr4 | 0x600u);
    uc_hook h;
    uint32_t base = (uint32_t)(uintptr_t)stub_page;
    check(uc_hook_add(t->uc, &h, UC_HOOK_CODE, hook_stub, t, base, base + STUB_MAX - 1), "stub hook");
    check(uc_hook_add(t->uc, &h, UC_HOOK_INTR, hook_intr, t, 1, 0), "interrupt hook");
    check(uc_hook_add(t->uc, &h, UC_HOOK_INSN, hook_rdtsc, t, 1, 0, UC_X86_INS_RDTSC), "rdtsc hook");
    check(uc_hook_add(t->uc, &h, UC_HOOK_INSN, hook_rdtsc, t, 1, 0, UC_X86_INS_RDTSCP), "rdtscp hook");
    check(uc_hook_add(t->uc, &h, UC_HOOK_INSN_INVALID, hook_invalid, t, 1, 0), "invalid hook");
    t->gen = code_gen;
    cur = t;
    return t;
}

void cpu_thread_attach(void *pcr, void *stack_limit, void *stack_base)
{
    struct cpu_thread *t = thread_engine();
    (void)stack_limit;
    t->stack_top = (uint32_t)(uintptr_t)stack_base;
    set_fs_base(t->uc, (uint32_t)(uintptr_t)pcr);
}

void cpu_thread_detach(void)
{
    /* The engine may be mid-call (PsTerminateSystemThread longjmps out of a
       hook), so it is left as it is; the thread is ending. */
    cur = NULL;
}

void cpu_code_changed(uint32_t addr, uint32_t len)
{
    (void)addr; (void)len;
    __sync_fetch_and_add(&code_gen, 1);
}

static void sync_code(struct cpu_thread *t)
{
    if (t->gen != code_gen) {
        t->gen = code_gen;
        uc_ctl_flush_tb(t->uc);
    }
}

/* ---- guest -> host ----------------------------------------------------- */

static uint32_t rd(uc_engine *uc, int reg)
{
    uint32_t v = 0;
    uc_reg_read(uc, reg, &v);
    return v;
}

static void wr(uc_engine *uc, int reg, uint32_t v) { uc_reg_write(uc, reg, &v); }

/* Push a floating point result on the x87 stack, where MSVC callers read it. */
static void push_st0(uc_engine *uc, double d)
{
    uint32_t sw = rd(uc, UC_X86_REG_FPSW);
    unsigned top = ((sw >> 11) - 1) & 7;
    sw = (sw & ~(7u << 11)) | (top << 11);
    /* double -> x87 extended */
    uint64_t bits;
    memcpy(&bits, &d, 8);
    uint16_t sign = (uint16_t)(bits >> 63) << 15;
    int exp = (int)((bits >> 52) & 0x7FF);
    uint64_t frac = bits & ((1ull << 52) - 1);
    struct { uint64_t mantissa; uint16_t exponent; } __attribute__((packed)) f = { 0, 0 };
    if (exp == 0 && frac == 0) {
        f.exponent = sign;
    } else if (exp == 0x7FF) {
        f.exponent = sign | 0x7FFF;
        f.mantissa = (1ull << 63) | (frac << 11);
    } else {
        if (exp == 0) {   /* subnormal double: normalize */
            exp = 1;
            while (!(frac & (1ull << 52))) { frac <<= 1; exp--; }
            frac &= (1ull << 52) - 1;
        }
        f.exponent = sign | (uint16_t)(exp - 1023 + 16383);
        f.mantissa = (1ull << 63) | (frac << 11);
    }
    uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
    uc_reg_write(uc, UC_X86_REG_FP0 + top, &f);
    uint32_t tag = rd(uc, UC_X86_REG_FPTAG);
    tag &= ~(3u << (top * 2));   /* valid */
    wr(uc, UC_X86_REG_FPTAG, tag);
}

static uint64_t run_entry(const struct stub *s, struct xa_frame *f, uint32_t *pop, uc_engine *uc)
{
    uint32_t args[16];
    if (s->nspec) {
        int sw = 0;
        for (int i = 0; i < s->nspec; i++)
            args[i] = s->spec[i] < 0 ? f->stack[sw++] : rd(uc, spec_uc[s->spec[i]]);
        f->stack = args;
        f->fast = 0;
        uint64_t r = s->e->adapter(f);
        *pop = s->spec_stack * 4;
        return r;
    }
    uint64_t r = s->e->adapter(f);
    *pop = s->e->conv == CONV_CDECL ? 0 : f->used * 4;
    return r;
}

static void hook_stub(uc_engine *uc, uint64_t address, uint32_t size, void *user)
{
    (void)size;
    struct cpu_thread *t = user;
    unsigned idx;
    if (!is_stub((uint32_t)address, &idx)) fatal("guest jumped into the stub page at %#x", (unsigned)address);
    const struct stub *s = &stubs[idx];
    uint32_t esp = rd(uc, UC_X86_REG_ESP);
    const uint32_t *sp = (const uint32_t *)(uintptr_t)esp;
    struct xa_frame f = { .stack = sp + 1, .ret = sp[0], .ctx = s->ctx };
    if (s->e->conv == CONV_FAST) {
        f.fast = 1;
        f.ecx = rd(uc, UC_X86_REG_ECX);
        f.edx = rd(uc, UC_X86_REG_EDX);
    }
    uint32_t pop = 0;
    uint64_t r = run_entry(s, &f, &pop, uc);
    if (f.fp) push_st0(uc, f.fret);
    else {
        wr(uc, UC_X86_REG_EAX, (uint32_t)r);
        wr(uc, UC_X86_REG_EDX, (uint32_t)(r >> 32));
    }
    wr(uc, UC_X86_REG_ESP, esp + 4 + pop);
    wr(uc, UC_X86_REG_EIP, f.ret);
    if (t->gen != code_gen) {
        /* Code was rewritten (a section load): drop our translations
           before running any more of it. */
        t->stop_pending = true;
        uc_emu_stop(uc);
    }
}

/* int 2Dh is the kernel debugger service (DebugService in the NT CRT):
   eax = service, ecx/edx = arguments, followed by an int 3 that the kernel
   skips.  xapilib's OutputDebugString uses it.  eip is past the int. */
static void debug_service(uc_engine *uc)
{
    uint32_t service = rd(uc, UC_X86_REG_EAX), eip = rd(uc, UC_X86_REG_EIP);
    if (service == 1 /* BREAKPOINT_PRINT */) {
        const ANSI_STRING *s = (const ANSI_STRING *)(uintptr_t)rd(uc, UC_X86_REG_ECX);
        if (s && s->Buffer) {
            int n = s->Length;
            while (n && (s->Buffer[n - 1] == '\n' || s->Buffer[n - 1] == '\r')) n--;
            xlog("[guest] %.*s", n, s->Buffer);
        }
    } else if (service == 2 /* BREAKPOINT_PROMPT */) {
        wr(uc, UC_X86_REG_EAX, 0);
    }
    if (*(const uint8_t *)(uintptr_t)eip == 0xCC) wr(uc, UC_X86_REG_EIP, eip + 1);
}

static void hook_intr(uc_engine *uc, uint32_t intno, void *user)
{
    (void)user;
    if (intno == 0x2D) { debug_service(uc); return; }
    if (intno == 3) {
        xlog("breakpoint at eip=%08x ignored", rd(uc, UC_X86_REG_EIP) - 1);
        return;
    }
    cpu_dump_guest();
    fatal("guest exception %u", intno);
}

static int hook_rdtsc(uc_engine *uc, void *user)
{
    (void)user;
    ULONGLONG v = ke_guest_tsc();
    wr(uc, UC_X86_REG_EAX, (uint32_t)v);
    wr(uc, UC_X86_REG_EDX, (uint32_t)(v >> 32));
    return 1;   /* skip the instruction's own result */
}

static bool hook_invalid(uc_engine *uc, void *user)
{
    (void)user;
    cpu_dump_guest();
    fatal("invalid instruction at eip=%08x", rd(uc, UC_X86_REG_EIP));
}

void cpu_dump_guest(void)
{
    struct cpu_thread *t = cur;
    if (!t || !t->depth) return;
    uc_engine *uc = t->uc;
    uint32_t esp = rd(uc, UC_X86_REG_ESP);
    xlog("  guest eip=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
         rd(uc, UC_X86_REG_EIP), rd(uc, UC_X86_REG_EAX), rd(uc, UC_X86_REG_EBX), rd(uc, UC_X86_REG_ECX),
         rd(uc, UC_X86_REG_EDX), rd(uc, UC_X86_REG_ESI), rd(uc, UC_X86_REG_EDI), rd(uc, UC_X86_REG_EBP), esp);
    if (esp > 0x10000) {
        const uint32_t *sp = (const uint32_t *)(uintptr_t)esp;
        xlog("  guest stack: %08x %08x %08x %08x %08x %08x %08x %08x", sp[0], sp[1], sp[2], sp[3], sp[4],
             sp[5], sp[6], sp[7]);
    }
}

/* ---- host -> guest ----------------------------------------------------- */

static const int saved_regs[] = { UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                                  UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
                                  UC_X86_REG_EIP, UC_X86_REG_EFLAGS };
#define NSAVED (sizeof(saved_regs) / sizeof(saved_regs[0]))

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
        uint64_t r = e->adapter(&f);
        return r;
    }

    struct cpu_thread *t = thread_engine();
    uc_engine *uc = t->uc;
    uint32_t saved[NSAVED];
    void *ptrs[NSAVED];
    for (unsigned i = 0; i < NSAVED; i++) ptrs[i] = &saved[i];
    if (t->depth) uc_reg_read_batch(uc, (int *)saved_regs, ptrs, NSAVED);
    if (!t->stack_top) {
        t->stack_top = (uint32_t)(uintptr_t)arena_alloc(GUEST_STACK_DEFAULT, 1) + GUEST_STACK_DEFAULT;
    }
    /* Below the interrupted guest code's stack, or at the top of this
       thread's own guest stack. */
    uint32_t esp = t->depth ? (rd(uc, UC_X86_REG_ESP) - 64) & ~15u : t->stack_top - 16;
    int sargs = n;
    if (conv == CONV_FAST) {
        wr(uc, UC_X86_REG_ECX, n > 0 ? args[0] : 0);
        wr(uc, UC_X86_REG_EDX, n > 1 ? args[1] : 0);
        sargs = n > 2 ? n - 2 : 0;
        args += n - sargs;
    }
    esp -= 4 * (sargs + 1);
    uint32_t *sp = (uint32_t *)(uintptr_t)esp;
    sp[0] = ret_addr;
    for (int i = 0; i < sargs; i++) sp[1 + i] = args[i];
    wr(uc, UC_X86_REG_ESP, esp);
    if (!t->depth) {
        uint32_t fl = 0x202;   /* IF, reserved bit; DF clear */
        wr(uc, UC_X86_REG_EFLAGS, fl);
    }

    uint32_t pc = fn;
    t->depth++;
    for (;;) {
        if (t->depth == 1) sync_code(t);
        uc_err err = uc_emu_start(uc, pc, ret_addr, 0, 0);
        pc = rd(uc, UC_X86_REG_EIP);
        if (err != UC_ERR_OK) {
            cpu_dump_guest();
            fatal("guest code at %08x failed: %s", pc, uc_strerror(err));
        }
        if (pc == ret_addr) break;
        if (t->stop_pending) { t->stop_pending = false; uc_ctl_flush_tb(uc); t->gen = code_gen; continue; }
        if (*(const uint8_t *)(uintptr_t)pc == 0xF4) {   /* hlt: wait for an interrupt */
            usleep(1000);
            pc++;
            continue;
        }
        /* Stopped by a nested call's uc_emu_stop or similar: carry on. */
    }
    t->depth--;
    uint64_t r = rd(uc, UC_X86_REG_EAX) | (uint64_t)rd(uc, UC_X86_REG_EDX) << 32;
    if (t->depth) uc_reg_write_batch(uc, (int *)saved_regs, ptrs, NSAVED);
    return r;
}

void cpu_init(void)
{
    stub_page = arena_alloc(STUB_PAGE, 1);
    memset(stub_page, 0xF4, STUB_PAGE);   /* hlt */
    ret_addr = (uint32_t)(uintptr_t)stub_page + RET_OFFSET;
    xlog("cpu: x86 guest code runs in Unicorn %s (%zu host functions)", uc_version(NULL, NULL) ? "2" : "?",
         nentries);
}

/* ---- guest varargs ----------------------------------------------------- */

int xa_format(char *out, size_t size, const char *fmt, const uint32_t *args)
{
    size_t o = 0;
    unsigned a = 0;
    char spec[32], piece[512];
    if (!size) return 0;
    out[0] = 0;
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            if (o + 1 < size) out[o++] = *p;
            p++;
            continue;
        }
        const char *start = p++;
        if (*p == '%') { if (o + 1 < size) out[o++] = '%'; p++; continue; }
        while (*p && strchr("-+ #0", *p)) p++;
        if (*p == '*') { p++; } else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; if (*p == '*') p++; else while (*p >= '0' && *p <= '9') p++; }
        int wide = 0, big = 0;
        for (;;) {
            if (*p == 'l' && p[1] == 'l') { big = 1; p += 2; }
            else if (*p == 'I' && p[1] == '6' && p[2] == '4') { big = 1; p += 3; }
            else if (*p == 'h' || *p == 'l' || *p == 'w' || *p == 'L') { if (*p == 'w' || *p == 'l') wide = 1; p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        size_t len = (size_t)(p - start);
        if (len >= sizeof(spec)) len = sizeof(spec) - 1;
        memcpy(spec, start, len);
        spec[len] = 0;
        /* Rebuild the spec without the size prefixes, then format one value. */
        char clean[32];
        size_t c = 0;
        for (size_t i = 0; i < len - 1 && c < sizeof(clean) - 4; i++)
            if (!strchr("hlwLI", spec[i]) && !(spec[i] == '6' && i && spec[i - 1] == 'I') &&
                !(spec[i] == '4' && i > 1 && spec[i - 2] == 'I'))
                clean[c++] = spec[i];
        int star = strchr(clean, '*') != NULL;
        int sw = star ? (int)args[a++] : 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (big) {
                uint64_t v = args[a] | (uint64_t)args[a + 1] << 32;
                a += 2;
                clean[c++] = 'l'; clean[c++] = 'l'; clean[c++] = conv; clean[c] = 0;
                if (star) snprintf(piece, sizeof(piece), clean, sw, (long long)v);
                else snprintf(piece, sizeof(piece), clean, (long long)v);
            } else {
                clean[c++] = conv; clean[c] = 0;
                if (star) snprintf(piece, sizeof(piece), clean, sw, args[a++]);
                else snprintf(piece, sizeof(piece), clean, args[a++]);
            }
            break;
        case 'p':
            snprintf(piece, sizeof(piece), "%08X", args[a++]);
            break;
        case 'f': case 'g': case 'e': case 'G': case 'E': {
            double d;
            memcpy(&d, &args[a], 8);
            a += 2;
            clean[c++] = conv; clean[c] = 0;
            if (star) snprintf(piece, sizeof(piece), clean, sw, d);
            else snprintf(piece, sizeof(piece), clean, d);
            break;
        }
        case 's': case 'S': case 'Z': {
            const void *s = (const void *)(uintptr_t)args[a++];
            if (!s) { snprintf(piece, sizeof(piece), "(null)"); break; }
            if (wide || conv == 'S') {
                const uint16_t *w = s;
                size_t k = 0;
                while (w[k] && k < sizeof(piece) - 1) { piece[k] = w[k] < 0x80 ? (char)w[k] : '?'; k++; }
                piece[k] = 0;
            } else {
                clean[c++] = 's'; clean[c] = 0;
                if (star) snprintf(piece, sizeof(piece), clean, sw, (const char *)s);
                else snprintf(piece, sizeof(piece), clean, (const char *)s);
            }
            break;
        }
        default:
            snprintf(piece, sizeof(piece), "%s", spec);
        }
        for (const char *q = piece; *q; q++)
            if (o + 1 < size) out[o++] = *q;
    }
    out[o] = 0;
    return (int)o;
}
