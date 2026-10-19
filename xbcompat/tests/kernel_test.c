/*
 * kernel_test: kernel exports without a title.
 *
 * Covers NtReadFileScatter/NtWriteFileGather, I/O completion ports (posted
 * packets and overlapped file reads bound to a port), KQUEUE, the
 * reader/writer lock, the Exf interlocked lists, ExInterlockedAddLargeInteger,
 * NtQueryMutant/Semaphore/Timer, the KeSystemTime/KeInterruptTime data
 * exports, the driver model (IRPs through a test driver and the kernel's own
 * disk device, completion routines, start-I/O, device queues, share access)
 * and the object namespace (directory objects, ObReferenceObjectByName).
 * It links against xbcompat's own objects (all but main.o) and runs the
 * kernel on host threads.
 *
 * Build (after `make`, from xbcompat/):
 *   gcc -m32 -no-pie -fno-pie -std=gnu11 -Wall -mstackrealign -D_FILE_OFFSET_BITS=64 \
 *       -Isrc $(pkg-config --cflags sdl2) -Wl,-Ttext-segment=0x60000000 \
 *       -o kernel_test tests/kernel_test.c $(find build/src -name '*.o' ! -name main.o) \
 *       $(pkg-config --libs sdl2) -lGL -lpthread -lm -ldl
 * (-Ttext-segment as in the Makefile: the guest's memory sits below it.)
 * Run:  ./kernel_test
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xbcompat.h"
#include "kernel/driver.h"

/* What main.c defines for the rest of xbcompat. */
int g_screenshot_frame = 60;
const char *g_screenshot_path;
int g_exit_after_frames;
volatile unsigned g_guest_traps;
char *g_dvd_root, *g_dvd_drive;
void install_fault_handlers(void) {}

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define FILE_OPEN_IF 3
#define FILE_OVERWRITE_IF 5
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20
#define FILE_NON_DIRECTORY_FILE 0x40

NTSTATUS NTAPI NtCreateFile(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, LARGE_INTEGER *,
                            ULONG, ULONG, ULONG, ULONG);
NTSTATUS NTAPI NtReadFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, LARGE_INTEGER *);
NTSTATUS NTAPI NtReadFileScatter(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, const ULONGLONG *, ULONG,
                                 LARGE_INTEGER *);
NTSTATUS NTAPI NtWriteFileGather(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, const ULONGLONG *, ULONG,
                                 LARGE_INTEGER *);
NTSTATUS NTAPI NtSetInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
NTSTATUS NTAPI NtClose(HANDLE);
NTSTATUS NTAPI NtCreateIoCompletion(HANDLE *, ULONG, OBJECT_ATTRIBUTES *, ULONG);
NTSTATUS NTAPI NtSetIoCompletion(HANDLE, PVOID, PVOID, NTSTATUS, ULONG_PTR);
NTSTATUS NTAPI NtRemoveIoCompletion(HANDLE, PVOID *, PVOID *, IO_STATUS_BLOCK *, LARGE_INTEGER *);
NTSTATUS NTAPI NtQueryIoCompletion(HANDLE, LONG *);
NTSTATUS NTAPI NtCreateMutant(HANDLE *, OBJECT_ATTRIBUTES *, BOOLEAN);
NTSTATUS NTAPI NtQueryMutant(HANDLE, PVOID);
NTSTATUS NTAPI NtCreateSemaphore(HANDLE *, OBJECT_ATTRIBUTES *, LONG, LONG);
NTSTATUS NTAPI NtQuerySemaphore(HANDLE, PVOID);
NTSTATUS NTAPI NtCreateTimer(HANDLE *, OBJECT_ATTRIBUTES *, ULONG);
NTSTATUS NTAPI NtSetTimerEx(HANDLE, LARGE_INTEGER *, PVOID, KPROCESSOR_MODE, PVOID, BOOLEAN, LONG, BOOLEAN *);
NTSTATUS NTAPI NtQueryTimer(HANDLE, PVOID);
void NTAPI KeInitializeQueue(KQUEUE *, ULONG);
LONG NTAPI KeInsertQueue(KQUEUE *, LIST_ENTRY *);
LONG NTAPI KeInsertHeadQueue(KQUEUE *, LIST_ENTRY *);
LIST_ENTRY *NTAPI KeRemoveQueue(KQUEUE *, KPROCESSOR_MODE, LARGE_INTEGER *);
LIST_ENTRY *NTAPI KeRundownQueue(KQUEUE *);
LONGLONG NTAPI ExInterlockedAddLargeInteger(LARGE_INTEGER *, ULONG, LONG, PVOID);
LIST_ENTRY *FASTCALL ExfInterlockedInsertHeadList(LIST_ENTRY *, LIST_ENTRY *);
LIST_ENTRY *FASTCALL ExfInterlockedInsertTailList(LIST_ENTRY *, LIST_ENTRY *);
LIST_ENTRY *FASTCALL ExfInterlockedRemoveHeadList(LIST_ENTRY *);
void NTAPI KeQuerySystemTime(LARGE_INTEGER *);
ULONGLONG NTAPI KeQueryInterruptTime(void);
extern volatile KSYSTEM_TIME KeSystemTime, KeInterruptTime;

