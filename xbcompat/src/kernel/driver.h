/*
 * The driver model as guest code sees it: object manager headers and types,
 * I/O request packets, device and driver objects, device queues
 * (private/ntos/inc/ob.h, io.h, ke.h).
 *
 * Routine pointers in these structures are guest addresses (PVOID here):
 * host code calls them through cpu.h on the emulated builds.
 */
#ifndef XBCOMPAT_DRIVER_H
#define XBCOMPAT_DRIVER_H

#include "../xbcompat.h"

/* io.h's #include "pshpack4.h" structures hold 64-bit fields 4-aligned. */
typedef LARGE_INTEGER LARGE_INTEGER_P4 __attribute__((aligned(4)));

/* ---- object manager ---------------------------------------------------- */

typedef struct OBJECT_TYPE {
    PVOID AllocateProcedure;    /* PVOID (SIZE_T, ULONG Tag) */
    PVOID FreeProcedure;        /* void (PVOID) */
    PVOID CloseProcedure;       /* void (PVOID Object, ULONG SystemHandleCount) */
    PVOID DeleteProcedure;      /* void (PVOID Object) */
    PVOID ParseProcedure;
    PVOID DefaultObject;
    ULONG PoolTag;
} OBJECT_TYPE;

typedef struct {
    LONG PointerCount;
    LONG HandleCount;
    OBJECT_TYPE *Type;
    ULONG Flags;
    /* the body follows */
} OBJECT_HEADER;

#define OB_FLAG_NAMED_OBJECT     0x01
#define OB_FLAG_PERMANENT_OBJECT 0x02
#define OB_FLAG_ATTACHED_OBJECT  0x04

typedef struct OBJECT_HEADER_NAME_INFO {
    struct OBJECT_HEADER_NAME_INFO *ChainLink;
    PVOID Directory;
    OBJECT_STRING Name;
} OBJECT_HEADER_NAME_INFO;

#define OB_NUMBER_HASH_BUCKETS 11
typedef struct { OBJECT_HEADER_NAME_INFO *HashBuckets[OB_NUMBER_HASH_BUCKETS]; } OBJECT_DIRECTORY;

typedef struct {
    OBJECT_STRING Name;
    ULONG Type;                 /* the object type's PoolTag */
} OBJECT_DIRECTORY_INFORMATION;

#define OBJ_INHERIT   0x00000002
#define OBJ_PERMANENT 0x00000010
#define OBJ_EXCLUSIVE 0x00000020
#define OBJ_OPENIF    0x00000080

#define OBJECT_TO_OBJECT_HEADER(o) ((OBJECT_HEADER *)(o) - 1)

extern OBJECT_TYPE ExEventObjectType, ExMutantObjectType, ExSemaphoreObjectType, ExTimerObjectType,
                   PsThreadObjectType, IoFileObjectType, IoDeviceObjectType, IoCompletionObjectType,
                   ObDirectoryObjectType, ObSymbolicLinkObjectType;

/* ---- device queues ----------------------------------------------------- */

typedef struct {
    CSHORT Type;
    UCHAR Size;
    BOOLEAN Busy;
    LIST_ENTRY DeviceListHead;
} KDEVICE_QUEUE;

typedef struct {
    LIST_ENTRY DeviceListEntry;
    ULONG SortKey;
    BOOLEAN Inserted;
} KDEVICE_QUEUE_ENTRY;

#define DeviceQueueObject 20

/* ---- device, driver and file objects ----------------------------------- */

#define IO_TYPE_DEVICE 3
#define IO_TYPE_IRP    6

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CLOSE                    0x01
#define IRP_MJ_READ                     0x02
#define IRP_MJ_WRITE                    0x03
#define IRP_MJ_QUERY_INFORMATION        0x04
#define IRP_MJ_SET_INFORMATION          0x05
#define IRP_MJ_FLUSH_BUFFERS            0x06
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x07
#define IRP_MJ_DIRECTORY_CONTROL        0x08
#define IRP_MJ_FILE_SYSTEM_CONTROL      0x09
#define IRP_MJ_DEVICE_CONTROL           0x0a
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0b
#define IRP_MJ_SHUTDOWN                 0x0c
#define IRP_MJ_CLEANUP                  0x0d
#define IRP_MJ_MAXIMUM_FUNCTION         0x0d

#define DO_EXCLUSIVE           0x00000002
#define DO_DIRECT_IO           0x00000004
#define DO_DEVICE_HAS_NAME     0x00000008
#define DO_DEVICE_INITIALIZING 0x00000010

#define DO_STARTIO_BUSY            0x01
#define DO_STARTIO_REQUESTED       0x02
#define DO_STARTIO_REQUESTED_BYKEY 0x04

#define FILE_DEVICE_CD_ROM       0x02
#define FILE_DEVICE_DISK         0x07
#define FILE_DEVICE_MEMORY_UNIT  0x38
#define FILE_DEVICE_MEDIA_BOARD  0x39

