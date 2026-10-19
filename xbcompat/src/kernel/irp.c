/*
 * The I/O manager's driver model (private/ntos/io/iosubs.c, internal.c,
 * ke/devquobj.c): I/O request packets, device and driver objects, device
 * queues, start-I/O packets and share access.
 *
 * Titles and the libraries linked into them make device objects for their
 * own drivers (a memory unit or other USB class driver, a file system), and
 * send IRPs to those and to the kernel's own disk devices.  A guest driver's
 * dispatch, start-I/O and completion routines are guest code.  The kernel's
 * devices (\Device\CdRom0, \Device\Harddisk0\PartitionN) have a host driver
 * here that carries a request out through the Nt file services in io.c, on
 * the same device files NtCreateFile opens, and completes it at once.
 *
 * Completion runs in IofCompleteRequest itself rather than in a kernel APC
 * on the requesting thread: it sets the event, writes the status block and
 * queues the caller's APC or completion packet just as IopCompleteRequest
 * would, only on whichever thread completes the packet.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>

#include "../cpu.h"
#include "../xbcompat.h"
#include "driver.h"

extern void NTAPI KeInitializeEvent(KEVENT *, ULONG, BOOLEAN);
extern LONG NTAPI KeSetEvent(KEVENT *, LONG, BOOLEAN);
extern NTSTATUS NTAPI KeWaitForSingleObject(PVOID, ULONG, KPROCESSOR_MODE, BOOLEAN, LARGE_INTEGER *);
extern KIRQL NTAPI KeRaiseIrqlToDpcLevel(void);
extern void FASTCALL KfLowerIrql(KIRQL);
extern void NTAPI KeBugCheckEx(ULONG, ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);
extern NTSTATUS NTAPI IoSetIoCompletion(PVOID, PVOID, PVOID, NTSTATUS, ULONG_PTR);

#define MULTIPLE_IRP_COMPLETE_REQUESTS 0x44
#define NO_MORE_IRP_STACK_LOCATIONS    0x35

/* Device queues and thread IRP lists are guarded by raising the IRQL on
   the console's single processor; here, by a lock.  Start-I/O calls are
   serialized by another, held across the driver's StartIo routine as the
   raised IRQL holds off other packets there. */
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t startio_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
extern NTSTATUS NTAPI NtClose(HANDLE);

/* ---- calls into guest code ------------------------------------------ */

#ifdef XBC_TRANSLATED
#define GUEST_CALL1(fn, a) CPU_CALL(fn, CONV_STD, (uint32_t)(a))
#define GUEST_CALL2(fn, a, b) CPU_CALL(fn, CONV_STD, (uint32_t)(a), (uint32_t)(b))
#define GUEST_CALL3(fn, a, b, c) CPU_CALL(fn, CONV_STD, (uint32_t)(a), (uint32_t)(b), (uint32_t)(c))
#else
typedef ULONG (__attribute__((stdcall)) *std1)(ULONG);
typedef ULONG (__attribute__((stdcall)) *std2)(ULONG, ULONG);
typedef ULONG (__attribute__((stdcall)) *std3)(ULONG, ULONG, ULONG);
#define GUEST_CALL1(fn, a) ((std1)(fn))((ULONG)(a))
#define GUEST_CALL2(fn, a, b) ((std2)(fn))((ULONG)(a), (ULONG)(b))
#define GUEST_CALL3(fn, a, b, c) ((std3)(fn))((ULONG)(a), (ULONG)(b), (ULONG)(c))
#endif

static DRIVER_OBJECT kernel_driver;
static NTSTATUS NTAPI kernel_dispatch(DEVICE_OBJECT *dev, IRP *irp);

/* ---- device queues (ke/devquobj.c) ----------------------------------- */

static void list_init(LIST_ENTRY *h) { h->Flink = h->Blink = h; }

static void list_insert_after(LIST_ENTRY *at, LIST_ENTRY *e)
{
    e->Flink = at->Flink;
    e->Blink = at;
    at->Flink->Blink = e;
    at->Flink = e;
}

static void list_remove(LIST_ENTRY *e)
{
    e->Blink->Flink = e->Flink;
    e->Flink->Blink = e->Blink;
}

void NTAPI KeInitializeDeviceQueue(KDEVICE_QUEUE *q)
{
    q->Type = DeviceQueueObject;
    q->Size = sizeof(*q);
    list_init(&q->DeviceListHead);
    q->Busy = 0;
}

/* Not busy: the caller starts the entry itself (FALSE).  Busy: queued. */
BOOLEAN NTAPI KeInsertDeviceQueue(KDEVICE_QUEUE *q, KDEVICE_QUEUE_ENTRY *e)
{
    pthread_mutex_lock(&io_lock);
    BOOLEAN inserted = q->Busy;
    if (inserted) list_insert_after(q->DeviceListHead.Blink, &e->DeviceListEntry);
    else q->Busy = 1;
    e->Inserted = inserted;
    pthread_mutex_unlock(&io_lock);
    return inserted;
}

/* Queued in ascending key order, after the entries with an equal key. */
BOOLEAN NTAPI KeInsertByKeyDeviceQueue(KDEVICE_QUEUE *q, KDEVICE_QUEUE_ENTRY *e, ULONG SortKey)
{
    pthread_mutex_lock(&io_lock);
    e->SortKey = SortKey;
    BOOLEAN inserted = q->Busy;
    if (inserted) {
        LIST_ENTRY *n = q->DeviceListHead.Flink;
        while (n != &q->DeviceListHead && SortKey >= ((KDEVICE_QUEUE_ENTRY *)n)->SortKey) n = n->Flink;
        list_insert_after(n->Blink, &e->DeviceListEntry);
    } else {
        q->Busy = 1;
    }
    e->Inserted = inserted;
    pthread_mutex_unlock(&io_lock);
    return inserted;
}

