/*
 * Ke*: dispatcher objects, waits, timers, DPCs, IRQL and time.
 *
 * Dispatcher objects live in guest memory (a KEVENT inside a critical
 * section, a KTIMER inside a title's struct), so waits work on the
 * DISPATCHER_HEADER directly.  One global lock and condition variable guard
 * every SignalState; that is coarse, but correct and plenty for a proof of
 * concept.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "../xbcompat.h"
#include "../cpu.h"

pthread_mutex_t g_disp_lock = PTHREAD_MUTEX_INITIALIZER;

/* Threads blocked in wait_objects, each on its own condition variable, so
   signaling an object wakes only the threads waiting on it (one shared
   broadcast woke every waiting thread on every event, timer and semaphore:
   thousands of wakeups a second, which a Raspberry Pi feels). */
struct disp_waiter {
    pthread_cond_t cv XBC_COND_ALIGN;
    ULONG count;
    PVOID *objects;
    struct disp_waiter *next;
};
static struct disp_waiter *disp_waiters;

void disp_signal(void *object)
{
    for (struct disp_waiter *w = disp_waiters; w; w = w->next)
        for (ULONG i = 0; i < w->count; i++)
            if (w->objects[i] == object) {
                pthread_cond_signal(&w->cv);
                break;
            }
}

void disp_signal_all(void)
{
    for (struct disp_waiter *w = disp_waiters; w; w = w->next) pthread_cond_signal(&w->cv);
}

/* ---- time ------------------------------------------------------------- */

#define EPOCH_DIFF_100NS 116444736000000000ULL   /* 1601 -> 1970 */

ULONGLONG system_time_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return EPOCH_DIFF_100NS + (ULONGLONG)ts.tv_sec * 10000000ULL + ts.tv_nsec / 100;
}

/* XBCOMPAT_FIXED_FPS=N: the guest clock stops following the wall clock and
   advances 1/N s per presented frame instead, so frame K of a title that
   animates by time always shows the moment K/N s.  Used to compare frames
   with a reference renderer; audio still mixes in real time. */
static ULONGLONG fixed_step, fixed_now;

static ULONGLONG mono_100ns(void)
{
    if (fixed_step) return __atomic_load_n(&fixed_now, __ATOMIC_ACQUIRE);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 10000000ULL + ts.tv_nsec / 100;
}

volatile ULONG KeTickCount;
static ULONGLONG boot_mono;

void NTAPI KeQuerySystemTime(LARGE_INTEGER *t)
{
    t->QuadPart = system_time_now();
}

ULONGLONG NTAPI KeQueryInterruptTime(void)
{
    return mono_100ns() - boot_mono;
}

/* The ACPI timer the Xbox kernel uses for the performance counter. */
#define XBOX_PERF_FREQ 3375000ULL

ULONGLONG NTAPI KeQueryPerformanceCounter(void)
{
    if (fixed_step) return mono_100ns() * XBOX_PERF_FREQ / 10000000ULL;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * XBOX_PERF_FREQ + (ULONGLONG)ts.tv_nsec * XBOX_PERF_FREQ / 1000000000ULL;
}

ULONGLONG NTAPI KeQueryPerformanceFrequency(void)
{
    return XBOX_PERF_FREQ;
}

/* ---- the CPU's time stamp counter -------------------------------------

   Titles time frames with rdtsc and take it to tick at the Xbox CPU's
   733 MHz: XAPI's QueryPerformanceCounter is a bare rdtsc and its
   QueryPerformanceFrequency returns 733333333.  On the host it ticks at the
   host's rate (2.1 GHz here), so frame timers saw time pass up to several
   times too fast: the ATG samples showed 28 fps while running at 60, and
   games that move by elapsed time ran that much faster.

   So guest threads run with the time stamp counter disabled
   (PR_SET_TSC), and the fault handler answers each rdtsc with the guest
   clock at 733 MHz, counted from when xbcompat started as an Xbox counts
   from power-on.  glibc's clock_gettime and gettimeofday read the counter
   inside the vDSO, which would fault too (and in SDL's handlers while SDL
   holds them), so those vDSO entries are pointed at the plain system calls
   first.  If any of that is refused, the counter stays the host's.
   XBCOMPAT_HOST_TSC=1 keeps the host's counter on purpose. */

#define XBOX_TSC_FREQ 733333333ULL

static bool tsc_trapped;

#ifdef XBC_NATIVE

static void __attribute__((naked, used)) sys_clock_gettime(void)
{
    __asm__("push %ebx\n\tmov 8(%esp), %ebx\n\tmov 12(%esp), %ecx\n\t"
            "mov $265, %eax\n\tint $0x80\n\tpop %ebx\n\tret");
}

static void __attribute__((naked, used)) sys_clock_gettime64(void)
{
    __asm__("push %ebx\n\tmov 8(%esp), %ebx\n\tmov 12(%esp), %ecx\n\t"
            "mov $403, %eax\n\tint $0x80\n\tpop %ebx\n\tret");
}

