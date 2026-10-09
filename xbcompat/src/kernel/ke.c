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

pthread_mutex_t g_disp_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_disp_cond;

void disp_signal_all(void)
{
    pthread_cond_broadcast(&g_disp_cond);
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

void apc_queue_user(PVOID routine, PVOID ctx, PVOID arg1, PVOID arg2)
{
    xthread *xt = thread_current();
    if (!xt || xt->napc == (int)(sizeof xt->apc / sizeof xt->apc[0])) {
        xlog("user APC %p dropped", routine);
        return;
    }
    xt->apc[xt->napc].routine = routine;
    xt->apc[xt->napc].ctx = ctx;
    xt->apc[xt->napc].arg1 = arg1;
    xt->apc[xt->napc].arg2 = arg2;
    xt->napc++;
}

bool apc_deliver_user(void)
{
    xthread *xt = thread_current();
    if (!xt || !xt->napc) return false;
    /* Oldest first; a routine may queue more (another ReadFileEx), which run too. */
    while (xt->napc) {
        __typeof__(xt->apc[0]) a = xt->apc[0];
        memmove(&xt->apc[0], &xt->apc[1], --xt->napc * sizeof a);
        ((void (NTAPI *)(PVOID, PVOID, PVOID))a.routine)(a.ctx, a.arg1, a.arg2);
    }
    return true;
}

NTSTATUS wait_objects(ULONG count, PVOID objects[], int wait_any,
                      BOOLEAN alertable, LARGE_INTEGER *timeout)
{
    if (alertable && apc_deliver_user()) return STATUS_USER_APC;
    xthread *xt = thread_current();
    KTHREAD *self = xt ? &xt->ethread.Tcb : NULL;
    struct timespec dl;
    bool has_dl = deadline_from(timeout, &dl);
    bool poll = timeout && timeout->QuadPart == 0;
    NTSTATUS st;

    pthread_mutex_lock(&g_disp_lock);
    for (;;) {
        if (wait_any) {
            for (ULONG i = 0; i < count; i++) {
                DISPATCHER_HEADER *h = objects[i];
                if (object_signaled(h, self)) {
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
        if (poll) { st = STATUS_TIMEOUT; goto done; }
        int rc = has_dl ? pthread_cond_timedwait(&g_disp_cond, &g_disp_lock, &dl)
                        : pthread_cond_wait(&g_disp_cond, &g_disp_lock);
        if (rc == ETIMEDOUT) {
            /* One last look: the signal may have raced the timeout. */
            poll = true;
        }
    }
done:
    pthread_mutex_unlock(&g_disp_lock);
    return st;
}

NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, ULONG WaitReason, KPROCESSOR_MODE WaitMode,
                                     BOOLEAN Alertable, LARGE_INTEGER *Timeout)
{
    (void)WaitReason; (void)WaitMode;
    return wait_objects(1, &Object, 1, Alertable, Timeout);
}

NTSTATUS NTAPI KeWaitForMultipleObjects(ULONG Count, PVOID Object[], ULONG WaitType,
                                        ULONG WaitReason, KPROCESSOR_MODE WaitMode,
                                        BOOLEAN Alertable, LARGE_INTEGER *Timeout,
                                        KWAIT_BLOCK *WaitBlockArray)
{
    (void)WaitReason; (void)WaitMode; (void)WaitBlockArray;
    return wait_objects(Count, Object, WaitType == 1 /* WaitAny */, Alertable, Timeout);
}

NTSTATUS NTAPI KeDelayExecutionThread(KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                      LARGE_INTEGER *Interval)
{
    (void)WaitMode;
    if (Alertable && apc_deliver_user()) return STATUS_USER_APC;
    LONGLONG t = Interval->QuadPart;
    ULONGLONG rel = t < 0 ? (ULONGLONG)-t : (t > (LONGLONG)system_time_now() ? t - system_time_now() : 0);
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
    disp_signal_all();
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
    disp_signal_all();
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
    disp_signal_all();
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
    disp_signal_all();
    pthread_mutex_unlock(&g_disp_lock);
    return old;
}

/* ---- IRQL ------------------------------------------------------------- */

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
static pthread_cond_t dpc_cond;
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
    (void)arg;
    thread_adopt_host("dpc");
    set_irql(2);
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
            disp_signal_all();
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

        /* The vertical blank interrupt's DPC, at 60 Hz of this clock. */
        static ULONGLONG next_vblank;
        if (g_vblank_hook && now >= next_vblank) {
            next_vblank = next_vblank && now - next_vblank < 1000000 ? next_vblank + 166667 : now + 166667;
            pthread_mutex_unlock(&dpc_lock);
            g_vblank_hook();
            pthread_mutex_lock(&dpc_lock);
        }

        /* Run DPCs with the lock dropped: they may queue more. */
        while (dpc_head != dpc_tail) {
            KDPC *d = dpc_queue[dpc_head++ % 256];
            if (!d) continue;
            d->Inserted = 0;
            pthread_mutex_unlock(&dpc_lock);
            ((dpc_fn)d->DeferredRoutine)(d, d->DeferredContext, d->SystemArgument1, d->SystemArgument2);
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
    pthread_cond_init(&g_disp_cond, &ca);          /* CLOCK_REALTIME deadlines */
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
    return Routine(Context);
}

/* ---- threads: priority and APC bits the guest pokes at -------------- */

LONG NTAPI KeSetBasePriorityThread(KTHREAD *Thread, LONG Increment)
{
    LONG old = Thread->BasePriority;
    TRACE("KeSetBasePriorityThread(%p, %d)", (void *)Thread, (int)Increment);
    Thread->BasePriority = (SCHAR)(8 + Increment);
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
    /* APCs need the target thread to enter an alertable wait; run user APCs
       immediately instead.  Good enough for completion callbacks. */
    Apc->SystemArgument1 = Arg1;
    Apc->SystemArgument2 = Arg2;
    xlog("KeInsertQueueApc: APC delivery is not implemented; dropping %p", (void *)Apc);
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
    nanosleep(&ts, NULL);
}