/* The head entry, or NULL (and the queue is no longer busy). */
KDEVICE_QUEUE_ENTRY *NTAPI KeRemoveDeviceQueue(KDEVICE_QUEUE *q)
{
    pthread_mutex_lock(&io_lock);
    KDEVICE_QUEUE_ENTRY *e = NULL;
    if (q->DeviceListHead.Flink == &q->DeviceListHead) {
        q->Busy = 0;
    } else {
        e = (KDEVICE_QUEUE_ENTRY *)q->DeviceListHead.Flink;
        list_remove(&e->DeviceListEntry);
        e->Inserted = 0;
    }
    pthread_mutex_unlock(&io_lock);
    return e;
}

/* The first entry whose key is at least SortKey, else the head. */
KDEVICE_QUEUE_ENTRY *NTAPI KeRemoveByKeyDeviceQueue(KDEVICE_QUEUE *q, ULONG SortKey)
{
    pthread_mutex_lock(&io_lock);
    KDEVICE_QUEUE_ENTRY *e = NULL;
    if (q->DeviceListHead.Flink == &q->DeviceListHead) {
        q->Busy = 0;
    } else {
        LIST_ENTRY *n = q->DeviceListHead.Flink;
        while (n != &q->DeviceListHead && SortKey > ((KDEVICE_QUEUE_ENTRY *)n)->SortKey) n = n->Flink;
        e = (KDEVICE_QUEUE_ENTRY *)(n != &q->DeviceListHead ? n : q->DeviceListHead.Flink);
        list_remove(&e->DeviceListEntry);
        e->Inserted = 0;
    }
    pthread_mutex_unlock(&io_lock);
    return e;
}

BOOLEAN NTAPI KeRemoveEntryDeviceQueue(KDEVICE_QUEUE *q, KDEVICE_QUEUE_ENTRY *e)
{
    (void)q;
    pthread_mutex_lock(&io_lock);
    BOOLEAN removed = e->Inserted;
    if (removed) {
        e->Inserted = 0;
        list_remove(&e->DeviceListEntry);
    }
    pthread_mutex_unlock(&io_lock);
    return removed;
}

/* ---- IRPs -------------------------------------------------------------- */

void NTAPI IoInitializeIrp(IRP *Irp, USHORT PacketSize, CCHAR StackSize)
{
    memset(Irp, 0, PacketSize);
    Irp->Type = IO_TYPE_IRP;
    Irp->Size = PacketSize;
    Irp->StackCount = StackSize;
    Irp->CurrentLocation = StackSize + 1;
    list_init(&Irp->ThreadListEntry);
    Irp->Tail.Overlay.CurrentStackLocation = (IO_STACK_LOCATION *)((char *)Irp + sizeof(IRP)) + StackSize;
}

IRP *NTAPI IoAllocateIrp(CCHAR StackSize)
{
    USHORT size = IoSizeOfIrp(StackSize);
    IRP *irp = pool_alloc(size);
    if (irp) IoInitializeIrp(irp, size, StackSize);
    return irp;
}

/* The requesting thread's list of pending IRPs (IopQueueThreadIrp). */
static void thread_queue_irp(IRP *irp)
{
    ETHREAD *t = irp->Tail.Overlay.Thread;
    if (!t) return;
    pthread_mutex_lock(&io_lock);
    if (!t->IrpList.Flink) list_init(&t->IrpList);
    list_insert_after(&t->IrpList, &irp->ThreadListEntry);
    pthread_mutex_unlock(&io_lock);
}

static void thread_dequeue_irp(IRP *irp)
{
    pthread_mutex_lock(&io_lock);
    if (irp->ThreadListEntry.Flink && irp->ThreadListEntry.Flink != &irp->ThreadListEntry) {
        list_remove(&irp->ThreadListEntry);
        list_init(&irp->ThreadListEntry);
    }
    pthread_mutex_unlock(&io_lock);
}

void NTAPI IoQueueThreadIrp(IRP *Irp)
{
    thread_queue_irp(Irp);
}

void NTAPI IoFreeIrp(IRP *Irp)
{
    if (Irp->Type != IO_TYPE_IRP)
        KeBugCheckEx(MULTIPLE_IRP_COMPLETE_REQUESTS, (ULONG_PTR)Irp, __LINE__, 0, 0);
    /* The kernel asserts the packet is off its thread's list by now; a
       driver that keeps a completed packet (STATUS_MORE_PROCESSING_REQUIRED)
       and frees it itself may not have, so take it off rather than leave
       the thread's list pointing at freed memory. */
    thread_dequeue_irp(Irp);
    Irp->Type = 0;
    pool_free(Irp);
}

static ETHREAD *current_ethread(void)
{
    xthread *t = thread_current();
    return t ? &t->ethread : NULL;
}

/* IoLockUserBuffer: guest memory never pages out, so only the bookkeeping. */
static void lock_user_buffer(IRP *irp, ULONG length)
{
    if (!(irp->Flags & (IRP_SCATTER_GATHER_OPERATION | IRP_UNLOCK_USER_BUFFER))) {
        irp->LockedBufferLength = length;
        irp->Flags |= IRP_UNLOCK_USER_BUFFER;
    }
}

