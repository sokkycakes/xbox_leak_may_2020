/*
 * XBCOMPAT_PROFILE=FILE: a sampling profiler for finding where the CPU time
 * goes on a slow host (a Raspberry Pi under box86, where no perf runs).
 * Each thread xbcompat starts gets a timer on its own CPU clock that raises
 * SIGPROF every 10 ms of its CPU time (a process-wide one would also hit
 * threads the native libraries start, which box86 can't hand to an x86
 * handler); the handler counts
 * the interrupted x86 address (box86 hands it the address of the x86 code it
 * was running) and the thread. Every five seconds FILE is rewritten with the
 * busiest addresses since the start: guest code resolves against the title's
 * .map, xbcompat's own against its binary (addr2line). An address that is
 * not x86 code (a call into a native library under box86) is counted under
 * the x86 return address on top of the stack, marked "native call from".
 * XBCOMPAT_PROFILE_WORK=1 without XBCOMPAT_PROFILE samples to
 * /tmp/xbcompat-profile.txt.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include "xbcompat.h"
#include "cpu.h"

#ifdef XBC_NATIVE

#define SLOTS 65536            /* power of two */
#define X86_CODE_END 0x10000000u /* the XBE and xbcompat itself; box86's bridges and libraries are above */

typedef struct { uint32_t key, count; } slot;
static slot addrs[SLOTS];
static slot tids[256];
static slot callers[SLOTS];   /* the nearest title code up the stack */
static uint32_t total, dropped;
static const char *out_path;

static void count(slot *t, unsigned n, uint32_t key)
{
    unsigned h = (key * 2654435761u) & (n - 1);
    for (unsigned i = 0; i < n; i++, h = (h + 1) & (n - 1)) {
        uint32_t k = t[h].key;
        if (k == key || (k == 0 && __sync_bool_compare_and_swap(&t[h].key, 0, key))) {
            __sync_fetch_and_add(&t[h].count, 1);
            return;
        }
    }
    __sync_fetch_and_add(&dropped, 1);
}

static void on_prof(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
    uint32_t eip = uc->uc_mcontext.gregs[REG_EIP];
    if (eip >= X86_CODE_END) {
        /* Inside a native call: charge the x86 code that made it. */
        uint32_t esp = uc->uc_mcontext.gregs[REG_ESP];
        eip = (esp && esp < 0xF0000000u) ? *(uint32_t *)esp | 1u : 1u;
    }
    count(addrs, SLOTS, eip ? eip : 2);
    /* Follow the frame pointers (xbcompat keeps them) out of xbcompat's
       own code to the title code that called it. */
    uint32_t pc = uc->uc_mcontext.gregs[REG_EIP], esp = uc->uc_mcontext.gregs[REG_ESP];
    uint32_t *fp = (uint32_t *)uc->uc_mcontext.gregs[REG_EBP];
    for (int depth = 0; depth < 24; depth++) {
        if (pc >= 0x10000 && pc < IMAGE_REGION_END) {
            count(callers, SLOTS, pc);
            break;
        }
        if ((uint32_t)fp <= esp || (uint32_t)fp - esp > (16u << 20) || ((uint32_t)fp & 3)) break;
        pc = fp[1];
        esp = (uint32_t)fp;
        fp = (uint32_t *)fp[0];
    }
    count(tids, 256, (uint32_t)syscall(SYS_gettid));
    __sync_fetch_and_add(&total, 1);
}

static int by_count(const void *a, const void *b)
{
    const slot *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static int sorted(const slot *t, slot *copy)
{
    int used = 0;
    for (int i = 0; i < SLOTS; i++)
        if (t[i].count) copy[used++] = t[i];
    qsort(copy, used, sizeof(slot), by_count);
    return used;
}

static void dump(void)
{
    static slot copy[SLOTS];
    int used = sorted(addrs, copy);
    slot tcopy[256];
    memcpy(tcopy, tids, sizeof tcopy);
    qsort(tcopy, 256, sizeof(slot), by_count);

    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    uint32_t n = total ? total : 1;
    fprintf(f, "%u samples (1 per 10 ms of CPU), %u dropped\n\nthreads:\n", total, dropped);
    for (int i = 0; i < 256 && tcopy[i].count; i++) {
        char path[64], name[32] = "?";
        snprintf(path, sizeof path, "/proc/self/task/%u/comm", tcopy[i].key);
        FILE *c = fopen(path, "r");
        if (c) {
            if (fgets(name, sizeof name, c)) name[strcspn(name, "\n")] = 0;
            fclose(c);
        }
        fprintf(f, "%6.2f%%  %u %s\n", 100.0 * tcopy[i].count / n, tcopy[i].key, name);
    }
    fprintf(f, "\naddresses:\n");
    for (int i = 0; i < 150 && i < used; i++) {
        uint32_t k = copy[i].key;
        fprintf(f, "%6.2f%%  %08x%s\n", 100.0 * copy[i].count / n, k & ~1u,
                (k & 1) ? "  native call from" : "");
    }
    used = sorted(callers, copy);
    fprintf(f, "\ntitle code on the stack (the title's own time plus what it called):\n");
    for (int i = 0; i < 100 && i < used; i++)
        fprintf(f, "%6.2f%%  %08x\n", 100.0 * copy[i].count / n, copy[i].key);
    fclose(f);
    rename(tmp, out_path);
}

static void *writer(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "profiler");
    for (;;) {
        sleep(5);
        dump();
    }
    return NULL;
}

typedef int (*timer_create_fn)(clockid_t, struct sigevent *, timer_t *);
typedef int (*timer_settime_fn)(timer_t, int, const struct itimerspec *, struct itimerspec *);
static timer_create_fn p_timer_create;
static timer_settime_fn p_timer_settime;