static void __attribute__((naked, used)) sys_gettimeofday(void)
{
    __asm__("push %ebx\n\tmov 8(%esp), %ebx\n\tmov 12(%esp), %ecx\n\t"
            "mov $78, %eax\n\tint $0x80\n\tpop %ebx\n\tret");
}

/* Point the vDSO's clock functions at system calls that don't read the
   counter.  Returns false if the vDSO can't be found or written. */
static bool vdso_use_syscalls(void)
{
    uint8_t *base = (uint8_t *)getauxval(AT_SYSINFO_EHDR);
    if (!base) return true;   /* no vDSO: glibc already makes system calls */
    Elf32_Ehdr *eh = (Elf32_Ehdr *)base;
    /* Under box86 (x86 on ARM) this is the ARM kernel's vDSO, which the
       ARM C library calls: never write x86 jumps into it. */
    if (eh->e_machine != EM_386) return false;
    Elf32_Phdr *ph = (Elf32_Phdr *)(base + eh->e_phoff);
    Elf32_Addr bias = 0, lo = ~0u, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
        }
    if (lo == ~0u) return false;
    bias = (Elf32_Addr)base - lo;
    Elf32_Shdr *sh = (Elf32_Shdr *)(base + eh->e_shoff);
    Elf32_Sym *syms = NULL;
    const char *strs = NULL;
    unsigned nsyms = 0;
    for (int i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_DYNSYM) {
            syms = (Elf32_Sym *)(base + sh[i].sh_offset);
            nsyms = sh[i].sh_size / sizeof(Elf32_Sym);
            strs = (const char *)(base + sh[sh[i].sh_link].sh_offset);
        }
    if (!syms) return false;

    static const struct { const char *name; void (*to)(void); } redirects[] = {
        { "__vdso_clock_gettime", sys_clock_gettime },
        { "__vdso_clock_gettime64", sys_clock_gettime64 },
        { "__vdso_gettimeofday", sys_gettimeofday },
    };
    uintptr_t page = sysconf(_SC_PAGESIZE);
    uintptr_t start = (bias + lo) & ~(page - 1), end = (bias + hi + page - 1) & ~(page - 1);
    if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    for (unsigned i = 0; i < nsyms; i++) {
        if (ELF32_ST_TYPE(syms[i].st_info) != STT_FUNC || !syms[i].st_value) continue;
        for (size_t j = 0; j < sizeof redirects / sizeof redirects[0]; j++) {
            if (strcmp(strs + syms[i].st_name, redirects[j].name)) continue;
            uint8_t *at = (uint8_t *)(bias + syms[i].st_value);
            int32_t rel = (int32_t)((uintptr_t)redirects[j].to - (uintptr_t)(at + 5));
            at[0] = 0xE9;   /* jmp rel32 */
            memcpy(at + 1, &rel, 4);
        }
    }
    mprotect((void *)start, end - start, PROT_READ | PROT_EXEC);
    return true;
}

static void tsc_init(void)
{
    const char *e = getenv("XBCOMPAT_HOST_TSC");
    if (e && *e && *e != '0') return;
    if (!vdso_use_syscalls()) { xlog("rdtsc: the vDSO can't be patched; titles see the host's counter"); return; }
    /* Try it on this thread; guest threads turn it on in thread_trap_tsc. */
    if (prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0) != 0) { xlog("rdtsc: PR_SET_TSC refused; titles see the host's counter"); return; }
    prctl(PR_SET_TSC, PR_TSC_ENABLE, 0, 0, 0);
    tsc_trapped = true;
}

#else
/* Translated guest code: the CPU emulator answers every rdtsc with
   ke_guest_tsc() (cpu_unicorn.c), so there is nothing to set up. */
static void tsc_init(void) {}
#endif

void thread_trap_tsc(void)
{
    if (tsc_trapped) prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0);
}

void thread_untrap_tsc(void)
{
    if (tsc_trapped) prctl(PR_SET_TSC, PR_TSC_ENABLE, 0, 0, 0);
}

/* The counter a guest rdtsc reads: 733 MHz since xbcompat started. */
ULONGLONG ke_guest_tsc(void)
{
    ULONGLONG t = mono_100ns() - boot_mono;   /* 100 ns units */
    return t / 10000000ULL * XBOX_TSC_FREQ + t % 10000000ULL * XBOX_TSC_FREQ / 10000000ULL;
}

/* Absolute deadline (CLOCK_REALTIME based timespec) for a Ke timeout. */
static bool deadline_from(LARGE_INTEGER *timeout, struct timespec *out)
{
    if (!timeout) return false;
    LONGLONG t = timeout->QuadPart;
    ULONGLONG now = system_time_now();
    ULONGLONG due = t < 0 ? now + (ULONGLONG)(-t) : (ULONGLONG)t;
    if (due < now) due = now;
    ULONGLONG unix100 = due - EPOCH_DIFF_100NS;
    out->tv_sec = unix100 / 10000000ULL;
    out->tv_nsec = (unix100 % 10000000ULL) * 100;
    return true;
}

