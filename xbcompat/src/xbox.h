/*
 * Xbox kernel types and structure layouts, as the guest code sees them.
 *
 * Layouts follow the leak's private/ntos/inc (ke.h, ps.h, i386.h, ntosdef.h).
 * Everything here is built with -m32, so pointers are 4 bytes like on the
 * console.  64-bit fields are explicitly 8-byte aligned because MSVC aligns
 * them that way and GCC's i386 ABI does not.
 */
#ifndef XBCOMPAT_XBOX_H
#define XBCOMPAT_XBOX_H

#include <stddef.h>
#include <stdint.h>

/*
 * Calling conventions of functions the guest calls.  MSVC only keeps the
 * stack 4-byte aligned, while GCC-built host libraries (SDL, Mesa) use SSE
 * spills that assume 16, so every entry point from the guest realigns.
 */
#define NTAPI __attribute__((stdcall, force_align_arg_pointer))
#define FASTCALL __attribute__((fastcall, force_align_arg_pointer))
#define CDECLAPI __attribute__((cdecl, force_align_arg_pointer))

typedef uint8_t UCHAR, BOOLEAN, KIRQL, KPROCESSOR_MODE;
typedef int8_t CHAR, CCHAR, SCHAR;
typedef uint16_t USHORT, WCHAR;
typedef int16_t SHORT, CSHORT;
typedef uint32_t ULONG, DWORD, ACCESS_MASK, SIZE_T, ULONG_PTR;
typedef int32_t LONG, NTSTATUS, LONG_PTR;
typedef void *PVOID, *HANDLE;
typedef int64_t LONGLONG __attribute__((aligned(8)));
typedef uint64_t ULONGLONG __attribute__((aligned(8)));