IRP *NTAPI IoBuildAsynchronousFsdRequest(ULONG MajorFunction, DEVICE_OBJECT *DeviceObject, PVOID Buffer,
                                         ULONG Length, LARGE_INTEGER *StartingOffset,
                                         IO_STATUS_BLOCK *IoStatusBlock)
{
    IRP *irp = IoAllocateIrp(DeviceObject->StackSize);
    if (!irp) return NULL;
    irp->Tail.Overlay.Thread = current_ethread();
    IO_STACK_LOCATION *sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = (UCHAR)MajorFunction;
    if (MajorFunction != IRP_MJ_FLUSH_BUFFERS && MajorFunction != IRP_MJ_SHUTDOWN) {
        irp->UserBuffer = Buffer;
        if ((DeviceObject->Flags & DO_DIRECT_IO) && Length) lock_user_buffer(irp, Length);
        /* Read and Write share a layout. */
        sp->Parameters.Read.Length = Length;
        sp->Parameters.Read.ByteOffset = *StartingOffset;
    }
    irp->UserIosb = IoStatusBlock;
    return irp;
}

IRP *NTAPI IoBuildSynchronousFsdRequest(ULONG MajorFunction, DEVICE_OBJECT *DeviceObject, PVOID Buffer,
                                        ULONG Length, LARGE_INTEGER *StartingOffset, KEVENT *Event,
                                        IO_STATUS_BLOCK *IoStatusBlock)
{
    IRP *irp = IoBuildAsynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length, StartingOffset,
                                             IoStatusBlock);
    if (!irp) return NULL;
    irp->UserEvent = Event;
    thread_queue_irp(irp);
    return irp;
}

IRP *NTAPI IoBuildDeviceIoControlRequest(ULONG IoControlCode, DEVICE_OBJECT *DeviceObject, PVOID InputBuffer,
                                         ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength,
                                         BOOLEAN InternalDeviceIoControl, KEVENT *Event,
                                         IO_STATUS_BLOCK *IoStatusBlock)
{
    IRP *irp = IoAllocateIrp(DeviceObject->StackSize);
    if (!irp) return NULL;
    irp->UserBuffer = OutputBuffer;
    IO_STACK_LOCATION *sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = InternalDeviceIoControl ? IRP_MJ_INTERNAL_DEVICE_CONTROL : IRP_MJ_DEVICE_CONTROL;
    sp->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;
    sp->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
    sp->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    sp->Parameters.DeviceIoControl.InputBuffer = InputBuffer;
    ULONG method = IoControlCode & 3;
    if (OutputBufferLength && (method == 1 /* METHOD_IN_DIRECT */ || method == 2 /* METHOD_OUT_DIRECT */))
        lock_user_buffer(irp, OutputBufferLength);
    irp->UserIosb = IoStatusBlock;
    irp->UserEvent = Event;
    irp->Tail.Overlay.Thread = current_ethread();
    thread_queue_irp(irp);
    return irp;
}

NTSTATUS FASTCALL IofCallDriver(DEVICE_OBJECT *DeviceObject, IRP *Irp)
{
    Irp->CurrentLocation--;
    if (Irp->CurrentLocation <= 0)
        KeBugCheckEx(NO_MORE_IRP_STACK_LOCATIONS, (ULONG_PTR)Irp, 0, 0, 0);
    IO_STACK_LOCATION *sp = IoGetNextIrpStackLocation(Irp);
    Irp->Tail.Overlay.CurrentStackLocation = sp;
    sp->DeviceObject = DeviceObject;
    DRIVER_OBJECT *drv = DeviceObject->DriverObject;
    TRACE_ALL("IofCallDriver(%p, irp %p, major %u)", (void *)DeviceObject, (void *)Irp, sp->MajorFunction);
    if (drv == &kernel_driver) return kernel_dispatch(DeviceObject, Irp);
    return (NTSTATUS)GUEST_CALL2(drv->MajorFunction[sp->MajorFunction], DeviceObject, Irp);
}

static LONG must_complete_packets;   /* IoPendingMustCompletePackets */

void NTAPI IoMarkIrpMustComplete(IRP *Irp)
{
    IO_STACK_LOCATION *sp = IoGetCurrentIrpStackLocation(Irp);
    if (!(sp->Control & SL_MUST_COMPLETE)) {
        sp->Control |= SL_MUST_COMPLETE;
        __sync_add_and_fetch(&must_complete_packets, 1);
    }
}