/* ---- waiting ---------------------------------------------------------- */

static bool object_signaled(DISPATCHER_HEADER *h, KTHREAD *self)
{
    if (h->Type == MutantObject) {
        KMUTANT *m = (KMUTANT *)h;
        return h->SignalState > 0 || m->OwnerThread == self;
    }
    return h->SignalState > 0;
}

static NTSTATUS consume(DISPATCHER_HEADER *h, KTHREAD *self)
{
    switch (h->Type) {
    case EventSynchronizationObject:
    case TimerSynchronizationObject:
        h->SignalState = 0;
        break;
    case SemaphoreObject:
        h->SignalState--;
        break;
    case MutantObject: {
        KMUTANT *m = (KMUTANT *)h;
        h->SignalState--;
        m->OwnerThread = self;
        if (m->Abandoned) {
            m->Abandoned = 0;
            return STATUS_ABANDONED;
        }
        break;
    }
    default:
        break;
    }
    return STATUS_SUCCESS;
}

/* ---- APCs ---------------------------------------------------------------
   Each guest thread has one queue.  User-mode APCs (I/O completion routines,
   QueueUserAPC) run when the thread enters a user-mode alertable wait, which
   then returns STATUS_USER_APC (ke/wait.c TestForAlertPending).  Kernel-mode
   APCs run at once on their own thread, or at the target's next wait. */

typedef struct xapc {
    struct xapc *next;
    PVOID routine, ctx, arg1, arg2;
    KAPC *kapc;
    bool user;
} xapc;

typedef void (NTAPI *normal_routine)(PVOID, PVOID, PVOID);
typedef void (NTAPI *kernel_routine)(KAPC *, PVOID *, PVOID *, PVOID *, PVOID *);

static void apc_run(xapc *a)
{
    PVOID routine = a->routine, ctx = a->ctx, a1 = a->arg1, a2 = a->arg2;
    if (a->kapc) {
        KAPC *k = a->kapc;
        k->Inserted = 0;
        /* The kernel routine may free the KAPC and may change what runs next. */
#ifdef XBC_TRANSLATED
        if (k->KernelRoutine)
            CPU_CALL(k->KernelRoutine, CONV_STD, (uint32_t)k, (uint32_t)&routine, (uint32_t)&ctx,
                     (uint32_t)&a1, (uint32_t)&a2);
#else
        if (k->KernelRoutine) ((kernel_routine)k->KernelRoutine)(k, &routine, &ctx, &a1, &a2);
#endif
    }
#ifdef XBC_TRANSLATED
    if (routine) CPU_CALL(routine, CONV_STD, (uint32_t)ctx, (uint32_t)a1, (uint32_t)a2);
#else
    if (routine) ((normal_routine)routine)(ctx, a1, a2);
#endif
    free(a);
}

static bool apc_pending(xthread *t, bool user)
{
    for (xapc *a = t->apc_head; a; a = a->next)
        if (a->user == user) return true;
    return false;
}

/* Run t's queued APCs of one mode; called on t with g_disp_lock held, which
   is dropped while each routine runs. */
static void apc_drain(xthread *t, bool user)
{
    for (;;) {
        xapc **pp = &t->apc_head, *a;
        while ((a = *pp) && a->user != user) pp = &a->next;
        if (!a) return;
        *pp = a->next;
        if (t->apc_tail == a) {
            t->apc_tail = NULL;
            for (xapc *p = t->apc_head; p; p = p->next) t->apc_tail = p;
        }
        pthread_mutex_unlock(&g_disp_lock);
        apc_run(a);
        /* The routine took the guest CPU (translated builds); a wait that
           goes on blocking must not keep it. */
        cpu_block();
        pthread_mutex_lock(&g_disp_lock);
    }
}

void apc_queue(xthread *t, PVOID routine, PVOID ctx, PVOID arg1, PVOID arg2, KAPC *kapc)
{
    xapc *a = calloc(1, sizeof(*a));
    a->routine = routine; a->ctx = ctx; a->arg1 = arg1; a->arg2 = arg2; a->kapc = kapc;
    a->user = kapc ? kapc->ApcMode == 1 : true;
    if (!a->user && t == thread_current()) {
        apc_run(a);
        return;
    }
    pthread_mutex_lock(&g_disp_lock);
    if (t->apc_tail) t->apc_tail->next = a; else t->apc_head = a;
    t->apc_tail = a;
    disp_signal_all();
    pthread_mutex_unlock(&g_disp_lock);
}

