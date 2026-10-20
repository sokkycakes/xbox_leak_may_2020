/*
 * Ob/Ps/Nt object services: the handle table, named objects, symbolic links,
 * events/semaphores/mutants/timers by handle, and system threads.
 *
 * Reference counting is simplified for the objects the Nt services create
 * (events, files, ...): they are released when their last handle closes,
 * and ObfDereferenceObject leaves them alone.  Objects made the kernel's
 * way, by ObCreateObject (device and directory objects, and any a title's
 * driver code makes), have a real OBJECT_HEADER and are counted as in
 * private/ntos/obx; named ones live in a namespace of directory objects.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../cpu.h"
#include "../xbcompat.h"
#include "driver.h"

#define MAX_HANDLES 4096
#define NtCurrentThread ((HANDLE)-2)

static pthread_mutex_t ob_lock = PTHREAD_MUTEX_INITIALIZER;
static xobject *handles[MAX_HANDLES];

/* The kernel's object types.  Guest code mostly compares their addresses;
   the pool tag is what NtQueryDirectoryObject reports.  Their procedures
   are left empty: objects of these types live in the pool. */
#define TAG(a, b, c, d) ((ULONG)(a) | (ULONG)(b) << 8 | (ULONG)(c) << 16 | (ULONG)(d) << 24)
OBJECT_TYPE ExEventObjectType = { .PoolTag = TAG('E', 'v', 'e', 'v') },
            ExMutantObjectType = { .PoolTag = TAG('M', 'u', 't', 'a') },
            ExSemaphoreObjectType = { .PoolTag = TAG('S', 'e', 'm', 'a') },
            ExTimerObjectType = { .PoolTag = TAG('T', 'i', 'm', 'e') },
            PsThreadObjectType = { .PoolTag = TAG('T', 'h', 'r', 'e') },
            IoFileObjectType = { .PoolTag = TAG('F', 'i', 'l', 'e') },
            IoDeviceObjectType = { .PoolTag = TAG('D', 'e', 'v', 'i') },
            IoCompletionObjectType = { .PoolTag = TAG('C', 'o', 'm', 'p') },
            ObDirectoryObjectType = { .PoolTag = TAG('D', 'i', 'r', 'e') },
            ObSymbolicLinkObjectType = { .PoolTag = TAG('S', 'y', 'm', 'b') };

static bool kernel_type(const OBJECT_TYPE *t)
{
    return t == &ExEventObjectType || t == &ExMutantObjectType || t == &ExSemaphoreObjectType ||
           t == &ExTimerObjectType || t == &PsThreadObjectType || t == &IoFileObjectType ||
           t == &IoDeviceObjectType || t == &IoCompletionObjectType || t == &ObDirectoryObjectType ||
           t == &ObSymbolicLinkObjectType;
}

/* The type of a handle-table object the Nt services made. */
static OBJECT_TYPE *kind_type(enum obj_kind kind)
{
    switch (kind) {
    case OBJ_EVENT: return &ExEventObjectType;
    case OBJ_SEMAPHORE: return &ExSemaphoreObjectType;
    case OBJ_MUTANT: return &ExMutantObjectType;
    case OBJ_TIMER: return &ExTimerObjectType;
    case OBJ_THREAD: return &PsThreadObjectType;
    case OBJ_FILE: return &IoFileObjectType;
    case OBJ_IOCOMPLETION: return &IoCompletionObjectType;
    case OBJ_SYMLINK: return &ObSymbolicLinkObjectType;
    default: return NULL;
    }
}

/* ObpObjectHandleTable (data export): a view of the handle table in the
   kernel's layout, tables of 64 object bodies (obx/obtable.c), for
   debugger code that walks it.  Free slots read as NULL. */
typedef struct {
    LONG HandleCount;
    LONG_PTR FirstFreeTableEntry;
    HANDLE NextHandleNeedingPool;
    PVOID **RootTable;
    PVOID *BuiltinRootTable[8];
} OBJECT_HANDLE_TABLE;

static PVOID handle_view[MAX_HANDLES];
static PVOID *handle_view_root[MAX_HANDLES / 64];
OBJECT_HANDLE_TABLE ObpObjectHandleTable = {
    .FirstFreeTableEntry = -1,
    .NextHandleNeedingPool = (HANDLE)(MAX_HANDLES * 4),
    .RootTable = handle_view_root,
};
static PVOID object_body(xobject *o);