typedef struct {
    LONG LockCount;
    ULONG WritersWaitingCount, ReadersWaitingCount, ReadersEntryCount;
    KEVENT WriterEvent;
    KSEMAPHORE ReaderSemaphore;
} ERWLOCK;
void NTAPI ExInitializeReadWriteLock(ERWLOCK *);
void NTAPI ExAcquireReadWriteLockExclusive(ERWLOCK *);
void NTAPI ExAcquireReadWriteLockShared(ERWLOCK *);
void NTAPI ExReleaseReadWriteLock(ERWLOCK *);

static HANDLE open_file(const char *xpath, bool async)
{
    OBJECT_STRING name = { (USHORT)strlen(xpath), (USHORT)strlen(xpath) + 1, (char *)xpath };
    OBJECT_ATTRIBUTES oa = { NULL, &name, 0 };
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    NTSTATUS st = NtCreateFile(&h, 0xC0100000 /* GENERIC_READ|WRITE|SYNCHRONIZE */, &oa, &iosb, NULL, 0x80, 0,
                               FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE | (async ? 0 : FILE_SYNCHRONOUS_IO_NONALERT));
    CHECK(NT_SUCCESS(st));
    return h;
}

static void test_scatter_gather(void)
{
    HANDLE h = open_file("\\Device\\Harddisk0\\Partition1\\scatter.bin", false);
    uint8_t *pages = aligned_alloc(4096, 3 * 4096);
    for (int i = 0; i < 3 * 4096; i++) pages[i] = (uint8_t)(i * 7 + 3);
    /* Pages handed over out of order: the segments decide the file order. */
    ULONGLONG segs[3] = { (ULONG)(pages + 2 * 4096), (ULONG)pages, (ULONG)(pages + 4096) };
    IO_STATUS_BLOCK iosb = { 0 };
    LARGE_INTEGER at = { .QuadPart = 0 };
    NTSTATUS st = NtWriteFileGather(h, NULL, NULL, NULL, &iosb, segs, 3 * 4096, &at);
    CHECK(st == STATUS_SUCCESS && iosb.Status == STATUS_SUCCESS && iosb.Information == 3 * 4096);

    uint8_t *back = aligned_alloc(4096, 3 * 4096);
    memset(back, 0, 3 * 4096);
    ULONGLONG rsegs[3] = { (ULONG)(back + 4096), (ULONG)(back + 2 * 4096), (ULONG)back };
    memset(&iosb, 0, sizeof iosb);
    st = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, rsegs, 3 * 4096, &at);
    CHECK(st == STATUS_SUCCESS && iosb.Information == 3 * 4096);
    /* File page k landed in the buffer of segment k. */
    CHECK(!memcmp(back + 4096, pages + 2 * 4096, 4096));
    CHECK(!memcmp(back + 2 * 4096, pages, 4096));
    CHECK(!memcmp(back, pages + 4096, 4096));

    /* A read that runs past the end returns what there is. */
    at.QuadPart = 2 * 4096;
    memset(&iosb, 0, sizeof iosb);
    st = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, rsegs, 2 * 4096, &at);
    CHECK(st == STATUS_SUCCESS && iosb.Information == 4096);
    /* ...and one that starts there reports the end of the file. */
    at.QuadPart = 3 * 4096;
    st = NtReadFileScatter(h, NULL, NULL, NULL, &iosb, rsegs, 4096, &at);
    CHECK(st == STATUS_END_OF_FILE);
    NtClose(h);
    free(pages);
    free(back);
}