/* IopCompleteRequest, the final part of completion. */
static void complete_to_caller(IRP *irp)
{
    FILE_OBJECT *fo = irp->Tail.Overlay.OriginalFileObject;
    NTSTATUS st = irp->IoStatus.Status;
    bool sync = (irp->Flags & IRP_SYNCHRONOUS_API) || (fo && (fo->Flags & FO_SYNCHRONOUS_IO));
    if (!NT_ERROR(st) || (irp->PendingReturned && !sync)) {
        PVOID port = NULL, key = NULL;
        if (fo && fo->CompletionContext) {
            port = fo->CompletionContext->Port;
            key = fo->CompletionContext->Key;
        }
        /* Information before Status: the status is what pollers watch. */
        if (irp->UserIosb) {
            irp->UserIosb->Information = irp->IoStatus.Information;
            __atomic_store_n(&irp->UserIosb->Status, st, __ATOMIC_RELEASE);
        }
        bool create = fo && (irp->Flags & IRP_CREATE_OPERATION);
        if (irp->UserEvent) {
            KeSetEvent(irp->UserEvent, 0, 0);
            if (fo && (fo->Flags & FO_SYNCHRONOUS_IO)) {
                fo->FinalStatus = st;
                KeSetEvent(&fo->Event, 0, 0);
            }
        } else if (fo) {
            fo->FinalStatus = st;
            KeSetEvent(&fo->Event, 0, 0);
        }
        thread_dequeue_irp(irp);
        PVOID apc = create ? NULL : irp->Overlay.AsynchronousParameters.UserApcRoutine;
        PVOID ctx = irp->Overlay.AsynchronousParameters.UserApcContext;
        if (apc) {
            ETHREAD *t = irp->Tail.Overlay.Thread;
            apc_queue(t ? (xthread *)t : thread_current(), apc, ctx, irp->UserIosb, NULL, NULL);
        } else if (port && ctx) {
            IoSetIoCompletion(port, key, ctx, st, irp->IoStatus.Information);
        }
        IoFreeIrp(irp);
        return;
    }
    /* Failed without pending: nothing is signaled, except for a
       synchronous request that did pend (the I/O system waits for it). */
    if (irp->PendingReturned && fo) {
        if (irp->Flags & IRP_SYNCHRONOUS_API) {
            if (irp->UserIosb) *irp->UserIosb = irp->IoStatus;
            KeSetEvent(irp->UserEvent ? irp->UserEvent : &fo->Event, 0, 0);
        } else {
            fo->FinalStatus = st;
            KeSetEvent(&fo->Event, 0, 0);
        }
    }
    thread_dequeue_irp(irp);
    IoFreeIrp(irp);
}

void FASTCALL IofCompleteRequest(IRP *Irp, CCHAR PriorityBoost)
{
    (void)PriorityBoost;
    if (Irp->CurrentLocation > Irp->StackCount + 1 || Irp->Type != IO_TYPE_IRP)
        KeBugCheckEx(MULTIPLE_IRP_COMPLETE_REQUESTS, (ULONG_PTR)Irp, __LINE__, 0, 0);
    TRACE_ALL("IofCompleteRequest(irp %p, status %#x)", (void *)Irp, (unsigned)Irp->IoStatus.Status);

    /* Walk back up the stack, calling the completion routines that asked
       for this outcome; one returning STATUS_MORE_PROCESSING_REQUIRED takes
       the packet over (and may have freed it already). */
    IO_STACK_LOCATION *sp = IoGetCurrentIrpStackLocation(Irp);
    Irp->CurrentLocation++;
    Irp->Tail.Overlay.CurrentStackLocation++;
    for (; Irp->CurrentLocation <= Irp->StackCount + 1;
         sp++, Irp->CurrentLocation++, Irp->Tail.Overlay.CurrentStackLocation++) {
        if (sp->Control & SL_MUST_COMPLETE) __sync_sub_and_fetch(&must_complete_packets, 1);
        Irp->PendingReturned = sp->Control & SL_PENDING_RETURNED;
        bool ok = NT_SUCCESS(Irp->IoStatus.Status);
        bool invoke = ok ? (sp->Control & SL_INVOKE_ON_SUCCESS) : (sp->Control & SL_INVOKE_ON_ERROR);
        if (!invoke && Irp->PendingReturned && Irp->CurrentLocation <= Irp->StackCount)
            IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED;   /* IoMarkIrpPending */
        /* ZeroIrpStackLocation: the routine and its context stay. */
        sp->MinorFunction = sp->Flags = sp->Control = 0;
        memset(&sp->Parameters, 0, sizeof(sp->Parameters.Others));
        sp->FileObject = NULL;
        if (invoke) {
            DEVICE_OBJECT *dev = Irp->CurrentLocation == Irp->StackCount + 1
                               ? NULL : IoGetCurrentIrpStackLocation(Irp)->DeviceObject;
            NTSTATUS st = (NTSTATUS)GUEST_CALL3(sp->CompletionRoutine, dev, Irp, sp->Context);
            if (st == STATUS_MORE_PROCESSING_REQUIRED) return;
        }
    }

    /* Mount and close packets: status and event only (a mount's packet is
       freed here, a close's by its waiter). */
    if (Irp->Flags & (IRP_MOUNT_COMPLETION | IRP_CLOSE_OPERATION)) {
        ULONG mount = Irp->Flags & IRP_MOUNT_COMPLETION;
        if (Irp->UserIosb) *Irp->UserIosb = Irp->IoStatus;
        if (Irp->UserEvent) KeSetEvent(Irp->UserEvent, PriorityBoost, 0);
        if (mount) IoFreeIrp(Irp);
        return;
    }
    Irp->Flags &= ~IRP_UNLOCK_USER_BUFFER;
    /* The originator completes a deferred packet that did not pend. */
    if ((Irp->Flags & IRP_DEFER_IO_COMPLETION) && !Irp->PendingReturned) return;
    complete_to_caller(Irp);
}

