/*
 * kernel.c - high-level emulation (HLE) of the Xbox kernel.
 *
 * A title reaches the kernel only through its import thunk table, which the
 * loader points at addresses in the HLE region. The dispatcher intercepts
 * those addresses and calls the C implementations below with the guest
 * arguments, then performs the callee-side stack cleanup of the export's
 * calling convention (stdcall, fastcall or cdecl, generated in kexports.h).
 *
 * Guest threads are scheduled on the single host thread (the Xbox has one
 * CPU), preemptively through cpu->pending polls. Blocking calls park the
 * thread with the call still pending and complete it once the wait is
 * satisfied. Callbacks into guest code (DPCs, SEH handlers) run as
 * continuations on guest threads via return traps, never by recursion.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "kernel.h"

int hle_trace;
int hle_strict;
double hle_run_seconds;     /* stop after this much time (0 = run until exit) */
int hle_sample;             /* report where threads are when preempted */

/* ------------------------------------------------------------------ */
/* Export table                                                        */
/* ------------------------------------------------------------------ */

typedef struct kexp {
    const char *name;
    int8_t nargs;
    char conv;            /* S stdcall, F fastcall, C cdecl, V variable */
} kexp_t;

#define KEXP(o, n, a, c) [o] = { #n, a, #c[0] },
static const kexp_t kexp[400] = {
#include "kexports.h"
};
#undef KEXP

typedef uint64_t (*khandler)(hthread *t, uint32_t *a, int *block);
static khandler handlers[400];
static uint32_t var_addr[400];
static uint8_t warned[400];

static int ordinal_of(const char *name)
{
    for (int i = 0; i < 400; i++)
        if (kexp[i].name && !strcmp(kexp[i].name, name))
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static uint64_t boot_ns;

static uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t uptime_ns(void) { return host_ns() - boot_ns; }

/* 100ns units since 1601-01-01 */
static uint64_t system_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}

/* Convert an NT timeout (LARGE_INTEGER*) to an absolute host deadline; 0 = infinite */
static uint64_t deadline_from(uint32_t timeout_ptr, int *poll)
{
    *poll = 0;
    if (!timeout_ptr)
        return 0;
    int64_t v = (int64_t)G64(timeout_ptr);
    uint64_t now = host_ns();
    if (v == 0) {
        *poll = 1;
        return now;
    }
    if (v < 0)
        return now + (uint64_t)(-v) * 100;
    uint64_t st = system_time();
    return (uint64_t)v > st ? now + ((uint64_t)v - st) * 100 : now;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static uint32_t ram_size;
static uint8_t *page_used;          /* physical pages */
static uint32_t hle_brk = XBOX_HLE_BASE + 0x10000;

/* HLE-owned guest memory (kernel objects, strings), never freed */
static uint32_t hle_alloc(uint32_t size)
{
    uint32_t a = (hle_brk + 15) & ~15u;
    hle_brk = a + size;
    if (hle_brk > XBOX_HLE_BASE + XBOX_HLE_SIZE) {
        XBT_LOG("HLE area exhausted\n");
        abort();
    }
    memset(GPTR(a), 0, size);
    return a;
}

void phys_reserve(uint32_t pa, uint32_t size)
{
    for (uint32_t p = pa >> 12; p < (pa + size + 0xfff) >> 12 && p < (ram_size >> 12); p++)
        page_used[p] = 1;
}

/* contiguous physical allocation; returns physical address or ~0 */
uint32_t phys_alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi)
{
    uint32_t n = (size + 0xfff) >> 12;
    if (align < 0x1000)
        align = 0x1000;
    if (hi == 0 || hi >= ram_size)
        hi = ram_size - 1;
    uint32_t step = align >> 12;
    /* search from the top, like the Xbox kernel does for contiguous memory */
    int64_t start = (int64_t)(((hi + 1) >> 12) - n);
    start -= start % (int64_t)step;
    for (; start >= (int64_t)(lo >> 12); start -= step) {
        uint32_t k;
        for (k = 0; k < n; k++)
            if (page_used[start + k])
                break;
        if (k == n) {
            memset(page_used + start, 1, n);
            memset(GPTR(XBOX_CONTIG_BASE + ((uint32_t)start << 12)), 0, (size_t)n << 12);
            return (uint32_t)start << 12;
        }
    }
    return ~0u;
}

static void phys_free(uint32_t pa, uint32_t size)
{
    for (uint32_t p = pa >> 12; p < (pa + size + 0xfff) >> 12 && p < (ram_size >> 12); p++)
        page_used[p] = 0;
}

/* allocation records for size queries and frees */
typedef struct arec {
    uint32_t va, size;
    int kind;              /* 0 contiguous/system (physical), 1 virtual, 2 pool */
} arec;
static arec *arecs;
static int narecs, carecs;

static void arec_add(uint32_t va, uint32_t size, int kind)
{
    if (narecs == carecs) {
        carecs = carecs ? carecs * 2 : 256;
        arecs = realloc(arecs, sizeof(arec) * (size_t)carecs);
    }
    arecs[narecs++] = (arec){ va, size, kind };
}

static arec *arec_find(uint32_t va)
{
    for (int i = 0; i < narecs; i++)
        if (va >= arecs[i].va && va < arecs[i].va + arecs[i].size)
            return &arecs[i];
    return NULL;
}

static void arec_del(arec *r)
{
    *r = arecs[--narecs];
}

/* user virtual memory lives above physical RAM, in [VA_LO, VA_HI) */
#define VA_LO 0x10000000u
#define VA_HI 0x7ff00000u
static uint32_t va_next = VA_LO;

static int va_free_range(uint32_t va, uint32_t size)
{
    for (int i = 0; i < narecs; i++)
        if (arecs[i].kind == 1 && va < arecs[i].va + arecs[i].size && arecs[i].va < va + size)
            return 0;
    return 1;
}

static uint32_t va_alloc(uint32_t want, uint32_t size)
{
    size = (size + 0xfff) & ~0xfffu;
    uint32_t va = want;
    if (va) {
        va &= ~0xfffu;
        if (va < VA_LO || va + size > VA_HI) {
            /* request inside the identity-mapped RAM view: only if free */
            if (va >= XBOX_LOW_BASE && va + size <= ram_size) {
                for (uint32_t p = va >> 12; p < (va + size) >> 12; p++)
                    if (page_used[p])
                        return 0;
                phys_reserve(va, size);
                arec_add(va, size, 0);
                return va;
            }
            return 0;
        }
        if (!va_free_range(va, size))
            return 0;
    } else {
        va = (va_next + 0xffff) & ~0xffffu;
        while (!va_free_range(va, size))
            va += 0x10000;
        if (va + size > VA_HI)
            return 0;
        va_next = va + size;
    }
    if (mem_map_anon(va, size))
        return 0;
    arec_add(va, size, 1);
    return va;
}

/* kernel pool: first-fit over a physical arena */
#define POOL_SIZE (4u << 20)
static uint32_t pool_base;
typedef struct pblk {
    uint32_t off, size;
    int used;
} pblk;
static pblk *pool;
static int npool, cpool;

static uint32_t pool_alloc(uint32_t size)
{
    size = (size + 15) & ~15u;
    if (!size)
        size = 16;
    for (int i = 0; i < npool; i++) {
        if (!pool[i].used && pool[i].size >= size) {
            if (pool[i].size > size) {
                if (npool == cpool) {
                    cpool *= 2;
                    pool = realloc(pool, sizeof(pblk) * (size_t)cpool);
                }
                memmove(pool + i + 1, pool + i, sizeof(pblk) * (size_t)(npool - i));
                npool++;
                pool[i + 1].off = pool[i].off + size;
                pool[i + 1].size = pool[i].size - size;
                pool[i + 1].used = 0;
                pool[i].size = size;
            }
            pool[i].used = 1;
            memset(GPTR(pool_base + pool[i].off), 0, size);
            return pool_base + pool[i].off;
        }
    }
    return 0;
}

static int pool_find(uint32_t va)
{
    for (int i = 0; i < npool; i++)
        if (pool_base + pool[i].off == va && pool[i].used)
            return i;
    return -1;
}

static void pool_free(uint32_t va)
{
    int i = pool_find(va);
    if (i < 0)
        return;
    pool[i].used = 0;
    if (i + 1 < npool && !pool[i + 1].used) {
        pool[i].size += pool[i + 1].size;
        memmove(pool + i + 1, pool + i + 2, sizeof(pblk) * (size_t)(npool - i - 2));
        npool--;
    }
    if (i > 0 && !pool[i - 1].used) {
        pool[i - 1].size += pool[i].size;
        memmove(pool + i, pool + i + 1, sizeof(pblk) * (size_t)(npool - i - 1));
        npool--;
    }
}

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

/* ANSI_STRING/OBJECT_STRING: USHORT Length, USHORT MaximumLength, PCHAR Buffer */
static void get_ansi(uint32_t str, char *out, size_t outlen)
{
    out[0] = 0;
    if (!str)
        return;
    uint32_t len = G16(str), buf = G32(str + 4);
    if (len >= outlen)
        len = (uint32_t)outlen - 1;
    if (buf)
        memcpy(out, GPTR(buf), len);
    out[len] = 0;
}

static uint32_t new_ansi(const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    uint32_t st = hle_alloc(8 + len + 1);
    G16(st) = (uint16_t)len;
    G16(st + 2) = (uint16_t)(len + 1);
    G32(st + 4) = st + 8;
    memcpy(GPTR(st + 8), s, len + 1);
    return st;
}

/* printf with guest varargs (DbgPrint) */
static int guest_vformat(char *out, size_t outlen, uint32_t fmt, uint32_t argp)
{
    size_t o = 0;
    const char *f = GPTR(fmt);
#define PUT(c) do { if (o + 1 < outlen) out[o++] = (char)(c); } while (0)
    while (*f) {
        if (*f != '%') {
            PUT(*f++);
            continue;
        }
        char spec[32];
        size_t sl = 0;
        spec[sl++] = *f++;
        while (*f && strchr("-+ #0123456789.", *f) && sl < 20)
            spec[sl++] = *f++;
        int is64 = 0, lng = 0, wide = 0;
        for (;;) {
            if (*f == 'l') { lng = 1; f++; }
            else if (*f == 'h') { f++; }
            else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { is64 = 1; f += 3; }
            else if (*f == 'w') { wide = 1; f++; }
            else break;
        }
        char c = *f ? *f++ : 0;
        char tmp[512];
        spec[sl] = 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (is64) {
                uint64_t v = G32(argp) | (uint64_t)G32(argp + 4) << 32;
                argp += 8;
                char sp[40];
                snprintf(sp, sizeof sp, "%sll%c", spec, c);
                snprintf(tmp, sizeof tmp, sp, v);
            } else {
                uint32_t v = G32(argp);
                argp += 4;
                char sp[40];
                snprintf(sp, sizeof sp, "%s%c", spec, c);
                if (c == 'd' || c == 'i')
                    snprintf(tmp, sizeof tmp, sp, (int)v);
                else
                    snprintf(tmp, sizeof tmp, sp, v);
            }
            break;
        }
        case 'p':
            snprintf(tmp, sizeof tmp, "%08X", G32(argp));
            argp += 4;
            break;
        case 's': case 'S': {
            uint32_t p = G32(argp);
            argp += 4;
            char sv[400];
            if (!p) {
                strcpy(sv, "(null)");
            } else if (c == 'S' || (wide || (lng && c == 's'))) {
                size_t k = 0;
                for (uint32_t q = p; G16(q) && k < sizeof sv - 1; q += 2)
                    sv[k++] = (char)G16(q);
                sv[k] = 0;
            } else {
                strncpy(sv, GPTR(p), sizeof sv - 1);
                sv[sizeof sv - 1] = 0;
            }
            char sp[40];
            snprintf(sp, sizeof sp, "%ss", spec);
            snprintf(tmp, sizeof tmp, sp, sv);
            break;
        }
        case 'f': case 'g': case 'e': {
            double d;
            uint64_t v = G32(argp) | (uint64_t)G32(argp + 4) << 32;
            memcpy(&d, &v, 8);
            argp += 8;
            char sp[40];
            snprintf(sp, sizeof sp, "%s%c", spec, c);
            snprintf(tmp, sizeof tmp, sp, d);
            break;
        }
        case '%':
            strcpy(tmp, "%");
            break;
        default:
            snprintf(tmp, sizeof tmp, "%%%c", c);
            break;
        }
        for (char *q = tmp; *q; q++)
            PUT(*q);
    }
    out[o] = 0;
#undef PUT
    return (int)o;
}

/* ------------------------------------------------------------------ */
/* Handles and objects                                                 */
/* ------------------------------------------------------------------ */

enum { H_FREE, H_OBJ, H_FILE, H_DIR, H_LINK, H_DEVICE };
typedef struct handle {
    int type;
    uint32_t obj;          /* guest dispatcher object */
    int fd;
    char path[512];        /* host path (files), target (links) */
    DIR *dir;
    int refs;
} handle;
#define MAX_HANDLES 4096
static handle htab[MAX_HANDLES];

static uint32_t handle_new(int type)
{
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (htab[i].type == H_FREE) {
            memset(&htab[i], 0, sizeof htab[i]);
            htab[i].type = type;
            htab[i].fd = -1;
            htab[i].refs = 1;
            return (uint32_t)i << 2;
        }
    }
    return 0;
}

static handle *handle_get(uint32_t h)
{
    uint32_t i = h >> 2;
    if (!h || (h & 3) || i >= MAX_HANDLES || htab[i].type == H_FREE)
        return NULL;
    return &htab[i];
}