typedef struct DRIVER_OBJECT {
    PVOID DriverStartIo;        /* void (DEVICE_OBJECT *, IRP *) */
    PVOID DriverDeleteDevice;   /* void (DEVICE_OBJECT *) */
    PVOID DriverDismountVolume;
    PVOID MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];   /* NTSTATUS (DEVICE_OBJECT *, IRP *) */
} DRIVER_OBJECT;

struct IRP;

typedef struct DEVICE_OBJECT {
    CSHORT Type;
    USHORT Size;
    LONG ReferenceCount;
    DRIVER_OBJECT *DriverObject;
    struct DEVICE_OBJECT *MountedOrSelfDevice;
    struct IRP *CurrentIrp;
    ULONG Flags;
    PVOID DeviceExtension;
    UCHAR DeviceType;
    UCHAR StartIoFlags;
    CCHAR StackSize;
    BOOLEAN DeletePending;
    ULONG SectorSize;
    ULONG AlignmentRequirement;
    KDEVICE_QUEUE DeviceQueue;
    KEVENT DeviceLock;
    ULONG StartIoKey;
} DEVICE_OBJECT;

#define FO_SYNCHRONOUS_IO 0x00000001

typedef struct { PVOID Port; PVOID Key; } IO_COMPLETION_CONTEXT;

typedef struct __attribute__((packed, aligned(4))) FILE_OBJECT {
    CSHORT Type;
    BOOLEAN DeletePending : 1;
    BOOLEAN ReadAccess : 1;
    BOOLEAN WriteAccess : 1;
    BOOLEAN DeleteAccess : 1;
    BOOLEAN SharedRead : 1;
    BOOLEAN SharedWrite : 1;
    BOOLEAN SharedDelete : 1;
    BOOLEAN Reserved : 1;
    UCHAR Flags;
    DEVICE_OBJECT *DeviceObject;
    PVOID FsContext;
    PVOID FsContext2;
    NTSTATUS FinalStatus;
    LARGE_INTEGER_P4 CurrentByteOffset;
    struct FILE_OBJECT *RelatedFileObject;
    IO_COMPLETION_CONTEXT *CompletionContext;
    LONG LockCount;
    KEVENT Lock;
    KEVENT Event;
} FILE_OBJECT;

typedef struct {
    UCHAR OpenCount;
    UCHAR Readers;
    UCHAR Writers;
    UCHAR Deleters;
    UCHAR SharedRead;
    UCHAR SharedWrite;
    UCHAR SharedDelete;
} SHARE_ACCESS;

#define FILE_READ_DATA    0x0001
#define FILE_WRITE_DATA   0x0002
#define FILE_APPEND_DATA  0x0004
#define FILE_EXECUTE      0x0020
#define DELETE            0x00010000
#define FILE_SHARE_READ   0x00000001
#define FILE_SHARE_WRITE  0x00000002
#define FILE_SHARE_DELETE 0x00000004

/* ---- I/O request packets ----------------------------------------------- */

#define IRP_NOCACHE                  0x00000001
#define IRP_MOUNT_COMPLETION         0x00000002
#define IRP_SYNCHRONOUS_API          0x00000004
#define IRP_CREATE_OPERATION         0x00000008
#define IRP_READ_OPERATION           0x00000010
#define IRP_WRITE_OPERATION          0x00000020
#define IRP_CLOSE_OPERATION          0x00000040
#define IRP_DEFER_IO_COMPLETION      0x00000080
#define IRP_UNLOCK_USER_BUFFER       0x00000200
#define IRP_SCATTER_GATHER_OPERATION 0x00000400

#define SL_PENDING_RETURNED  0x01
#define SL_MUST_COMPLETE     0x02
#define SL_INVOKE_ON_CANCEL  0x20
#define SL_INVOKE_ON_SUCCESS 0x40
#define SL_INVOKE_ON_ERROR   0x80

typedef struct __attribute__((packed, aligned(4))) IO_STACK_LOCATION {
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    UCHAR Flags;
    UCHAR Control;
    union __attribute__((packed, aligned(4))) {
        struct { ACCESS_MASK DesiredAccess; ULONG Options; USHORT FileAttributes; USHORT ShareAccess;
                 OBJECT_STRING *RemainingName; } Create;
        struct __attribute__((packed, aligned(4))) { ULONG Length; union { ULONG BufferOffset; PVOID CacheBuffer; };
                 LARGE_INTEGER_P4 ByteOffset; } Read;
        struct __attribute__((packed, aligned(4))) { ULONG Length; union { ULONG BufferOffset; PVOID CacheBuffer; };
                 LARGE_INTEGER_P4 ByteOffset; } Write;
        struct { ULONG Length; ULONG FileInformationClass; } QueryFile;
        struct { ULONG Length; ULONG FsInformationClass; } QueryVolume;
        struct { ULONG OutputBufferLength; PVOID InputBuffer; ULONG InputBufferLength;
                 ULONG IoControlCode; } DeviceIoControl;
        struct { ULONG OutputBufferLength; PVOID InputBuffer; ULONG InputBufferLength;
                 ULONG FsControlCode; } FileSystemControl;
        struct { PVOID Argument1, Argument2, Argument3, Argument4; } Others;
    } Parameters;
    DEVICE_OBJECT *DeviceObject;
    FILE_OBJECT *FileObject;
    PVOID CompletionRoutine;    /* NTSTATUS (DEVICE_OBJECT *, IRP *, PVOID Context) */
    PVOID Context;
} IO_STACK_LOCATION;