static void handle_view_set(int i, xobject *o)
{
    if (!handle_view_root[0]) {
        for (int t = 0; t < MAX_HANDLES / 64; t++) handle_view_root[t] = &handle_view[t * 64];
        for (int t = 0; t < 8; t++) ObpObjectHandleTable.BuiltinRootTable[t] = handle_view_root[t];
    }
    handle_view[i] = o ? object_body(o) : NULL;
    ObpObjectHandleTable.HandleCount += o ? 1 : -1;
}

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
            handle_view_set(i, obj);
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
    if (o) handle_view_set(v / 4, NULL);
    pthread_mutex_unlock(&ob_lock);
    if (!o) return STATUS_INVALID_HANDLE;
    if (o->kind == OBJ_OB) {
        extern void ob_handle_closed(xobject *o);
        ob_handle_closed(o);
        return STATUS_SUCCESS;
    }
    object_release(o);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtClose(HANDLE Handle)
{
    gl_yield_point();
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
    case OBJ_OB: return o->body;
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
    if (o->kind == OBJ_OB) {
        OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(o->body);
        if (ObjectType && h->Type != ObjectType) return STATUS_OBJECT_TYPE_MISMATCH;
        __sync_add_and_fetch(&h->PointerCount, 1);
        *Object = o->body;
        return STATUS_SUCCESS;
    }
    /* A type no Nt-made object has (a device, a directory, a title's own)
       does not match; the kernel's other types match their kind.  Unknown
       kinds (OBJ_DEVICE) are let through as before. */
    OBJECT_TYPE *have = kind_type(o->kind);
    if (ObjectType && have && ObjectType != have) return STATUS_OBJECT_TYPE_MISMATCH;
    *Object = object_body(o);
    return STATUS_SUCCESS;
}

static HANDLE ob_handle(xobject *o);