/* resolve a waitable handle or pseudo-handle to a dispatcher object */
static hthread *current;
static uint32_t handle_obj(uint32_t h)
{
    if (h == 0xfffffffeu)     /* NtCurrentThread() */
        return current ? current->ethread : 0;
    handle *e = handle_get(h);
    if (!e)
        return 0;
    if (e->type == H_OBJ)
        return e->obj;
    return 0;
}

/* dispatcher header: Type(1) Absolute(1) Size(1) Inserted(1) SignalState(4) WaitListHead(8) */
#define OBJ_TYPE(o)   G8(o)
#define OBJ_SIGNAL(o) (*(volatile int32_t *)(uintptr_t)((o) + 4))

static void init_header(uint32_t o, int type, int size, int signal)
{
    G8(o) = (uint8_t)type;
    G8(o + 1) = 0;
    G8(o + 2) = (uint8_t)(size / 4);
    G8(o + 3) = 0;
    OBJ_SIGNAL(o) = signal;
    G32(o + 8) = o + 8;           /* empty WaitListHead */
    G32(o + 12) = o + 8;
}

/* ------------------------------------------------------------------ */
/* Threads and scheduling                                              */
/* ------------------------------------------------------------------ */

static hthread *threads;
static uint32_t next_tid = 1;
static hthread *dpc_thread;
static int stop_requested;
static int exit_code;

#define ETHREAD_SIZE 0x140
#define KT_STACKBASE 0x1c
#define KT_STACKLIMIT 0x20
#define KT_TLSDATA 0x28
#define ET_UNIQUETHREAD 0x12c
#define ET_STARTADDRESS 0x130
#define KPCR_SIZE 0x300

static int kexp_stack_args(int ord)
{
    const kexp_t *e = &kexp[ord];
    if (e->conv == 'C')
        return 0;
    int n = e->nargs < 0 ? 0 : e->nargs;
    if (e->conv == 'F')
        n = n > 2 ? n - 2 : 0;
    return n;
}

/* finish an HLE call: return value and callee-side stack cleanup */
static void finish_call(xbt_cpu *cpu, int ord, uint64_t r)
{
    cpu->r[R_EAX] = (uint32_t)r;
    cpu->r[R_EDX] = (uint32_t)(r >> 32);
    uint32_t esp = cpu->r[R_ESP];
    cpu->eip = G32(esp);
    cpu->r[R_ESP] = esp + 4 + 4 * (uint32_t)kexp_stack_args(ord);
}

static hthread *thread_new(uint32_t stack_size, uint32_t tls_size)
{
    hthread *t = calloc(1, sizeof *t);
    xbt_cpu_init(&t->cpu);
    t->cpu.user = t;
    t->id = next_tid++;
    stack_size = (stack_size + 0xffff) & ~0xffffu;
    if (stack_size < 0x10000)
        stack_size = 0x10000;
    uint32_t lo = va_alloc(0, stack_size);
    if (!lo) {
        XBT_LOG("cannot allocate a thread stack\n");
        abort();
    }
    t->stack_lo = lo;
    t->stack_hi = lo + stack_size;
    /* TLS sits right below StackBase; XAPI derives _tls_index from the exact
       size (fs:[4] + index*4 == KTHREAD.TlsData), so it must not be rounded */
    uint32_t tls = t->stack_hi - tls_size;

    t->ethread = hle_alloc(ETHREAD_SIZE);
    init_header(t->ethread, 6 /* ThreadObject */, ETHREAD_SIZE, 0);
    G32(t->ethread + KT_STACKBASE) = t->stack_hi;
    G32(t->ethread + KT_STACKLIMIT) = t->stack_lo;
    G32(t->ethread + KT_TLSDATA) = tls;
    G32(t->ethread + ET_UNIQUETHREAD) = t->id;

    t->kpcr = hle_alloc(KPCR_SIZE);
    G32(t->kpcr + 0x00) = 0xffffffffu;          /* NtTib.ExceptionList: end of chain */
    G32(t->kpcr + 0x04) = t->stack_hi;          /* NtTib.StackBase = TLS data top */
    G32(t->kpcr + 0x08) = t->stack_lo;          /* NtTib.StackLimit */
    G32(t->kpcr + 0x18) = t->kpcr;              /* NtTib.Self */
    G32(t->kpcr + 0x1c) = t->kpcr;              /* SelfPcr */
    G32(t->kpcr + 0x20) = t->kpcr + 0x28;       /* Prcb */
    G8(t->kpcr + 0x24) = 0;                     /* Irql */
    G32(t->kpcr + 0x28) = t->ethread;           /* PrcbData.CurrentThread */
    t->cpu.fs_base = t->kpcr;
    t->cpu.r[R_ESP] = (tls - 16) & ~15u;

    t->state = TS_READY;
    t->next = threads;
    threads = t;
    return t;
}

static void push32(xbt_cpu *cpu, uint32_t v)
{
    cpu->r[R_ESP] -= 4;
    G32(cpu->r[R_ESP]) = v;
}

static void thread_exit(hthread *t, uint32_t status)
{
    t->state = TS_DONE;
    OBJ_SIGNAL(t->ethread) = 1;
    if (hle_trace)
        XBT_LOG("thread %u exited (status %08x)\n", t->id, status);
}

/* ---- waits ---- */

static int obj_signaled(uint32_t o, hthread *t)
{
    switch (OBJ_TYPE(o)) {
    case 2:   /* mutant: free, or owned by this thread */
        return OBJ_SIGNAL(o) > 0 || G32(o + 0x18) == t->ethread;
    default:
        return OBJ_SIGNAL(o) > 0;
    }
}

static void obj_acquire(uint32_t o, hthread *t)
{
    switch (OBJ_TYPE(o)) {
    case 1: case 9:      /* synchronization event / timer: auto reset */
        OBJ_SIGNAL(o) = 0;
        break;
    case 5:              /* semaphore */
        OBJ_SIGNAL(o)--;
        break;
    case 2:              /* mutant */
        OBJ_SIGNAL(o)--;
        G32(o + 0x18) = t->ethread;
        break;
    }
}

/* returns status if the wait is satisfied, or -1 */
static int64_t try_wait(hthread *t)
{
    if (t->wait_delay)
        return host_ns() >= t->deadline ? (int64_t)STATUS_SUCCESS : -1;
    if (t->wait_all) {
        for (int i = 0; i < t->nwait; i++)
            if (!obj_signaled(t->wait_obj[i], t))
                goto timeout;
        for (int i = 0; i < t->nwait; i++)
            obj_acquire(t->wait_obj[i], t);
        return STATUS_SUCCESS;
    }
    for (int i = 0; i < t->nwait; i++) {
        if (obj_signaled(t->wait_obj[i], t)) {
            obj_acquire(t->wait_obj[i], t);
            return STATUS_WAIT_0 + (uint32_t)i;
        }
    }
timeout:
    if (t->deadline && host_ns() >= t->deadline)
        return STATUS_TIMEOUT;
    return -1;
}

/* Start a wait from inside an HLE call. Returns the status if it can
   complete immediately, else blocks the thread. */
static uint64_t begin_wait(hthread *t, uint32_t *objs, int n, int all, uint32_t timeout, int *block)
{
    int poll;
    t->nwait = n;
    for (int i = 0; i < n; i++)
        t->wait_obj[i] = objs[i];
    t->wait_all = all;
    t->wait_delay = 0;
    t->deadline = deadline_from(timeout, &poll);
    int64_t r = try_wait(t);
    if (r >= 0)
        return (uint64_t)r;
    if (poll)
        return STATUS_TIMEOUT;
    t->state = TS_WAIT;
    *block = 1;
    return 0;
}

/* ---- timers and DPCs ---- */

typedef struct ktimer_rec {
    uint32_t timer;
    uint64_t due;
    uint32_t period_ms;
    uint32_t dpc;
    struct ktimer_rec *next;
} ktimer_rec;
static ktimer_rec *timers;

#define MAX_DPCS 256
static uint32_t dpcq[MAX_DPCS];
static int ndpc;

static int queue_dpc(uint32_t dpc, uint32_t a1, uint32_t a2, int set_args)
{
    if (G8(dpc + 2))
        return 0;
    G8(dpc + 2) = 1;
    if (set_args) {
        G32(dpc + 0x14) = a1;
        G32(dpc + 0x18) = a2;
    }
    if (ndpc < MAX_DPCS)
        dpcq[ndpc++] = dpc;
    return 1;
}

static int cancel_timer(uint32_t timer)
{
    for (ktimer_rec **pp = &timers; *pp; pp = &(*pp)->next) {
        if ((*pp)->timer == timer) {
            ktimer_rec *r = *pp;
            *pp = r->next;
            free(r);
            G8(timer + 3) = 0;
            return 1;
        }
    }
    return 0;
}

static void fire_timers(void)
{
    uint64_t now = host_ns();
    for (ktimer_rec **pp = &timers; *pp;) {
        ktimer_rec *r = *pp;
        if (now < r->due) {
            pp = &r->next;
            continue;
        }
        OBJ_SIGNAL(r->timer) = 1;
        if (r->dpc)
            queue_dpc(r->dpc, (uint32_t)system_time(), (uint32_t)(system_time() >> 32), 1);
        if (r->period_ms) {
            r->due = now + (uint64_t)r->period_ms * 1000000ull;
            pp = &r->next;
        } else {
            G8(r->timer + 3) = 0;
            *pp = r->next;
            free(r);
        }
    }
}

/* run the next queued DPC on the DPC thread: DeferredRoutine(Dpc, Context, Arg1, Arg2) */
static void start_dpc(void)
{
    hthread *t = dpc_thread;
    if (!ndpc || t->state != TS_DONE)
        return;
    uint32_t dpc = dpcq[0];
    memmove(dpcq, dpcq + 1, sizeof(uint32_t) * (size_t)--ndpc);
    G8(dpc + 2) = 0;
    xbt_cpu *c = &t->cpu;
    c->r[R_ESP] = (t->stack_hi - 64) & ~15u;
    push32(c, G32(dpc + 0x18));
    push32(c, G32(dpc + 0x14));
    push32(c, G32(dpc + 0x10));
    push32(c, dpc);
    push32(c, TRAP_THREAD_EXIT);
    c->eip = G32(dpc + 0x0c);
    t->state = TS_READY;
}

/* ---- preemption ---- */

static void on_alarm(int sig)
{
    (void)sig;
    if (current) {
        xbt_preempt = 1;
        current->cpu.pending = 1;
    }
}

/* ------------------------------------------------------------------ */
/* SEH: exception dispatch and unwinding as guest continuations        */
/* ------------------------------------------------------------------ */

#define TRAP_SEH_RET    (XBOX_HLE_BASE + 0xf08)
#define TRAP_UNWIND_RET (XBOX_HLE_BASE + 0xf0c)
#define CONTEXT_SIZE 0x2cc
#define EXCEPTION_NONCONTINUABLE 1
#define EXCEPTION_UNWINDING 2
#define EXCEPTION_EXIT_UNWIND 4

typedef struct seh_state {
    int active;
    uint32_t record, context, reg, dispctx;
    /* unwind */
    int unwinding;
    uint32_t target_frame, ret_eip, ret_esp, saved[8];
} seh_state;
static seh_state seh[1024];

static seh_state *seh_of(hthread *t) { return &seh[t->id & 1023]; }

/* x86 CONTEXT (CONTEXT_FULL subset) */
static void context_store(uint32_t ctx, xbt_cpu *c, uint32_t eip, uint32_t esp)
{
    memset(GPTR(ctx), 0, CONTEXT_SIZE);
    G32(ctx + 0x00) = 0x10007;                  /* CONTEXT_FULL */
    G32(ctx + 0x9c) = c->r[R_EDI];
    G32(ctx + 0xa0) = c->r[R_ESI];
    G32(ctx + 0xa4) = c->r[R_EBX];
    G32(ctx + 0xa8) = c->r[R_EDX];
    G32(ctx + 0xac) = c->r[R_ECX];
    G32(ctx + 0xb0) = c->r[R_EAX];
    G32(ctx + 0xb4) = c->r[R_EBP];
    G32(ctx + 0xb8) = eip;
    G32(ctx + 0xbc) = 0x08;
    G32(ctx + 0xc0) = xbt_flags(c);
    G32(ctx + 0xc4) = esp;
    G32(ctx + 0xc8) = 0x10;
}

static void context_load(uint32_t ctx, xbt_cpu *c)
{
    c->r[R_EDI] = G32(ctx + 0x9c);
    c->r[R_ESI] = G32(ctx + 0xa0);
    c->r[R_EBX] = G32(ctx + 0xa4);
    c->r[R_EDX] = G32(ctx + 0xa8);
    c->r[R_ECX] = G32(ctx + 0xac);
    c->r[R_EAX] = G32(ctx + 0xb0);
    c->r[R_EBP] = G32(ctx + 0xb4);
    c->eip = G32(ctx + 0xb8);
    xbt_set_flags(c, G32(ctx + 0xc0), 0x0cd5);
    c->r[R_ESP] = G32(ctx + 0xc4);
}

/* call the handler of the current registration record */
static void seh_call_handler(hthread *t, seh_state *s, uint32_t trap)
{
    xbt_cpu *c = &t->cpu;
    uint32_t handler = G32(s->reg + 4);
    push32(c, s->dispctx);
    push32(c, s->context);
    push32(c, s->reg);
    push32(c, s->record);
    push32(c, trap);
    c->eip = handler;
}

