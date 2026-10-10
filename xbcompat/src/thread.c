/*
 * Guest threads.  Each Xbox thread is a host pthread whose FS segment points
 * at its own KPCR, exactly where Xbox code expects to find it (fs:[0]).  The
 * host C library uses GS for its own TLS on i386, so FS is ours.
 */
#define _GNU_SOURCE
#if defined(__i386__)
#include <asm/ldt.h>
#endif
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "xbcompat.h"
#include "cpu.h"

static pthread_key_t current_key;
static pthread_mutex_t ldt_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t ldt_used[8192];
static LONG next_thread_id = 1;

#ifdef XBC_NATIVE
static int ldt_alloc(void *base, uint32_t limit)
{
    pthread_mutex_lock(&ldt_lock);
    int idx = 1;
    while (idx < 8192 && ldt_used[idx]) idx++;
    if (idx == 8192) fatal("out of LDT entries");
    ldt_used[idx] = 1;
    pthread_mutex_unlock(&ldt_lock);

    struct user_desc d = {
        .entry_number = idx,
        .base_addr = (uint32_t)base,
        .limit = limit,
        .seg_32bit = 1,
        .contents = 0,
        .read_exec_only = 0,
        .limit_in_pages = 0,
        .seg_not_present = 0,
        .useable = 1,
    };
    if (syscall(SYS_modify_ldt, 1, &d, sizeof(d)) != 0)
        fatal("modify_ldt: %s", strerror(errno));
    return idx;
}

static void ldt_free(int idx)
{
    pthread_mutex_lock(&ldt_lock);
    ldt_used[idx] = 0;
    pthread_mutex_unlock(&ldt_lock);
}

static void load_fs(int idx)
{
    uint16_t sel = (uint16_t)((idx << 3) | 7);   /* LDT, RPL 3 */
    __asm__ volatile("movw %0, %%fs" : : "r"(sel));
}
#else
/* Translated guest code gets its fs base from the CPU emulator. */
static void ldt_free(int idx) { (void)idx; }
#endif

xthread *thread_current(void)
{
    return pthread_getspecific(current_key);
}

/* Build the guest-visible KPCR/ETHREAD for the calling host thread. */
static void attach(xthread *t)
{
    thread_trap_tsc();
#ifdef XBC_TRANSLATED
    /* Guest code runs on a stack of its own in guest memory, as roomy as the
       host stacks the native build gives guest threads. */
    size_t guest_stack_size = t->stack_size * 4;
    if (guest_stack_size < (1u << 20)) guest_stack_size = 1u << 20;
    void *guest_stack = arena_alloc(guest_stack_size, 1);
#endif
    KPCR *pcr = t->pcr;
    memset(pcr, 0, sizeof(*pcr));
    pcr->NtTib.ExceptionList = (PVOID)-1;
    pcr->NtTib.Self = &pcr->NtTib;
    pcr->SelfPcr = pcr;
    pcr->Prcb = &pcr->PrcbData;
    pcr->PrcbData.CurrentThread = &t->ethread.Tcb;

    /* XAPI keeps TLS directly below fs:StackBase and asserts that
       StackBase - TlsSize == KTHREAD.TlsData. */
    if (t->tls_size) {
        /* StackBase is 16-byte aligned; XAPI rounds the TLS size so that the
           block after its 4-byte index slot is 16-byte aligned too. */
        size_t top = (t->tls_size + 15) & ~15u;
        t->tls_block = arena_alloc(top + 16, 0);
        pcr->NtTib.StackBase = (char *)t->tls_block + top + 16;
        t->ethread.Tcb.TlsData = (char *)pcr->NtTib.StackBase - t->tls_size;
    } else {
#ifdef XBC_NATIVE
        pcr->NtTib.StackBase = (PVOID)((uintptr_t)__builtin_frame_address(0) & ~15u);
#else
        pcr->NtTib.StackBase = (char *)guest_stack + guest_stack_size;
#endif
    }
    pcr->NtTib.StackLimit = (char *)pcr->NtTib.StackBase - t->stack_size;
    t->ethread.Tcb.StackBase = pcr->NtTib.StackBase;
    t->ethread.Tcb.StackLimit = pcr->NtTib.StackLimit;

#ifdef XBC_NATIVE
    t->ldt_index = ldt_alloc(pcr, sizeof(*pcr) - 1);
    load_fs(t->ldt_index);
#else
    cpu_thread_attach(pcr, guest_stack, (char *)guest_stack + guest_stack_size);
#endif
    pthread_setspecific(current_key, t);
}