NTSTATUS NTAPI IoInvalidDeviceRequest(DEVICE_OBJECT *DeviceObject, IRP *Irp)
{
    (void)DeviceObject;
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    IofCompleteRequest(Irp, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* Send a packet built with an event and status block of the caller's, and
   wait for it if the driver pends it. */
static NTSTATUS call_and_wait(DEVICE_OBJECT *dev, IRP *irp, KEVENT *ev, IO_STATUS_BLOCK *iosb)
{
    NTSTATUS st = IofCallDriver(dev, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(ev, 0 /* Executive */, 0 /* KernelMode */, 0, NULL);
        st = iosb->Status;
    }
    return st;
}

NTSTATUS NTAPI IoSynchronousFsdRequest(ULONG MajorFunction, DEVICE_OBJECT *DeviceObject, PVOID Buffer,
                                       ULONG Length, LARGE_INTEGER *StartingOffset)
{
    KEVENT ev;
    IO_STATUS_BLOCK iosb = { 0 };
    KeInitializeEvent(&ev, 0 /* NotificationEvent */, 0);
    IRP *irp = IoBuildSynchronousFsdRequest(MajorFunction, DeviceObject, Buffer, Length, StartingOffset,
                                            &ev, &iosb);
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;
    return call_and_wait(DeviceObject, irp, &ev, &iosb);
}

NTSTATUS NTAPI IoSynchronousDeviceIoControlRequest(ULONG IoControlCode, DEVICE_OBJECT *DeviceObject,
                                                   PVOID InputBuffer, ULONG InputBufferLength,
                                                   PVOID OutputBuffer, ULONG OutputBufferLength,
                                                   ULONG *ReturnedOutputBufferLength,
                                                   BOOLEAN InternalDeviceIoControl)
{
    TRACE("IoSynchronousDeviceIoControlRequest(%#x, %p)", IoControlCode, (void *)DeviceObject);
    if (!ob_object_of(DeviceObject) || DeviceObject->Type != IO_TYPE_DEVICE) {
        /* Not a device object made here: answer as before there were any,
           with an empty success. */
        static int once;
        if (!once++) xlog("IoSynchronousDeviceIoControlRequest(%#x): %p is no device object, "
                          "returning zeros", IoControlCode, (void *)DeviceObject);
        if (OutputBuffer) memset(OutputBuffer, 0, OutputBufferLength);
        if (ReturnedOutputBufferLength) *ReturnedOutputBufferLength = 0;
        return STATUS_SUCCESS;
    }
    KEVENT ev;
    IO_STATUS_BLOCK iosb = { 0 };
    KeInitializeEvent(&ev, 0, 0);
    IRP *irp = IoBuildDeviceIoControlRequest(IoControlCode, DeviceObject, InputBuffer, InputBufferLength,
                                             OutputBuffer, OutputBufferLength, InternalDeviceIoControl,
                                             &ev, &iosb);
    NTSTATUS st = irp ? call_and_wait(DeviceObject, irp, &ev, &iosb) : STATUS_INSUFFICIENT_RESOURCES;
    if (ReturnedOutputBufferLength) *ReturnedOutputBufferLength = iosb.Information;
    return st;
}

/* ---- device objects ---------------------------------------------------- */

NTSTATUS NTAPI IoCreateDevice(DRIVER_OBJECT *DriverObject, ULONG DeviceExtensionSize, OBJECT_STRING *DeviceName,
                              ULONG DeviceType, BOOLEAN Exclusive, DEVICE_OBJECT **DeviceObject)
{
    OBJECT_ATTRIBUTES oa = { NULL, DeviceName, (Exclusive ? OBJ_EXCLUSIVE : 0) | (DeviceName ? OBJ_PERMANENT : 0) };
    ULONG pad = (sizeof(DEVICE_OBJECT) + DeviceExtensionSize) % 8;
    ULONG rounded = (pad ? 8 - pad : 0) + DeviceExtensionSize;
    DEVICE_OBJECT *d;
    NTSTATUS st = ObCreateObject(&IoDeviceObjectType, &oa, sizeof(DEVICE_OBJECT) + rounded, (PVOID *)&d);
    if (NT_SUCCESS(st)) {
        memset(d, 0, sizeof(DEVICE_OBJECT) + rounded);
        d->Type = IO_TYPE_DEVICE;
        d->Size = (USHORT)(sizeof(DEVICE_OBJECT) + DeviceExtensionSize);
        d->DeviceType = (UCHAR)DeviceType;
        /* Storage devices get a file system mounted on them on first open;
           everything else is its own "mounted" device. */
        if (DeviceType == FILE_DEVICE_DISK || DeviceType == FILE_DEVICE_MEMORY_UNIT ||
            DeviceType == FILE_DEVICE_CD_ROM || DeviceType == FILE_DEVICE_MEDIA_BOARD) {
            KeInitializeEvent(&d->DeviceLock, 1 /* SynchronizationEvent */, 1);
            d->MountedOrSelfDevice = NULL;
        } else {
            d->MountedOrSelfDevice = d;
        }
        d->Flags = DO_DEVICE_INITIALIZING | (Exclusive ? DO_EXCLUSIVE : 0) | (DeviceName ? DO_DEVICE_HAS_NAME : 0);
        d->DeviceExtension = DeviceExtensionSize ? d + 1 : NULL;
        d->StackSize = 1;
        KeInitializeDeviceQueue(&d->DeviceQueue);
        HANDLE h;
        st = ObInsertObject(d, &oa, 1, &h);
        if (NT_SUCCESS(st)) {
            d->DriverObject = DriverObject;
            NtClose(h);
        } else {
            d = NULL;
        }
    } else {
        d = NULL;
    }
    TRACE("IoCreateDevice(driver %p, ext %u, %.*s, type %#x) = %#x, %p", (void *)DriverObject,
          (unsigned)DeviceExtensionSize, DeviceName ? DeviceName->Length : 0, DeviceName ? DeviceName->Buffer : "",
          (unsigned)DeviceType, (unsigned)st, (void *)d);
    *DeviceObject = d;
    return st;
}

void NTAPI IoDeleteDevice(DEVICE_OBJECT *DeviceObject)
{
    TRACE("IoDeleteDevice(%p)", (void *)DeviceObject);
    if (DeviceObject->Flags & DO_DEVICE_HAS_NAME) {
        extern void NTAPI ObMakeTemporaryObject(PVOID);
        ObMakeTemporaryObject(DeviceObject);
    }
    DeviceObject->DeletePending = 1;
    /* IopCompleteUnloadOrDelete, once nothing holds the device open. */
    if (!DeviceObject->ReferenceCount) {
        DRIVER_OBJECT *drv = DeviceObject->DriverObject;
        if (drv && drv->DriverDeleteDevice) GUEST_CALL1(drv->DriverDeleteDevice, DeviceObject);
        else ObfDereferenceObject(DeviceObject);
    }
}

/* ---- start-I/O packets (IoStartPacket and friends) ------------------- */

static void start_io(DEVICE_OBJECT *dev, IRP *irp)
{
    dev->CurrentIrp = irp;
    dev->StartIoFlags |= DO_STARTIO_BUSY;
    GUEST_CALL2(dev->DriverObject->DriverStartIo, dev, irp);
    dev->StartIoFlags &= ~DO_STARTIO_BUSY;
}

/* StartIo routines run at DISPATCH_LEVEL.  Host threads that never ran
   guest code (tests) have no KPCR to keep an IRQL in. */
static KIRQL raise_to_dpc(void) { return thread_current() ? KeRaiseIrqlToDpcLevel() : 0; }
static void lower_irql(KIRQL irql) { if (thread_current()) KfLowerIrql(irql); }

#define IRP_OF_QUEUE_ENTRY(e) ((IRP *)((char *)(e) - offsetof(IRP, Tail.Overlay.DeviceQueueEntry)))

static void start_next(DEVICE_OBJECT *dev, bool by_key, ULONG key)
{
    dev->CurrentIrp = NULL;
    KDEVICE_QUEUE_ENTRY *e = by_key ? KeRemoveByKeyDeviceQueue(&dev->DeviceQueue, key)
                                    : KeRemoveDeviceQueue(&dev->DeviceQueue);
    if (e) start_io(dev, IRP_OF_QUEUE_ENTRY(e));
}

/* A StartIo routine that asks for the next packet gets it after it
   returns, not nested inside itself (IopStartNextPacketDeferred). */
static void run_deferred(DEVICE_OBJECT *dev)
{
    while (dev->StartIoFlags & DO_STARTIO_REQUESTED) {
        dev->StartIoFlags &= ~DO_STARTIO_REQUESTED;
        start_next(dev, dev->StartIoFlags & DO_STARTIO_REQUESTED_BYKEY, dev->StartIoKey);
    }
}

static void start_next_packet(DEVICE_OBJECT *dev, bool by_key, ULONG key)
{
    KIRQL irql = raise_to_dpc();
    pthread_mutex_lock(&startio_lock);
    if (!(dev->StartIoFlags & DO_STARTIO_BUSY)) {
        start_next(dev, by_key, key);
        run_deferred(dev);
    } else {
        dev->StartIoFlags |= DO_STARTIO_REQUESTED;
        if (by_key) {
            dev->StartIoFlags |= DO_STARTIO_REQUESTED_BYKEY;
            dev->StartIoKey = key;
        } else {
            dev->StartIoFlags &= ~DO_STARTIO_REQUESTED_BYKEY;
        }
    }
    pthread_mutex_unlock(&startio_lock);
    lower_irql(irql);
}

void NTAPI IoStartNextPacket(DEVICE_OBJECT *DeviceObject)
{
    start_next_packet(DeviceObject, false, 0);
}

void NTAPI IoStartNextPacketByKey(DEVICE_OBJECT *DeviceObject, ULONG Key)
{
    start_next_packet(DeviceObject, true, Key);
}

void NTAPI IoStartPacket(DEVICE_OBJECT *DeviceObject, IRP *Irp, ULONG *Key)
{
    KIRQL irql = raise_to_dpc();
    pthread_mutex_lock(&startio_lock);
    KDEVICE_QUEUE_ENTRY *e = &Irp->Tail.Overlay.DeviceQueueEntry;
    BOOLEAN queued = Key ? KeInsertByKeyDeviceQueue(&DeviceObject->DeviceQueue, e, *Key)
                         : KeInsertDeviceQueue(&DeviceObject->DeviceQueue, e);
    if (!queued) {
        start_io(DeviceObject, Irp);
        run_deferred(DeviceObject);
    }
    pthread_mutex_unlock(&startio_lock);
    lower_irql(irql);
}

/* ---- share access ------------------------------------------------------ */

static bool set_file_access(ACCESS_MASK access, ULONG share, FILE_OBJECT *fo)
{
    fo->ReadAccess = (access & (FILE_EXECUTE | FILE_READ_DATA)) != 0;
    fo->WriteAccess = (access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    fo->DeleteAccess = (access & DELETE) != 0;
    if (!fo->ReadAccess && !fo->WriteAccess && !fo->DeleteAccess) return false;
    fo->SharedRead = (share & FILE_SHARE_READ) != 0;
    fo->SharedWrite = (share & FILE_SHARE_WRITE) != 0;
    fo->SharedDelete = (share & FILE_SHARE_DELETE) != 0;
    return true;
}

NTSTATUS NTAPI IoCheckShareAccess(ACCESS_MASK DesiredAccess, ULONG DesiredShareAccess, FILE_OBJECT *FileObject,
                                  SHARE_ACCESS *ShareAccess, BOOLEAN Update)
{
    if (!set_file_access(DesiredAccess, DesiredShareAccess, FileObject)) return STATUS_SUCCESS;
    UCHAR n = ShareAccess->OpenCount;
    if (n == 0xFF ||
        (FileObject->ReadAccess && ShareAccess->SharedRead < n) ||
        (FileObject->WriteAccess && ShareAccess->SharedWrite < n) ||
        (FileObject->DeleteAccess && ShareAccess->SharedDelete < n) ||
        (ShareAccess->Readers && !FileObject->SharedRead) ||
        (ShareAccess->Writers && !FileObject->SharedWrite) ||
        (ShareAccess->Deleters && !FileObject->SharedDelete))
        return STATUS_SHARING_VIOLATION;
    if (Update) {
        ShareAccess->OpenCount++;
        ShareAccess->Readers += FileObject->ReadAccess;
        ShareAccess->Writers += FileObject->WriteAccess;
        ShareAccess->Deleters += FileObject->DeleteAccess;
        ShareAccess->SharedRead += FileObject->SharedRead;
        ShareAccess->SharedWrite += FileObject->SharedWrite;
        ShareAccess->SharedDelete += FileObject->SharedDelete;
    }
    return STATUS_SUCCESS;
}

void NTAPI IoSetShareAccess(ACCESS_MASK DesiredAccess, ULONG DesiredShareAccess, FILE_OBJECT *FileObject,
                            SHARE_ACCESS *ShareAccess)
{
    if (set_file_access(DesiredAccess, DesiredShareAccess, FileObject)) {
        ShareAccess->OpenCount = 1;
        ShareAccess->Readers = FileObject->ReadAccess;
        ShareAccess->Writers = FileObject->WriteAccess;
        ShareAccess->Deleters = FileObject->DeleteAccess;
        ShareAccess->SharedRead = FileObject->SharedRead;
        ShareAccess->SharedWrite = FileObject->SharedWrite;
        ShareAccess->SharedDelete = FileObject->SharedDelete;
    } else {
        memset(ShareAccess, 0, sizeof(*ShareAccess));
    }
}

void NTAPI IoRemoveShareAccess(FILE_OBJECT *FileObject, SHARE_ACCESS *ShareAccess)
{
    if (!FileObject->ReadAccess && !FileObject->WriteAccess && !FileObject->DeleteAccess) return;
    ShareAccess->OpenCount--;
    ShareAccess->Readers -= FileObject->ReadAccess;
    ShareAccess->Writers -= FileObject->WriteAccess;
    ShareAccess->Deleters -= FileObject->DeleteAccess;
    ShareAccess->SharedRead -= FileObject->SharedRead;
    ShareAccess->SharedWrite -= FileObject->SharedWrite;
    ShareAccess->SharedDelete -= FileObject->SharedDelete;
}

/* ---- files by object --------------------------------------------------- */

extern NTSTATUS NTAPI NtCreateFile(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *,
                                   LARGE_INTEGER *, ULONG, ULONG, ULONG, ULONG);
extern NTSTATUS NTAPI NtQueryInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
extern NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
extern NTSTATUS NTAPI NtReadFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, LARGE_INTEGER *);
extern NTSTATUS NTAPI NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, LARGE_INTEGER *);
extern NTSTATUS NTAPI NtFlushBuffersFile(HANDLE, IO_STATUS_BLOCK *);
extern NTSTATUS NTAPI NtDeviceIoControlFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, ULONG, PVOID, ULONG,
                                            PVOID, ULONG);

