/*
 * kernel_test: kernel exports without a title.
 *
 * Covers NtReadFileScatter/NtWriteFileGather, I/O completion ports (posted
 * packets and overlapped file reads bound to a port), KQUEUE, the
 * reader/writer lock, the Exf interlocked lists, ExInterlockedAddLargeInteger,
 * NtQueryMutant/Semaphore/Timer and the KeSystemTime/KeInterruptTime data
 * exports.  It links against xbcompat's own objects (all but main.o) and
 * runs the kernel on host threads.
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
static volatile int writer_in;

static void *writer(void *arg)
{
    (void)arg;
    ExAcquireReadWriteLockExclusive(&rw);
    writer_in = 1;
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
    usleep(20000);
    CHECK(!writer_in && rw.WritersWaitingCount == 1);
    ExReleaseReadWriteLock(&rw);
    usleep(10000);
    CHECK(!writer_in);              /* one reader still holds it */
    ExReleaseReadWriteLock(&rw);
    usleep(5000);
    CHECK(writer_in);
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

    printf("kernel_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