static void test_completion_port(void)
{
    HANDLE port;
    CHECK(NtCreateIoCompletion(&port, 0, NULL, 0) == STATUS_SUCCESS);
    PVOID key, ctx;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER zero = { .QuadPart = 0 }, short_wait = { .QuadPart = -100000 /* 10 ms */ };
    CHECK(NtRemoveIoCompletion(port, &key, &ctx, &iosb, &zero) == STATUS_TIMEOUT);
    CHECK(NtRemoveIoCompletion(port, &key, &ctx, &iosb, &short_wait) == STATUS_TIMEOUT);

    /* PostQueuedCompletionStatus. */
    CHECK(NtSetIoCompletion(port, (PVOID)0x11, (PVOID)0x22, STATUS_SUCCESS, 33) == STATUS_SUCCESS);
    CHECK(NtSetIoCompletion(port, (PVOID)0x44, (PVOID)0x55, STATUS_SUCCESS, 66) == STATUS_SUCCESS);
    LONG depth = -1;
    CHECK(NtQueryIoCompletion(port, &depth) == STATUS_SUCCESS && depth == 2);
    CHECK(NtRemoveIoCompletion(port, &key, &ctx, &iosb, NULL) == STATUS_SUCCESS);
    CHECK(key == (PVOID)0x11 && ctx == (PVOID)0x22 && iosb.Information == 33);
    CHECK(NtRemoveIoCompletion(port, &key, &ctx, &iosb, NULL) == STATUS_SUCCESS);
    CHECK(key == (PVOID)0x44 && ctx == (PVOID)0x55 && iosb.Information == 66);

    /* CreateIoCompletionPort on an overlapped file, then ReadFile. */
    HANDLE f = open_file("\\Device\\Harddisk0\\Partition1\\port.bin", true);
    char data[100];
    memset(data, 'x', sizeof data);
    IO_STATUS_BLOCK wio;
    LARGE_INTEGER at = { .QuadPart = 0 };
    extern NTSTATUS NTAPI NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, LARGE_INTEGER *);
    NtWriteFile(f, NULL, NULL, NULL, &wio, data, sizeof data, &at);
    struct { HANDLE Port; PVOID Key; } info = { port, (PVOID)0x1234 };
    CHECK(NtSetInformationFile(f, &iosb, &info, sizeof info, 30) == STATUS_SUCCESS);
    CHECK(NtSetInformationFile(f, &iosb, &info, sizeof info, 30) == STATUS_INVALID_PARAMETER);
    /* The write above had no APC context: nothing was queued for it. */
    CHECK(NtQueryIoCompletion(port, &depth) == STATUS_SUCCESS && depth == 0);
    char buf[100];
    struct { IO_STATUS_BLOCK io; LARGE_INTEGER off; HANDLE ev; } ov = { 0 };
    NTSTATUS st = NtReadFile(f, NULL, NULL, &ov, &ov.io, buf, 60, &at);
    CHECK(st == STATUS_PENDING);
    CHECK(NtRemoveIoCompletion(port, &key, &ctx, &iosb, &short_wait) == STATUS_SUCCESS);
    CHECK(key == (PVOID)0x1234 && ctx == &ov && iosb.Status == STATUS_SUCCESS && iosb.Information == 60);
    NtClose(f);
    NtClose(port);
}

static KQUEUE q;
static LIST_ENTRY late_entry;

static void *late_insert(void *arg)
{
    (void)arg;
    usleep(20000);
    KeInsertQueue(&q, &late_entry);
    return NULL;
}

static void test_queue(void)
{
    KeInitializeQueue(&q, 0);
    LIST_ENTRY a, b, c;
    CHECK(KeInsertQueue(&q, &a) == 0);
    CHECK(KeInsertQueue(&q, &b) == 1);
    CHECK(KeInsertHeadQueue(&q, &c) == 2);
    CHECK(KeRemoveQueue(&q, 0, NULL) == &c);
    CHECK(KeRemoveQueue(&q, 0, NULL) == &a);
    CHECK(q.Header.SignalState == 1);
    /* Rundown hands back the rest as a ring through its first entry. */
    LIST_ENTRY d;
    KeInsertQueue(&q, &d);
    LIST_ENTRY *first = KeRundownQueue(&q);
    CHECK(first == &b && b.Flink == &d && d.Flink == &b && q.Header.SignalState == 0);
    CHECK(KeRundownQueue(&q) == NULL);
    LARGE_INTEGER zero = { .QuadPart = 0 };
    CHECK((ULONG_PTR)KeRemoveQueue(&q, 0, &zero) == STATUS_TIMEOUT);
    /* A waiter is woken by an insert from another thread. */
    pthread_t t;
    pthread_create(&t, NULL, late_insert, NULL);
    LARGE_INTEGER second = { .QuadPart = -10000000 };
    CHECK(KeRemoveQueue(&q, 0, &second) == &late_entry);
    pthread_join(t, NULL);
}

static ERWLOCK rw;
static volatile int writer_in, writer_seen;

static void *writer(void *arg)
{
    (void)arg;
    ExAcquireReadWriteLockExclusive(&rw);
    writer_in = writer_seen = 1;
    usleep(10000);
    writer_in = 0;
    ExReleaseReadWriteLock(&rw);
    return NULL;
}

static void test_rwlock(void)
{
    ExInitializeReadWriteLock(&rw);
    ExAcquireReadWriteLockShared(&rw);
    ExAcquireReadWriteLockShared(&rw);
    CHECK(rw.ReadersEntryCount == 2 && rw.LockCount == 1);
    pthread_t t;
    pthread_create(&t, NULL, writer, NULL);
    for (int i = 0; i < 2000 && rw.WritersWaitingCount != 1; i++) usleep(1000);
    CHECK(!writer_in && rw.WritersWaitingCount == 1);
    ExReleaseReadWriteLock(&rw);
    usleep(10000);
    CHECK(!writer_in);              /* one reader still holds it */
    ExReleaseReadWriteLock(&rw);
    /* Wait for the writer to get in (a loaded machine can take a while):
       taking a shared hold before it has would leave it waiting on us. */
    for (int i = 0; i < 2000 && !writer_seen; i++) usleep(1000);
    CHECK(writer_seen);
    /* A reader arriving while the writer holds it waits for it. */
    ExAcquireReadWriteLockShared(&rw);
    CHECK(!writer_in);
    pthread_join(t, NULL);
    ExReleaseReadWriteLock(&rw);
    CHECK(rw.LockCount == -1 && rw.ReadersEntryCount == 0 && rw.WritersWaitingCount == 0);
    /* Exclusive, uncontended. */
    ExAcquireReadWriteLockExclusive(&rw);
    CHECK(rw.LockCount == 0);
    ExReleaseReadWriteLock(&rw);
    CHECK(rw.LockCount == -1);
}

