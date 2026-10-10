/*
 * Ob/Ps/Nt object services: the handle table, named objects, symbolic links,
 * events/semaphores/mutants/timers by handle, and system threads.
 *
 * Reference counting is simplified: objects are released when their last
 * handle closes, and ObfDereferenceObject is a no-op.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../cpu.h"
#include "../xbcompat.h"

#define MAX_HANDLES 4096
#define NtCurrentThread ((HANDLE)-2)

static pthread_mutex_t ob_lock = PTHREAD_MUTEX_INITIALIZER;
static xobject *handles[MAX_HANDLES];

/* Object type "objects": the guest only compares their addresses. */
typedef struct { ULONG dummy[8]; } OBJECT_TYPE;
OBJECT_TYPE ExEventObjectType, ExMutantObjectType, ExSemaphoreObjectType, ExTimerObjectType,
            PsThreadObjectType, IoFileObjectType, IoDeviceObjectType, IoCompletionObjectType,
            ObDirectoryObjectType, ObSymbolicLinkObjectType;

xobject *object_new(enum obj_kind kind)
{
    xobject *o = pool_alloc(sizeof(*o));
    o->kind = kind;
    o->refs = 1;
    return o;
}

void object_release(xobject *o)
{
    if (__sync_sub_and_fetch(&o->refs, 1) > 0)
        return;
    if (o->kind == OBJ_FILE) {
        extern void file_close(struct xfile *f);
        file_close(o->file);
    }
    free(o->name);
    free(o->link_target);
    /* Dispatcher bodies may still be waited on by a racing thread; keep the
       memory (it is small) rather than risk a use after free. */
}

HANDLE handle_insert(xobject *obj)
{
    pthread_mutex_lock(&ob_lock);
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (!handles[i]) {
            handles[i] = obj;
            pthread_mutex_unlock(&ob_lock);
            return (HANDLE)(ULONG_PTR)(i * 4);
        }
    }
    pthread_mutex_unlock(&ob_lock);
    fatal("handle table full");
}

xobject *handle_lookup(HANDLE h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v % 4 || v / 4 >= MAX_HANDLES) return NULL;
    return handles[v / 4];
}

NTSTATUS handle_close(HANDLE h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v % 4 || v / 4 >= MAX_HANDLES) return STATUS_INVALID_HANDLE;
    pthread_mutex_lock(&ob_lock);
    xobject *o = handles[v / 4];
    handles[v / 4] = NULL;
    pthread_mutex_unlock(&ob_lock);
    if (!o) return STATUS_INVALID_HANDLE;
    object_release(o);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtClose(HANDLE Handle)
{
    TRACE("NtClose(%p)", Handle);
    if (Handle == NtCurrentThread) return STATUS_SUCCESS;
    return handle_close(Handle);
}

static char *object_name(OBJECT_ATTRIBUTES *oa)
{
    if (!oa || !oa->ObjectName || !oa->ObjectName->Length) return NULL;
    return strndup(oa->ObjectName->Buffer, oa->ObjectName->Length);
}

/* Named objects are looked up by walking the handle table; titles create a
   handful at most. */
static xobject *find_named(const char *name, enum obj_kind kind)
{
    xobject *found = NULL;
    pthread_mutex_lock(&ob_lock);
    for (int i = 1; i < MAX_HANDLES && !found; i++)
        if (handles[i] && handles[i]->kind == kind && handles[i]->name &&
            !strcasecmp(handles[i]->name, name))
            found = handles[i];
    if (found) found->refs++;
    pthread_mutex_unlock(&ob_lock);
    return found;
}

/* Guest pointer to the waitable/dispatcher body of an object. */
static PVOID object_body(xobject *o)
{
    switch (o->kind) {
    case OBJ_THREAD: return &o->thread->ethread;
    case OBJ_FILE:
    case OBJ_DEVICE:
    case OBJ_SYMLINK: return o;
    default: return &o->u;
    }
}

static xobject *thread_object(xthread *t)
{
    xobject *o = object_new(OBJ_THREAD);
    o->thread = t;
    return o;
}

NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE Handle, OBJECT_TYPE *ObjectType, PVOID *Object)
{
    if (Handle == NtCurrentThread) {
        *Object = &thread_current()->ethread;
        return STATUS_SUCCESS;
    }
    xobject *o = handle_lookup(Handle);
    if (!o) return STATUS_INVALID_HANDLE;
    static const struct { OBJECT_TYPE *type; enum obj_kind kind; } map[] = {
        { &ExEventObjectType, OBJ_EVENT }, { &ExSemaphoreObjectType, OBJ_SEMAPHORE },
        { &ExMutantObjectType, OBJ_MUTANT }, { &ExTimerObjectType, OBJ_TIMER },
        { &PsThreadObjectType, OBJ_THREAD }, { &IoFileObjectType, OBJ_FILE },
    };
    if (ObjectType) {
        for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (map[i].type == ObjectType && map[i].kind != o->kind)
                return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *Object = object_body(o);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ObReferenceObjectByPointer(PVOID Object, OBJECT_TYPE *ObjectType)
{
    (void)Object; (void)ObjectType;
    return STATUS_SUCCESS;
}

void FASTCALL ObfReferenceObject(PVOID Object) { (void)Object; }
void FASTCALL ObfDereferenceObject(PVOID Object) { (void)Object; }
void NTAPI ObMakeTemporaryObject(PVOID Object) { (void)Object; }

NTSTATUS NTAPI ObOpenObjectByPointer(PVOID Object, OBJECT_TYPE *ObjectType, HANDLE *Handle)
{
    if (ObjectType == &PsThreadObjectType) {
        *Handle = handle_insert(thread_object((xthread *)Object));
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}

/* ---- waits by handle -------------------------------------------------- */

static NTSTATUS handle_to_waitable(HANDLE h, PVOID *out)
{
    if (h == NtCurrentThread) {
        *out = &thread_current()->ethread.Tcb.Header;
        return STATUS_SUCCESS;
    }
    xobject *o = handle_lookup(h);
    if (!o) return STATUS_INVALID_HANDLE;
    switch (o->kind) {
    case OBJ_EVENT: case OBJ_SEMAPHORE: case OBJ_MUTANT: case OBJ_TIMER:
        *out = &o->u; return STATUS_SUCCESS;
    case OBJ_THREAD:
        *out = &o->thread->ethread.Tcb.Header; return STATUS_SUCCESS;
    default:
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
}

NTSTATUS NTAPI NtWaitForSingleObjectEx(HANDLE Handle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                       LARGE_INTEGER *Timeout)
{
    PVOID obj;
    NTSTATUS st = handle_to_waitable(Handle, &obj);
    if (!NT_SUCCESS(st)) return st;
    return wait_objects(1, &obj, 1, WaitMode, Alertable, Timeout);
}

NTSTATUS NTAPI NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, LARGE_INTEGER *Timeout)
{
    return NtWaitForSingleObjectEx(Handle, 1, Alertable, Timeout);
}

NTSTATUS NTAPI NtWaitForMultipleObjectsEx(ULONG Count, HANDLE Handles[], ULONG WaitType,
                                          KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                          LARGE_INTEGER *Timeout)
{
    PVOID objs[64];
    if (Count > 64) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < Count; i++) {
        NTSTATUS st = handle_to_waitable(Handles[i], &objs[i]);
        if (!NT_SUCCESS(st)) return st;
    }
    return wait_objects(Count, objs, WaitType == 1, WaitMode, Alertable, Timeout);
}

NTSTATUS NTAPI NtSignalAndWaitForSingleObjectEx(HANDLE Signal, HANDLE Wait, KPROCESSOR_MODE Mode,
                                                BOOLEAN Alertable, LARGE_INTEGER *Timeout)
{
    extern NTSTATUS NTAPI NtSetEvent(HANDLE, LONG *);
    NtSetEvent(Signal, NULL);
    return NtWaitForSingleObjectEx(Wait, Mode, Alertable, Timeout);
}

/* ---- events, semaphores, mutants, timers by handle ------------------- */

extern void NTAPI KeInitializeEvent(KEVENT *, ULONG, BOOLEAN);
extern LONG NTAPI KeSetEvent(KEVENT *, LONG, BOOLEAN);
extern LONG NTAPI KeResetEvent(KEVENT *);
extern LONG NTAPI KePulseEvent(KEVENT *, LONG, BOOLEAN);
extern void NTAPI KeInitializeSemaphore(KSEMAPHORE *, LONG, LONG);
extern LONG NTAPI KeReleaseSemaphore(KSEMAPHORE *, LONG, LONG, BOOLEAN);
extern void NTAPI KeInitializeMutant(KMUTANT *, BOOLEAN);
extern LONG NTAPI KeReleaseMutant(KMUTANT *, LONG, BOOLEAN, BOOLEAN);
extern void NTAPI KeInitializeTimerEx(KTIMER *, ULONG);
extern BOOLEAN NTAPI KeSetTimerEx(KTIMER *, ULONG, LONG, LONG, KDPC *);
extern BOOLEAN NTAPI KeCancelTimer(KTIMER *);

static NTSTATUS create_named(HANDLE *out, OBJECT_ATTRIBUTES *oa, enum obj_kind kind, xobject **created)
{
    char *name = object_name(oa);
    *created = NULL;
    if (name) {
        xobject *o = find_named(name, kind);
        if (o) {
            free(name);
            *out = handle_insert(o);
            return STATUS_OBJECT_NAME_EXISTS;
        }
    }
    xobject *o = object_new(kind);
    o->name = name;
    *created = o;
    *out = handle_insert(o);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtCreateEvent(HANDLE *EventHandle, OBJECT_ATTRIBUTES *oa, ULONG EventType,
                             BOOLEAN InitialState)
{
    xobject *o;
    NTSTATUS st = create_named(EventHandle, oa, OBJ_EVENT, &o);
    if (o) KeInitializeEvent(&o->u.event, EventType, InitialState);
    TRACE("NtCreateEvent(type %u, state %u) = %p", EventType, InitialState, *EventHandle);
    return st;
}

NTSTATUS NTAPI NtOpenEvent(HANDLE *h, OBJECT_ATTRIBUTES *oa)
{
    char *name = object_name(oa);
    xobject *o = name ? find_named(name, OBJ_EVENT) : NULL;
    free(name);
    if (!o) return STATUS_OBJECT_NAME_NOT_FOUND;
    *h = handle_insert(o);
    return STATUS_SUCCESS;
}

static KEVENT *event_of(HANDLE h)
{
    xobject *o = handle_lookup(h);
    return o && o->kind == OBJ_EVENT ? &o->u.event : NULL;
}

NTSTATUS NTAPI NtSetEvent(HANDLE h, LONG *Prev)
{
    KEVENT *e = event_of(h);
    if (!e) return STATUS_INVALID_HANDLE;
    LONG p = KeSetEvent(e, 0, 0);
    if (Prev) *Prev = p;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtClearEvent(HANDLE h)
{
    KEVENT *e = event_of(h);
    if (!e) return STATUS_INVALID_HANDLE;
    KeResetEvent(e);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtPulseEvent(HANDLE h, LONG *Prev)
{
    KEVENT *e = event_of(h);
    if (!e) return STATUS_INVALID_HANDLE;
    LONG p = KePulseEvent(e, 0, 0);
    if (Prev) *Prev = p;
    return STATUS_SUCCESS;
}

typedef struct { ULONG EventType; LONG EventState; } EVENT_BASIC_INFORMATION;

NTSTATUS NTAPI NtQueryEvent(HANDLE h, EVENT_BASIC_INFORMATION *info)
{
    KEVENT *e = event_of(h);
    if (!e) return STATUS_INVALID_HANDLE;
    info->EventType = e->Header.Type;
    info->EventState = e->Header.SignalState;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtCreateSemaphore(HANDLE *h, OBJECT_ATTRIBUTES *oa, LONG Initial, LONG Maximum)
{
    xobject *o;
    NTSTATUS st = create_named(h, oa, OBJ_SEMAPHORE, &o);
    if (o) KeInitializeSemaphore(&o->u.semaphore, Initial, Maximum);
    return st;
}

NTSTATUS NTAPI NtReleaseSemaphore(HANDLE h, LONG Release, LONG *Prev)
{
    xobject *o = handle_lookup(h);
    if (!o || o->kind != OBJ_SEMAPHORE) return STATUS_INVALID_HANDLE;
    if (o->u.semaphore.Header.SignalState + Release > o->u.semaphore.Limit)
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    LONG p = KeReleaseSemaphore(&o->u.semaphore, 0, Release, 0);
    if (Prev) *Prev = p;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtCreateMutant(HANDLE *h, OBJECT_ATTRIBUTES *oa, BOOLEAN InitialOwner)
{
    xobject *o;
    NTSTATUS st = create_named(h, oa, OBJ_MUTANT, &o);
    if (o) KeInitializeMutant(&o->u.mutant, InitialOwner);
    return st;
}

NTSTATUS NTAPI NtReleaseMutant(HANDLE h, LONG *Prev)
{
    xobject *o = handle_lookup(h);
    if (!o || o->kind != OBJ_MUTANT) return STATUS_INVALID_HANDLE;
    if (o->u.mutant.OwnerThread != &thread_current()->ethread.Tcb)
        return STATUS_MUTANT_NOT_OWNED;
    LONG p = KeReleaseMutant(&o->u.mutant, 0, 0, 0);
    if (Prev) *Prev = p;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtCreateTimer(HANDLE *h, OBJECT_ATTRIBUTES *oa, ULONG TimerType)
{
    xobject *o;
    NTSTATUS st = create_named(h, oa, OBJ_TIMER, &o);
    if (o) KeInitializeTimerEx(&o->u.timer, TimerType);
    return st;
}

NTSTATUS NTAPI NtSetTimerEx(HANDLE h, LARGE_INTEGER *DueTime, PVOID ApcRoutine, KPROCESSOR_MODE Mode,
                            PVOID ApcContext, BOOLEAN Resume, LONG Period, BOOLEAN *PrevState)
{
    (void)Mode; (void)Resume;
    xobject *o = handle_lookup(h);
    if (!o || o->kind != OBJ_TIMER) return STATUS_INVALID_HANDLE;
    if (ApcRoutine) xlog("NtSetTimerEx: timer APCs are not delivered (%p/%p)", ApcRoutine, ApcContext);
    if (PrevState) *PrevState = (BOOLEAN)o->u.timer.Header.SignalState;
    KeSetTimerEx(&o->u.timer, DueTime->LowPart, DueTime->HighPart, Period, NULL);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtCancelTimer(HANDLE h, BOOLEAN *CurrentState)
{
    xobject *o = handle_lookup(h);
    if (!o || o->kind != OBJ_TIMER) return STATUS_INVALID_HANDLE;
    if (CurrentState) *CurrentState = (BOOLEAN)o->u.timer.Header.SignalState;
    KeCancelTimer(&o->u.timer);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtDuplicateObject(HANDLE Source, HANDLE *Target, ULONG Options)
{
    (void)Options;
    if (Source == NtCurrentThread) {
        *Target = handle_insert(thread_object(thread_current()));
        return STATUS_SUCCESS;
    }
    xobject *o = handle_lookup(Source);
    if (!o) return STATUS_INVALID_HANDLE;
    __sync_add_and_fetch(&o->refs, 1);
    *Target = handle_insert(o);
    if (Options & 1 /* DUPLICATE_CLOSE_SOURCE */) handle_close(Source);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtYieldExecution(void)
{
    xthread *xt = thread_current();
    if (xt) __atomic_store_n(&xt->in_wait, 1, __ATOMIC_SEQ_CST);
    cpu_block();
    sched_yield();
    thread_wait_end(xt);
    return STATUS_SUCCESS;
}

/* ---- system threads --------------------------------------------------- */

NTSTATUS NTAPI PsCreateSystemThreadEx(HANDLE *ThreadHandle, SIZE_T ThreadExtensionSize,
                                      SIZE_T KernelStackSize, SIZE_T TlsDataSize, HANDLE *ThreadId,
                                      PVOID StartRoutine, PVOID StartContext, BOOLEAN CreateSuspended,
                                      BOOLEAN DebuggerThread, PVOID SystemRoutine)
{
    (void)DebuggerThread;
    if (ThreadExtensionSize)
        xlog("PsCreateSystemThreadEx: thread extension of %u bytes is not provided", ThreadExtensionSize);
    xthread *t = thread_create(KernelStackSize ? KernelStackSize : 0x10000, TlsDataSize,
                               SystemRoutine, StartRoutine, StartContext, CreateSuspended);
    TRACE("PsCreateSystemThreadEx(stack %#x, tls %#x, start %p, system %p) = thread %u",
          KernelStackSize, TlsDataSize, StartRoutine, SystemRoutine,
          (unsigned)(ULONG_PTR)t->ethread.UniqueThread);
    if (ThreadHandle) *ThreadHandle = handle_insert(thread_object(t));
    if (ThreadId) *ThreadId = t->ethread.UniqueThread;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI PsCreateSystemThread(HANDLE *ThreadHandle, HANDLE *ThreadId, PVOID StartRoutine,
                                    PVOID StartContext, BOOLEAN DebuggerThread)
{
    return PsCreateSystemThreadEx(ThreadHandle, 0, 0x3000, 0, ThreadId, StartRoutine,
                                  StartContext, 0, DebuggerThread, NULL);
}

void NTAPI PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    thread_exit(ExitStatus);
}

NTSTATUS NTAPI PsQueryStatistics(ULONG *stats)
{
    stats[1] = 1;  /* ThreadCount, roughly */
    stats[2] = 16; /* HandleCount */
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI PsSetCreateThreadNotifyRoutine(PVOID Routine)
{
    (void)Routine;
    return STATUS_SUCCESS;
}

extern void thread_resume(xthread *t);

NTSTATUS NTAPI NtResumeThread(HANDLE h, ULONG *Prev)
{
    xobject *o = handle_lookup(h);
    if (!o || o->kind != OBJ_THREAD) return STATUS_INVALID_HANDLE;
    if (Prev) *Prev = o->thread->ethread.Tcb.SuspendCount;
    thread_resume(o->thread);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtSuspendThread(HANDLE h, ULONG *Prev)
{
    xobject *o = handle_lookup(h);
    if (!o || o->kind != OBJ_THREAD) return STATUS_INVALID_HANDLE;
    ULONG prev = thread_suspend(o->thread);
    TRACE("NtSuspendThread(thread %u) = previous count %u",
          (unsigned)(ULONG_PTR)o->thread->ethread.UniqueThread, prev);
    if (Prev) *Prev = prev;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtSetInformationThread(HANDLE h, ULONG Class, PVOID Info, ULONG Length)
{
    (void)h; (void)Class; (void)Info; (void)Length;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtQueueApcThread(HANDLE h, PVOID Routine, PVOID Ctx, PVOID Arg1, PVOID Arg2)
{
    xobject *o = handle_lookup(h);
    if (!o || o->kind != OBJ_THREAD) return STATUS_INVALID_HANDLE;
    apc_queue(o->thread, Routine, Ctx, Arg1, Arg2, NULL);
    return STATUS_SUCCESS;
}

/* ---- symbolic links --------------------------------------------------- */

NTSTATUS NTAPI IoCreateSymbolicLink(OBJECT_STRING *Link, OBJECT_STRING *Target)
{
    char *l = strndup(Link->Buffer, Link->Length), *t = strndup(Target->Buffer, Target->Length);
    TRACE("IoCreateSymbolicLink(%s -> %s)", l, t);
    xobject *o = find_named(l, OBJ_SYMLINK);
    if (o) {
        free(l); free(t);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    o = object_new(OBJ_SYMLINK);
    o->name = l;
    o->link_target = t;
    handle_insert(o);   /* the kernel keeps it alive until IoDeleteSymbolicLink */
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI IoDeleteSymbolicLink(OBJECT_STRING *Link)
{
    char *l = strndup(Link->Buffer, Link->Length);
    NTSTATUS st = STATUS_OBJECT_NAME_NOT_FOUND;
    pthread_mutex_lock(&ob_lock);
    for (int i = 1; i < MAX_HANDLES; i++) {
        xobject *o = handles[i];
        if (o && o->kind == OBJ_SYMLINK && o->name && !strcasecmp(o->name, l)) {
            handles[i] = NULL;
            st = STATUS_SUCCESS;
            break;
        }
    }
    pthread_mutex_unlock(&ob_lock);
    free(l);
    return st;
}

/* Used by the file system to resolve \??\D: style prefixes. */
char *symlink_lookup(const char *name)
{
    xobject *o = find_named(name, OBJ_SYMLINK);
    if (!o) return NULL;
    char *r = strdup(o->link_target);
    __sync_sub_and_fetch(&o->refs, 1);
    return r;
}

NTSTATUS NTAPI NtOpenSymbolicLinkObject(HANDLE *LinkHandle, OBJECT_ATTRIBUTES *oa)
{
    char *name = object_name(oa);
    xobject *o = name ? find_named(name, OBJ_SYMLINK) : NULL;
    TRACE("NtOpenSymbolicLinkObject(%s) = %s", name ? name : "?", o ? "found" : "not found");
    free(name);
    if (!o) return STATUS_OBJECT_NAME_NOT_FOUND;
    *LinkHandle = handle_insert(o);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtQuerySymbolicLinkObject(HANDLE LinkHandle, OBJECT_STRING *Target, ULONG *ReturnedLength)
{
    xobject *o = handle_lookup(LinkHandle);
    if (!o || o->kind != OBJ_SYMLINK) return STATUS_INVALID_HANDLE;
    size_t n = strlen(o->link_target);
    if (ReturnedLength) *ReturnedLength = n;
    if (Target->MaximumLength < n) return STATUS_BUFFER_TOO_SMALL;
    memcpy(Target->Buffer, o->link_target, n);
    Target->Length = n;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ObOpenObjectByName(OBJECT_ATTRIBUTES *oa, OBJECT_TYPE *Type, PVOID ParseContext,
                                  HANDLE *Handle)
{
    (void)ParseContext;
    if (Type == &ObSymbolicLinkObjectType)
        return NtOpenSymbolicLinkObject(Handle, oa);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