static xthread *thread_alloc(SIZE_T stack_size, SIZE_T tls_size)
{
    /* Kernel objects the guest can see live below 0x80000000 like on a console. */
    xthread *t = pool_alloc(sizeof(*t));
    memset(t, 0, sizeof(*t));
    t->pcr = pool_alloc(sizeof(KPCR));
    t->tls_size = tls_size;
    t->stack_size = stack_size;
    KTHREAD *k = &t->ethread.Tcb;
    k->Header.Type = ThreadObject;
    k->Header.Size = sizeof(KTHREAD) / 4;
    k->Header.WaitListHead.Flink = k->Header.WaitListHead.Blink = &k->Header.WaitListHead;
    k->Priority = k->BasePriority = 8;
    t->ethread.UniqueThread = (HANDLE)(ULONG_PTR)__sync_fetch_and_add(&next_thread_id, 1);
    return t;
}

#ifdef XBC_TRANSLATED
int thread_guest_priority(void)
{
    xthread *t = thread_current();
    return t ? t->ethread.Tcb.Priority : 32;
}
#endif

void thread_init_main(void)
{
    pthread_key_create(&current_key, NULL);
}

xthread *thread_adopt_host(const char *name)
{
    (void)name;
    xthread *t = thread_alloc(1 << 20, 0);
    attach(t);
    return t;
}

typedef ULONG (NTAPI *start_fn)(PVOID);
typedef void (NTAPI *system_fn)(PVOID, PVOID);

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;

static void *thread_main(void *arg)
{
    xthread *t = arg;
    attach(t);

    pthread_mutex_lock(&start_lock);
    while (t->ethread.Tcb.SuspendCount > 0)
        pthread_cond_wait(&start_cond, &start_lock);
    t->started = true;
    pthread_mutex_unlock(&start_lock);

    if (setjmp(t->exit_jmp) == 0) {
        TRACE("thread %u starting at %p", (unsigned)(ULONG_PTR)t->ethread.UniqueThread,
              t->start_routine);
#ifdef XBC_TRANSLATED
        if (t->system_routine)
            CPU_CALL(t->system_routine, CONV_STD, (uint32_t)t->start_routine, (uint32_t)t->start_context);
        else
            CPU_CALL(t->start_routine, CONV_STD, (uint32_t)t->start_context);
#else
        if (t->system_routine)
            ((system_fn)t->system_routine)(t->start_routine, t->start_context);
        else
            ((start_fn)t->start_routine)(t->start_context);
#endif
        t->ethread.ExitStatus = STATUS_SUCCESS;
    }
    /* Returned or PsTerminateSystemThread'ed: signal the thread object. */
    TRACE("thread %u exited with %#x", (unsigned)(ULONG_PTR)t->ethread.UniqueThread,
          t->ethread.ExitStatus);
    pthread_mutex_lock(&g_disp_lock);
    t->ethread.Tcb.HasTerminated = 1;
    t->ethread.Tcb.Header.SignalState = 1;
    disp_signal_all();
    pthread_mutex_unlock(&g_disp_lock);
    ldt_free(t->ldt_index);
#ifdef XBC_TRANSLATED
    cpu_thread_detach();
#endif
    return NULL;
}

xthread *thread_create(SIZE_T stack_size, SIZE_T tls_size, PVOID system_routine,
                       PVOID start_routine, PVOID start_context, bool suspended)
{
    xthread *t = thread_alloc(stack_size, tls_size);
    t->system_routine = system_routine;
    t->start_routine = start_routine;
    t->start_context = start_context;
    t->ethread.StartAddress = start_routine;
    t->ethread.Tcb.SuspendCount = suspended ? 1 : 0;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* Xbox titles ask for small stacks and MSVC code only keeps 4-byte stack
       alignment; give every guest thread a roomy host stack. */
    size_t host_stack = stack_size * 4;
    if (host_stack < (1u << 20)) host_stack = 1u << 20;
    pthread_attr_setstacksize(&attr, host_stack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->host, &attr, thread_main, t) != 0)
        fatal("pthread_create failed");
    pthread_attr_destroy(&attr);
    return t;
}

void thread_resume(xthread *t)
{
    pthread_mutex_lock(&start_lock);
    if (t->ethread.Tcb.SuspendCount > 0 &&
        __atomic_sub_fetch(&t->ethread.Tcb.SuspendCount, 1, __ATOMIC_SEQ_CST) == 0)
        __atomic_add_fetch(&t->resume_gen, 1, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_lock);
}

/* NtSuspendThread.  A thread suspending itself waits here until resumed.
   Another thread is stopped with a signal, but only while it runs guest
   code: parked inside xbcompat or a host library it could hold a lock its
   suspender needs (the log, GL, the dispatcher), so the request is repeated
   until it lands in guest code, the way the Xbox kernel only suspends at
   its next return to the title.  A thread blocked in a kernel wait counts
   as stopped at once and parks on its way out of the wait. */