/* NtCreateFile and NtOpenFile are this with no options (io/iosubs.c). */
NTSTATUS NTAPI IoCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *ObjectAttributes,
                            IO_STATUS_BLOCK *IoStatusBlock, LARGE_INTEGER *AllocationSize, ULONG FileAttributes,
                            ULONG ShareAccess, ULONG Disposition, ULONG CreateOptions, ULONG Options)
{
    (void)Options;
    return NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize,
                        FileAttributes, ShareAccess, Disposition, CreateOptions);
}

/* A file object is what ObReferenceObjectByHandle(IoFileObjectType) gave:
   the file's xobject.  The query runs on a handle made for it. */
static NTSTATUS query_file_object(PVOID FileObject, ULONG Class, ULONG Length, PVOID Info,
                                  ULONG *ReturnedLength, bool volume)
{
    xobject *o = FileObject;
    if (ReturnedLength) *ReturnedLength = 0;
    if (!pool_owns(o) || o->kind != OBJ_FILE) return STATUS_OBJECT_TYPE_MISMATCH;
    __sync_add_and_fetch(&o->refs, 1);
    HANDLE h = handle_insert(o);
    IO_STATUS_BLOCK iosb = { 0 };
    NTSTATUS st = volume ? NtQueryVolumeInformationFile(h, &iosb, Info, Length, Class)
                         : NtQueryInformationFile(h, &iosb, Info, Length, Class);
    handle_close(h);
    if (NT_SUCCESS(st) && ReturnedLength) *ReturnedLength = iosb.Information;
    return st;
}