/* start dispatching an exception for thread t (state already in cpu) */
static void seh_dispatch(hthread *t, uint32_t record, uint32_t fault_eip, uint32_t fault_esp)
{
    seh_state *s = seh_of(t);
    xbt_cpu *c = &t->cpu;
    uint32_t sp = (fault_esp - 0x400) & ~15u;
    s->context = sp - CONTEXT_SIZE - 16;
    s->context &= ~15u;
    context_store(s->context, c, fault_eip, fault_esp);
    s->dispctx = s->context - 16;
    s->record = record;
    s->reg = G32(t->kpcr);
    s->active = 1;
    c->r[R_ESP] = s->dispctx - 16;
    if (s->reg == 0xffffffffu || !s->reg) {
        XBT_LOG("unhandled exception %08x at %08x\n", G32(record), G32(record + 12));
        stop_requested = 1;
        exit_code = 3;
        t->state = TS_DONE;
        return;
    }
    seh_call_handler(t, s, TRAP_SEH_RET);
}

static void seh_handler_returned(hthread *t)
{
    seh_state *s = seh_of(t);
    xbt_cpu *c = &t->cpu;
    uint32_t disp = c->r[R_EAX];
    if (disp == 0) {                       /* ExceptionContinueExecution */
        s->active = 0;
        context_load(s->context, c);
        return;
    }
    s->reg = G32(s->reg);                   /* ContinueSearch (or nested): next frame */
    if (s->reg == 0xffffffffu || !s->reg) {
        XBT_LOG("unhandled exception %08x at %08x\n", G32(s->record), G32(s->record + 12));
        stop_requested = 1;
        exit_code = 3;
        t->state = TS_DONE;
        return;
    }
    c->r[R_ESP] = s->dispctx - 16;
    seh_call_handler(t, s, TRAP_SEH_RET);
}

/* RtlUnwind continuation: call the handlers between fs:[0] and TargetFrame */
static void unwind_step(hthread *t)
{
    seh_state *s = seh_of(t);
    xbt_cpu *c = &t->cpu;
    uint32_t head = G32(t->kpcr);
    if (head == s->target_frame || head == 0xffffffffu || !head) {
        /* done: return from RtlUnwind to its caller */
        for (int r = 0; r < 8; r++)
            if (r != R_EAX && r != R_ECX && r != R_EDX && r != R_ESP)
                c->r[r] = s->saved[r];
        c->eip = s->ret_eip;
        c->r[R_ESP] = s->ret_esp;
        s->unwinding = 0;
        return;
    }
    s->reg = head;
    G32(t->kpcr) = G32(head);               /* unlink before calling, like RtlUnwind */
    c->r[R_ESP] = s->dispctx - 16;
    seh_call_handler(t, s, TRAP_UNWIND_RET);
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

typedef struct mapping {
    char from[128];
    char to[512];
} mapping;
static mapping devmap[16], links[64];
static int ndev, nlinks;
static char game_dir[512];

static void add_link(const char *from, const char *to)
{
    for (int i = 0; i < nlinks; i++)
        if (!strcasecmp(links[i].from, from)) {
            snprintf(links[i].to, sizeof links[i].to, "%s", to);
            return;
        }
    if (nlinks < 64) {
        snprintf(links[nlinks].from, sizeof links[0].from, "%s", from);
        snprintf(links[nlinks].to, sizeof links[0].to, "%s", to);
        nlinks++;
    }
}

/* Translate an Xbox object path to a host path. Returns 0 on success. */
static int resolve_path(const char *in, char *out, size_t outlen)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", in);
    /* drive letter forms */
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "\\??\\%s", p);
        snprintf(p, sizeof p, "%s", tmp);
    }
    for (int iter = 0; iter < 4; iter++) {
        int changed = 0;
        for (int i = 0; i < nlinks; i++) {
            size_t l = strlen(links[i].from);
            if (!strncasecmp(p, links[i].from, l) && (p[l] == '\\' || p[l] == 0)) {
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%s%s", links[i].to, p + l);
                snprintf(p, sizeof p, "%s", tmp);
                changed = 1;
                break;
            }
        }
        if (!changed)
            break;
    }
    for (int i = 0; i < ndev; i++) {
        size_t l = strlen(devmap[i].from);
        if (!strncasecmp(p, devmap[i].from, l) && (p[l] == '\\' || p[l] == 0)) {
            snprintf(out, outlen, "%s%s", devmap[i].to, p + l);
            for (char *q = out; *q; q++)
                if (*q == '\\')
                    *q = '/';
            /* case-insensitive match of each component */
            char fixed[1024] = "";
            char *save, *tok;
            char work[1024];
            snprintf(work, sizeof work, "%s", out);
            size_t base = strlen(devmap[i].to);
            snprintf(fixed, sizeof fixed, "%.*s", (int)base, work);
            for (tok = strtok_r(work + base, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
                char cand[1024];
                snprintf(cand, sizeof cand, "%s/%s", fixed, tok);
                if (access(cand, F_OK) != 0) {
                    DIR *d = opendir(fixed);
                    if (d) {
                        struct dirent *de;
                        while ((de = readdir(d)))
                            if (!strcasecmp(de->d_name, tok)) {
                                snprintf(cand, sizeof cand, "%s/%s", fixed, de->d_name);
                                break;
                            }
                        closedir(d);
                    }
                }
                snprintf(fixed, sizeof fixed, "%s", cand);
            }
            snprintf(out, outlen, "%s", fixed);
            return 0;
        }
    }
    return -1;
}

/* OBJECT_ATTRIBUTES: RootDirectory, ObjectName (POBJECT_STRING), Attributes */
static int oa_path(uint32_t oa, char *xbox, size_t xl)
{
    char name[600];
    get_ansi(G32(oa + 4), name, sizeof name);
    uint32_t root = G32(oa);
    if (root) {
        handle *h = handle_get(root);
        if (h && (h->type == H_DIR || h->type == H_FILE)) {
            snprintf(xbox, xl, "@%s/%s", h->path, name);
            return 0;
        }
    }
    snprintf(xbox, xl, "%s", name);
    return 0;
}

static void iosb(uint32_t p, uint32_t status, uint32_t info)
{
    if (p) {
        G32(p) = status;
        G32(p + 4) = info;
    }
}

static uint64_t to_filetime(time_t t)
{
    return ((uint64_t)t + 11644473600ull) * 10000000ull;
}

static uint32_t file_attrs(const struct stat *st)
{
    return S_ISDIR(st->st_mode) ? 0x10 : 0x80;
}

/* ------------------------------------------------------------------ */
/* Export implementations                                              */
/* ------------------------------------------------------------------ */

#define H(name) static uint64_t h_##name(hthread *t, uint32_t *a, int *block)
#define UNUSED (void)t; (void)a; (void)block

H(DbgPrint)
{
    UNUSED;
    char buf[4096];
    guest_vformat(buf, sizeof buf, a[0], t->cpu.r[R_ESP] + 8);
    fprintf(stdout, "%s", buf);
    fflush(stdout);
    return STATUS_SUCCESS;
}

H(DbgBreakPoint) { UNUSED; XBT_LOG("DbgBreakPoint\n"); return 0; }
H(KeBugCheck) { UNUSED; XBT_LOG("KeBugCheck(%08x)\n", a[0]); stop_requested = 1; exit_code = 4; *block = 1; t->state = TS_DONE; return 0; }
H(KeBugCheckEx) { UNUSED; XBT_LOG("KeBugCheckEx(%08x, %08x, %08x, %08x, %08x)\n", a[0], a[1], a[2], a[3], a[4]); stop_requested = 1; exit_code = 4; *block = 1; t->state = TS_DONE; return 0; }

H(HalReturnToFirmware)
{
    UNUSED;
    XBT_LOG("HalReturnToFirmware(%u): title exited\n", a[0]);
    stop_requested = 1;
    *block = 1;
    t->state = TS_DONE;
    return 0;
}

H(HalInitiateShutdown) { UNUSED; stop_requested = 1; *block = 1; t->state = TS_DONE; return 0; }
H(HalIsResetOrShutdownPending) { UNUSED; return 0; }
H(HalReadSMCTrayState) { UNUSED; if (a[0]) G32(a[0]) = 0x60; if (a[1]) G32(a[1]) = 0; return STATUS_SUCCESS; }
H(HalReadSMBusValue) { UNUSED; if (a[3]) G32(a[3]) = 0; return STATUS_SUCCESS; }
H(HalWriteSMBusValue) { UNUSED; return STATUS_SUCCESS; }
H(HalReadWritePCISpace)
{
    UNUSED;
    if (!a[5] && a[3])
        memset(GPTR(a[3]), 0xff, a[4]);
    return 0;
}
H(HalGetInterruptVector) { UNUSED; if (a[1]) G8(a[1]) = (uint8_t)(a[0] + 4); return a[0] + 0x30; }
H(HalRegisterShutdownNotification) { UNUSED; return 0; }
H(HalEnableSystemInterrupt) { UNUSED; return 0; }
H(HalDisableSystemInterrupt) { UNUSED; return 0; }
H(HalClearSoftwareInterrupt) { UNUSED; return 0; }
H(HalRequestSoftwareInterrupt) { UNUSED; return 0; }

H(AvGetSavedDataAddress) { UNUSED; return 0; }
H(AvSetSavedDataAddress) { UNUSED; return 0; }
H(AvSendTVEncoderOption) { UNUSED; if (a[3]) G32(a[3]) = 0; return 0; }
H(AvSetDisplayMode)
{
    UNUSED;
    if (hle_trace)
        XBT_LOG("AvSetDisplayMode(mode %08x, format %08x, pitch %u, fb %08x)\n", a[2], a[3], a[4], a[5]);
    return STATUS_SUCCESS;
}

/* ---- memory ---- */

H(ExAllocatePool) { UNUSED; uint32_t p = pool_alloc(a[0]); return p; }
H(ExAllocatePoolWithTag) { UNUSED; return pool_alloc(a[0]); }
H(ExFreePool) { UNUSED; pool_free(a[0]); return 0; }
H(ExQueryPoolBlockSize)
{
    UNUSED;
    int i = pool_find(a[0]);
    return i < 0 ? 0 : pool[i].size;
}

H(MmAllocateContiguousMemoryEx)
{
    UNUSED;
    uint32_t pa = phys_alloc(a[0], a[3], a[1], a[2]);
    if (pa == ~0u)
        return 0;
    arec_add(XBOX_CONTIG_BASE + pa, (a[0] + 0xfff) & ~0xfffu, 0);
    return XBOX_CONTIG_BASE + pa;
}

H(MmAllocateContiguousMemory)
{
    uint32_t b[5] = { a[0], 0, ram_size - 1, 0x1000, 4 };
    return h_MmAllocateContiguousMemoryEx(t, b, block);
}

H(MmAllocateSystemMemory)
{
    uint32_t b[5] = { a[0], 0, ram_size - 1, 0x1000, a[1] };
    return h_MmAllocateContiguousMemoryEx(t, b, block);
}

static void free_contig(uint32_t va)
{
    arec *r = arec_find(va);
    if (r && r->kind == 0) {
        phys_free(r->va >= XBOX_CONTIG_BASE ? r->va - XBOX_CONTIG_BASE : r->va, r->size);
        arec_del(r);
    }
}

H(MmFreeContiguousMemory) { UNUSED; free_contig(a[0]); return 0; }
H(MmFreeSystemMemory) { UNUSED; free_contig(a[0]); return a[1] >> 12; }
H(MmQueryAllocationSize)
{
    UNUSED;
    arec *r = arec_find(a[0]);
    return r ? r->size : 0;
}
H(MmGetPhysicalAddress)
{
    UNUSED;
    uint32_t va = a[0];
    if (va >= XBOX_CONTIG_BASE && va - XBOX_CONTIG_BASE < ram_size)
        return va - XBOX_CONTIG_BASE;
    if (va >= XBOX_WC_BASE && va - XBOX_WC_BASE < ram_size)
        return va - XBOX_WC_BASE;
    if (va < ram_size)
        return va;
    return va & (ram_size - 1);
}
H(MmMapIoSpace)
{
    UNUSED;
    if (a[0] < ram_size)
        return XBOX_CONTIG_BASE + a[0];
    return a[0];
}
H(MmUnmapIoSpace) { UNUSED; return 0; }
H(MmPersistContiguousMemory) { UNUSED; return 0; }
H(MmLockUnlockBufferPages) { UNUSED; return 0; }
H(MmLockUnlockPhysicalPage) { UNUSED; return 0; }
H(MmQueryAddressProtect) { UNUSED; return 4; }
H(MmSetAddressProtect) { UNUSED; return 0; }
H(MmClaimGpuInstanceMemory)
{
    UNUSED;
    if (a[1])
        G32(a[1]) = 0;
    /* NV2A instance memory is at the top of physical memory */
    return XBOX_CONTIG_BASE + ram_size;
}
H(MmQueryStatistics)
{
    UNUSED;
    uint32_t s = a[0];
    uint32_t used = 0;
    for (uint32_t p = 0; p < ram_size >> 12; p++)
        used += page_used[p];
    if (s) {
        G32(s + 4) = ram_size >> 12;            /* TotalPhysicalPages */
        G32(s + 8) = (ram_size >> 12) - used;   /* AvailablePages */
    }
    return STATUS_SUCCESS;
}

H(NtAllocateVirtualMemory)
{
    UNUSED;
    uint32_t want = G32(a[0]), size = G32(a[2]);
    uint32_t type = a[3];
    if (want && arec_find(want) && !(type & 0x2000)) {
        /* MEM_COMMIT inside an existing reservation: already backed */
        G32(a[2]) = (size + 0xfff) & ~0xfffu;
        G32(a[0]) = want & ~0xfffu;
        return STATUS_SUCCESS;
    }
    uint32_t va = va_alloc(want, size);
    if (!va)
        return want ? STATUS_CONFLICTING_ADDRESSES : STATUS_NO_MEMORY;
    G32(a[0]) = va;
    G32(a[2]) = (size + 0xfff) & ~0xfffu;
    return STATUS_SUCCESS;
}