/* Called by every thread that runs x86 code, as it starts. */
void prof_thread_start(void)
{
    if (!out_path || !p_timer_create) return;
    struct sigevent ev = { 0 };
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = SIGPROF;
    ev._sigev_un._tid = syscall(SYS_gettid);
    timer_t timer;
    if (p_timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &timer) != 0) return;
    struct itimerspec it = { { 0, 10000000 }, { 0, 10000000 } };
    p_timer_settime(timer, 0, &it, NULL);
}

void prof_init(void)
{
    out_path = getenv("XBCOMPAT_PROFILE");
    if (!out_path || !*out_path) {
        /* XBCOMPAT_PROFILE_WORK samples too, to a fixed file. */
        const char *w = getenv("XBCOMPAT_PROFILE_WORK");
        out_path = w && *w && *w != '0' ? "/tmp/xbcompat-profile.txt" : NULL;
    }
    if (!out_path) return;
    /* From librt: box86 wraps the timer calls there, not in libc. */
    void *rt = dlopen("librt.so.1", RTLD_NOW);
    if (rt) {
        p_timer_create = (timer_create_fn)dlsym(rt, "timer_create");
        p_timer_settime = (timer_settime_fn)dlsym(rt, "timer_settime");
    }
    if (!p_timer_create || !p_timer_settime) {
        p_timer_create = timer_create;
        p_timer_settime = timer_settime;
    }
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    prof_thread_start();
    pthread_t t;
    pthread_create(&t, NULL, writer, NULL);
    pthread_detach(t);
    xlog("profiling to %s", out_path);
}

#else
void prof_init(void) {}
void prof_thread_start(void) {}
#endif

/* ---- blocked time ------------------------------------------------------------ */

#include <time.h>

static int waits_on = -1;
static pthread_mutex_t waits_lock = PTHREAD_MUTEX_INITIALIZER;
#define WAIT_SLOTS 512
static struct wait_slot { uint32_t site, tid; int kind; uint32_t n; uint64_t ns; } waits[WAIT_SLOTS];
static uint64_t waits_since;
static uint32_t waits_dropped;

static uint64_t wait_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

uint64_t prof_wait_begin(void)
{
    if (waits_on < 0) {
        const char *w = getenv("XBCOMPAT_PROFILE_WORK");
        waits_on = w && *w && *w != '0';
    }
    return waits_on ? wait_now() : 0;
}

static int by_ns(const void *a, const void *b)
{
    const struct wait_slot *x = a, *y = b;
    return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}

/* Called with waits_lock held: log the window's biggest waits and clear it. */
static void waits_log(uint64_t now)
{
    static struct wait_slot copy[WAIT_SLOTS];
    int used = 0;
    for (int i = 0; i < WAIT_SLOTS; i++)
        if (waits[i].n) copy[used++] = waits[i];
    qsort(copy, used, sizeof(*copy), by_ns);
    double secs = (now - waits_since) / 1e9;
    for (int i = 0; i < used && i < 10; i++) {
        char path[64], name[32] = "?";
        snprintf(path, sizeof path, "/proc/self/task/%u/comm", copy[i].tid);
        FILE *c = fopen(path, "r");
        if (c) {
            if (fgets(name, sizeof name, c)) name[strcspn(name, "\n")] = 0;
            fclose(c);
        }
        xlog("Waits: %s (%u) blocked %.1f ms/s in %.0f %s/s from %08x", name, copy[i].tid,
             copy[i].ns / 1e6 / secs, copy[i].n / secs,
             copy[i].kind < 0 ? "delays" : copy[i].kind == 0 || copy[i].kind == 1 ? "event waits" :
             copy[i].kind == 2 ? "mutant waits" : copy[i].kind == 5 ? "semaphore waits" :
             copy[i].kind == 6 ? "thread waits" : copy[i].kind == 8 || copy[i].kind == 9 ? "timer waits" : "other waits",
             copy[i].site);
    }
    if (waits_dropped) xlog("Waits: %u not counted (table full)", waits_dropped);
    memset(waits, 0, sizeof waits);
    waits_dropped = 0;
    waits_since = now;
}

void prof_wait_end(uint64_t start, int kind)
{
    if (!start) return;
    uint64_t now = wait_now();
    /* The title code that made the call: follow xbcompat's frame pointers
       out of its own code. */
    uint32_t site = 0;
    uintptr_t *fp = __builtin_frame_address(0);
    for (int depth = 0; depth < 16 && fp; depth++) {
        uintptr_t pc = fp[1];
        if (pc >= 0x10000 && pc < IMAGE_REGION_END) { site = (uint32_t)pc; break; }
        uintptr_t *next = (uintptr_t *)fp[0];
        if (next <= fp || (uintptr_t)next - (uintptr_t)fp > (1u << 20)) break;
        fp = next;
    }
    uint32_t tid = (uint32_t)syscall(SYS_gettid);
    pthread_mutex_lock(&waits_lock);
    if (!waits_since) waits_since = now;
    unsigned h = ((site ^ tid * 31u ^ (uint32_t)kind * 7919u) * 2654435761u) % WAIT_SLOTS;
    struct wait_slot *w = NULL;
    for (unsigned i = 0; i < WAIT_SLOTS; i++, h = (h + 1) % WAIT_SLOTS) {
        struct wait_slot *t = &waits[h];
        if (!t->n) { t->site = site; t->tid = tid; t->kind = kind; w = t; break; }
        if (t->site == site && t->tid == tid && t->kind == kind) { w = t; break; }
    }
    if (w) { w->n++; w->ns += now - start; }
    else waits_dropped++;
    if (now - waits_since >= 5000000000u) waits_log(now);
    pthread_mutex_unlock(&waits_lock);
}