NTSTATUS NTAPI IoQueryFileInformation(FILE_OBJECT *FileObject, ULONG FileInformationClass, ULONG Length,
                                      PVOID FileInformation, ULONG *ReturnedLength)
{
    return query_file_object(FileObject, FileInformationClass, Length, FileInformation, ReturnedLength, false);
}

NTSTATUS NTAPI IoQueryVolumeInformation(FILE_OBJECT *FileObject, ULONG FsInformationClass, ULONG Length,
                                        PVOID FsInformation, ULONG *ReturnedLength)
{
    return query_file_object(FileObject, FsInformationClass, Length, FsInformation, ReturnedLength, true);
}

/* ---- the kernel's own devices ----------------------------------------- */

/* Each one's extension: the device's object path, and a handle on it for
   the Nt services, opened on first use. */
typedef struct {
    const char *path;
    HANDLE handle;
} kernel_device;

static pthread_mutex_t kdev_lock = PTHREAD_MUTEX_INITIALIZER;

static HANDLE kernel_device_handle(kernel_device *k)
{
    pthread_mutex_lock(&kdev_lock);
    if (!k->handle) {
        OBJECT_STRING n = { (USHORT)strlen(k->path), (USHORT)strlen(k->path), (char *)k->path };
        OBJECT_ATTRIBUTES oa = { NULL, &n, 0 };
        IO_STATUS_BLOCK iosb;
        HANDLE h;
        if (NT_SUCCESS(NtCreateFile(&h, 0xC0100000 /* GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE */, &oa, &iosb, NULL,
                                    0, 3, 1 /* FILE_OPEN */, 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */)))
            k->handle = h;
        else
            xlog("%s: cannot open the device for its IRPs", k->path);
    }
    pthread_mutex_unlock(&kdev_lock);
    return k->handle;
}