H(NtFreeVirtualMemory)
{
    UNUSED;
    uint32_t va = G32(a[0]);
    arec *r = arec_find(va);
    if (r && (a[2] & 0x8000) && r->va == va) {   /* MEM_RELEASE */
        if (r->kind == 1)
            mmap(GPTR(r->va), r->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        else
            phys_free(r->va, r->size);
        G32(a[1]) = r->size;
        arec_del(r);
    }
    return STATUS_SUCCESS;
}

H(NtQueryVirtualMemory)
{
    UNUSED;
    uint32_t va = a[0], mbi = a[1];
    arec *r = arec_find(va);
    memset(GPTR(mbi), 0, 28);
    if (r) {
        G32(mbi) = va & ~0xfffu;
        G32(mbi + 4) = r->va;
        G32(mbi + 8) = 4;
        G32(mbi + 12) = r->va + r->size - (va & ~0xfffu);
        G32(mbi + 16) = 0x1000;     /* MEM_COMMIT */
        G32(mbi + 20) = 4;
        G32(mbi + 24) = 0x20000;    /* MEM_PRIVATE */
    } else {
        G32(mbi) = va & ~0xfffu;
        G32(mbi + 12) = 0x1000;
        G32(mbi + 16) = 0x10000;    /* MEM_FREE */
        G32(mbi + 20) = 1;
    }
    return STATUS_SUCCESS;
}

/* ---- IRQL / interlocked (fastcall) ---- */

H(KfRaiseIrql)
{
    UNUSED;
    uint32_t old = G8(t->kpcr + 0x24);
    G8(t->kpcr + 0x24) = (uint8_t)a[0];
    return old;
}
H(KfLowerIrql) { UNUSED; G8(t->kpcr + 0x24) = (uint8_t)a[0]; return 0; }
H(KeRaiseIrqlToDpcLevel) { UNUSED; uint32_t old = G8(t->kpcr + 0x24); G8(t->kpcr + 0x24) = 2; return old; }
H(KeRaiseIrqlToSynchLevel) { UNUSED; uint32_t old = G8(t->kpcr + 0x24); G8(t->kpcr + 0x24) = 2; return old; }
H(KeGetCurrentIrql) { UNUSED; return G8(t->kpcr + 0x24); }
H(KeGetCurrentThread) { UNUSED; return t->ethread; }

H(InterlockedCompareExchange)
{
    UNUSED;
    uint32_t old = G32(a[0]);
    if (old == a[2])
        G32(a[0]) = a[1];
    return old;
}
H(InterlockedIncrement) { UNUSED; return ++G32(a[0]); }
H(InterlockedDecrement) { UNUSED; return --G32(a[0]); }
H(InterlockedExchange) { UNUSED; uint32_t o = G32(a[0]); G32(a[0]) = a[1]; return o; }
H(InterlockedExchangeAdd) { UNUSED; uint32_t o = G32(a[0]); G32(a[0]) = o + a[1]; return o; }
H(InterlockedFlushSList) { UNUSED; uint32_t o = G32(a[0]); G32(a[0]) = 0; G32(a[0] + 4) = 0; return o; }
H(InterlockedPushEntrySList)
{
    UNUSED;
    uint32_t head = a[0], e = a[1], o = G32(head);
    G32(e) = o;
    G32(head) = e;
    G16(head + 4)++;
    G16(head + 6)++;
    return o;
}
H(InterlockedPopEntrySList)
{
    UNUSED;
    uint32_t head = a[0], o = G32(head);
    if (o) {
        G32(head) = G32(o);
        G16(head + 4)--;
        G16(head + 6)++;
    }
    return o;
}
H(ExInterlockedCompareExchange64)
{
    UNUSED;
    uint64_t old = G64(a[0]), exch = G64(a[1]), cmp = G64(a[2]);
    if (old == cmp)
        G64(a[0]) = exch;
    return old;
}
H(ExfInterlockedInsertHeadList)
{
    UNUSED;
    uint32_t head = a[0], e = a[1], first = G32(head);
    G32(e) = first;
    G32(e + 4) = head;
    G32(first + 4) = e;
    G32(head) = e;
    return first == head ? 0 : first;
}
H(ExfInterlockedInsertTailList)
{
    UNUSED;
    uint32_t head = a[0], e = a[1], last = G32(head + 4);
    G32(e) = head;
    G32(e + 4) = last;
    G32(last) = e;
    G32(head + 4) = e;
    return last == head ? 0 : last;
}
H(ExfInterlockedRemoveHeadList)
{
    UNUSED;
    uint32_t head = a[0], first = G32(head);
    if (first == head)
        return 0;
    uint32_t next = G32(first);
    G32(head) = next;
    G32(next + 4) = head;
    return first;
}

/* ---- time ---- */

H(KeQueryPerformanceCounter) { UNUSED; return uptime_ns() * 3375000ull / 1000000000ull; }
H(KeQueryPerformanceFrequency) { UNUSED; return 3375000; }
H(KeQueryInterruptTime) { UNUSED; return uptime_ns() / 100; }
H(KeQuerySystemTime) { UNUSED; G64(a[0]) = system_time(); return 0; }
H(NtSetSystemTime) { UNUSED; return STATUS_SUCCESS; }
H(KeStallExecutionProcessor) { UNUSED; return 0; }

H(RtlTimeToTimeFields)
{
    UNUSED;
    uint64_t v = G64(a[0]);
    time_t secs = (time_t)(v / 10000000ull) - (time_t)11644473600ll;
    struct tm tm;
    gmtime_r(&secs, &tm);
    uint32_t tf = a[1];
    G16(tf) = (uint16_t)(tm.tm_year + 1900);
    G16(tf + 2) = (uint16_t)(tm.tm_mon + 1);
    G16(tf + 4) = (uint16_t)tm.tm_mday;
    G16(tf + 6) = (uint16_t)tm.tm_hour;
    G16(tf + 8) = (uint16_t)tm.tm_min;
    G16(tf + 10) = (uint16_t)tm.tm_sec;
    G16(tf + 12) = (uint16_t)((v / 10000) % 1000);
    G16(tf + 14) = (uint16_t)tm.tm_wday;
    return 0;
}

H(RtlTimeFieldsToTime)
{
    UNUSED;
    uint32_t tf = a[0];
    struct tm tm = { 0 };
    tm.tm_year = G16(tf) - 1900;
    tm.tm_mon = G16(tf + 2) - 1;
    tm.tm_mday = G16(tf + 4);
    tm.tm_hour = G16(tf + 6);
    tm.tm_min = G16(tf + 8);
    tm.tm_sec = G16(tf + 10);
    time_t secs = timegm(&tm);
    G64(a[1]) = to_filetime(secs) + (uint64_t)G16(tf + 12) * 10000;
    return 1;
}

/* ---- threads ---- */

H(PsCreateSystemThreadEx)
{
    UNUSED;
    /* ThreadHandle*, ExtensionSize, KernelStackSize, TlsDataSize, ThreadId*,
       StartRoutine, StartContext, CreateSuspended, DebuggerThread, SystemRoutine */
    hthread *n = thread_new(a[2] ? a[2] : 0x10000, a[3]);
    G32(n->ethread + ET_STARTADDRESS) = a[5];
    xbt_cpu *c = &n->cpu;
    if (a[9]) {
        push32(c, a[6]);
        push32(c, a[5]);
        push32(c, TRAP_THREAD_EXIT);
        c->eip = a[9];
    } else {
        push32(c, a[6]);
        push32(c, TRAP_THREAD_EXIT);
        c->eip = a[5];
    }
    uint32_t h = handle_new(H_OBJ);
    handle_get(h)->obj = n->ethread;
    if (a[0])
        G32(a[0]) = h;
    if (a[4])
        G32(a[4]) = n->id;
    if (hle_trace)
        XBT_LOG("thread %u created: start %08x context %08x system %08x tls %u\n", n->id, a[5], a[6], a[9], a[3]);
    return STATUS_SUCCESS;
}

H(PsCreateSystemThread)
{
    uint32_t b[10] = { a[0], 0, 0x10000, 0, a[1], a[2], a[3], 0, a[4], 0 };
    return h_PsCreateSystemThreadEx(t, b, block);
}

H(PsTerminateSystemThread)
{
    UNUSED;
    thread_exit(t, a[0]);
    *block = 1;
    return 0;
}

H(KeDelayExecutionThread)
{
    UNUSED;
    int poll;
    t->deadline = deadline_from(a[2], &poll);
    t->wait_delay = 1;
    t->nwait = 0;
    t->state = TS_WAIT;
    *block = 1;
    return STATUS_SUCCESS;
}

H(NtYieldExecution) { UNUSED; *block = 1; return STATUS_SUCCESS; }
H(KeSetBasePriorityThread) { UNUSED; return 0; }
H(KeSetPriorityThread) { UNUSED; return 0; }
H(KeAlertThread) { UNUSED; return 0; }
H(KeAlertResumeThread) { UNUSED; return 0; }
H(KeResumeThread) { UNUSED; return 0; }
H(KeSuspendThread) { UNUSED; return 0; }
H(KeSetDisableBoostThread) { UNUSED; return 0; }
H(NtResumeThread) { UNUSED; if (a[1]) G32(a[1]) = 0; return STATUS_SUCCESS; }
H(NtSuspendThread) { UNUSED; if (a[1]) G32(a[1]) = 0; return STATUS_SUCCESS; }
H(ObReferenceObjectByHandle)
{
    UNUSED;
    uint32_t o = handle_obj(a[0]);
    if (!o)
        return STATUS_INVALID_HANDLE;
    if (a[2])
        G32(a[2]) = o;
    return STATUS_SUCCESS;
}
H(ObfDereferenceObject) { UNUSED; return 0; }
H(ObfReferenceObject) { UNUSED; return 0; }

/* ---- dispatcher objects ---- */

H(KeInitializeEvent) { UNUSED; init_header(a[0], (int)a[1], 16, (int)(a[2] & 0xff)); return 0; }
H(KeSetEvent)
{
    UNUSED;
    int32_t old = OBJ_SIGNAL(a[0]);
    OBJ_SIGNAL(a[0]) = 1;
    return (uint32_t)old;
}
H(KeResetEvent) { UNUSED; int32_t old = OBJ_SIGNAL(a[0]); OBJ_SIGNAL(a[0]) = 0; return (uint32_t)old; }
H(KePulseEvent) { UNUSED; int32_t old = OBJ_SIGNAL(a[0]); OBJ_SIGNAL(a[0]) = 0; return (uint32_t)old; }
H(KeInitializeSemaphore) { UNUSED; init_header(a[0], 5, 20, (int)a[1]); G32(a[0] + 16) = a[2]; return 0; }
H(KeReleaseSemaphore)
{
    UNUSED;
    int32_t old = OBJ_SIGNAL(a[0]);
    OBJ_SIGNAL(a[0]) = old + (int32_t)a[2];
    return (uint32_t)old;
}
H(KeInitializeMutant)
{
    UNUSED;
    init_header(a[0], 2, 32, a[1] ? 0 : 1);
    G32(a[0] + 0x18) = a[1] ? t->ethread : 0;
    return 0;
}
H(KeReleaseMutant)
{
    UNUSED;
    int32_t old = OBJ_SIGNAL(a[0]);
    OBJ_SIGNAL(a[0]) = old + 1;
    if (OBJ_SIGNAL(a[0]) > 0)
        G32(a[0] + 0x18) = 0;
    return (uint32_t)old;
}
H(KeInitializeTimerEx)
{
    UNUSED;
    init_header(a[0], 8 + (int)a[1], 40, 0);
    return 0;
}
H(KeSetTimerEx)
{
    UNUSED;
    /* Timer, DueTime (2 slots), Period, Dpc */
    int was = cancel_timer(a[0]);
    int poll;
    G32(t->cpu.r[R_ESP] - 64) = a[1];
    G32(t->cpu.r[R_ESP] - 60) = a[2];
    uint64_t due = deadline_from(t->cpu.r[R_ESP] - 64, &poll);
    ktimer_rec *r = calloc(1, sizeof *r);
    r->timer = a[0];
    r->due = due ? due : host_ns();
    r->period_ms = a[3];
    r->dpc = a[4];
    r->next = timers;
    timers = r;
    OBJ_SIGNAL(a[0]) = 0;
    G8(a[0] + 3) = 1;
    G32(a[0] + 0x20) = a[4];
    G32(a[0] + 0x24) = a[3];
    return (uint32_t)was;
}
H(KeSetTimer)
{
    uint32_t b[5] = { a[0], a[1], a[2], 0, a[3] };
    return h_KeSetTimerEx(t, b, block);
}
H(KeCancelTimer) { UNUSED; return (uint32_t)cancel_timer(a[0]); }
H(KeInitializeDpc)
{
    UNUSED;
    uint32_t d = a[0];
    G16(d) = 19;          /* DpcObject */
    G8(d + 2) = 0;
    G32(d + 0x0c) = a[1];
    G32(d + 0x10) = a[2];
    return 0;
}
H(KeInsertQueueDpc) { UNUSED; return (uint32_t)queue_dpc(a[0], a[1], a[2], 1); }
H(KeRemoveQueueDpc)
{
    UNUSED;
    for (int i = 0; i < ndpc; i++)
        if (dpcq[i] == a[0]) {
            memmove(dpcq + i, dpcq + i + 1, sizeof(uint32_t) * (size_t)(ndpc - i - 1));
            ndpc--;
            G8(a[0] + 2) = 0;
            return 1;
        }
    return 0;
}
H(KeInitializeInterrupt) { UNUSED; return 0; }
H(KeConnectInterrupt)
{
    UNUSED;
    if (hle_trace)
        XBT_LOG("KeConnectInterrupt(%08x): interrupts are not delivered\n", a[0]);
    return 1;
}
H(KeDisconnectInterrupt) { UNUSED; return 1; }
H(KeIsExecutingDpc) { UNUSED; return t->is_dpc; }

H(KeWaitForSingleObject)
{
    uint32_t o = a[0];
    return begin_wait(t, &o, 1, 0, a[4], block);
}
H(KeWaitForMultipleObjects)
{
    /* Count, Object[], WaitType, WaitReason, WaitMode, Alertable, Timeout, WaitBlockArray */
    uint32_t objs[64];
    int n = (int)a[0] > 64 ? 64 : (int)a[0];
    for (int i = 0; i < n; i++)
        objs[i] = G32(a[1] + 4 * (uint32_t)i);
    return begin_wait(t, objs, n, a[2] == 0, a[6], block);
}
H(NtWaitForSingleObjectEx)
{
    uint32_t o = handle_obj(a[0]);
    if (!o)
        return STATUS_INVALID_HANDLE;
    return begin_wait(t, &o, 1, 0, a[3], block);
}
H(NtWaitForSingleObject)
{
    uint32_t o = handle_obj(a[0]);
    if (!o)
        return STATUS_INVALID_HANDLE;
    return begin_wait(t, &o, 1, 0, a[2], block);
}
H(NtWaitForMultipleObjectsEx)
{
    /* Count, Handles[], WaitType, WaitMode, Alertable, Timeout */
    uint32_t objs[64];
    int n = (int)a[0] > 64 ? 64 : (int)a[0];
    for (int i = 0; i < n; i++) {
        objs[i] = handle_obj(G32(a[1] + 4 * (uint32_t)i));
        if (!objs[i])
            return STATUS_INVALID_HANDLE;
    }
    return begin_wait(t, objs, n, a[2] == 0, a[5], block);
}

static uint64_t create_obj(uint32_t ph, int type, int size, int signal)
{
    uint32_t o = hle_alloc((uint32_t)size);
    init_header(o, type, size, signal);
    uint32_t h = handle_new(H_OBJ);
    handle_get(h)->obj = o;
    G32(ph) = h;
    return STATUS_SUCCESS;
}

H(NtCreateEvent) { UNUSED; return create_obj(a[0], (int)a[2], 16, (int)(a[3] & 0xff)); }
H(NtCreateSemaphore)
{
    UNUSED;
    uint64_t r = create_obj(a[0], 5, 20, (int)a[2]);
    G32(handle_obj(G32(a[0])) + 16) = a[3];
    return r;
}
H(NtCreateMutant)
{
    UNUSED;
    uint64_t r = create_obj(a[0], 2, 32, a[2] ? 0 : 1);
    if (a[2])
        G32(handle_obj(G32(a[0])) + 0x18) = t->ethread;
    return r;
}
H(NtCreateTimer) { UNUSED; return create_obj(a[0], 8 + (int)a[2], 40, 0); }
H(NtSetEvent)
{
    UNUSED;
    uint32_t o = handle_obj(a[0]);
    if (!o)
        return STATUS_INVALID_HANDLE;
    if (a[1])
        G32(a[1]) = (uint32_t)OBJ_SIGNAL(o);
    OBJ_SIGNAL(o) = 1;
    return STATUS_SUCCESS;
}
H(NtClearEvent) { UNUSED; uint32_t o = handle_obj(a[0]); if (!o) return STATUS_INVALID_HANDLE; OBJ_SIGNAL(o) = 0; return STATUS_SUCCESS; }
H(NtPulseEvent) { UNUSED; uint32_t o = handle_obj(a[0]); if (!o) return STATUS_INVALID_HANDLE; if (a[1]) G32(a[1]) = (uint32_t)OBJ_SIGNAL(o); OBJ_SIGNAL(o) = 0; return STATUS_SUCCESS; }
H(NtReleaseSemaphore)
{
    UNUSED;
    uint32_t o = handle_obj(a[0]);
    if (!o)
        return STATUS_INVALID_HANDLE;
    if (a[2])
        G32(a[2]) = (uint32_t)OBJ_SIGNAL(o);
    OBJ_SIGNAL(o) += (int32_t)a[1];
    return STATUS_SUCCESS;
}
H(NtReleaseMutant)
{
    UNUSED;
    uint32_t o = handle_obj(a[0]);
    if (!o)
        return STATUS_INVALID_HANDLE;
    if (a[1])
        G32(a[1]) = (uint32_t)OBJ_SIGNAL(o);
    OBJ_SIGNAL(o)++;
    if (OBJ_SIGNAL(o) > 0)
        G32(o + 0x18) = 0;
    return STATUS_SUCCESS;
}
H(NtSetTimerEx)
{
    UNUSED;
    /* TimerHandle, DueTime*, ApcRoutine, ApcMode, ApcContext, Resume, Period, PreviousState* */
    uint32_t o = handle_obj(a[0]);
    if (!o)
        return STATUS_INVALID_HANDLE;
    cancel_timer(o);
    int poll;
    ktimer_rec *r = calloc(1, sizeof *r);
    r->timer = o;
    r->due = deadline_from(a[1], &poll);
    if (!r->due)
        r->due = host_ns();
    r->period_ms = a[6];
    r->next = timers;
    timers = r;
    OBJ_SIGNAL(o) = 0;
    if (a[7])
        G8(a[7]) = 0;
    return STATUS_SUCCESS;
}
H(NtCancelTimer) { UNUSED; uint32_t o = handle_obj(a[0]); if (a[1]) G8(a[1]) = 0; if (o) cancel_timer(o); return STATUS_SUCCESS; }
H(NtDuplicateObject)
{
    UNUSED;
    handle *e = handle_get(a[0]);
    if (!e && a[0] != 0xfffffffeu)
        return STATUS_INVALID_HANDLE;
    uint32_t h = handle_new(H_OBJ);
    if (e) {
        handle *n = handle_get(h);
        *n = *e;
        if (e->fd >= 0)
            n->fd = dup(e->fd);
        n->dir = NULL;
    } else {
        handle_get(h)->obj = t->ethread;
    }
    G32(a[1]) = h;
    return STATUS_SUCCESS;
}
H(NtClose)
{
    UNUSED;
    handle *e = handle_get(a[0]);
    if (!e)
        return STATUS_INVALID_HANDLE;
    if (e->fd >= 0)
        close(e->fd);
    if (e->dir)
        closedir(e->dir);
    e->type = H_FREE;
    return STATUS_SUCCESS;
}

/* ---- critical sections ---- */
/* Synchronization event (16 bytes), LockCount (-1 free), RecursionCount, OwningThread */

H(RtlInitializeCriticalSection)
{
    UNUSED;
    init_header(a[0], 1, 16, 0);
    G32(a[0] + 16) = 0xffffffffu;
    G32(a[0] + 20) = 0;
    G32(a[0] + 24) = 0;
    return 0;
}

H(RtlEnterCriticalSection)
{
    uint32_t cs = a[0];
    if (G32(cs + 24) == t->ethread) {
        G32(cs + 20)++;
        G32(cs + 16)++;
        return 0;
    }
    if (++*(volatile int32_t *)GPTR(cs + 16) == 0) {
        G32(cs + 24) = t->ethread;
        G32(cs + 20) = 1;
        return 0;
    }
    /* contended: wait for the event; ownership is taken when the wait ends */
    uint32_t ev = cs;
    uint64_t r = begin_wait(t, &ev, 1, 0, 0, block);
    if (!*block) {
        G32(cs + 24) = t->ethread;
        G32(cs + 20) = 1;
    } else {
        t->wait_cs = cs;
    }
    return r;
}

H(RtlTryEnterCriticalSection)
{
    UNUSED;
    uint32_t cs = a[0];
    if (G32(cs + 24) == t->ethread) {
        G32(cs + 20)++;
        G32(cs + 16)++;
        return 1;
    }
    if (*(volatile int32_t *)GPTR(cs + 16) == -1) {
        G32(cs + 16) = 0;
        G32(cs + 24) = t->ethread;
        G32(cs + 20) = 1;
        return 1;
    }
    return 0;
}

H(RtlLeaveCriticalSection)
{
    UNUSED;
    uint32_t cs = a[0];
    if (--*(volatile int32_t *)GPTR(cs + 20) > 0) {
        --*(volatile int32_t *)GPTR(cs + 16);
        return 0;
    }
    G32(cs + 24) = 0;
    if (--*(volatile int32_t *)GPTR(cs + 16) >= 0)
        OBJ_SIGNAL(cs) = 1;          /* wake one waiter (synchronization event) */
    return 0;
}

/* ---- Rtl ---- */

H(RtlInitAnsiString)
{
    UNUSED;
    uint32_t d = a[0], s = a[1];
    uint32_t len = s ? (uint32_t)strlen(GPTR(s)) : 0;
    G16(d) = (uint16_t)len;
    G16(d + 2) = (uint16_t)(s ? len + 1 : 0);
    G32(d + 4) = s;
    return 0;
}
H(RtlInitUnicodeString)
{
    UNUSED;
    uint32_t d = a[0], s = a[1], len = 0;
    if (s)
        while (G16(s + 2 * len))
            len++;
    G16(d) = (uint16_t)(2 * len);
    G16(d + 2) = (uint16_t)(s ? 2 * len + 2 : 0);
    G32(d + 4) = s;
    return 0;
}
H(RtlEqualString)
{
    UNUSED;
    char x[600], y[600];
    get_ansi(a[0], x, sizeof x);
    get_ansi(a[1], y, sizeof y);
    return a[2] ? !strcasecmp(x, y) : !strcmp(x, y);
}
H(RtlCompareString)
{
    UNUSED;
    char x[600], y[600];
    get_ansi(a[0], x, sizeof x);
    get_ansi(a[1], y, sizeof y);
    return (uint32_t)(a[2] ? strcasecmp(x, y) : strcmp(x, y));
}
H(RtlUpperChar) { UNUSED; return (uint32_t)toupper((int)(a[0] & 0xff)); }
H(RtlLowerChar) { UNUSED; return (uint32_t)tolower((int)(a[0] & 0xff)); }
H(RtlUnicodeStringToAnsiString)
{
    UNUSED;
    uint32_t d = a[0], s = a[1];
    uint32_t len = G16(s) / 2, buf = G32(s + 4);
    if (a[2]) {
        uint32_t p = pool_alloc(len + 1);
        G32(d + 4) = p;
        G16(d + 2) = (uint16_t)(len + 1);
    }
    uint32_t out = G32(d + 4), max = G16(d + 2);
    uint32_t n = len < max ? len : max ? max - 1 : 0;
    for (uint32_t i = 0; i < n; i++)
        G8(out + i) = (uint8_t)G16(buf + 2 * i);
    if (n < max)
        G8(out + n) = 0;
    G16(d) = (uint16_t)n;
    return STATUS_SUCCESS;
}
H(RtlAnsiStringToUnicodeString)
{
    UNUSED;
    uint32_t d = a[0], s = a[1];
    uint32_t len = G16(s), buf = G32(s + 4);
    if (a[2]) {
        uint32_t p = pool_alloc(2 * len + 2);
        G32(d + 4) = p;
        G16(d + 2) = (uint16_t)(2 * len + 2);
    }
    uint32_t out = G32(d + 4);
    for (uint32_t i = 0; i < len; i++)
        G16(out + 2 * i) = G8(buf + i);
    G16(out + 2 * len) = 0;
    G16(d) = (uint16_t)(2 * len);
    return STATUS_SUCCESS;
}
H(RtlFreeAnsiString) { UNUSED; pool_free(G32(a[0] + 4)); return 0; }
H(RtlFreeUnicodeString) { UNUSED; pool_free(G32(a[0] + 4)); return 0; }
H(RtlCompareMemory)
{
    UNUSED;
    uint32_t i;
    for (i = 0; i < a[2]; i++)
        if (G8(a[0] + i) != G8(a[1] + i))
            break;
    return i;
}
H(RtlCompareMemoryUlong)
{
    UNUSED;
    uint32_t i;
    for (i = 0; i + 4 <= a[1]; i += 4)
        if (G32(a[0] + i) != a[2])
            break;
    return i;
}
H(RtlFillMemory) { UNUSED; memset(GPTR(a[0]), (int)(a[2] & 0xff), a[1]); return 0; }
H(RtlFillMemoryUlong)
{
    UNUSED;
    for (uint32_t i = 0; i + 4 <= a[1]; i += 4)
        G32(a[0] + i) = a[2];
    return 0;
}
H(RtlMoveMemory) { UNUSED; memmove(GPTR(a[0]), GPTR(a[1]), a[2]); return 0; }
H(RtlZeroMemory) { UNUSED; memset(GPTR(a[0]), 0, a[1]); return 0; }
H(RtlNtStatusToDosError)
{
    UNUSED;
    switch (a[0]) {
    case STATUS_SUCCESS: return 0;
    case STATUS_NO_SUCH_FILE: case STATUS_OBJECT_NAME_NOT_FOUND: return 2;
    case STATUS_OBJECT_PATH_NOT_FOUND: return 3;
    case STATUS_ACCESS_DENIED: return 5;
    case STATUS_INVALID_HANDLE: return 6;
    case STATUS_NO_MEMORY: return 8;
    case STATUS_END_OF_FILE: return 38;
    case STATUS_OBJECT_NAME_COLLISION: return 183;
    case STATUS_TIMEOUT: return 1460;
    case STATUS_NO_MORE_FILES: return 18;
    default: return 317;
    }
}
H(RtlRaiseException)
{
    UNUSED;
    /* the exception occurs at the return address of this call */
    uint32_t esp = t->cpu.r[R_ESP];
    uint32_t ret = G32(esp);
    G32(a[0] + 12) = ret;                     /* ExceptionAddress */
    t->cpu.r[R_ESP] = esp + 8;               /* as if returned (stdcall, 1 arg) */
    seh_dispatch(t, a[0], ret, esp + 8);
    *block = 2;                              /* continue in the dispatch state */
    return 0;
}
H(RtlUnwind)
{
    UNUSED;
    /* TargetFrame, TargetIp, ExceptionRecord, ReturnValue */
    seh_state *s = seh_of(t);
    xbt_cpu *c = &t->cpu;
    uint32_t esp = c->r[R_ESP];
    s->ret_eip = G32(esp);
    s->ret_esp = esp + 4 + 16;
    for (int r = 0; r < 8; r++)
        s->saved[r] = c->r[r];
    s->target_frame = a[0] ? a[0] : 0xffffffffu;
    uint32_t sp = (esp - 0x400) & ~15u;
    s->record = a[2];
    if (!s->record) {
        s->record = sp - 0x60;
        memset(GPTR(s->record), 0, 0x50);
        G32(s->record) = 0xc0000027;          /* STATUS_UNWIND */
        G32(s->record + 12) = s->ret_eip;
    }
    G32(s->record + 4) |= EXCEPTION_UNWINDING | (a[0] ? 0 : EXCEPTION_EXIT_UNWIND);
    s->context = (sp - 0x100 - CONTEXT_SIZE) & ~15u;
    context_store(s->context, c, s->ret_eip, s->ret_esp);
    s->dispctx = s->context - 16;
    s->unwinding = 1;
    unwind_step(t);
    *block = 2;
    return 0;
}
H(RtlRip) { UNUSED; XBT_LOG("RtlRip\n"); return 0; }
H(RtlAssert)
{
    UNUSED;
    XBT_LOG("RtlAssert: %s (%s:%u) %s\n", a[0] ? (char *)GPTR(a[0]) : "", a[1] ? (char *)GPTR(a[1]) : "", a[2],
            a[3] ? (char *)GPTR(a[3]) : "");
    return 0;
}

/* ---- non-volatile settings / misc ---- */

H(ExQueryNonVolatileSetting)
{
    UNUSED;
    /* ValueIndex, Type*, Value, ValueLength, ResultLength* */
    if (a[1])
        G32(a[1]) = 4;
    if (a[2] && a[3])
        memset(GPTR(a[2]), 0, a[3]);
    uint32_t v = 0;
    switch (a[0]) {
    case 0x103: v = 0x00400000; break;    /* XC_VIDEO_FLAGS: allow 480p etc. */
    case 0x104: v = 0x00000100; break;    /* XC_AUDIO_FLAGS */
    case 0x007: v = 1; break;             /* language: English */
    case 0x008: v = 0x00400100; break;    /* video standard NTSC-M */
    case 0x011: v = 1; break;             /* game region NA */
    }
    if (a[2] && a[3] >= 4)
        G32(a[2]) = v;
    if (a[4])
        G32(a[4]) = 4;
    return STATUS_SUCCESS;
}
H(ExSaveNonVolatileSetting) { UNUSED; return STATUS_SUCCESS; }
H(ExReadWriteRefurbInfo) { UNUSED; return STATUS_SUCCESS; }
H(XeLoadSection) { UNUSED; G32(a[0] + 0x18)++; return STATUS_SUCCESS; }
H(XeUnloadSection) { UNUSED; if (G32(a[0] + 0x18)) G32(a[0] + 0x18)--; return STATUS_SUCCESS; }
H(FscSetCacheSize) { UNUSED; return STATUS_SUCCESS; }
H(FscGetCacheSize) { UNUSED; return 16; }
H(FscInvalidateIdleBlocks) { UNUSED; return 0; }
H(KeSaveFloatingPointState) { UNUSED; return STATUS_SUCCESS; }
H(KeRestoreFloatingPointState) { UNUSED; return STATUS_SUCCESS; }
H(IoCreateSymbolicLink)
{
    UNUSED;
    char from[600], to[600];
    get_ansi(a[0], from, sizeof from);
    get_ansi(a[1], to, sizeof to);
    add_link(from, to);
    if (hle_trace)
        XBT_LOG("symbolic link %s -> %s\n", from, to);
    return STATUS_SUCCESS;
}
H(IoDeleteSymbolicLink) { UNUSED; return STATUS_SUCCESS; }
H(NtOpenSymbolicLinkObject)
{
    UNUSED;
    char name[600];
    oa_path(a[1], name, sizeof name);
    for (int i = 0; i < nlinks; i++) {
        if (!strcasecmp(links[i].from, name)) {
            uint32_t h = handle_new(H_LINK);
            snprintf(handle_get(h)->path, sizeof htab[0].path, "%s", links[i].to);
            G32(a[0]) = h;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
H(NtQuerySymbolicLinkObject)
{
    UNUSED;
    handle *e = handle_get(a[0]);
    if (!e || e->type != H_LINK)
        return STATUS_INVALID_HANDLE;
    uint32_t len = (uint32_t)strlen(e->path);
    uint32_t buf = G32(a[1] + 4), max = G16(a[1] + 2);
    if (a[2])
        G32(a[2]) = len;
    if (len > max)
        return STATUS_BUFFER_TOO_SMALL;
    memcpy(GPTR(buf), e->path, len);
    G16(a[1]) = (uint16_t)len;
    return STATUS_SUCCESS;
}

/* ---- crypto used by titles (RC4, SHA-1, HMAC-SHA1) ---- */

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha1;

static uint32_t rol(uint32_t x, int s) { return x << s | x >> (32 - s); }

static void sha1_block(sha1 *c, const uint8_t *p)
{
    uint32_t w[80], a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8f1bbcdc; }
        else { f = b ^ cc ^ d; k = 0xca62c1d6; }
        uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(sha1 *c)
{
    static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

static void sha1_update(sha1 *c, const uint8_t *p, uint32_t n)
{
    c->len += n;
    while (n--) {
        c->buf[c->n++] = *p++;
        if (c->n == 64) {
            sha1_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha1_final(sha1 *c, uint8_t *out)
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0;
    sha1_update(c, &pad, 1);
    while (c->n != 56)
        sha1_update(c, &z, 1);
    for (int i = 7; i >= 0; i--) {
        uint8_t b = (uint8_t)(bits >> (8 * i));
        sha1_update(c, &b, 1);
    }
    for (int i = 0; i < 20; i++)
        out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* XcSHA*: the context lives in guest memory; keep it there as our struct */
H(XcSHAInit) { UNUSED; sha1 c; sha1_init(&c); memcpy(GPTR(a[0]), &c, sizeof c); return 0; }
H(XcSHAUpdate) { UNUSED; sha1 c; memcpy(&c, GPTR(a[0]), sizeof c); sha1_update(&c, GPTR(a[1]), a[2]); memcpy(GPTR(a[0]), &c, sizeof c); return 0; }
H(XcSHAFinal) { UNUSED; sha1 c; memcpy(&c, GPTR(a[0]), sizeof c); sha1_final(&c, GPTR(a[1])); return 0; }

H(XcRC4Key)
{
    UNUSED;
    uint8_t *s = GPTR(a[0]), *k = GPTR(a[2]);
    for (int i = 0; i < 256; i++)
        s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + s[i] + k[i % a[1]]) & 255;
        uint8_t x = s[i]; s[i] = s[j]; s[j] = x;
    }
    s[256] = s[257] = 0;
    return 0;
}
H(XcRC4Crypt)
{
    UNUSED;
    uint8_t *s = GPTR(a[0]), *d = GPTR(a[2]);
    int i = s[256], j = s[257];
    for (uint32_t n = 0; n < a[1]; n++) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        uint8_t x = s[i]; s[i] = s[j]; s[j] = x;
        d[n] ^= s[(s[i] + s[j]) & 255];
    }
    s[256] = (uint8_t)i;
    s[257] = (uint8_t)j;
    return 0;
}
H(XcHMAC)
{
    UNUSED;
    /* key, keylen, data1, len1, data2, len2, digest */
    uint8_t k[64] = { 0 }, ipad[64], opad[64], inner[20];
    memcpy(k, GPTR(a[0]), a[1] > 64 ? 64 : a[1]);
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    sha1 c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    if (a[2])
        sha1_update(&c, GPTR(a[2]), a[3]);
    if (a[4])
        sha1_update(&c, GPTR(a[4]), a[5]);
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, GPTR(a[6]));
    return 0;
}

/* ---- files ---- */

static uint64_t open_common(hthread *t, uint32_t ph, uint32_t oa, uint32_t iosb_p, uint32_t disposition,
                            uint32_t options)
{
    (void)t;
    char xpath[700], host[1024];
    oa_path(oa, xpath, sizeof xpath);
    if (xpath[0] == '@') {
        snprintf(host, sizeof host, "%s", xpath + 1);
        for (char *q = host; *q; q++)
            if (*q == '\\')
                *q = '/';
    } else if (resolve_path(xpath, host, sizeof host)) {
        if (hle_trace)
            XBT_LOG("open %s: no such device\n", xpath);
        iosb(iosb_p, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    struct stat st;
    int exists = stat(host, &st) == 0;
    /* dispositions: 0 supersede 1 open 2 create 3 open_if 4 overwrite 5 overwrite_if */
    if (!exists && (disposition == 1 || disposition == 4)) {
        if (hle_trace)
            XBT_LOG("open %s (%s): not found\n", xpath, host);
        iosb(iosb_p, STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (exists && disposition == 2) {
        iosb(iosb_p, STATUS_OBJECT_NAME_COLLISION, 0);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    uint32_t h;
    if ((exists && S_ISDIR(st.st_mode)) || (options & 1 /* FILE_DIRECTORY_FILE */)) {
        if (!exists && mkdir(host, 0755) != 0) {
            iosb(iosb_p, STATUS_OBJECT_PATH_NOT_FOUND, 0);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        h = handle_new(H_DIR);
    } else {
        int flags = O_RDWR;
        if (!exists)
            flags |= O_CREAT;
        if (disposition == 0 || disposition == 4 || disposition == 5)
            flags |= O_TRUNC;
        int fd = open(host, flags | O_CLOEXEC, 0644);
        if (fd < 0)
            fd = open(host, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            iosb(iosb_p, STATUS_ACCESS_DENIED, 0);
            return STATUS_ACCESS_DENIED;
        }
        h = handle_new(H_FILE);
        handle_get(h)->fd = fd;
    }
    snprintf(handle_get(h)->path, sizeof htab[0].path, "%s", host);
    G32(ph) = h;
    iosb(iosb_p, STATUS_SUCCESS, exists ? 1 : 2);
    if (hle_trace)
        XBT_LOG("open %s -> %s (handle %x)\n", xpath, host, h);
    return STATUS_SUCCESS;
}

H(NtCreateFile)
{
    UNUSED;
    /* FileHandle*, DesiredAccess, OA, IoStatusBlock, AllocationSize*, FileAttributes,
       ShareAccess, CreateDisposition, CreateOptions */
    return open_common(t, a[0], a[2], a[3], a[7], a[8]);
}
H(NtOpenFile)
{
    UNUSED;
    /* FileHandle*, DesiredAccess, OA, IoStatusBlock, ShareAccess, OpenOptions */
    return open_common(t, a[0], a[2], a[3], 1, a[5]);
}
H(NtReadFile)
{
    UNUSED;
    /* FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset* */
    handle *e = handle_get(a[0]);
    if (!e || e->fd < 0)
        return STATUS_INVALID_HANDLE;
    if (a[7] && G32(a[7] + 4) != 0xffffffffu)
        lseek(e->fd, (off_t)G64(a[7]), SEEK_SET);
    ssize_t n = read(e->fd, GPTR(a[5]), a[6]);
    if (n <= 0) {
        iosb(a[4], STATUS_END_OF_FILE, 0);
        return STATUS_END_OF_FILE;
    }
    iosb(a[4], STATUS_SUCCESS, (uint32_t)n);
    if (a[1])
        OBJ_SIGNAL(handle_obj(a[1])) = 1;
    return STATUS_SUCCESS;
}
H(NtWriteFile)
{
    UNUSED;
    handle *e = handle_get(a[0]);
    if (!e || e->fd < 0)
        return STATUS_INVALID_HANDLE;
    if (a[7] && G32(a[7] + 4) != 0xffffffffu)
        lseek(e->fd, (off_t)G64(a[7]), SEEK_SET);
    ssize_t n = write(e->fd, GPTR(a[5]), a[6]);
    iosb(a[4], STATUS_SUCCESS, n > 0 ? (uint32_t)n : 0);
    if (a[1])
        OBJ_SIGNAL(handle_obj(a[1])) = 1;
    return STATUS_SUCCESS;
}
H(NtFlushBuffersFile) { UNUSED; iosb(a[1], 0, 0); return STATUS_SUCCESS; }

static void fill_basic(uint32_t p, const struct stat *st)
{
    G64(p) = to_filetime(st->st_ctime);
    G64(p + 8) = to_filetime(st->st_atime);
    G64(p + 16) = to_filetime(st->st_mtime);
    G64(p + 24) = to_filetime(st->st_mtime);
    G32(p + 32) = file_attrs(st);
}

H(NtQueryInformationFile)
{
    UNUSED;
    /* FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass */
    handle *e = handle_get(a[0]);
    if (!e)
        return STATUS_INVALID_HANDLE;
    struct stat st;
    if (e->fd >= 0)
        fstat(e->fd, &st);
    else
        stat(e->path, &st);
    uint32_t p = a[2];
    switch (a[4]) {
    case 4:   /* FileBasicInformation */
        fill_basic(p, &st);
        iosb(a[1], 0, 36);
        break;
    case 5:   /* FileStandardInformation */
        G64(p) = ((uint64_t)st.st_size + 4095) & ~4095ull;
        G64(p + 8) = (uint64_t)st.st_size;
        G32(p + 16) = 1;
        G8(p + 20) = 0;
        G8(p + 21) = S_ISDIR(st.st_mode);
        iosb(a[1], 0, 24);
        break;
    case 14:  /* FilePositionInformation */
        G64(p) = e->fd >= 0 ? (uint64_t)lseek(e->fd, 0, SEEK_CUR) : 0;
        iosb(a[1], 0, 8);
        break;
    case 34:  /* FileNetworkOpenInformation */
        G64(p) = to_filetime(st.st_ctime);
        G64(p + 8) = to_filetime(st.st_atime);
        G64(p + 16) = to_filetime(st.st_mtime);
        G64(p + 24) = to_filetime(st.st_mtime);
        G64(p + 32) = ((uint64_t)st.st_size + 4095) & ~4095ull;
        G64(p + 40) = (uint64_t)st.st_size;
        G32(p + 48) = file_attrs(&st);
        iosb(a[1], 0, 56);
        break;
    default:
        if (hle_trace)
            XBT_LOG("NtQueryInformationFile class %u not supported\n", a[4]);
        memset(GPTR(p), 0, a[3]);
        iosb(a[1], 0, a[3]);
    }
    return STATUS_SUCCESS;
}

H(NtSetInformationFile)
{
    UNUSED;
    handle *e = handle_get(a[0]);
    if (!e)
        return STATUS_INVALID_HANDLE;
    uint32_t p = a[2];
    switch (a[4]) {
    case 14:  /* position */
        if (e->fd >= 0)
            lseek(e->fd, (off_t)G64(p), SEEK_SET);
        break;
    case 19: case 20:  /* allocation / end of file */
        if (e->fd >= 0 && a[4] == 20 && ftruncate(e->fd, (off_t)G64(p)) != 0)
            return STATUS_ACCESS_DENIED;
        break;
    case 13:  /* disposition: delete on close */
        if (G8(p))
            unlink(e->path);
        break;
    case 10: {   /* rename: FILE_RENAME_INFORMATION { Replace, RootDir, OBJECT_STRING Name } */
        char xp[600], host[1024];
        get_ansi(p + 8, xp, sizeof xp);
        if (!resolve_path(xp, host, sizeof host))
            rename(e->path, host);
        break;
    }
    }
    iosb(a[1], 0, 0);
    return STATUS_SUCCESS;
}

H(NtQueryFullAttributesFile)
{
    UNUSED;
    char xpath[700], host[1024];
    oa_path(a[0], xpath, sizeof xpath);
    struct stat st;
    if ((xpath[0] == '@' ? (snprintf(host, sizeof host, "%s", xpath + 1), 0) : resolve_path(xpath, host, sizeof host)) ||
        stat(host, &st) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    uint32_t p = a[1];
    G64(p) = to_filetime(st.st_ctime);
    G64(p + 8) = to_filetime(st.st_atime);
    G64(p + 16) = to_filetime(st.st_mtime);
    G64(p + 24) = to_filetime(st.st_mtime);
    G64(p + 32) = ((uint64_t)st.st_size + 4095) & ~4095ull;
    G64(p + 40) = (uint64_t)st.st_size;
    G32(p + 48) = file_attrs(&st);
    return STATUS_SUCCESS;
}

H(NtQueryVolumeInformationFile)
{
    UNUSED;
    uint32_t p = a[2];
    memset(GPTR(p), 0, a[3]);
    if (a[4] == 3) {   /* FileFsSizeInformation */
        G64(p) = 0x100000;        /* total allocation units */
        G64(p + 8) = 0x80000;     /* available */
        G32(p + 16) = 32;         /* sectors per unit */
        G32(p + 20) = 512;
    }
    iosb(a[1], 0, a[3]);
    return STATUS_SUCCESS;
}

H(NtQueryDirectoryFile)
{
    UNUSED;
    /* FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation,
       Length, FileInformationClass, FileName (pattern), RestartScan */
    handle *e = handle_get(a[0]);
    if (!e || e->type != H_DIR)
        return STATUS_INVALID_HANDLE;
    char pat[260] = "*";
    if (a[8])
        get_ansi(a[8], pat, sizeof pat);
    if (!e->dir || a[9]) {
        if (e->dir)
            closedir(e->dir);
        e->dir = opendir(e->path);
    }
    struct dirent *de;
    while (e->dir && (de = readdir(e->dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        /* simple wildcard: "*", "*.ext", exact */
        if (strcmp(pat, "*") && strcmp(pat, "*.*")) {
            if (pat[0] == '*') {
                size_t lp = strlen(pat + 1), ln = strlen(de->d_name);
                if (ln < lp || strcasecmp(de->d_name + ln - lp, pat + 1))
                    continue;
            } else if (strcasecmp(pat, de->d_name)) {
                continue;
            }
        }
        char full[1100];
        snprintf(full, sizeof full, "%s/%s", e->path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        /* FILE_DIRECTORY_INFORMATION */
        uint32_t p = a[5];
        uint32_t nlen = (uint32_t)strlen(de->d_name);
        memset(GPTR(p), 0, 64);
        G32(p) = 0;                               /* NextEntryOffset */
        G64(p + 8) = to_filetime(st.st_ctime);
        G64(p + 16) = to_filetime(st.st_atime);
        G64(p + 24) = to_filetime(st.st_mtime);
        G64(p + 32) = to_filetime(st.st_mtime);
        G64(p + 40) = (uint64_t)st.st_size;
        G64(p + 48) = ((uint64_t)st.st_size + 4095) & ~4095ull;
        G32(p + 56) = file_attrs(&st);
        G32(p + 60) = nlen;
        memcpy(GPTR(p + 64), de->d_name, nlen);
        iosb(a[4], 0, 64 + nlen);
        return STATUS_SUCCESS;
    }
    iosb(a[4], STATUS_NO_MORE_FILES, 0);
    return STATUS_NO_MORE_FILES;
}

H(NtDeviceIoControlFile)
{
    UNUSED;
    if (hle_trace)
        XBT_LOG("NtDeviceIoControlFile(handle %x, code %08x): no device\n", a[0], a[5]);
    iosb(a[4], STATUS_INVALID_DEVICE_REQUEST, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}
H(NtFsControlFile)
{
    UNUSED;
    iosb(a[4], STATUS_INVALID_DEVICE_REQUEST, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static uint64_t unimplemented(hthread *t, int ord, uint32_t *a, int *block)
{
    (void)t;
    (void)block;
    if (!warned[ord] || hle_trace) {
        warned[ord] = 1;
        XBT_LOG("unimplemented kernel call %s (ordinal %d) args %08x %08x %08x %08x\n",
                kexp[ord].name, ord, a[0], a[1], a[2], a[3]);
    }
    if (hle_strict) {
        stop_requested = 1;
        exit_code = 2;
    }
    return STATUS_SUCCESS;
}

static int hle_call(xbt_cpu *cpu, uint32_t eip)
{
    hthread *t = cpu->user;
    if (eip == TRAP_THREAD_EXIT) {
        if (t == dpc_thread) {
            t->state = TS_DONE;   /* DPC finished; the thread is reused */
            t->is_dpc = 0;
            return 1;
        }
        thread_exit(t, cpu->r[R_EAX]);
        return 1;
    }
    if (eip == TRAP_SEH_RET) {
        seh_handler_returned(t);
        return 0;
    }
    if (eip == TRAP_UNWIND_RET) {
        unwind_step(t);
        return 0;
    }
    uint32_t ord = (eip - XBOX_HLE_BASE) / 4;
    if (ord >= 400 || !kexp[ord].name || kexp[ord].conv == 'V') {
        XBT_LOG("call into kernel data at %08x\n", eip);
        return -1;
    }
    const kexp_t *e = &kexp[ord];
    uint32_t args[16] = { 0 };
    uint32_t esp = cpu->r[R_ESP];
    int n = e->nargs < 0 ? 4 : e->nargs;
    if (n > 16)
        n = 16;
    for (int i = 0; i < n; i++) {
        if (e->conv == 'F' && i < 2)
            args[i] = cpu->r[i == 0 ? R_ECX : R_EDX];
        else
            args[i] = G32(esp + 4 + 4 * (uint32_t)(e->conv == 'F' ? i - 2 : i));
    }
    if (hle_trace)
        XBT_LOG("[t%u] %s(%08x, %08x, %08x, %08x, %08x) from %08x\n", t->id, e->name, args[0], args[1],
                args[2], args[3], args[4], G32(esp));
    int block = 0;
    uint64_t r = handlers[ord] ? handlers[ord](t, args, &block) : unimplemented(t, (int)ord, args, &block);
    if (block == 2)
        return 0;                /* handler redirected execution itself */
    if (block) {
        t->pending_ord = (int)ord;
        return 1;
    }
    finish_call(cpu, (int)ord, r);
    if (hle_trace > 1)
        XBT_LOG("[t%u]   -> %08x\n", t->id, (uint32_t)r);
    return stop_requested ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

#define REG(name) { #name, h_##name }
static const struct { const char *name; khandler fn; } impl[] = {
    REG(DbgPrint), REG(DbgBreakPoint), REG(KeBugCheck), REG(KeBugCheckEx), REG(HalReturnToFirmware),
    REG(HalInitiateShutdown), REG(HalIsResetOrShutdownPending), REG(HalReadSMCTrayState),
    REG(HalReadSMBusValue), REG(HalWriteSMBusValue), REG(HalReadWritePCISpace), REG(HalGetInterruptVector),
    REG(HalRegisterShutdownNotification), REG(HalEnableSystemInterrupt), REG(HalDisableSystemInterrupt),
    REG(HalClearSoftwareInterrupt), REG(HalRequestSoftwareInterrupt),
    REG(AvGetSavedDataAddress), REG(AvSetSavedDataAddress), REG(AvSendTVEncoderOption), REG(AvSetDisplayMode),
    REG(ExAllocatePool), REG(ExAllocatePoolWithTag), REG(ExFreePool), REG(ExQueryPoolBlockSize),
    REG(MmAllocateContiguousMemoryEx), REG(MmAllocateContiguousMemory), REG(MmAllocateSystemMemory),
    REG(MmFreeContiguousMemory), REG(MmFreeSystemMemory), REG(MmQueryAllocationSize), REG(MmGetPhysicalAddress),
    REG(MmMapIoSpace), REG(MmUnmapIoSpace), REG(MmPersistContiguousMemory), REG(MmLockUnlockBufferPages),
    REG(MmLockUnlockPhysicalPage), REG(MmQueryAddressProtect), REG(MmSetAddressProtect),
    REG(MmClaimGpuInstanceMemory), REG(MmQueryStatistics),
    REG(NtAllocateVirtualMemory), REG(NtFreeVirtualMemory), REG(NtQueryVirtualMemory),
    REG(KfRaiseIrql), REG(KfLowerIrql), REG(KeRaiseIrqlToDpcLevel), REG(KeRaiseIrqlToSynchLevel),
    REG(KeGetCurrentIrql), REG(KeGetCurrentThread),
    REG(InterlockedCompareExchange), REG(InterlockedIncrement), REG(InterlockedDecrement),
    REG(InterlockedExchange), REG(InterlockedExchangeAdd), REG(InterlockedFlushSList),
    REG(InterlockedPushEntrySList), REG(InterlockedPopEntrySList), REG(ExInterlockedCompareExchange64),
    REG(ExfInterlockedInsertHeadList), REG(ExfInterlockedInsertTailList), REG(ExfInterlockedRemoveHeadList),
    REG(KeQueryPerformanceCounter), REG(KeQueryPerformanceFrequency), REG(KeQueryInterruptTime),
    REG(KeQuerySystemTime), REG(NtSetSystemTime), REG(KeStallExecutionProcessor),
    REG(RtlTimeToTimeFields), REG(RtlTimeFieldsToTime),
    REG(PsCreateSystemThreadEx), REG(PsCreateSystemThread), REG(PsTerminateSystemThread),
    REG(KeDelayExecutionThread), REG(NtYieldExecution), REG(KeSetBasePriorityThread), REG(KeSetPriorityThread),
    REG(KeAlertThread), REG(KeAlertResumeThread), REG(KeResumeThread), REG(KeSuspendThread),
    REG(KeSetDisableBoostThread), REG(NtResumeThread), REG(NtSuspendThread),
    REG(ObReferenceObjectByHandle), REG(ObfDereferenceObject), REG(ObfReferenceObject),
    REG(KeInitializeEvent), REG(KeSetEvent), REG(KeResetEvent), REG(KePulseEvent), REG(KeInitializeSemaphore),
    REG(KeReleaseSemaphore), REG(KeInitializeMutant), REG(KeReleaseMutant), REG(KeInitializeTimerEx),
    REG(KeSetTimerEx), REG(KeSetTimer), REG(KeCancelTimer), REG(KeInitializeDpc),
    REG(KeInsertQueueDpc), REG(KeRemoveQueueDpc), REG(KeInitializeInterrupt), REG(KeConnectInterrupt),
    REG(KeDisconnectInterrupt), REG(KeIsExecutingDpc),
    REG(KeWaitForSingleObject), REG(KeWaitForMultipleObjects), REG(NtWaitForSingleObjectEx),
    REG(NtWaitForSingleObject), REG(NtWaitForMultipleObjectsEx),
    REG(NtCreateEvent), REG(NtCreateSemaphore), REG(NtCreateMutant), REG(NtCreateTimer), REG(NtSetEvent),
    REG(NtClearEvent), REG(NtPulseEvent), REG(NtReleaseSemaphore), REG(NtReleaseMutant), REG(NtSetTimerEx),
    REG(NtCancelTimer), REG(NtDuplicateObject), REG(NtClose),
    REG(RtlInitializeCriticalSection), REG(RtlEnterCriticalSection), REG(RtlTryEnterCriticalSection),
    REG(RtlLeaveCriticalSection),
    REG(RtlInitAnsiString), REG(RtlInitUnicodeString), REG(RtlEqualString), REG(RtlCompareString),
    REG(RtlUpperChar), REG(RtlLowerChar), REG(RtlUnicodeStringToAnsiString), REG(RtlAnsiStringToUnicodeString),
    REG(RtlFreeAnsiString), REG(RtlFreeUnicodeString), REG(RtlCompareMemory), REG(RtlCompareMemoryUlong),
    REG(RtlFillMemory), REG(RtlFillMemoryUlong), REG(RtlMoveMemory), REG(RtlZeroMemory),
    REG(RtlNtStatusToDosError), REG(RtlRaiseException), REG(RtlUnwind), REG(RtlRip), REG(RtlAssert),
    REG(ExQueryNonVolatileSetting), REG(ExSaveNonVolatileSetting), REG(ExReadWriteRefurbInfo),
    REG(XeLoadSection), REG(XeUnloadSection), REG(FscSetCacheSize), REG(FscGetCacheSize),
    REG(FscInvalidateIdleBlocks), REG(KeSaveFloatingPointState), REG(KeRestoreFloatingPointState),
    REG(IoCreateSymbolicLink), REG(IoDeleteSymbolicLink), REG(NtOpenSymbolicLinkObject),
    REG(NtQuerySymbolicLinkObject),
    REG(XcSHAInit), REG(XcSHAUpdate), REG(XcSHAFinal), REG(XcRC4Key), REG(XcRC4Crypt), REG(XcHMAC),
    REG(NtCreateFile), REG(NtOpenFile), REG(NtReadFile), REG(NtWriteFile), REG(NtFlushBuffersFile),
    REG(NtQueryInformationFile), REG(NtSetInformationFile), REG(NtQueryFullAttributesFile),
    REG(NtQueryVolumeInformationFile), REG(NtQueryDirectoryFile), REG(NtDeviceIoControlFile),
    REG(NtFsControlFile),
};

static uint32_t new_var(int ord, uint32_t size)
{
    uint32_t a = hle_alloc(size < 16 ? 16 : size);
    var_addr[ord] = a;
    return a;
}

/* Address that a thunk entry for `ord` resolves to. */
uint32_t kernel_thunk_target(uint32_t ord)
{
    if (ord >= 400 || !kexp[ord].name)
        return XBOX_HLE_BASE + 4 * (ord & 0x3ff);
    if (kexp[ord].conv == 'V')
        return var_addr[ord];
    return XBOX_HLE_BASE + 4 * ord;
}

static void init_vars(void)
{
    for (int i = 0; i < 400; i++)
        if (kexp[i].name && kexp[i].conv == 'V' && !var_addr[i])
            new_var(i, 64);
    int o;
    if ((o = ordinal_of("XboxHardwareInfo")) >= 0) {
        G32(var_addr[o]) = 0x00000020;     /* flags */
        G8(var_addr[o] + 4) = 0xa2;        /* GPU revision */
        G8(var_addr[o] + 5) = 0xd3;        /* MCP revision */
    }
    if ((o = ordinal_of("XboxKrnlVersion")) >= 0) {
        G16(var_addr[o]) = 1;
        G16(var_addr[o] + 2) = 0;
        G16(var_addr[o] + 4) = 5838;
        G16(var_addr[o] + 6) = 1;
    }
    if ((o = ordinal_of("HalDiskCachePartitionCount")) >= 0)
        G32(var_addr[o]) = 3;
    if ((o = ordinal_of("HalDiskModelNumber")) >= 0) {
        uint32_t s = new_ansi("XBT VIRTUAL DISK");
        memcpy(GPTR(var_addr[o]), GPTR(s), 8);
    }
    if ((o = ordinal_of("HalDiskSerialNumber")) >= 0) {
        uint32_t s = new_ansi("0000000000000000");
        memcpy(GPTR(var_addr[o]), GPTR(s), 8);
    }
    if ((o = ordinal_of("XeImageFileName")) >= 0) {
        uint32_t s = new_ansi("\\Device\\CdRom0\\default.xbe");
        memcpy(GPTR(var_addr[o]), GPTR(s), 8);
    }
    if ((o = ordinal_of("HalBootSMCVideoMode")) >= 0)
        G32(var_addr[o]) = 1;
    if ((o = ordinal_of("LaunchDataPage")) >= 0)
        G32(var_addr[o]) = 0;
    /* object type exports point at small dummy OBJECT_TYPE structures (already zeroed) */
}

static void update_tick(void)
{
    static int o = -2;
    if (o == -2)
        o = ordinal_of("KeTickCount");
    if (o >= 0)
        G32(var_addr[o]) = (uint32_t)(uptime_ns() / 1000000ull);
}

int kernel_init(uint32_t ramsz, const char *gamedir)
{
    ram_size = ramsz;
    boot_ns = host_ns();
    page_used = calloc(ram_size >> 12, 1);
    phys_reserve(0, 0x10000);                     /* null page area */
    phys_reserve(ram_size - 0x10000, 0x10000);    /* NV2A instance memory */
    uint32_t pa = phys_alloc(POOL_SIZE, 0x1000, 0, ram_size / 2);
    pool_base = XBOX_CONTIG_BASE + pa;
    cpool = 64;
    pool = calloc((size_t)cpool, sizeof(pblk));
    pool[0] = (pblk){ 0, POOL_SIZE, 0 };
    npool = 1;

    for (size_t i = 0; i < sizeof impl / sizeof impl[0]; i++) {
        int o = ordinal_of(impl[i].name);
        if (o >= 0)
            handlers[o] = impl[i].fn;
        else
            XBT_LOG("internal: %s is not a kernel export\n", impl[i].name);
    }
    init_vars();

    snprintf(game_dir, sizeof game_dir, "%s", gamedir);
    char tmp[600];
    snprintf(devmap[ndev].from, sizeof devmap[0].from, "\\Device\\CdRom0");
    snprintf(devmap[ndev++].to, sizeof devmap[0].to, "%s", gamedir);
    static const char *parts[] = { "Partition1", "E", "Partition2", "C", "Partition3", "X",
                                   "Partition4", "Y", "Partition5", "Z", "Partition6", "F" };
    for (int i = 0; i < 12; i += 2) {
        snprintf(tmp, sizeof tmp, "%s/xbt_hdd/%s", gamedir, parts[i + 1]);
        mkdir(tmp, 0755);
        snprintf(devmap[ndev].from, sizeof devmap[0].from, "\\Device\\Harddisk0\\%s", parts[i]);
        snprintf(devmap[ndev++].to, sizeof devmap[0].to, "%s", tmp);
    }
    snprintf(tmp, sizeof tmp, "%s/xbt_hdd", gamedir);
    mkdir(tmp, 0755);
    add_link("\\??\\D:", "\\Device\\CdRom0");
    add_link("\\??\\C:", "\\Device\\Harddisk0\\Partition2");
    add_link("\\??\\E:", "\\Device\\Harddisk0\\Partition1");
    add_link("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");

    xbt_set_hle_handler(hle_call);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scheduler                                                           */
/* ------------------------------------------------------------------ */

static void dump_thread(hthread *t, const char *why)
{
    xbt_cpu *c = &t->cpu;
    XBT_LOG("thread %u: %s at eip %08x\n", t->id, why, c->eip);
    XBT_LOG("  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n",
            c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
    if (mem_readable(c->eip, 16)) {
        fprintf(stderr, "[xbt]   code:");
        for (int i = 0; i < 16; i++)
            fprintf(stderr, " %02x", G8(c->eip + (uint32_t)i));
        fprintf(stderr, "\n");
    }
}

int kernel_run(uint32_t entry, uint32_t stack_size)
{
    hthread *main_t = thread_new(stack_size, 0);
    push32(&main_t->cpu, TRAP_THREAD_EXIT);
    main_t->cpu.eip = entry;

    dpc_thread = thread_new(0x10000, 0);
    dpc_thread->state = TS_DONE;
    dpc_thread->is_dpc = 1;
    G8(dpc_thread->kpcr + 0x24) = 2;    /* DISPATCH_LEVEL */

    signal(SIGALRM, on_alarm);
    struct itimerval it = { { 0, 5000 }, { 0, 5000 } };
    setitimer(ITIMER_REAL, &it, NULL);

    hthread *last = NULL;
    enum { NSAMP = 64 };
    uint32_t samp_eip[NSAMP] = { 0 }, samp_n[NSAMP] = { 0 };
    while (!stop_requested) {
        if (hle_run_seconds > 0 && uptime_ns() > (uint64_t)(hle_run_seconds * 1e9)) {
            XBT_LOG("stopping after %.1f s\n", hle_run_seconds);
            break;
        }
        update_tick();
        fire_timers();
        if (dpc_thread->state == TS_DONE && ndpc) {
            start_dpc();
            dpc_thread->is_dpc = 1;
        }
        /* complete satisfied waits */
        int alive = 0;
        uint64_t next_deadline = 0;
        for (hthread *t = threads; t; t = t->next) {
            if (t->state == TS_WAIT) {
                int64_t r = try_wait(t);
                if (r >= 0) {
                    t->state = TS_READY;
                    t->wait_delay = 0;
                    if (t->wait_cs) {
                        G32(t->wait_cs + 24) = t->ethread;
                        G32(t->wait_cs + 20) = 1;
                        t->wait_cs = 0;
                        r = 0;
                    }
                    finish_call(&t->cpu, t->pending_ord, (uint64_t)r);
                } else if (t->deadline && (!next_deadline || t->deadline < next_deadline)) {
                    next_deadline = t->deadline;
                }
            }
            if (t->state != TS_DONE && t != dpc_thread)
                alive = 1;
        }
        if (!alive)
            break;
        /* pick: the DPC thread first, then round robin after the last one */
        hthread *pick = NULL;
        if (dpc_thread->state == TS_READY)
            pick = dpc_thread;
        if (!pick) {
            hthread *start = last && last->next ? last->next : threads;
            hthread *t = start;
            do {
                if (t->state == TS_READY && t != dpc_thread) {
                    pick = t;
                    break;
                }
                t = t->next ? t->next : threads;
            } while (t != start);
        }
        if (!pick) {
            /* everybody waits: sleep until the next deadline or timer */
            uint64_t now = host_ns(), until = now + 2000000;
            if (next_deadline && next_deadline < until)
                until = next_deadline;
            for (ktimer_rec *r = timers; r; r = r->next)
                if (r->due < until)
                    until = r->due;
            if (!next_deadline && !timers && !ndpc) {
                int any_timeout = 0;
                for (hthread *t = threads; t; t = t->next)
                    if (t->state == TS_WAIT && t->deadline)
                        any_timeout = 1;
                if (!any_timeout) {
                    static int idle;
                    if (++idle > 1500) {   /* ~3 s of nothing to do */
                        XBT_LOG("all threads are blocked forever (deadlock)\n");
                        for (hthread *t = threads; t; t = t->next)
                            if (t->state == TS_WAIT)
                                XBT_LOG("  thread %u waits in %s\n", t->id, kexp[t->pending_ord].name);
                        exit_code = 5;
                        break;
                    }
                }
            }
            if (until > now) {
                struct timespec ts = { 0, (long)(until - now) };
                nanosleep(&ts, NULL);
            }
            continue;
        }
        current = pick;
        if (pick != dpc_thread)
            last = pick;
        int reason = xbt_run(&pick->cpu);
        current = NULL;
        if (reason == EXIT_PENDING && hle_sample) {
            uint32_t e = pick->cpu.eip;
            int k;
            for (k = 0; k < NSAMP && samp_n[k] && samp_eip[k] != e; k++)
                ;
            if (k < NSAMP) {
                samp_eip[k] = e;
                samp_n[k]++;
            }
        }
        switch (reason) {
        case EXIT_PENDING:
        case EXIT_YIELD:
            break;
        case EXIT_HLT:
            break;
        case EXIT_INT:
            if (pick->cpu.exit_info == 3) {
                XBT_LOG("int3 at %08x (continuing)\n", pick->cpu.eip - 1);
                break;
            }
            /* fallthrough */
        default:
            dump_thread(pick, reason == EXIT_DIVIDE ? "divide error" : reason == EXIT_UD ?
                        "unsupported instruction or unmapped code" : "stopped");
            stop_requested = 1;
            exit_code = 1;
            break;
        }
    }
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &off, NULL);
    if (hle_sample) {
        XBT_LOG("preemption samples (guest eip: count):\n");
        for (int k = 0; k < NSAMP && samp_n[k]; k++)
            if (samp_n[k] > 1)
                XBT_LOG("  %08x: %u\n", samp_eip[k], samp_n[k]);
    }
    return exit_code;
}