NTSTATUS wait_objects(ULONG count, PVOID objects[], int wait_any, KPROCESSOR_MODE mode,
                      BOOLEAN alertable, LARGE_INTEGER *timeout)
{
    bool user_apcs = alertable && mode == 1;
    xthread *xt = thread_current();
    KTHREAD *self = xt ? &xt->ethread.Tcb : NULL;
    struct timespec dl;
    bool has_dl = deadline_from(timeout, &dl);
    bool poll = timeout && timeout->QuadPart == 0;
    NTSTATUS st;

    static __thread struct disp_waiter me;
    static __thread bool me_init;
    if (!me_init) {
        pthread_cond_init(&me.cv, NULL);   /* CLOCK_REALTIME, as deadline_from */
        me_init = true;
    }
    me.count = count;
    me.objects = objects;
    bool listed = false;
    /* A loop that waits on a manual-reset event nobody resets never blocks.
       The dashboard's audio stream thread does that while its stream is
       stopped: on the Xbox it is a low-priority thread that only gets idle
       time, here it took a whole core (and the dispatcher lock) from the
       render thread, ~250000 waits a second. Past 64 such waits in a row,
       each one first gives up the CPU for a while. */
    static __thread unsigned spins;
    bool spin_hit = false;

    pthread_mutex_lock(&g_disp_lock);
    for (;;) {
        if (xt && xt->apc_head) apc_drain(xt, false);
        if (wait_any) {
            for (ULONG i = 0; i < count; i++) {
                DISPATCHER_HEADER *h = objects[i];
                if (object_signaled(h, self)) {
                    if (h->Type == EventNotificationObject) spin_hit = true;
                    st = consume(h, self);
                    st = st == STATUS_ABANDONED ? (NTSTATUS)(STATUS_ABANDONED + i) : (NTSTATUS)i;
                    goto done;
                }
            }
        } else {
            ULONG ready = 0;
            for (ULONG i = 0; i < count; i++)
                ready += object_signaled(objects[i], self);
            if (ready == count) {
                for (ULONG i = 0; i < count; i++)
                    consume(objects[i], self);
                st = STATUS_SUCCESS;
                goto done;
            }
        }
        if (user_apcs && xt && apc_pending(xt, true)) {
            apc_drain(xt, true);
            st = STATUS_USER_APC;
            goto done;
        }
        if (poll) { st = STATUS_TIMEOUT; goto done; }
        if (!listed) {
            cpu_block();
            me.next = disp_waiters;
            disp_waiters = &me;
            listed = true;
        }
        /* A second at most between looks, in case something changed a
           SignalState without disp_signal. */
        struct timespec cap;
        clock_gettime(CLOCK_REALTIME, &cap);
        cap.tv_sec += 1;
        bool capped = !has_dl || cap.tv_sec < dl.tv_sec || (cap.tv_sec == dl.tv_sec && cap.tv_nsec < dl.tv_nsec);
        int rc = pthread_cond_timedwait(&me.cv, &g_disp_lock, capped ? &cap : &dl);
        if (rc == ETIMEDOUT && !capped) {
            /* One last look: the signal may have raced the timeout. */
            poll = true;
        }
    }
done:
    if (listed)
        for (struct disp_waiter **w = &disp_waiters; *w; w = &(*w)->next)
            if (*w == &me) {
                *w = me.next;
                break;
            }
    pthread_mutex_unlock(&g_disp_lock);
    if (listed) spins = 0;
    else if (spin_hit && !poll && ++spins > 64) {
        /* The dashboard runs dozens of these threads, one per sound
           stream: after a while, back off to 10 ms. */
        cpu_block();
        usleep(spins > 256 ? 10000 : 1000);
    }
    return st;
}

NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, ULONG WaitReason, KPROCESSOR_MODE WaitMode,
                                     BOOLEAN Alertable, LARGE_INTEGER *Timeout)
{
    (void)WaitReason;
    return wait_objects(1, &Object, 1, WaitMode, Alertable, Timeout);
}

NTSTATUS NTAPI KeWaitForMultipleObjects(ULONG Count, PVOID Object[], ULONG WaitType,
                                        ULONG WaitReason, KPROCESSOR_MODE WaitMode,
                                        BOOLEAN Alertable, LARGE_INTEGER *Timeout,
                                        KWAIT_BLOCK *WaitBlockArray)
{
    (void)WaitReason; (void)WaitBlockArray;
    return wait_objects(Count, Object, WaitType == 1 /* WaitAny */, WaitMode, Alertable, Timeout);
}

NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                      LARGE_INTEGER *Interval)
{
    xthread *xt = thread_current();
    if ((Alertable && WaitMode == 1) || (xt && xt->apc_head)) {
        /* A wait on nothing: it ends at the interval or at an APC. */
        NTSTATUS st = wait_objects(0, NULL, 1, WaitMode, Alertable, Interval);
        return st == STATUS_TIMEOUT ? STATUS_SUCCESS : st;
    }
    LONGLONG t = Interval->QuadPart;
    ULONGLONG rel = t < 0 ? (ULONGLONG)-t : (t > (LONGLONG)system_time_now() ? t - system_time_now() : 0);
    cpu_block();
    if (rel == 0) {
        sched_yield();
    } else {
        struct timespec ts = { rel / 10000000ULL, (rel % 10000000ULL) * 100 };
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    }
    return STATUS_SUCCESS;
}

/* ---- events, semaphores, mutants ------------------------------------- */