NTSTATUS NTAPI ObOpenObjectByPointer(PVOID Object, OBJECT_TYPE *ObjectType, HANDLE *Handle)
{
    if (ObjectType == &PsThreadObjectType) {
        *Handle = handle_insert(thread_object((xthread *)Object));
        return STATUS_SUCCESS;
    }
    xobject *o = ob_object_of(Object);
    if (o) {
        OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(Object);
        if (ObjectType && h->Type != ObjectType) return STATUS_OBJECT_TYPE_MISMATCH;
        __sync_add_and_fetch(&h->PointerCount, 1);
        *Handle = ob_handle(o);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_IMPLEMENTED;
}

/* ---- waits by handle -------------------------------------------------- */

/* What a wait on a file handle waits for: a notification event that stays
   signaled (see handle_to_waitable). */
static KEVENT file_done;
static pthread_once_t file_done_once = PTHREAD_ONCE_INIT;
void NTAPI KeInitializeEvent(KEVENT *Event, ULONG Type, BOOLEAN State);
static void file_done_init(void) { KeInitializeEvent(&file_done, 0 /* NotificationEvent */, 1); }

static NTSTATUS handle_to_waitable(HANDLE h, PVOID *out)
{
    if (h == NtCurrentThread) {
        *out = &thread_current()->ethread.Tcb.Header;
        return STATUS_SUCCESS;
    }
    xobject *o = handle_lookup(h);
    if (!o) return STATUS_INVALID_HANDLE;
    switch (o->kind) {
    case OBJ_EVENT: case OBJ_SEMAPHORE: case OBJ_MUTANT: case OBJ_TIMER: case OBJ_IOCOMPLETION:
        *out = &o->u; return STATUS_SUCCESS;
    case OBJ_THREAD:
        *out = &o->thread->ethread.Tcb.Header; return STATUS_SUCCESS;
    case OBJ_FILE: {
        /* A file object is signaled when its I/O completes, and I/O here
           completes before the call that started it returns: XAPI's
           ReadFile on a handle opened for overlapped I/O, and
           GetOverlappedResult without an event, wait on the handle. */
        pthread_once(&file_done_once, file_done_init);
        *out = &file_done;
        return STATUS_SUCCESS;
    }
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

/* ---- queries (ex/mutant.c, ex/semphore.c, ex/timer.c) ---------------- */

typedef struct { LONG CurrentCount; BOOLEAN OwnedByCaller; BOOLEAN AbandonedState; } MUTANT_BASIC_INFORMATION;
typedef struct { LONG CurrentCount; LONG MaximumCount; } SEMAPHORE_BASIC_INFORMATION;
typedef struct { LARGE_INTEGER RemainingTime; BOOLEAN TimerState; } TIMER_BASIC_INFORMATION;

extern LONGLONG ke_timer_remaining(KTIMER *t);

NTSTATUS NTAPI NtQueryMutant(HANDLE h, MUTANT_BASIC_INFORMATION *info)
{
    xobject *o = handle_lookup(h);
    if (!o) return STATUS_INVALID_HANDLE;
    if (o->kind != OBJ_MUTANT) return STATUS_OBJECT_TYPE_MISMATCH;
    xthread *t = thread_current();
    info->CurrentCount = o->u.mutant.Header.SignalState;
    info->OwnedByCaller = t && o->u.mutant.OwnerThread == &t->ethread.Tcb;
    info->AbandonedState = o->u.mutant.Abandoned;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtQuerySemaphore(HANDLE h, SEMAPHORE_BASIC_INFORMATION *info)
{
    xobject *o = handle_lookup(h);
    if (!o) return STATUS_INVALID_HANDLE;
    if (o->kind != OBJ_SEMAPHORE) return STATUS_OBJECT_TYPE_MISMATCH;
    info->CurrentCount = o->u.semaphore.Header.SignalState;
    info->MaximumCount = o->u.semaphore.Limit;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtQueryTimer(HANDLE h, TIMER_BASIC_INFORMATION *info)
{
    xobject *o = handle_lookup(h);
    if (!o) return STATUS_INVALID_HANDLE;
    if (o->kind != OBJ_TIMER) return STATUS_OBJECT_TYPE_MISMATCH;
    info->TimerState = o->u.timer.Header.SignalState != 0;
    info->RemainingTime.QuadPart = ke_timer_remaining(&o->u.timer);
    return STATUS_SUCCESS;
}

/* ---- I/O completion ports (io/complete.c) ----------------------------
   A port is a KQUEUE of packets.  The packets are host allocations; only
   NtRemoveIoCompletion takes them apart. */

extern void NTAPI KeInitializeQueue(KQUEUE *, ULONG);
extern LONG NTAPI KeInsertQueue(KQUEUE *, LIST_ENTRY *);
extern LIST_ENTRY *NTAPI KeRemoveQueue(KQUEUE *, KPROCESSOR_MODE, LARGE_INTEGER *);

typedef struct {
    LIST_ENTRY ListEntry;
    PVOID KeyContext;
    PVOID ApcContext;
    NTSTATUS IoStatus;
    ULONG_PTR IoStatusInformation;
} iocp_packet;

NTSTATUS iocp_post(xobject *port, PVOID key, PVOID apc_context, NTSTATUS status, ULONG_PTR info)
{
    iocp_packet *p = malloc(sizeof(*p));
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    p->KeyContext = key;
    p->ApcContext = apc_context;
    p->IoStatus = status;
    p->IoStatusInformation = info;
    KeInsertQueue(&port->u.queue, &p->ListEntry);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtCreateIoCompletion(HANDLE *h, ULONG DesiredAccess, OBJECT_ATTRIBUTES *oa, ULONG Count)
{
    (void)DesiredAccess;
    xobject *o;
    NTSTATUS st = create_named(h, oa, OBJ_IOCOMPLETION, &o);
    if (o) KeInitializeQueue(&o->u.queue, Count);
    TRACE("NtCreateIoCompletion(count %u) = %p", (unsigned)Count, *h);
    return st;
}

static xobject *port_of(HANDLE h, NTSTATUS *st)
{
    xobject *o = handle_lookup(h);
    *st = !o ? STATUS_INVALID_HANDLE : o->kind != OBJ_IOCOMPLETION ? STATUS_OBJECT_TYPE_MISMATCH : STATUS_SUCCESS;
    return *st == STATUS_SUCCESS ? o : NULL;
}

NTSTATUS NTAPI IoSetIoCompletion(PVOID IoCompletion, PVOID KeyContext, PVOID ApcContext,
                                 NTSTATUS IoStatus, ULONG_PTR IoStatusInformation)
{
    /* IoCompletion is the port's body, as ObReferenceObjectByHandle gave it. */
    xobject *o = (xobject *)((char *)IoCompletion - offsetof(xobject, u));
    return iocp_post(o, KeyContext, ApcContext, IoStatus, IoStatusInformation);
}

NTSTATUS NTAPI NtSetIoCompletion(HANDLE h, PVOID KeyContext, PVOID ApcContext, NTSTATUS IoStatus,
                                 ULONG_PTR IoStatusInformation)
{
    NTSTATUS st;
    xobject *o = port_of(h, &st);
    if (!o) return st;
    return iocp_post(o, KeyContext, ApcContext, IoStatus, IoStatusInformation);
}

NTSTATUS NTAPI NtRemoveIoCompletion(HANDLE h, PVOID *KeyContext, PVOID *ApcContext,
                                    IO_STATUS_BLOCK *iosb, LARGE_INTEGER *Timeout)
{
    NTSTATUS st;
    xobject *o = port_of(h, &st);
    if (!o) return st;
    LIST_ENTRY *e = KeRemoveQueue(&o->u.queue, 0 /* KernelMode */, Timeout);
    if ((ULONG_PTR)e == STATUS_TIMEOUT || (ULONG_PTR)e == STATUS_USER_APC)
        return (NTSTATUS)(ULONG_PTR)e;
    iocp_packet *p = (iocp_packet *)e;
    *KeyContext = p->KeyContext;
    *ApcContext = p->ApcContext;
    iosb->Information = p->IoStatusInformation;
    iosb->Status = p->IoStatus;
    free(p);
    return STATUS_SUCCESS;
}

typedef struct { LONG Depth; } IO_COMPLETION_BASIC_INFORMATION;

NTSTATUS NTAPI NtQueryIoCompletion(HANDLE h, IO_COMPLETION_BASIC_INFORMATION *info)
{
    NTSTATUS st;
    xobject *o = port_of(h, &st);
    if (!o) return st;
    info->Depth = o->u.queue.Header.SignalState;
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
    gl_yield_point();
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

/* ---- objects with headers (obx/obcreate.c, obref.c, obdir.c) ---------
   ObCreateObject lays an object out as the kernel does, [name info]
   header body [name], so guest code that steps from a body to its header
   finds one.  Each also has an xobject (kind OBJ_OB) for the handle table,
   on the ob_objects list.  Allocation and the type's close and delete
   procedures go to the guest for a title's own object types; the kernel's
   types use the pool.

   The namespace holds the OBJ_OB objects inserted with a name: each keeps
   its full path in xobject.name, and is linked into its directory's hash
   buckets like the kernel's.  The symbolic links and named events and such
   the Nt services make stay in the handle table, where name lookups (and
   directory listings) also find them. */

static xobject *ob_objects;

xobject *ob_object_of(const void *body)
{
    if (!body) return NULL;
    pthread_mutex_lock(&ob_lock);
    xobject *o = ob_objects;
    while (o && o->body != body) o = o->ob_next;
    pthread_mutex_unlock(&ob_lock);
    return o;
}

#ifdef XBC_TRANSLATED
#define GUEST_CALL1(fn, a) CPU_CALL(fn, CONV_STD, (uint32_t)(a))
#define GUEST_CALL2(fn, a, b) CPU_CALL(fn, CONV_STD, (uint32_t)(a), (uint32_t)(b))
#else
#define GUEST_CALL1(fn, a) ((ULONG (__attribute__((stdcall)) *)(ULONG))(fn))((ULONG)(a))
#define GUEST_CALL2(fn, a, b) ((ULONG (__attribute__((stdcall)) *)(ULONG, ULONG))(fn))((ULONG)(a), (ULONG)(b))
#endif

static void ob_delete(xobject *o)
{
    OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(o->body);
    OBJECT_TYPE *t = h->Type;
    pthread_mutex_lock(&ob_lock);
    for (xobject **pp = &ob_objects; *pp; pp = &(*pp)->ob_next)
        if (*pp == o) { *pp = o->ob_next; break; }
    pthread_mutex_unlock(&ob_lock);
    TRACE("ObDeleteObject(%p)", o->body);
    if (!kernel_type(t) && t->DeleteProcedure) GUEST_CALL1(t->DeleteProcedure, o->body);
    if (!kernel_type(t) && t->FreeProcedure) GUEST_CALL1(t->FreeProcedure, o->ob_base);
    else pool_free(o->ob_base);
    /* As with the other objects, the xobject itself is kept: a stale
       handle-table lookup then sees an empty kind rather than freed memory. */
    o->kind = OBJ_NONE;
    o->body = o->ob_base = NULL;
    free(o->name);
    o->name = NULL;
}

void FASTCALL ObfReferenceObject(PVOID Object)
{
    if (ob_object_of(Object)) __sync_add_and_fetch(&OBJECT_TO_OBJECT_HEADER(Object)->PointerCount, 1);
}

void FASTCALL ObfDereferenceObject(PVOID Object)
{
    xobject *o = ob_object_of(Object);
    if (o && __sync_sub_and_fetch(&OBJECT_TO_OBJECT_HEADER(Object)->PointerCount, 1) == 0)
        ob_delete(o);
}

NTSTATUS NTAPI ObReferenceObjectByPointer(PVOID Object, OBJECT_TYPE *ObjectType)
{
    if (ob_object_of(Object)) {
        OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(Object);
        if (ObjectType && h->Type != ObjectType) return STATUS_OBJECT_TYPE_MISMATCH;
        __sync_add_and_fetch(&h->PointerCount, 1);
    }
    return STATUS_SUCCESS;
}

/* ObpComputeHashIndex */
static ULONG ob_hash(const char *name, size_t len)
{
    ULONG h = 0;
    for (size_t i = 0; i < len; i++) {
        UCHAR c = (UCHAR)name[i];
        if (c >= 0x80) continue;
        c |= 0x20;
        h += (h << 1) + (h >> 1) + c;
    }
    return h % OB_NUMBER_HASH_BUCKETS;
}

static OBJECT_HEADER_NAME_INFO *name_info(PVOID body)
{
    return (OBJECT_HEADER_NAME_INFO *)OBJECT_TO_OBJECT_HEADER(body) - 1;
}

/* The inserted object at a full path; under ob_lock. */
static xobject *ns_find(const char *path)
{
    for (xobject *o = ob_objects; o; o = o->ob_next)
        if (o->name && !strcasecmp(o->name, path)) return o;
    return NULL;
}

/* Unlink a named object from its directory (ObpDetachNamedObject); drops
   the reference the name held.  Call without ob_lock. */
static void ns_detach(xobject *o)
{
    OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(o->body);
    PVOID dir = NULL;
    pthread_mutex_lock(&ob_lock);
    if (h->Flags & OB_FLAG_ATTACHED_OBJECT) {
        OBJECT_HEADER_NAME_INFO *ni = name_info(o->body);
        dir = ni->Directory;
        if (dir) {
            OBJECT_HEADER_NAME_INFO **pp = &((OBJECT_DIRECTORY *)dir)->HashBuckets[ob_hash(ni->Name.Buffer, ni->Name.Length)];
            while (*pp && *pp != ni) pp = &(*pp)->ChainLink;
            if (*pp) *pp = ni->ChainLink;
        }
        ni->Directory = NULL;
        ni->ChainLink = NULL;
        h->Flags &= ~OB_FLAG_ATTACHED_OBJECT;
        free(o->name);
        o->name = NULL;
    } else {
        h = NULL;
    }
    pthread_mutex_unlock(&ob_lock);
    if (!h) return;
    if (dir) ObfDereferenceObject(dir);
    ObfDereferenceObject(o->body);
}

void NTAPI ObMakeTemporaryObject(PVOID Object)
{
    xobject *o = ob_object_of(Object);
    if (!o) return;
    OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(Object);
    h->Flags &= ~OB_FLAG_PERMANENT_OBJECT;
    if (h->HandleCount == 0) ns_detach(o);
}

/* A handle to an OBJ_OB object closed (ObpDecrementHandleCount, then the
   handle's pointer reference). */
void ob_handle_closed(xobject *o)
{
    PVOID body = o->body;
    if (!body) return;
    OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(body);
    LONG count = h->HandleCount;
    __sync_sub_and_fetch(&h->HandleCount, 1);
    if (!kernel_type(h->Type) && h->Type->CloseProcedure) GUEST_CALL2(h->Type->CloseProcedure, body, count);
    if (h->HandleCount == 0 && !(h->Flags & OB_FLAG_PERMANENT_OBJECT)) ns_detach(o);
    ObfDereferenceObject(body);
}

static HANDLE ob_handle(xobject *o)
{
    HANDLE hd = handle_insert(o);
    __sync_add_and_fetch(&OBJECT_TO_OBJECT_HEADER(o->body)->HandleCount, 1);
    return hd;
}

NTSTATUS NTAPI ObCreateObject(OBJECT_TYPE *Type, OBJECT_ATTRIBUTES *oa, ULONG BodySize, PVOID *Object)
{
    *Object = NULL;
    /* The last element of the name (ObDissectName); a name that ends in a
       separator, or is just one, has none. */
    const char *elem = NULL;
    size_t elen = 0;
    if (oa && oa->ObjectName) {
        const char *n = oa->ObjectName->Buffer;
        size_t len = oa->ObjectName->Length;
        if (!len || n[len - 1] == '\\') return STATUS_OBJECT_NAME_INVALID;
        size_t i = len;
        while (i > 0 && n[i - 1] != '\\') i--;
        elem = n + i;
        elen = len - i;
        for (size_t j = 1; j < i; j++)
            if (n[j] == '\\' && n[j - 1] == '\\') return STATUS_OBJECT_NAME_INVALID;
    }
    size_t body = (BodySize + 3) & ~3u;
    size_t size = (elem ? sizeof(OBJECT_HEADER_NAME_INFO) : 0) + sizeof(OBJECT_HEADER) + body + elen;
    void *base = !kernel_type(Type) && Type->AllocateProcedure
               ? (void *)(uintptr_t)GUEST_CALL2(Type->AllocateProcedure, size, Type->PoolTag)
               : pool_alloc(size);
    if (!base) return STATUS_INSUFFICIENT_RESOURCES;
    OBJECT_HEADER *h = (OBJECT_HEADER *)((char *)base + (elem ? sizeof(OBJECT_HEADER_NAME_INFO) : 0));
    PVOID b = h + 1;
    if (elem) {
        OBJECT_HEADER_NAME_INFO *ni = base;
        ni->ChainLink = NULL;
        ni->Directory = NULL;
        ni->Name.Buffer = (char *)b + body;
        ni->Name.Length = ni->Name.MaximumLength = (USHORT)elen;
        memcpy(ni->Name.Buffer, elem, elen);
    }
    h->PointerCount = 1;
    h->HandleCount = 0;
    h->Type = Type;
    h->Flags = elem ? OB_FLAG_NAMED_OBJECT : 0;

    xobject *o = object_new(OBJ_OB);
    o->body = b;
    o->ob_base = base;
    pthread_mutex_lock(&ob_lock);
    o->ob_next = ob_objects;
    ob_objects = o;
    pthread_mutex_unlock(&ob_lock);
    *Object = b;
    TRACE("ObCreateObject(type %p, %u bytes, %.*s) = %p", (void *)Type, (unsigned)BodySize, (int)elen,
          elem ? elem : "", b);
    return STATUS_SUCCESS;
}

/* The full path an OBJECT_ATTRIBUTES names, from the root or from its
   root directory handle (or the \?? and \Win32NamedObjects handles). */
static char *ob_full_path(OBJECT_ATTRIBUTES *oa, NTSTATUS *st)
{
    const char *n = oa && oa->ObjectName ? oa->ObjectName->Buffer : "";
    size_t len = oa && oa->ObjectName ? oa->ObjectName->Length : 0;
    char *root = NULL, *r = NULL;
    *st = STATUS_SUCCESS;
    if (oa && oa->RootDirectory) {
        if (oa->RootDirectory == (HANDLE)-3) root = strdup("\\??");
        else if (oa->RootDirectory == (HANDLE)-4) root = strdup("\\Win32NamedObjects");
        else {
            xobject *d = handle_lookup(oa->RootDirectory);
            if (!d) { *st = STATUS_INVALID_HANDLE; return NULL; }
            if (d->kind != OBJ_OB || OBJECT_TO_OBJECT_HEADER(d->body)->Type != &ObDirectoryObjectType || !d->name) {
                *st = STATUS_OBJECT_TYPE_MISMATCH;
                return NULL;
            }
            root = strdup(d->name);
        }
        if (len && n[0] == '\\') { free(root); *st = STATUS_OBJECT_NAME_INVALID; return NULL; }
        bool at_root = !strcmp(root, "\\");
        if (asprintf(&r, "%s%s%.*s", root, at_root || !len ? "" : "\\", (int)len, n) < 0) r = NULL;
        free(root);
    } else {
        if (!len || n[0] != '\\') { *st = STATUS_OBJECT_NAME_INVALID; return NULL; }
        r = strndup(n, len);
    }
    size_t l = r ? strlen(r) : 0;
    while (l > 1 && r[l - 1] == '\\') r[--l] = 0;
    return r;
}

static char *parent_path(const char *path)
{
    const char *s = strrchr(path, '\\');
    return s == path ? strdup("\\") : strndup(path, s - path);
}

static void ns_init(void);

NTSTATUS NTAPI ObInsertObject(PVOID Object, OBJECT_ATTRIBUTES *oa, ULONG PointerBias, HANDLE *Handle)
{
    xobject *o = ob_object_of(Object), *insert = o;
    NTSTATUS st = STATUS_SUCCESS;
    HANDLE hd = NULL;
    if (!o) { *Handle = NULL; return STATUS_INVALID_PARAMETER; }
    OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(Object);
    if (oa && oa->ObjectName && (h->Flags & OB_FLAG_NAMED_OBJECT)) {
        ns_init();
        char *path = ob_full_path(oa, &st);
        if (!path) goto out;
        char *parent = parent_path(path);
        pthread_mutex_lock(&ob_lock);
        xobject *found = ns_find(path), *dir = ns_find(parent);
        free(parent);
        if (found) {
            if (!(oa->Attributes & OBJ_OPENIF)) st = STATUS_OBJECT_NAME_COLLISION;
            else if (OBJECT_TO_OBJECT_HEADER(found->body)->Type != h->Type) st = STATUS_OBJECT_TYPE_MISMATCH;
            else { insert = found; st = STATUS_OBJECT_NAME_EXISTS; }
        } else if (!dir || OBJECT_TO_OBJECT_HEADER(dir->body)->Type != &ObDirectoryObjectType) {
            st = STATUS_OBJECT_PATH_NOT_FOUND;
        } else {
            OBJECT_HEADER_NAME_INFO *ni = name_info(Object);
            OBJECT_DIRECTORY *d = dir->body;
            ULONG i = ob_hash(ni->Name.Buffer, ni->Name.Length);
            ni->Directory = d;
            ni->ChainLink = d->HashBuckets[i];
            d->HashBuckets[i] = ni;
            h->Flags |= OB_FLAG_ATTACHED_OBJECT;
            o->name = path;
            path = NULL;
            __sync_add_and_fetch(&OBJECT_TO_OBJECT_HEADER(d)->PointerCount, 1);
            __sync_add_and_fetch(&h->PointerCount, 1);
        }
        if (insert != o && NT_SUCCESS(st))
            __sync_add_and_fetch(&OBJECT_TO_OBJECT_HEADER(insert->body)->PointerCount, 1);
        pthread_mutex_unlock(&ob_lock);
        free(path);
        if (NT_ERROR(st)) goto out;
        if (insert != o) ObfDereferenceObject(insert->body);   /* the handle below takes it again */
    }
    hd = ob_handle(insert);
    __sync_add_and_fetch(&OBJECT_TO_OBJECT_HEADER(insert->body)->PointerCount, PointerBias + 1);
    if (oa && (oa->Attributes & OBJ_PERMANENT)) OBJECT_TO_OBJECT_HEADER(insert->body)->Flags |= OB_FLAG_PERMANENT_OBJECT;
out:
    TRACE("ObInsertObject(%p, %s) = %#x, handle %p", Object, o->name ? o->name : "unnamed", (unsigned)st, hd);
    ObfDereferenceObject(Object);
    *Handle = hd;
    return st;
}

/* A handle-table object (made by the Nt services) by its name. */
static xobject *find_named_any(const char *name)
{
    xobject *found = NULL;
    pthread_mutex_lock(&ob_lock);
    for (int i = 1; i < MAX_HANDLES && !found; i++)
        if (handles[i] && handles[i]->kind != OBJ_OB && handles[i]->name && !strcasecmp(handles[i]->name, name))
            found = handles[i];
    if (found) found->refs++;
    pthread_mutex_unlock(&ob_lock);
    return found;
}

/* Follow the symbolic links a path goes through, the last element too
   unless final is false.  Returns a new string. */
static char *resolve_links(const char *path, bool final)
{
    char *p = strdup(path);
    for (int depth = 0; depth < 8; depth++) {
        bool changed = false;
        for (size_t j = 1; !changed; j++) {
            if (p[j] && p[j] != '\\') continue;
            if (!p[j] && !final) break;
            char *prefix = strndup(p, j);
            char *target = symlink_lookup(prefix);
            free(prefix);
            if (target) {
                size_t tl = strlen(target);
                while (tl > 1 && target[tl - 1] == '\\') target[--tl] = 0;
                char *np;
                if (asprintf(&np, "%s%s", target, p + j) < 0) np = NULL;
                free(target);
                if (!np) break;
                free(p);
                p = np;
                changed = true;
                break;
            }
            if (!p[j]) break;
        }
        if (!changed) break;
    }
    return p;
}

NTSTATUS ob_reference_by_path(const char *path, OBJECT_TYPE *type, PVOID *body)
{
    ns_init();
    *body = NULL;
    char *r = resolve_links(path, type != &ObSymbolicLinkObjectType);
    NTSTATUS st;
    pthread_mutex_lock(&ob_lock);
    xobject *o = ns_find(r);
    if (o) {
        OBJECT_HEADER *h = OBJECT_TO_OBJECT_HEADER(o->body);
        if (type && h->Type != type) st = STATUS_OBJECT_TYPE_MISMATCH;
        else {
            __sync_add_and_fetch(&h->PointerCount, 1);
            *body = o->body;
            st = STATUS_SUCCESS;
        }
        pthread_mutex_unlock(&ob_lock);
    } else {
        char *parent = parent_path(r);
        xobject *dir = ns_find(parent);
        bool in_dir = dir && OBJECT_TO_OBJECT_HEADER(dir->body)->Type == &ObDirectoryObjectType;
        free(parent);
        pthread_mutex_unlock(&ob_lock);
        xobject *n = find_named_any(r);
        if (n) {
            /* Handle-table objects are not counted (see the top). */
            __sync_sub_and_fetch(&n->refs, 1);
            OBJECT_TYPE *have = kind_type(n->kind);
            if (type && have != type) st = STATUS_OBJECT_TYPE_MISMATCH;
            else { *body = object_body(n); st = STATUS_SUCCESS; }
        } else {
            st = in_dir || !strcmp(r, "\\") ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
        }
    }
    TRACE("ob_reference_by_path(%s -> %s) = %#x", path, r, (unsigned)st);
    free(r);
    return st;
}

NTSTATUS NTAPI ObReferenceObjectByName(OBJECT_STRING *ObjectName, ULONG Attributes, OBJECT_TYPE *ObjectType,
                                       PVOID ParseContext, PVOID *Object)
{
    (void)Attributes; (void)ParseContext;
    OBJECT_ATTRIBUTES oa = { NULL, ObjectName, 0 };
    NTSTATUS st;
    *Object = NULL;
    char *path = ob_full_path(&oa, &st);
    if (!path) return st;
    st = ob_reference_by_path(path, ObjectType, Object);
    free(path);
    return st;
}

NTSTATUS NTAPI ObOpenObjectByName(OBJECT_ATTRIBUTES *oa, OBJECT_TYPE *Type, PVOID ParseContext,
                                  HANDLE *Handle)
{
    (void)ParseContext;
    *Handle = NULL;
    if (Type == &ObSymbolicLinkObjectType)
        return NtOpenSymbolicLinkObject(Handle, oa);
    NTSTATUS st;
    char *path = ob_full_path(oa, &st);
    if (!path) return st;
    PVOID body;
    st = ob_reference_by_path(path, Type, &body);
    free(path);
    if (!NT_SUCCESS(st)) return st;
    xobject *o = ob_object_of(body);
    if (o) {
        *Handle = ob_handle(o);   /* takes over the reference */
        return STATUS_SUCCESS;
    }
    /* An object the Nt services made: body back to its xobject. */
    pthread_mutex_lock(&ob_lock);
    for (int i = 1; i < MAX_HANDLES && !o; i++)
        if (handles[i] && handles[i]->kind != OBJ_OB && object_body(handles[i]) == body) o = handles[i];
    if (o) o->refs++;
    pthread_mutex_unlock(&ob_lock);
    if (!o) return STATUS_OBJECT_NAME_NOT_FOUND;
    *Handle = handle_insert(o);
    return STATUS_SUCCESS;
}

/* ---- directory objects (obx/obdir.c) ---------------------------------- */

NTSTATUS NTAPI NtCreateDirectoryObject(HANDLE *DirectoryHandle, OBJECT_ATTRIBUTES *oa)
{
    PVOID d;
    *DirectoryHandle = NULL;
    NTSTATUS st = ObCreateObject(&ObDirectoryObjectType, oa, sizeof(OBJECT_DIRECTORY), &d);
    if (NT_SUCCESS(st)) {
        memset(d, 0, sizeof(OBJECT_DIRECTORY));
        st = ObInsertObject(d, oa, 0, DirectoryHandle);
    }
    return st;
}

NTSTATUS NTAPI NtOpenDirectoryObject(HANDLE *DirectoryHandle, OBJECT_ATTRIBUTES *oa)
{
    return ObOpenObjectByName(oa, &ObDirectoryObjectType, NULL, DirectoryHandle);
}

NTSTATUS NTAPI NtQueryDirectoryObject(HANDLE DirectoryHandle, PVOID Buffer, ULONG Length, BOOLEAN RestartScan,
                                      ULONG *Context, ULONG *ReturnedLength)
{
    if (Length < sizeof(OBJECT_DIRECTORY_INFORMATION)) return STATUS_INVALID_PARAMETER;
    xobject *dir = handle_lookup(DirectoryHandle);
    if (!dir) return STATUS_INVALID_HANDLE;
    if (dir->kind != OBJ_OB || OBJECT_TO_OBJECT_HEADER(dir->body)->Type != &ObDirectoryObjectType)
        return STATUS_OBJECT_TYPE_MISMATCH;
    ULONG want = RestartScan ? 0 : *Context, index = 0;
    char name[256];
    size_t nlen = 0;
    ULONG tag = 0;
    bool found = false;
    pthread_mutex_lock(&ob_lock);
    /* The directory's own entries, in hash bucket order as the kernel walks them... */
    OBJECT_DIRECTORY *d = dir->body;
    for (int b = 0; b < OB_NUMBER_HASH_BUCKETS && !found; b++)
        for (OBJECT_HEADER_NAME_INFO *ni = d->HashBuckets[b]; ni && !found; ni = ni->ChainLink, index++)
            if (index == want) {
                nlen = ni->Name.Length < sizeof(name) ? ni->Name.Length : sizeof(name);
                memcpy(name, ni->Name.Buffer, nlen);
                tag = ((OBJECT_HEADER *)(ni + 1))->Type->PoolTag;
                found = true;
            }
    /* ...then the handle-table objects whose names are in it (the \??
       links, say), each once. */
    size_t dl = dir->name ? strlen(dir->name) : 0;
    for (int i = 1; i < MAX_HANDLES && !found && dl; i++) {
        xobject *o = handles[i];
        if (!o || o->kind == OBJ_OB || !o->name || strncasecmp(o->name, dir->name, dl)) continue;
        const char *rest = o->name + dl;
        if (dl > 1) { if (*rest != '\\') continue; rest++; }
        if (!*rest || strchr(rest, '\\')) continue;
        bool dup = false;
        for (int k = 1; k < i && !dup; k++) dup = handles[k] == o;
        if (dup) continue;
        if (index++ == want) {
            nlen = strlen(rest) < sizeof(name) ? strlen(rest) : sizeof(name);
            memcpy(name, rest, nlen);
            OBJECT_TYPE *t = kind_type(o->kind);
            tag = t ? t->PoolTag : 0;
            found = true;
        }
    }
    pthread_mutex_unlock(&ob_lock);
    if (!found) {
        if (ReturnedLength) *ReturnedLength = 0;
        return STATUS_NO_MORE_ENTRIES;
    }
    OBJECT_DIRECTORY_INFORMATION *info = Buffer;
    ULONG need = sizeof(*info) + nlen, copy = nlen;
    NTSTATUS st = STATUS_SUCCESS;
    if (need > Length) { copy = Length - sizeof(*info); st = STATUS_BUFFER_TOO_SMALL; }
    info->Type = tag;
    info->Name.Buffer = (char *)(info + 1);
    info->Name.Length = info->Name.MaximumLength = (USHORT)nlen;
    memcpy(info + 1, name, copy);
    *Context = want + 1;
    if (ReturnedLength) *ReturnedLength = need;
    return st;
}

/* The kernel's directories and devices, made the first time the namespace
   is used (kernel devices: irp.c). */
extern void io_kernel_devices_init(void);
static pthread_mutex_t ns_init_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static volatile int ns_state;

static void ns_init(void)
{
    if (ns_state == 2) return;
    pthread_mutex_lock(&ns_init_lock);
    if (ns_state == 0) {
        ns_state = 1;
        /* The root has no name to insert by: it is attached by hand. */
        PVOID root;
        ObCreateObject(&ObDirectoryObjectType, NULL, sizeof(OBJECT_DIRECTORY), &root);
        memset(root, 0, sizeof(OBJECT_DIRECTORY));
        xobject *r = ob_object_of(root);
        r->name = strdup("\\");
        OBJECT_TO_OBJECT_HEADER(root)->Flags |= OB_FLAG_PERMANENT_OBJECT;
        static const char *dirs[] = { "\\??", "\\Device", "\\Device\\Harddisk0", "\\Win32NamedObjects" };
        for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            OBJECT_STRING n = { (USHORT)strlen(dirs[i]), (USHORT)strlen(dirs[i]), (char *)dirs[i] };
            OBJECT_ATTRIBUTES oa = { NULL, &n, OBJ_PERMANENT };
            HANDLE hd;
            if (NT_SUCCESS(NtCreateDirectoryObject(&hd, &oa))) NtClose(hd);
        }
        io_kernel_devices_init();
        ns_state = 2;
    }
    pthread_mutex_unlock(&ns_init_lock);
}