static void test_interlocked(void)
{
    LARGE_INTEGER v = { .QuadPart = 0xFFFFFFFFLL };
    CHECK(ExInterlockedAddLargeInteger(&v, 1, 0, NULL) == 0xFFFFFFFFLL);
    CHECK(v.QuadPart == 0x100000000LL);
    CHECK(ExInterlockedAddLargeInteger(&v, (ULONG)-2, -1, NULL) == 0x100000000LL);
    CHECK(v.QuadPart == 0xFFFFFFFELL);

    LIST_ENTRY head = { &head, &head }, a, b, c;
    CHECK(ExfInterlockedInsertTailList(&head, &a) == NULL);
    CHECK(ExfInterlockedInsertTailList(&head, &b) == &a);
    CHECK(ExfInterlockedInsertHeadList(&head, &c) == &a);
    CHECK(ExfInterlockedRemoveHeadList(&head) == &c);
    CHECK(ExfInterlockedRemoveHeadList(&head) == &a);
    CHECK(ExfInterlockedRemoveHeadList(&head) == &b);
    CHECK(ExfInterlockedRemoveHeadList(&head) == NULL);
}

static void test_queries(void)
{
    HANDLE m, s, t;
    CHECK(NtCreateMutant(&m, NULL, 0) == STATUS_SUCCESS);
    struct { LONG count; BOOLEAN owned, abandoned; } mi;
    CHECK(NtQueryMutant(m, &mi) == STATUS_SUCCESS && mi.count == 1 && !mi.owned && !mi.abandoned);
    CHECK(NtCreateSemaphore(&s, NULL, 2, 5) == STATUS_SUCCESS);
    struct { LONG count, max; } si;
    CHECK(NtQuerySemaphore(s, &si) == STATUS_SUCCESS && si.count == 2 && si.max == 5);
    CHECK(NtQueryMutant(s, &mi) == STATUS_OBJECT_TYPE_MISMATCH);
    CHECK(NtCreateTimer(&t, NULL, 0) == STATUS_SUCCESS);
    LARGE_INTEGER due = { .QuadPart = -50000000 };   /* 5 s */
    CHECK(NtSetTimerEx(t, &due, NULL, 0, NULL, 0, 0, NULL) == STATUS_SUCCESS);
    struct { LARGE_INTEGER remaining; BOOLEAN state; } ti;
    CHECK(NtQueryTimer(t, &ti) == STATUS_SUCCESS && !ti.state);
    CHECK(ti.remaining.QuadPart > 40000000 && ti.remaining.QuadPart <= 50000000);
    NtClose(m); NtClose(s); NtClose(t);
}

static ULONGLONG read_ksystem_time(volatile KSYSTEM_TIME *k)
{
    for (;;) {
        LONG hi = k->High1Time;
        ULONG lo = k->LowPart;
        if (hi == k->High2Time) return (ULONGLONG)(ULONG)hi << 32 | lo;
    }
}

static void test_clock_data(void)
{
    usleep(5000);
    LARGE_INTEGER now;
    KeQuerySystemTime(&now);
    ULONGLONG sys = read_ksystem_time(&KeSystemTime);
    CHECK(sys <= (ULONGLONG)now.QuadPart && (ULONGLONG)now.QuadPart - sys < 1000000);   /* within 100 ms */
    ULONGLONG it0 = read_ksystem_time(&KeInterruptTime);
    usleep(20000);
    ULONGLONG it1 = read_ksystem_time(&KeInterruptTime);
    CHECK(it1 > it0 && it1 <= KeQueryInterruptTime());
}


/* ---- the driver model ---------------------------------------------------- */

NTSTATUS NTAPI IoCreateDevice(DRIVER_OBJECT *, ULONG, OBJECT_STRING *, ULONG, BOOLEAN, DEVICE_OBJECT **);
void NTAPI IoDeleteDevice(DEVICE_OBJECT *);
IRP *NTAPI IoAllocateIrp(CCHAR);
void NTAPI IoFreeIrp(IRP *);
IRP *NTAPI IoBuildSynchronousFsdRequest(ULONG, DEVICE_OBJECT *, PVOID, ULONG, LARGE_INTEGER *, KEVENT *,
                                        IO_STATUS_BLOCK *);
IRP *NTAPI IoBuildAsynchronousFsdRequest(ULONG, DEVICE_OBJECT *, PVOID, ULONG, LARGE_INTEGER *, IO_STATUS_BLOCK *);
NTSTATUS FASTCALL IofCallDriver(DEVICE_OBJECT *, IRP *);
void FASTCALL IofCompleteRequest(IRP *, CCHAR);
NTSTATUS NTAPI IoSynchronousFsdRequest(ULONG, DEVICE_OBJECT *, PVOID, ULONG, LARGE_INTEGER *);
NTSTATUS NTAPI IoSynchronousDeviceIoControlRequest(ULONG, DEVICE_OBJECT *, PVOID, ULONG, PVOID, ULONG, ULONG *,
                                                   BOOLEAN);