static void init_header(DISPATCHER_HEADER *h, UCHAR type, UCHAR size, LONG state)
{
    h->Type = type;
    h->Size = size;
    h->Absolute = 0;
    h->Inserted = 0;
    h->SignalState = state;
    h->WaitListHead.Flink = h->WaitListHead.Blink = &h->WaitListHead;
}

void NTAPI KeInitializeEvent(KEVENT *Event, ULONG Type, BOOLEAN State)
{
    init_header(&Event->Header, (UCHAR)Type, sizeof(KEVENT) / 4, State);
}

LONG NTAPI KeSetEvent(KEVENT *Event, LONG Increment, BOOLEAN Wait)
{
    (void)Increment; (void)Wait;
    pthread_mutex_lock(&g_disp_lock);
    LONG old = Event->Header.SignalState;
    Event->Header.SignalState = 1;
    disp_signal(Event);
    pthread_mutex_unlock(&g_disp_lock);
    return old;
}

LONG NTAPI KeResetEvent(KEVENT *Event)
{
    pthread_mutex_lock(&g_disp_lock);
    LONG old = Event->Header.SignalState;
    Event->Header.SignalState = 0;
    pthread_mutex_unlock(&g_disp_lock);
    return old;
}

LONG NTAPI KePulseEvent(KEVENT *Event, LONG Increment, BOOLEAN Wait)
{
    (void)Increment; (void)Wait;
    /* Waking only current waiters needs wait lists; approximate by signalling
       and letting the next waiter consume it. */
    pthread_mutex_lock(&g_disp_lock);
    LONG old = Event->Header.SignalState;
    Event->Header.SignalState = 1;
    disp_signal(Event);
    pthread_mutex_unlock(&g_disp_lock);
    if (Event->Header.Type == EventNotificationObject) {
        sched_yield();
        KeResetEvent(Event);
    }
    return old;
}

void NTAPI KeInitializeSemaphore(KSEMAPHORE *Semaphore, LONG Count, LONG Limit)
{
    init_header(&Semaphore->Header, SemaphoreObject, sizeof(KSEMAPHORE) / 4, Count);
    Semaphore->Limit = Limit;
}

LONG NTAPI KeReleaseSemaphore(KSEMAPHORE *Semaphore, LONG Increment, LONG Adjustment, BOOLEAN Wait)
{
    (void)Increment; (void)Wait;
    pthread_mutex_lock(&g_disp_lock);
    LONG old = Semaphore->Header.SignalState;
    Semaphore->Header.SignalState += Adjustment;
    disp_signal(Semaphore);
    pthread_mutex_unlock(&g_disp_lock);
    return old;
}

void NTAPI KeInitializeMutant(KMUTANT *Mutant, BOOLEAN InitialOwner)
{
    init_header(&Mutant->Header, MutantObject, sizeof(KMUTANT) / 4, InitialOwner ? 0 : 1);
    xthread *t = thread_current();
    Mutant->OwnerThread = InitialOwner && t ? &t->ethread.Tcb : NULL;
    Mutant->Abandoned = 0;
}

LONG NTAPI KeReleaseMutant(KMUTANT *Mutant, LONG Increment, BOOLEAN Abandoned, BOOLEAN Wait)
{
    (void)Increment; (void)Wait;
    pthread_mutex_lock(&g_disp_lock);
    LONG old = Mutant->Header.SignalState;
    if (Abandoned) {
        Mutant->Header.SignalState = 1;
        Mutant->Abandoned = 1;
    } else {
        Mutant->Header.SignalState++;
    }
    if (Mutant->Header.SignalState > 0)
        Mutant->OwnerThread = NULL;
    disp_signal(Mutant);
    pthread_mutex_unlock(&g_disp_lock);
    return old;
}

/* ---- IRQL ------------------------------------------------------------- */

#ifdef XBC_NATIVE
static inline KIRQL get_irql(void)
{
    KIRQL v;
    __asm__ volatile("movb %%fs:0x24, %0" : "=q"(v));
    return v;
}

static inline void set_irql(KIRQL v)
{
    __asm__ volatile("movb %0, %%fs:0x24" : : "q"(v));
}
#else
static inline KIRQL get_irql(void)
{
    xthread *t = thread_current();
    return t ? t->pcr->Irql : 0;
}

static inline void set_irql(KIRQL v)
{
    xthread *t = thread_current();
    if (t) t->pcr->Irql = v;
}
#endif

KIRQL NTAPI KeGetCurrentIrql(void) { return get_irql(); }
KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql) { KIRQL o = get_irql(); set_irql(NewIrql); return o; }
void FASTCALL KfLowerIrql(KIRQL NewIrql) { set_irql(NewIrql); }
KIRQL NTAPI KeRaiseIrqlToDpcLevel(void) { return KfRaiseIrql(2); }
KIRQL NTAPI KeRaiseIrqlToSynchLevel(void) { return KfRaiseIrql(2); }

KTHREAD *NTAPI KeGetCurrentThread(void)
{
    xthread *t = thread_current();
    return t ? &t->ethread.Tcb : NULL;
}

