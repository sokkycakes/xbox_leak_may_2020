/*
 * Guest threads.  Each Xbox thread is a host pthread whose FS segment points
 * at its own KPCR, exactly where Xbox code expects to find it (fs:[0]).  The
 * host C library uses GS for its own TLS on i386, so FS is ours.
 */
#define _GNU_SOURCE
#if defined(__i386__)
#include <asm/ldt.h>
#endif
#include <errno.h>
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
    disp_signal(&t->ethread.Tcb.Header);
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
    if (t->ethread.Tcb.SuspendCount > 0)
        t->ethread.Tcb.SuspendCount--;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_lock);
}

void thread_exit(NTSTATUS status)
{
    xthread *t = thread_current();
    t->ethread.ExitStatus = status;
    longjmp(t->exit_jmp, 1);
}