typedef struct IRP {
    CSHORT Type;
    USHORT Size;
    ULONG Flags;
    LIST_ENTRY ThreadListEntry;
    IO_STATUS_BLOCK IoStatus;
    CHAR StackCount;
    CHAR CurrentLocation;
    BOOLEAN PendingReturned;
    BOOLEAN Cancel;
    IO_STATUS_BLOCK *UserIosb;
    KEVENT *UserEvent;
    union {
        struct { PVOID UserApcRoutine; PVOID UserApcContext; } AsynchronousParameters;
        LARGE_INTEGER AllocationSize;
    } Overlay;
    PVOID UserBuffer;
    ULONGLONG *SegmentArray;
    ULONG LockedBufferLength;
    union {
        struct {
            union {
                KDEVICE_QUEUE_ENTRY DeviceQueueEntry;
                PVOID DriverContext[5];
            };
            ETHREAD *Thread;
            struct {
                LIST_ENTRY ListEntry;
                union { IO_STACK_LOCATION *CurrentStackLocation; ULONG PacketType; };
            };
            FILE_OBJECT *OriginalFileObject;
        } Overlay;
        KAPC Apc;
        PVOID CompletionKey;
    } Tail;
} IRP;

#define IoGetCurrentIrpStackLocation(irp) ((irp)->Tail.Overlay.CurrentStackLocation)
#define IoGetNextIrpStackLocation(irp) ((irp)->Tail.Overlay.CurrentStackLocation - 1)
#define IoSizeOfIrp(n) ((USHORT)(sizeof(IRP) + (n) * sizeof(IO_STACK_LOCATION)))

_Static_assert(sizeof(OBJECT_HEADER) == 16, "OBJECT_HEADER");
_Static_assert(sizeof(DEVICE_OBJECT) == 0x48, "DEVICE_OBJECT");
_Static_assert(offsetof(DEVICE_OBJECT, DeviceQueue) == 0x28, "DEVICE_OBJECT.DeviceQueue");
_Static_assert(sizeof(DRIVER_OBJECT) == 0x44, "DRIVER_OBJECT");
_Static_assert(offsetof(FILE_OBJECT, CurrentByteOffset) == 0x14, "FILE_OBJECT.CurrentByteOffset");
_Static_assert(sizeof(FILE_OBJECT) == 0x48, "FILE_OBJECT");
_Static_assert(sizeof(IO_STACK_LOCATION) == 0x24, "IO_STACK_LOCATION");
_Static_assert(offsetof(IO_STACK_LOCATION, Parameters.Read.ByteOffset) == 0x0C, "IO_STACK_LOCATION.Read");
_Static_assert(offsetof(IO_STACK_LOCATION, DeviceObject) == 0x14, "IO_STACK_LOCATION.DeviceObject");
_Static_assert(offsetof(IRP, Overlay) == 0x28, "IRP.Overlay");
_Static_assert(offsetof(IRP, Tail) == 0x3C, "IRP.Tail");
_Static_assert(offsetof(IRP, Tail.Overlay.CurrentStackLocation) == 0x5C, "IRP.CurrentStackLocation");
_Static_assert(sizeof(IRP) == 0x68, "IRP");

#define STATUS_INVALID_DEVICE_REQUEST    ((NTSTATUS)0xC0000010)
#define STATUS_MORE_PROCESSING_REQUIRED  ((NTSTATUS)0xC0000016)
#define STATUS_SHARING_VIOLATION         ((NTSTATUS)0xC0000043)
#define STATUS_NO_MORE_ENTRIES           ((NTSTATUS)0x8000001A)

/* ---- between ob.c and irp.c -------------------------------------------- */

/* The object an Ob-created body belongs to (ObCreateObject), or NULL. */
xobject *ob_object_of(const void *body);
/* Look an object up by its full path, following symbolic links; the
   found body is referenced as ObReferenceObjectByName does. */
NTSTATUS ob_reference_by_path(const char *path, OBJECT_TYPE *type, PVOID *body);

NTSTATUS NTAPI ObCreateObject(OBJECT_TYPE *Type, OBJECT_ATTRIBUTES *oa, ULONG BodySize, PVOID *Object);
NTSTATUS NTAPI ObInsertObject(PVOID Object, OBJECT_ATTRIBUTES *oa, ULONG PointerBias, HANDLE *Handle);
void FASTCALL ObfDereferenceObject(PVOID Object);

#endif