/* Spin locks: the guest uses raise/lower IRQL around a single ULONG.  Guest
   code on real hardware is uniprocessor, but our threads are truly parallel,
   so take a real lock. */
static pthread_mutex_t spin_lock = PTHREAD_MUTEX_INITIALIZER;
KIRQL FASTCALL KfAcquireSpinLock(ULONG *SpinLock) { (void)SpinLock; pthread_mutex_lock(&spin_lock); return KfRaiseIrql(2); }
void FASTCALL KfReleaseSpinLock(ULONG *SpinLock, KIRQL OldIrql) { (void)SpinLock; KfLowerIrql(OldIrql); pthread_mutex_unlock(&spin_lock); }
void FASTCALL KefAcquireSpinLockAtDpcLevel(ULONG *SpinLock) { (void)SpinLock; pthread_mutex_lock(&spin_lock); }
void FASTCALL KefReleaseSpinLockFromDpcLevel(ULONG *SpinLock) { (void)SpinLock; pthread_mutex_unlock(&spin_lock); }

/* ---- DPCs and timers -------------------------------------------------- */

typedef struct timer_node {
    KTIMER *timer;
    ULONGLONG due;   /* monotonic 100 ns */
    struct timer_node *next;
} timer_node;

static pthread_mutex_t dpc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dpc_cond XBC_COND_ALIGN;
static KDPC *dpc_queue[256];
static unsigned dpc_head, dpc_tail;
static timer_node *timers;

void NTAPI KeInitializeDpc(KDPC *Dpc, PVOID DeferredRoutine, PVOID DeferredContext)
{
    memset(Dpc, 0, sizeof(*Dpc));
    Dpc->Type = DpcObject;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
}

BOOLEAN NTAPI KeInsertQueueDpc(KDPC *Dpc, PVOID Arg1, PVOID Arg2)
{
    pthread_mutex_lock(&dpc_lock);
    if (Dpc->Inserted) {
        pthread_mutex_unlock(&dpc_lock);
        return 0;
    }
    Dpc->Inserted = 1;
    Dpc->SystemArgument1 = Arg1;
    Dpc->SystemArgument2 = Arg2;
    dpc_queue[dpc_tail++ % 256] = Dpc;
    pthread_cond_broadcast(&dpc_cond);
    pthread_mutex_unlock(&dpc_lock);
    return 1;
}

BOOLEAN NTAPI KeRemoveQueueDpc(KDPC *Dpc)
{
    pthread_mutex_lock(&dpc_lock);
    BOOLEAN was = Dpc->Inserted;
    if (was) {
        for (unsigned i = dpc_head; i != dpc_tail; i++)
            if (dpc_queue[i % 256] == Dpc) dpc_queue[i % 256] = NULL;
        Dpc->Inserted = 0;
    }
    pthread_mutex_unlock(&dpc_lock);
    return was;
}

void NTAPI KeInitializeTimerEx(KTIMER *Timer, ULONG Type)
{
    init_header(&Timer->Header, (UCHAR)(TimerNotificationObject + Type), sizeof(KTIMER) / 4, 0);
    Timer->DueTime.QuadPart = 0;
    Timer->Period = 0;
    Timer->Dpc = NULL;
}

static bool timer_unlink(KTIMER *t)
{
    for (timer_node **pp = &timers; *pp; pp = &(*pp)->next) {
        if ((*pp)->timer == t) {
            timer_node *n = *pp;
            *pp = n->next;
            free(n);
            return true;
        }
    }
    return false;
}

/* DueTime is a LARGE_INTEGER passed by value; take it as two stack slots so
   GCC's i386 ABI cannot disagree with MSVC about its alignment. */
BOOLEAN NTAPI KeSetTimerEx(KTIMER *Timer, ULONG DueLow, LONG DueHigh, LONG Period, KDPC *Dpc)
{
    LARGE_INTEGER DueTime;
    DueTime.LowPart = DueLow;
    DueTime.HighPart = DueHigh;
    ULONGLONG now = mono_100ns();
    ULONGLONG due = DueTime.QuadPart < 0 ? now + (ULONGLONG)(-DueTime.QuadPart)
                  : now + (DueTime.QuadPart > (LONGLONG)system_time_now()
                           ? DueTime.QuadPart - system_time_now() : 0);
    pthread_mutex_lock(&dpc_lock);
    bool was = timer_unlink(Timer);
    pthread_mutex_lock(&g_disp_lock);
    Timer->Header.SignalState = 0;
    Timer->Header.Inserted = 1;
    pthread_mutex_unlock(&g_disp_lock);
    Timer->Period = Period;
    Timer->Dpc = Dpc;
    Timer->DueTime.QuadPart = due;
    timer_node *n = malloc(sizeof(*n));
    n->timer = Timer;
    n->due = due;
    n->next = timers;
    timers = n;
    pthread_cond_broadcast(&dpc_cond);
    pthread_mutex_unlock(&dpc_lock);
    return was;
}