NTSTATUS NTAPI IoInvalidDeviceRequest(DEVICE_OBJECT *, IRP *);
void NTAPI IoStartPacket(DEVICE_OBJECT *, IRP *, ULONG *);
void NTAPI IoStartNextPacket(DEVICE_OBJECT *);
NTSTATUS NTAPI IoCheckShareAccess(ACCESS_MASK, ULONG, FILE_OBJECT *, SHARE_ACCESS *, BOOLEAN);
void NTAPI IoSetShareAccess(ACCESS_MASK, ULONG, FILE_OBJECT *, SHARE_ACCESS *);
void NTAPI IoRemoveShareAccess(FILE_OBJECT *, SHARE_ACCESS *);
void NTAPI KeInitializeDeviceQueue(KDEVICE_QUEUE *);
BOOLEAN NTAPI KeInsertDeviceQueue(KDEVICE_QUEUE *, KDEVICE_QUEUE_ENTRY *);
BOOLEAN NTAPI KeInsertByKeyDeviceQueue(KDEVICE_QUEUE *, KDEVICE_QUEUE_ENTRY *, ULONG);
KDEVICE_QUEUE_ENTRY *NTAPI KeRemoveDeviceQueue(KDEVICE_QUEUE *);
KDEVICE_QUEUE_ENTRY *NTAPI KeRemoveByKeyDeviceQueue(KDEVICE_QUEUE *, ULONG);
BOOLEAN NTAPI KeRemoveEntryDeviceQueue(KDEVICE_QUEUE *, KDEVICE_QUEUE_ENTRY *);
void NTAPI KeInitializeEvent(KEVENT *, ULONG, BOOLEAN);
NTSTATUS NTAPI ObReferenceObjectByName(OBJECT_STRING *, ULONG, OBJECT_TYPE *, PVOID, PVOID *);
NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE, OBJECT_TYPE *, PVOID *);
NTSTATUS NTAPI NtCreateDirectoryObject(HANDLE *, OBJECT_ATTRIBUTES *);
NTSTATUS NTAPI NtOpenDirectoryObject(HANDLE *, OBJECT_ATTRIBUTES *);
NTSTATUS NTAPI NtQueryDirectoryObject(HANDLE, PVOID, ULONG, BOOLEAN, ULONG *, ULONG *);
NTSTATUS NTAPI IoCreateSymbolicLink(OBJECT_STRING *, OBJECT_STRING *);
NTSTATUS NTAPI IoDeleteSymbolicLink(OBJECT_STRING *);
void NTAPI READ_PORT_BUFFER_USHORT(USHORT *, USHORT *, ULONG);

static OBJECT_STRING ostr(const char *s)
{
    OBJECT_STRING o = { (USHORT)strlen(s), (USHORT)strlen(s) + 1, (char *)s };
    return o;
}

/* A test driver: reads fill the buffer with a byte pattern (the low byte of
   the offset), writes are counted; IOCTL 0x1234 pends the packet, which
   another thread completes. */
static int test_writes;

static void *complete_later(void *arg)
{
    IRP *irp = arg;
    usleep(20000);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 77;
    IofCompleteRequest(irp, 0);
    return NULL;
}

static NTSTATUS NTAPI test_dispatch(DEVICE_OBJECT *dev, IRP *irp)
{
    IO_STACK_LOCATION *sp = IoGetCurrentIrpStackLocation(irp);
    CHECK(sp->DeviceObject == dev);
    NTSTATUS st = STATUS_SUCCESS;
    ULONG info = 0;
    switch (sp->MajorFunction) {
    case IRP_MJ_READ:
        memset(irp->UserBuffer, (int)(sp->Parameters.Read.ByteOffset.LowPart & 0xFF), sp->Parameters.Read.Length);
        info = sp->Parameters.Read.Length;
        break;
    case IRP_MJ_WRITE:
        test_writes++;
        info = sp->Parameters.Write.Length;
        break;
    case IRP_MJ_DEVICE_CONTROL:
        if (sp->Parameters.DeviceIoControl.IoControlCode == 0x1234) {
            sp->Control |= SL_PENDING_RETURNED;   /* IoMarkIrpPending */
            pthread_t t;
            pthread_create(&t, NULL, complete_later, irp);
            pthread_detach(t);
            return STATUS_PENDING;
        }
        /* fall through */
    default:
        return IoInvalidDeviceRequest(dev, irp);
    }
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IofCompleteRequest(irp, 0);
    return st;
}

static int completions;