typedef union {
    struct { ULONG LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;
typedef union {
    struct { ULONG LowPart; ULONG HighPart; };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;

_Static_assert(sizeof(LARGE_INTEGER) == 8 && _Alignof(LARGE_INTEGER) == 8, "LARGE_INTEGER");

typedef struct LIST_ENTRY {
    struct LIST_ENTRY *Flink, *Blink;
} LIST_ENTRY;

typedef struct {
    USHORT Length;
    USHORT MaximumLength;
    char *Buffer;
} ANSI_STRING, OBJECT_STRING;

typedef struct {
    USHORT Length;
    USHORT MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING;

typedef struct {
    HANDLE RootDirectory;
    OBJECT_STRING *ObjectName;
    ULONG Attributes;
} OBJECT_ATTRIBUTES;

typedef struct {
    union { NTSTATUS Status; PVOID Pointer; };
    ULONG_PTR Information;
} IO_STATUS_BLOCK;

/* Dispatcher objects. */
enum {
    EventNotificationObject = 0,
    EventSynchronizationObject = 1,
    MutantObject = 2,
    ProcessObject = 3,
    QueueObject = 4,
    SemaphoreObject = 5,
    ThreadObject = 6,
    TimerNotificationObject = 8,
    TimerSynchronizationObject = 9,
    ApcObject = 18,
    DpcObject = 19,
};

typedef struct {
    UCHAR Type;
    UCHAR Absolute;
    UCHAR Size;
    UCHAR Inserted;
    LONG SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;

typedef struct { DISPATCHER_HEADER Header; } KEVENT;

typedef struct {
    DISPATCHER_HEADER Header;
    LONG Limit;
} KSEMAPHORE;

typedef struct KTHREAD KTHREAD;

typedef struct {
    DISPATCHER_HEADER Header;
    LIST_ENTRY MutantListEntry;
    KTHREAD *OwnerThread;
    BOOLEAN Abandoned;
} KMUTANT;

typedef struct KDPC {
    CSHORT Type;
    UCHAR Inserted;
    UCHAR Padding;
    LIST_ENTRY DpcListEntry;
    void (NTAPI *DeferredRoutine)(struct KDPC *, PVOID, PVOID, PVOID);
    PVOID DeferredContext;
    PVOID SystemArgument1;
    PVOID SystemArgument2;
} KDPC;

typedef struct {
    DISPATCHER_HEADER Header;
    ULARGE_INTEGER DueTime;
    LIST_ENTRY TimerListEntry;
    KDPC *Dpc;
    LONG Period;
} KTIMER;

typedef struct KWAIT_BLOCK {
    LIST_ENTRY WaitListEntry;
    KTHREAD *Thread;
    PVOID Object;
    struct KWAIT_BLOCK *NextWaitBlock;
    USHORT WaitKey;
    USHORT WaitType;
} KWAIT_BLOCK;

typedef struct {
    CSHORT Type;
    KPROCESSOR_MODE ApcMode;
    BOOLEAN Inserted;
    KTHREAD *Thread;
    LIST_ENTRY ApcListEntry;
    PVOID KernelRoutine, RundownRoutine, NormalRoutine, NormalContext;
    PVOID SystemArgument1, SystemArgument2;
} KAPC;

typedef struct {
    LIST_ENTRY ApcListHead[2];
    PVOID Process;
    BOOLEAN KernelApcInProgress;
    BOOLEAN KernelApcPending;
    BOOLEAN UserApcPending;
    BOOLEAN ApcQueueable;
} KAPC_STATE;

struct KTHREAD {
    DISPATCHER_HEADER Header;
    LIST_ENTRY MutantListHead;
    ULONG KernelTime;
    PVOID StackBase;
    PVOID StackLimit;
    PVOID KernelStack;
    PVOID TlsData;
    UCHAR State;
    BOOLEAN Alerted[2];
    BOOLEAN Alertable;
    UCHAR NpxState;
    CHAR Saturation;
    SCHAR Priority;
    UCHAR Padding;
    KAPC_STATE ApcState;
    ULONG ContextSwitches;
    LONG_PTR WaitStatus;
    KIRQL WaitIrql;
    KPROCESSOR_MODE WaitMode;
    BOOLEAN WaitNext;
    UCHAR WaitReason;
    KWAIT_BLOCK *WaitBlockList;
    LIST_ENTRY WaitListEntry;
    ULONG WaitTime;
    ULONG KernelApcDisable;
    LONG Quantum;
    SCHAR BasePriority;
    UCHAR DecrementCount;
    SCHAR PriorityDecrement;
    BOOLEAN DisableBoost;
    UCHAR NpxIrql;
    CCHAR SuspendCount;
    BOOLEAN Preempted;
    BOOLEAN HasTerminated;
    PVOID Queue;
    LIST_ENTRY QueueListEntry;
    KTIMER Timer;
    KWAIT_BLOCK TimerWaitBlock;
    KAPC SuspendApc;
    KSEMAPHORE SuspendSemaphore;
    LIST_ENTRY ThreadListEntry;
};

_Static_assert(offsetof(KTHREAD, TlsData) == 0x28, "KTHREAD.TlsData");

typedef struct {
    KTHREAD Tcb;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER ExitTime;
    NTSTATUS ExitStatus;
    LIST_ENTRY ReaperListEntry;
    HANDLE UniqueThread;
    PVOID StartAddress;
    LIST_ENTRY IrpList;
    PVOID DebugData;
} ETHREAD;

typedef struct {
    PVOID ExceptionList;
    PVOID StackBase;
    PVOID StackLimit;
    PVOID SubSystemTib;
    PVOID FiberData;
    PVOID ArbitraryUserPointer;
    PVOID Self;
} NT_TIB;

typedef struct {
    KTHREAD *CurrentThread;
    KTHREAD *NextThread;
    KTHREAD *IdleThread;
    KTHREAD *NpxThread;
    ULONG InterruptCount;
    ULONG DpcTime;
    ULONG InterruptTime;
    ULONG DebugDpcTime;
    ULONG KeContextSwitches;
    ULONG DpcInterruptRequested;
    LIST_ENTRY DpcListHead;
    ULONG DpcRoutineActive;
    PVOID DpcStack;
    ULONG QuantumEnd;
    UCHAR NpxSaveArea[0x210];
} KPRCB;

typedef struct {
    NT_TIB NtTib;
    PVOID SelfPcr;
    KPRCB *Prcb;
    KIRQL Irql;
    KPRCB PrcbData;
} KPCR;

_Static_assert(offsetof(KPCR, Irql) == 0x24, "KPCR.Irql");
_Static_assert(offsetof(KPCR, PrcbData) == 0x28, "KPCR.PrcbData");

typedef struct {
    ULONG Signature;
    UCHAR EncryptedDigest[256];
    ULONG BaseAddress;
    ULONG SizeOfHeaders;
    ULONG SizeOfImage;
    ULONG SizeOfImageHeader;
    ULONG TimeDateStamp;
    ULONG Certificate;
    ULONG NumberOfSections;
    ULONG SectionHeaders;
    ULONG InitFlags;
    ULONG AddressOfEntryPoint;
    ULONG TlsDirectory;
    ULONG SizeOfStackCommit;
    ULONG SizeOfHeapReserve;
    ULONG SizeOfHeapCommit;
    ULONG NtBaseOfDll;
    ULONG NtSizeOfImage;
    ULONG NtCheckSum;
    ULONG NtTimeDateStamp;
    ULONG DebugPathName;
    ULONG DebugFileName;
    ULONG DebugUnicodeFileName;
    ULONG XboxKernelThunkData;
    ULONG ImportDirectory;
    ULONG NumberOfLibraryVersions;
    ULONG LibraryVersions;
    ULONG XboxKernelLibraryVersion;
    ULONG XapiLibraryVersion;
    ULONG MicrosoftLogo;
    ULONG SizeOfMicrosoftLogo;
} XBE_HEADER;

_Static_assert(offsetof(XBE_HEADER, AddressOfEntryPoint) == 0x128, "XBE entry");
_Static_assert(offsetof(XBE_HEADER, XboxKernelThunkData) == 0x158, "XBE kthunk");

typedef struct {
    ULONG SectionFlags;
    ULONG VirtualAddress;
    ULONG VirtualSize;
    ULONG PointerToRawData;
    ULONG SizeOfRawData;
    ULONG SectionName;
    ULONG SectionReferenceCount;
    ULONG HeadSharedPageReferenceCount;
    ULONG TailSharedPageReferenceCount;
    UCHAR SectionDigest[20];
} XBE_SECTION;

#define XBE_SECTION_PRELOAD 0x00000002

typedef struct {
    ULONG SizeOfCertificate;
    ULONG TimeDateStamp;
    ULONG TitleID;
    WCHAR TitleName[40];
    ULONG AlternateTitleIDs[16];
    ULONG AllowedMediaTypes;
    ULONG GameRegion;
    ULONG GameRatings;
    ULONG DiskNumber;
    ULONG Version;
} XBE_CERTIFICATE;

#define XBE_BASE 0x00010000u

/* NTSTATUS values used by the kernel shims. */
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000)
#define STATUS_WAIT_0                   ((NTSTATUS)0x00000000)
#define STATUS_ABANDONED                ((NTSTATUS)0x00000080)
#define STATUS_USER_APC                 ((NTSTATUS)0x000000C0)
#define STATUS_ALERTED                  ((NTSTATUS)0x00000101)
#define STATUS_TIMEOUT                  ((NTSTATUS)0x00000102)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103)
#define STATUS_OBJECT_NAME_EXISTS       ((NTSTATUS)0x40000000)
#define STATUS_NO_MORE_FILES            ((NTSTATUS)0x80000006)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_HANDLE           ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000D)
#define STATUS_NO_SUCH_FILE             ((NTSTATUS)0xC000000F)
#define STATUS_END_OF_FILE              ((NTSTATUS)0xC0000011)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017)
#define STATUS_ACCESS_DENIED            ((NTSTATUS)0xC0000022)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_TYPE_MISMATCH     ((NTSTATUS)0xC0000024)
#define STATUS_OBJECT_NAME_INVALID      ((NTSTATUS)0xC0000033)
#define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION    ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_PATH_NOT_FOUND    ((NTSTATUS)0xC000003A)
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED ((NTSTATUS)0xC0000047)
#define STATUS_MUTANT_NOT_OWNED         ((NTSTATUS)0xC0000046)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009A)
#define STATUS_FILE_IS_A_DIRECTORY      ((NTSTATUS)0xC00000BA)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BB)
#define STATUS_DIRECTORY_NOT_EMPTY      ((NTSTATUS)0xC0000101)
#define STATUS_NOT_A_DIRECTORY          ((NTSTATUS)0xC0000103)
#define STATUS_UNRECOGNIZED_VOLUME      ((NTSTATUS)0xC000014F)

#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

#endif