BOOLEAN NTAPI KeSetTimer(KTIMER *Timer, ULONG DueLow, LONG DueHigh, KDPC *Dpc)
{
    return KeSetTimerEx(Timer, DueLow, DueHigh, 0, Dpc);
}

BOOLEAN NTAPI KeCancelTimer(KTIMER *Timer)
{
    pthread_mutex_lock(&dpc_lock);
    bool was = timer_unlink(Timer);
    Timer->Header.Inserted = 0;
    pthread_mutex_unlock(&dpc_lock);
    return was;
}

typedef void (NTAPI *dpc_fn)(KDPC *, PVOID, PVOID, PVOID);

void (*g_vblank_hook)(void);

/* The DPC thread: fires timers, runs DPCs and keeps KeTickCount moving. */
static void *dpc_thread(void *arg)
{
    pthread_setname_np(pthread_self(), "dpc");
    prof_thread_start();
    (void)arg;
    thread_adopt_host("dpc");
    set_irql(2);
    ULONGLONG next_vblank = 0;
    pthread_mutex_lock(&dpc_lock);
    for (;;) {
        ULONGLONG now = mono_100ns();
        KeTickCount = (ULONG)((now - boot_mono) / 10000);
        if (g_apu_sample_counter) *g_apu_sample_counter = (ULONG)((now - boot_mono) * 48 / 10000);

        /* Expire timers. */
        for (timer_node **pp = &timers; *pp;) {
            timer_node *n = *pp;
            if (n->due > now) { pp = &n->next; continue; }
            KTIMER *t = n->timer;
            pthread_mutex_lock(&g_disp_lock);
            t->Header.SignalState = 1;
            disp_signal(t);
            pthread_mutex_unlock(&g_disp_lock);
            if (t->Dpc) {
                KDPC *d = t->Dpc;
                if (!d->Inserted) {
                    d->Inserted = 1;
                    d->SystemArgument1 = (PVOID)(ULONG)now;
                    d->SystemArgument2 = 0;
                    dpc_queue[dpc_tail++ % 256] = d;
                }
            }
            if (t->Period > 0) {
                /* Keep the period's phase, as the kernel's clock interrupt
                   does: rescheduling from `now` would add this thread's
                   wake-up latency to every period (a 5 ms music timer ran
                   about 20% slow).  Far behind (a stall), skip ahead. */
                ULONGLONG period = (ULONGLONG)t->Period * 10000;
                n->due += period;
                if (!fixed_step && n->due + 100 * 10000ULL < now) n->due = now + period;
                pp = &n->next;
            } else {
                t->Header.Inserted = 0;
                *pp = n->next;
                free(n);
            }
        }

        /* Vertical blanks, 60 a second. */
        if (g_vblank_hook) {
            if (!next_vblank) next_vblank = now;
            if (now >= next_vblank) {
                next_vblank += 166667;
                if (next_vblank + 100 * 10000ULL < now) next_vblank = now + 166667;
                pthread_mutex_unlock(&dpc_lock);
                g_vblank_hook();
                pthread_mutex_lock(&dpc_lock);
            }
        }

        /* Run DPCs with the lock dropped: they may queue more. */
        while (dpc_head != dpc_tail) {
            KDPC *d = dpc_queue[dpc_head++ % 256];
            if (!d) continue;
            d->Inserted = 0;
            pthread_mutex_unlock(&dpc_lock);
#ifdef XBC_TRANSLATED
            CPU_CALL(d->DeferredRoutine, CONV_STD, (uint32_t)d, (uint32_t)d->DeferredContext,
                     (uint32_t)d->SystemArgument1, (uint32_t)d->SystemArgument2);
#else
            ((dpc_fn)d->DeferredRoutine)(d, d->DeferredContext, d->SystemArgument1, d->SystemArgument2);
#endif
            pthread_mutex_lock(&dpc_lock);
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += 1000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&dpc_cond, &dpc_lock, &ts);
    }
    return NULL;
}

/* Called once per presented frame. */
void ke_frame_presented(void)
{
    if (!fixed_step) return;
    __atomic_add_fetch(&fixed_now, fixed_step, __ATOMIC_RELEASE);
    pthread_mutex_lock(&dpc_lock);
    pthread_cond_broadcast(&dpc_cond);
    pthread_mutex_unlock(&dpc_lock);
}

void timers_init(void)
{
    const char *fps = getenv("XBCOMPAT_FIXED_FPS");
    if (fps && atoi(fps) > 0) {
        fixed_step = 10000000ULL / (ULONGLONG)atoi(fps);
        fixed_now = 1;   /* boot_mono below: nonzero so differences stay positive */
    }
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&dpc_cond, &ca);
    pthread_condattr_destroy(&ca);
    boot_mono = mono_100ns();
    tsc_init();
    pthread_t t;
    pthread_create(&t, NULL, dpc_thread, NULL);
    pthread_detach(t);
}

/* ---- interrupts (no hardware to deliver them) ------------------------ */