static NTSTATUS NTAPI keep_irp(DEVICE_OBJECT *dev, IRP *irp, PVOID ctx)
{
    completions++;
    CHECK(dev == NULL && ctx == (PVOID)0x5150);
    CHECK(irp->IoStatus.Status == STATUS_SUCCESS && irp->IoStatus.Information == 16);
    IoFreeIrp(irp);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static IRP *started[4];
static int nstarted;

static void NTAPI test_start_io(DEVICE_OBJECT *dev, IRP *irp)
{
    started[nstarted++] = irp;
    CHECK(dev->CurrentIrp == irp);
}

static void test_irp_path(void)
{
    static DRIVER_OBJECT drv;
    for (int i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) drv.MajorFunction[i] = (PVOID)test_dispatch;
    drv.DriverStartIo = (PVOID)test_start_io;
    OBJECT_STRING name = ostr("\\Device\\KernelTest0");
    DEVICE_OBJECT *dev = NULL;
    CHECK(IoCreateDevice(&drv, 32, &name, 0x22, 0, &dev) == STATUS_SUCCESS && dev);
    CHECK(dev->Type == IO_TYPE_DEVICE && dev->DriverObject == &drv && dev->StackSize == 1);
    CHECK(dev->DeviceExtension == dev + 1 && dev->MountedOrSelfDevice == dev);
    CHECK((dev->Flags & DO_DEVICE_HAS_NAME) && OBJECT_TO_OBJECT_HEADER(dev)->Type == &IoDeviceObjectType);
    DEVICE_OBJECT *again;
    CHECK(IoCreateDevice(&drv, 0, &name, 0x22, 0, &again) == STATUS_OBJECT_NAME_COLLISION && !again);
    PVOID found = NULL;
    CHECK(ObReferenceObjectByName(&name, 0, &IoDeviceObjectType, NULL, &found) == STATUS_SUCCESS && found == dev);
    ObfDereferenceObject(found);

    /* A synchronous FSD read through the driver. */
    KEVENT ev;
    KeInitializeEvent(&ev, 0, 0);
    IO_STATUS_BLOCK iosb = { 0 };
    char buf[64];
    LARGE_INTEGER at = { .QuadPart = 0x42 };
    IRP *irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, dev, buf, sizeof buf, &at, &ev, &iosb);
    CHECK(irp && irp->StackCount == 1 && irp->CurrentLocation == 2 && irp->UserEvent == &ev);
    CHECK(IoGetNextIrpStackLocation(irp)->MajorFunction == IRP_MJ_READ);
    CHECK(IoGetNextIrpStackLocation(irp)->Parameters.Read.ByteOffset.QuadPart == 0x42);
    CHECK(IofCallDriver(dev, irp) == STATUS_SUCCESS);
    CHECK(iosb.Status == STATUS_SUCCESS && iosb.Information == sizeof buf && ev.Header.SignalState == 1);
    CHECK(buf[0] == 0x42 && buf[63] == 0x42);

    /* The same through IoSynchronousFsdRequest, and a write. */
    at.QuadPart = 0x17;
    CHECK(IoSynchronousFsdRequest(IRP_MJ_READ, dev, buf, 8, &at) == STATUS_SUCCESS && buf[7] == 0x17);
    CHECK(IoSynchronousFsdRequest(IRP_MJ_WRITE, dev, buf, 8, &at) == STATUS_SUCCESS && test_writes == 1);
    /* An unsupported request fails without pending; nothing is signaled. */
    ULONG got = 99;
    CHECK(IoSynchronousDeviceIoControlRequest(0x99, dev, NULL, 0, buf, 4, &got, 0) == STATUS_INVALID_DEVICE_REQUEST);
    /* A pended request completed from another thread is waited for. */
    CHECK(IoSynchronousDeviceIoControlRequest(0x1234, dev, NULL, 0, buf, 4, &got, 0) == STATUS_SUCCESS && got == 77);

    /* A completion routine that keeps the packet. */
    IRP *a = IoBuildAsynchronousFsdRequest(IRP_MJ_READ, dev, buf, 16, &at, NULL);
    IO_STACK_LOCATION *sp = IoGetNextIrpStackLocation(a);
    sp->CompletionRoutine = (PVOID)keep_irp;
    sp->Context = (PVOID)0x5150;
    sp->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR;
    CHECK(IofCallDriver(dev, a) == STATUS_SUCCESS && completions == 1);

    /* Start-I/O: the first packet starts, the rest queue until the driver
       asks for the next. */
    IRP *p1 = IoAllocateIrp(1), *p2 = IoAllocateIrp(1), *p3 = IoAllocateIrp(1);
    ULONG key = 5;
    IoStartPacket(dev, p1, NULL);
    IoStartPacket(dev, p2, &key);
    key = 2;
    IoStartPacket(dev, p3, &key);
    CHECK(nstarted == 1 && started[0] == p1 && dev->DeviceQueue.Busy);
    IoStartNextPacket(dev);
    CHECK(nstarted == 2 && started[1] == p3);   /* lower key first */
    IoStartNextPacket(dev);
    CHECK(nstarted == 3 && started[2] == p2);
    IoStartNextPacket(dev);
    CHECK(nstarted == 3 && !dev->DeviceQueue.Busy && !dev->CurrentIrp);
    IoFreeIrp(p1); IoFreeIrp(p2); IoFreeIrp(p3);

    /* Deleting the device takes its name away. */
    IoDeleteDevice(dev);
    CHECK(ObReferenceObjectByName(&name, 0, &IoDeviceObjectType, NULL, &found) == STATUS_OBJECT_NAME_NOT_FOUND);
}