static NTSTATUS NTAPI kernel_dispatch(DEVICE_OBJECT *dev, IRP *irp)
{
    kernel_device *k = dev->DeviceExtension;
    IO_STACK_LOCATION *sp = IoGetCurrentIrpStackLocation(irp);
    IO_STATUS_BLOCK iosb = { 0 };
    NTSTATUS st;
    HANDLE h;
    switch (sp->MajorFunction) {
    case IRP_MJ_READ:
    case IRP_MJ_WRITE: {
        if (irp->Flags & IRP_SCATTER_GATHER_OPERATION) {
            static int once;
            if (!once++) xlog("%s: scatter/gather IRPs are not supported", k->path);
            st = STATUS_NOT_SUPPORTED;
            break;
        }
        if (!(h = kernel_device_handle(k))) { st = STATUS_UNSUCCESSFUL; break; }
        LARGE_INTEGER off = sp->Parameters.Read.ByteOffset;
        st = sp->MajorFunction == IRP_MJ_READ
           ? NtReadFile(h, NULL, NULL, NULL, &iosb, irp->UserBuffer, sp->Parameters.Read.Length, &off)
           : NtWriteFile(h, NULL, NULL, NULL, &iosb, irp->UserBuffer, sp->Parameters.Write.Length, &off);
        if (st == STATUS_PENDING) st = iosb.Status;
        break;
    }
    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        if (!(h = kernel_device_handle(k))) { st = STATUS_UNSUCCESSFUL; break; }
        st = NtDeviceIoControlFile(h, NULL, NULL, NULL, &iosb, sp->Parameters.DeviceIoControl.IoControlCode,
                                   sp->Parameters.DeviceIoControl.InputBuffer,
                                   sp->Parameters.DeviceIoControl.InputBufferLength, irp->UserBuffer,
                                   sp->Parameters.DeviceIoControl.OutputBufferLength);
        if (st == STATUS_PENDING) st = iosb.Status;
        break;
    case IRP_MJ_FLUSH_BUFFERS:
        if (!(h = kernel_device_handle(k))) { st = STATUS_UNSUCCESSFUL; break; }
        st = NtFlushBuffersFile(h, &iosb);
        break;
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
    case IRP_MJ_CLEANUP:
    case IRP_MJ_SHUTDOWN:
        st = STATUS_SUCCESS;
        break;
    default:
        return IoInvalidDeviceRequest(dev, irp);
    }
    TRACE("%s: IRP major %u = %#x, %u bytes", k->path, sp->MajorFunction, (unsigned)st, (unsigned)iosb.Information);
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = NT_ERROR(st) ? 0 : iosb.Information;
    IofCompleteRequest(irp, 0);
    return st;
}

/* Called by ob.c when it makes the namespace. */
void io_kernel_devices_init(void)
{
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        kernel_driver.MajorFunction[i] = (PVOID)(uintptr_t)cpu_guest_entry((void *)kernel_dispatch);
    static const struct { const char *path; UCHAR type; ULONG sector; } devs[] = {
        { "\\Device\\CdRom0", FILE_DEVICE_CD_ROM, 2048 },
        { "\\Device\\Harddisk0\\Partition0", FILE_DEVICE_DISK, 512 },
        { "\\Device\\Harddisk0\\Partition1", FILE_DEVICE_DISK, 512 },
        { "\\Device\\Harddisk0\\Partition2", FILE_DEVICE_DISK, 512 },
        { "\\Device\\Harddisk0\\Partition3", FILE_DEVICE_DISK, 512 },
        { "\\Device\\Harddisk0\\Partition4", FILE_DEVICE_DISK, 512 },
        { "\\Device\\Harddisk0\\Partition5", FILE_DEVICE_DISK, 512 },
        { "\\Device\\Harddisk0\\Partition6", FILE_DEVICE_DISK, 512 },
        { "\\Device\\Harddisk0\\Partition7", FILE_DEVICE_DISK, 512 },
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        OBJECT_STRING n = { (USHORT)strlen(devs[i].path), (USHORT)strlen(devs[i].path), (char *)devs[i].path };
        DEVICE_OBJECT *d;
        if (!NT_SUCCESS(IoCreateDevice(&kernel_driver, sizeof(kernel_device), &n, devs[i].type, 0, &d))) {
            xlog("cannot make the kernel device %s", devs[i].path);
            continue;
        }
        ((kernel_device *)d->DeviceExtension)->path = devs[i].path;
        d->SectorSize = devs[i].sector;
        d->AlignmentRequirement = 0;
        d->Flags = (d->Flags & ~DO_DEVICE_INITIALIZING) | DO_DIRECT_IO;
    }
}