ULONG NTAPI HalGetInterruptVector(ULONG BusInterruptLevel, KIRQL *Irql)
{
    *Irql = (KIRQL)(26 - BusInterruptLevel);
    return 0x30 + BusInterruptLevel;
}

void NTAPI KeInitializeInterrupt(PVOID Interrupt, PVOID ServiceRoutine, PVOID ServiceContext,
                                 ULONG Vector, KIRQL Irql, ULONG InterruptMode, BOOLEAN ShareVector)
{
    (void)Interrupt; (void)ServiceRoutine; (void)ServiceContext; (void)Vector;
    (void)Irql; (void)InterruptMode; (void)ShareVector;
}

BOOLEAN NTAPI KeConnectInterrupt(PVOID Interrupt) { (void)Interrupt; return 1; }
BOOLEAN NTAPI KeDisconnectInterrupt(PVOID Interrupt) { (void)Interrupt; return 1; }

BOOLEAN NTAPI KeSynchronizeExecution(PVOID Interrupt, BOOLEAN (NTAPI *Routine)(PVOID), PVOID Context)
{
    (void)Interrupt;
#ifdef XBC_TRANSLATED
    return (BOOLEAN)CPU_CALL(Routine, CONV_STD, (uint32_t)Context);
#else
    return Routine(Context);
#endif
}

/* ---- threads: priority and APC bits the guest pokes at -------------- */

LONG NTAPI KeSetBasePriorityThread(KTHREAD *Thread, LONG Increment)
{
    LONG old = Thread->BasePriority;
    TRACE("KeSetBasePriorityThread(%p, %d)", (void *)Thread, (int)Increment);
    Thread->BasePriority = (SCHAR)(8 + Increment);
    /* The thread runs at its new base priority (no boosts here). Increments
       of +-16 saturate (THREAD_PRIORITY_TIME_CRITICAL, _IDLE). */
    LONG p = 8 + Increment;
    Thread->Priority = (SCHAR)(p < 1 ? 1 : p > 31 ? 31 : p);
    return old - 8;
}

LONG NTAPI KeQueryBasePriorityThread(KTHREAD *Thread) { return Thread->BasePriority - 8; }

LONG NTAPI KeSetPriorityThread(KTHREAD *Thread, LONG Priority)
{
    LONG old = Thread->Priority;
    TRACE("KeSetPriorityThread(%p, %d)", (void *)Thread, (int)Priority);
    Thread->Priority = (SCHAR)Priority;
    return old;
}

BOOLEAN NTAPI KeSetDisableBoostThread(KTHREAD *Thread, BOOLEAN Disable)
{
    BOOLEAN old = Thread->DisableBoost;
    Thread->DisableBoost = Disable;
    return old;
}

void NTAPI KeEnterCriticalRegion(void) {}
void NTAPI KeLeaveCriticalRegion(void) {}

BOOLEAN NTAPI KeAlertThread(KTHREAD *Thread, KPROCESSOR_MODE Mode) { (void)Thread; (void)Mode; return 0; }
BOOLEAN NTAPI KeTestAlertThread(KPROCESSOR_MODE Mode) { (void)Mode; return 0; }

void NTAPI KeInitializeApc(KAPC *Apc, KTHREAD *Thread, PVOID KernelRoutine, PVOID RundownRoutine,
                           PVOID NormalRoutine, KPROCESSOR_MODE ApcMode, PVOID NormalContext)
{
    memset(Apc, 0, sizeof(*Apc));
    Apc->Type = ApcObject;
    Apc->Thread = Thread;
    Apc->KernelRoutine = KernelRoutine;
    Apc->RundownRoutine = RundownRoutine;
    Apc->NormalRoutine = NormalRoutine;
    Apc->ApcMode = ApcMode;
    Apc->NormalContext = NormalContext;
}

BOOLEAN NTAPI KeInsertQueueApc(KAPC *Apc, PVOID Arg1, PVOID Arg2, LONG Increment)
{
    (void)Increment;
    if (Apc->Inserted || !Apc->Thread) return 0;
    Apc->SystemArgument1 = Arg1;
    Apc->SystemArgument2 = Arg2;
    Apc->Inserted = 1;
    xthread *t = (xthread *)((char *)Apc->Thread - offsetof(xthread, ethread.Tcb));
    apc_queue(t, Apc->NormalRoutine, Apc->NormalContext, Arg1, Arg2, Apc);
    return 1;
}

void NTAPI KeBugCheck(ULONG BugCheckCode)
{
    fatal("KeBugCheck(%#x)", BugCheckCode);
}

void NTAPI KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
{
    fatal("KeBugCheckEx(%#x, %#x, %#x, %#x, %#x)", Code, P1, P2, P3, P4);
}

void NTAPI KeStallExecutionProcessor(ULONG MicroSeconds)
{
    struct timespec ts = { MicroSeconds / 1000000, (MicroSeconds % 1000000) * 1000 };
    cpu_block();
    nanosleep(&ts, NULL);
}