/* IRPs to the kernel's own hard disk, by name and through a \?? link. */
static void test_kernel_device(void)
{
    OBJECT_STRING name = ostr("\\Device\\Harddisk0\\Partition0");
    DEVICE_OBJECT *disk = NULL;
    CHECK(ObReferenceObjectByName(&name, 0, &IoDeviceObjectType, NULL, (PVOID *)&disk) == STATUS_SUCCESS && disk);
    CHECK(disk && disk->Type == IO_TYPE_DEVICE && disk->DeviceType == FILE_DEVICE_DISK && disk->SectorSize == 512);
    char out[512], in[512];
    for (int i = 0; i < 512; i++) out[i] = (char)(i * 3 + 1);
    LARGE_INTEGER at = { .QuadPart = 8 * 512 };
    CHECK(IoSynchronousFsdRequest(IRP_MJ_WRITE, disk, out, sizeof out, &at) == STATUS_SUCCESS);
    memset(in, 0, sizeof in);
    CHECK(IoSynchronousFsdRequest(IRP_MJ_READ, disk, in, sizeof in, &at) == STATUS_SUCCESS);
    CHECK(!memcmp(in, out, sizeof in));
    ULONG geo[6] = { 0 }, got = 0;
    CHECK(IoSynchronousDeviceIoControlRequest(0x00070000 /* IOCTL_DISK_GET_DRIVE_GEOMETRY */, disk, NULL, 0,
                                              geo, sizeof geo, &got, 0) == STATUS_SUCCESS);
    CHECK(got == 24 && geo[5] == 512);
    ObfDereferenceObject(disk);

    OBJECT_STRING link = ostr("\\??\\K:"), target = ostr("\\Device\\CdRom0");
    CHECK(IoCreateSymbolicLink(&link, &target) == STATUS_SUCCESS);
    PVOID cd = NULL;
    CHECK(ObReferenceObjectByName(&link, 0, &IoDeviceObjectType, NULL, &cd) == STATUS_SUCCESS);
    CHECK(cd && ((DEVICE_OBJECT *)cd)->DeviceType == FILE_DEVICE_CD_ROM);
    PVOID sym = NULL;
    CHECK(ObReferenceObjectByName(&link, 0, &ObSymbolicLinkObjectType, NULL, &sym) == STATUS_SUCCESS && sym && sym != cd);
    CHECK(ObReferenceObjectByName(&link, 0, &ObDirectoryObjectType, NULL, &sym) == STATUS_OBJECT_TYPE_MISMATCH);
    OBJECT_STRING none = ostr("\\Device\\NoSuchDevice"), nopath = ostr("\\NoDir\\Thing");
    CHECK(ObReferenceObjectByName(&none, 0, NULL, NULL, &sym) == STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(ObReferenceObjectByName(&nopath, 0, NULL, NULL, &sym) == STATUS_OBJECT_PATH_NOT_FOUND);
    IoDeleteSymbolicLink(&link);
}

static void test_share_access(void)
{
    FILE_OBJECT a = { 0 }, b = { 0 }, c = { 0 };
    SHARE_ACCESS sa;
    IoSetShareAccess(FILE_READ_DATA, FILE_SHARE_READ, &a, &sa);
    CHECK(sa.OpenCount == 1 && sa.Readers == 1 && sa.SharedRead == 1 && a.ReadAccess && !a.WriteAccess);
    /* A writer is refused: the first opener did not share writing. */
    CHECK(IoCheckShareAccess(FILE_WRITE_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &b, &sa, 1) == STATUS_SHARING_VIOLATION);
    CHECK(sa.OpenCount == 1);
    /* Another reader sharing reads is let in and counted. */
    CHECK(IoCheckShareAccess(FILE_READ_DATA, FILE_SHARE_READ, &c, &sa, 1) == STATUS_SUCCESS);
    CHECK(sa.OpenCount == 2 && sa.Readers == 2 && sa.SharedRead == 2);
    /* A reader that does not share reading is refused. */
    CHECK(IoCheckShareAccess(FILE_READ_DATA, 0, &b, &sa, 0) == STATUS_SHARING_VIOLATION);
    IoRemoveShareAccess(&c, &sa);
    IoRemoveShareAccess(&a, &sa);
    CHECK(sa.OpenCount == 0 && sa.Readers == 0 && sa.SharedRead == 0);
    /* Attribute-only access is not counted at all. */
    CHECK(IoCheckShareAccess(0x80 /* FILE_READ_ATTRIBUTES */, 0, &b, &sa, 1) == STATUS_SUCCESS && sa.OpenCount == 0);
}

static void test_device_queue(void)
{
    KDEVICE_QUEUE q;
    KDEVICE_QUEUE_ENTRY a, b, c, d;
    KeInitializeDeviceQueue(&q);
    CHECK(!KeInsertDeviceQueue(&q, &a) && q.Busy && !a.Inserted);   /* idle: start it now */
    CHECK(KeInsertByKeyDeviceQueue(&q, &b, 10) && b.Inserted);
    CHECK(KeInsertByKeyDeviceQueue(&q, &c, 3));
    CHECK(KeInsertByKeyDeviceQueue(&q, &d, 10));                     /* after the other 10 */
    CHECK(KeRemoveByKeyDeviceQueue(&q, 4) == &b && !b.Inserted);     /* first key >= 4 */
    CHECK(KeRemoveEntryDeviceQueue(&q, &c) && !KeRemoveEntryDeviceQueue(&q, &c));
    CHECK(KeRemoveByKeyDeviceQueue(&q, 50) == &d);                   /* none that high: the head */
    CHECK(KeRemoveDeviceQueue(&q) == NULL && !q.Busy);
}

static void test_directories(void)
{
    OBJECT_STRING dname = ostr("\\KernelTestDir"), sname = ostr("Sub");
    OBJECT_ATTRIBUTES oa = { NULL, &dname, 0 };
    HANDLE dir, sub, again;
    CHECK(NtCreateDirectoryObject(&dir, &oa) == STATUS_SUCCESS);
    CHECK(NtCreateDirectoryObject(&again, &oa) == STATUS_OBJECT_NAME_COLLISION && !again);
    OBJECT_ATTRIBUTES soa = { dir, &sname, 0 };
    CHECK(NtCreateDirectoryObject(&sub, &soa) == STATUS_SUCCESS);
    PVOID body;
    CHECK(ObReferenceObjectByHandle(sub, &ObDirectoryObjectType, &body) == STATUS_SUCCESS);
    ObfDereferenceObject(body);
    CHECK(ObReferenceObjectByHandle(sub, &IoDeviceObjectType, &body) == STATUS_OBJECT_TYPE_MISMATCH);

    /* Listing: the one entry, then the end. */
    HANDLE open;
    CHECK(NtOpenDirectoryObject(&open, &oa) == STATUS_SUCCESS);
    union { OBJECT_DIRECTORY_INFORMATION info; char raw[64]; } u;
    ULONG ctx = 0, len = 0;
    CHECK(NtQueryDirectoryObject(open, &u, sizeof u, 1, &ctx, &len) == STATUS_SUCCESS);
    CHECK(u.info.Name.Length == 3 && !memcmp(u.info.Name.Buffer, "Sub", 3) && ctx == 1 && len == sizeof u.info + 3);
    CHECK(u.info.Type == ObDirectoryObjectType.PoolTag);
    CHECK(NtQueryDirectoryObject(open, &u, sizeof u, 0, &ctx, &len) == STATUS_NO_MORE_ENTRIES);
    CHECK(NtQueryDirectoryObject(open, &u, 4, 1, &ctx, &len) == STATUS_INVALID_PARAMETER);

    /* \Device holds the kernel's devices. */
    OBJECT_STRING devname = ostr("\\Device");
    OBJECT_ATTRIBUTES doa = { NULL, &devname, 0 };
    HANDLE devdir;
    CHECK(NtOpenDirectoryObject(&devdir, &doa) == STATUS_SUCCESS);
    bool cdrom = false, harddisk = false;
    ctx = 0;
    for (int i = 0; i < 64 && NtQueryDirectoryObject(devdir, &u, sizeof u, i == 0, &ctx, &len) == STATUS_SUCCESS; i++) {
        if (u.info.Name.Length == 6 && !memcmp(u.info.Name.Buffer, "CdRom0", 6))
            cdrom = u.info.Type == IoDeviceObjectType.PoolTag;
        if (u.info.Name.Length == 9 && !memcmp(u.info.Name.Buffer, "Harddisk0", 9)) harddisk = true;
    }
    CHECK(cdrom && harddisk);
    NtClose(devdir);

    /* A directory is temporary: when its last handle closes, so does its name. */
    NtClose(sub);
    NtClose(open);
    NtClose(dir);
    CHECK(NtOpenDirectoryObject(&open, &oa) == STATUS_OBJECT_NAME_NOT_FOUND);
}

static void test_ports(void)
{
    USHORT w[3] = { 1, 2, 3 };
    READ_PORT_BUFFER_USHORT((USHORT *)0x1F0, w, 3);
    CHECK(w[0] == 0xFFFF && w[2] == 0xFFFF);
}

int main(void)
{
    /* The hard disk's root: $KERNEL_TEST_DIR, or a new directory in /tmp. */
    char dir[4096] = "/tmp/kernel_test.XXXXXX";
    const char *tmp = getenv("KERNEL_TEST_DIR");
    if (tmp) snprintf(dir, sizeof dir, "%s", tmp);
    else if (!mkdtemp(dir)) return 2;
    g_log = stderr;
    mem_init();
    thread_init_main();
    timers_init();
    /* D: is the directory of the title's XBE: an empty one will do. */
    char xbe[4200];
    snprintf(xbe, sizeof xbe, "%s/default.xbe", dir);
    FILE *fx = fopen(xbe, "w");
    if (fx) fclose(fx);
    fs_init(xbe, dir, NULL, NULL);

    test_scatter_gather();
    test_completion_port();
    test_queue();
    test_rwlock();
    test_interlocked();
    test_queries();
    test_clock_data();
    test_device_queue();
    test_share_access();
    test_irp_path();
    test_kernel_device();
    test_directories();
    test_ports();

    printf("kernel_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