#define SIG_SUSPEND (SIGRTMIN + 4)

static void suspend_handler(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    xthread *t = thread_current();
    if (!t) return;
#ifdef XBC_NATIVE
    ucontext_t *uc = uc_;
    greg_t ip = uc->uc_mcontext.gregs[REG_EIP];
    if ((ULONG)ip < XBE_BASE || (ULONG)ip >= IMAGE_REGION_END) {
        t->suspend_ip = (ULONG)ip;
        return;
    }
#else
    (void)uc_;
    return;
#endif
    int saved = errno;
    __atomic_store_n(&t->parked, 1, __ATOMIC_SEQ_CST);
    struct timespec ms = { 0, 1000000 };
    int gen = __atomic_load_n(&t->resume_gen, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&t->ethread.Tcb.SuspendCount, __ATOMIC_SEQ_CST) > 0 &&
           __atomic_load_n(&t->resume_gen, __ATOMIC_SEQ_CST) == gen)
        nanosleep(&ms, NULL);
    __atomic_store_n(&t->parked, 0, __ATOMIC_SEQ_CST);
    errno = saved;
}

static void suspend_install(void)
{
    struct sigaction sa = { 0 };
    sa.sa_sigaction = suspend_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIG_SUSPEND, &sa, NULL);
}

ULONG thread_suspend(xthread *t)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, suspend_install);

    pthread_mutex_lock(&start_lock);
    ULONG prev = t->ethread.Tcb.SuspendCount;
    if (t->ethread.Tcb.HasTerminated || prev >= 127) {
        pthread_mutex_unlock(&start_lock);
        return prev;
    }
    __atomic_add_fetch(&t->ethread.Tcb.SuspendCount, 1, __ATOMIC_SEQ_CST);
    if (t == thread_current()) {
        /* Stopped: a suspend from elsewhere only counts.  Run again once a
           resume brings the count to 0, even if another suspend follows
           before this thread gets to look. */
        __atomic_store_n(&t->in_wait, 1, __ATOMIC_SEQ_CST);
        int gen = t->resume_gen;
        while (t->ethread.Tcb.SuspendCount > 0 && t->resume_gen == gen)
            pthread_cond_wait(&start_cond, &start_lock);
        pthread_mutex_unlock(&start_lock);
        thread_wait_end(t);
        return prev;
    }
    bool started = t->started;
    pthread_mutex_unlock(&start_lock);
    if (prev > 0 || !started) return prev;   /* already stopped, or not yet running */
    if (__atomic_load_n(&t->in_wait, __ATOMIC_SEQ_CST)) return prev;

    /* Ask until it parks, it is resumed meanwhile, or it ends. */
    struct timespec ms = { 0, 1000000 };
    for (int i = 0; i < 10000; i++) {
        if (__atomic_load_n(&t->parked, __ATOMIC_SEQ_CST)) return prev;
        if (__atomic_load_n(&t->in_wait, __ATOMIC_SEQ_CST)) return prev;
        if (__atomic_load_n(&t->ethread.Tcb.SuspendCount, __ATOMIC_SEQ_CST) == 0) return prev;
        if (t->ethread.Tcb.HasTerminated) return prev;
        if (i % 4 == 0) pthread_kill(t->host, SIG_SUSPEND);
        nanosleep(&ms, NULL);
    }
    Dl_info info;
    const char *where = dladdr((void *)t->suspend_ip, &info) && info.dli_sname ? info.dli_sname : "?";
    xlog("NtSuspendThread: thread %u never reached guest code to be suspended (last at %#x, %s)",
         (unsigned)(ULONG_PTR)t->ethread.UniqueThread, t->suspend_ip, where);
    return prev;
}

/* Leaving a kernel wait: stop here if NtSuspendThread came meanwhile. */
void thread_wait_end(xthread *t)
{
    if (!t) return;
    __atomic_store_n(&t->in_wait, 0, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&t->ethread.Tcb.SuspendCount, __ATOMIC_SEQ_CST) == 0) return;
    pthread_mutex_lock(&start_lock);
    int gen = t->resume_gen;
    while (t->ethread.Tcb.SuspendCount > 0 && t->resume_gen == gen)
        pthread_cond_wait(&start_cond, &start_lock);
    pthread_mutex_unlock(&start_lock);
}

void thread_exit(NTSTATUS status)
{
    xthread *t = thread_current();
    t->ethread.ExitStatus = status;
    longjmp(t->exit_jmp, 1);
}
