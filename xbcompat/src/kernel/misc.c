/* Hal, Av, Dbg, Ex settings, Xe section loading and the kernel's data exports. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../xbcompat.h"

int xvsnprintf(char *out, size_t size, const char *fmt, va_list ap);

/* ---- data exports ----------------------------------------------------- */

typedef struct { ULONG Flags; UCHAR GpuRevision, McpRevision, Unknown3, Unknown4; } XBOX_HARDWARE_INFO;
typedef struct { USHORT Major, Minor, Build, Qfe; } XBOX_KRNL_VERSION;

XBOX_HARDWARE_INFO XboxHardwareInfo = { 0x20 /* devkit */, 0xD4, 0xD4, 0, 0 };
XBOX_KRNL_VERSION XboxKrnlVersion = { 1, 0, 4400, 1 };
ULONG HalDiskCachePartitionCount = 3;
ULONG HalDiskModelNumber[2], HalDiskSerialNumber[2];
ULONG HalBootSMCVideoMode = 0;
BOOLEAN KdDebuggerEnabled = 0, KdDebuggerNotPresent = 1;
PVOID LaunchDataPage = NULL;
ULONG IdexChannelObject[32];
UCHAR XboxEEPROMKey[16], XboxHDKey[16], XboxSignatureKey[16], XboxLANKey[16];
UCHAR XboxAlternateSignatureKeys[16][16];
UCHAR XePublicKeyData[284];
OBJECT_STRING XeImageFileName;
static char image_file_name[] = "\\Device\\CdRom0\\default.xbe";
ULONG KiBugCheckData[5];
ULONG ExEventObjectTypeDummy;
UCHAR AvpCurrentSettings[64];

void misc_init(void)
{
    XeImageFileName.Buffer = image_file_name;
    XeImageFileName.Length = strlen(image_file_name);
    XeImageFileName.MaximumLength = XeImageFileName.Length + 1;
}

/* ---- debug output ----------------------------------------------------- */

ULONG CDECLAPI DbgPrint(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    xvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    xlog("[guest] %s", buf);
    return 0;
}

void NTAPI DbgBreakPoint(void) { xlog("DbgBreakPoint (ignored)"); }
void NTAPI DbgBreakPointWithStatus(ULONG st) { xlog("DbgBreakPointWithStatus(%#x) (ignored)", st); }
void NTAPI DbgLoadImageSymbols(PVOID a, PVOID b, ULONG c) { (void)a; (void)b; (void)c; }
void NTAPI DbgUnLoadImageSymbols(PVOID a, PVOID b, ULONG c) { (void)a; (void)b; (void)c; }
ULONG NTAPI DbgPrompt(const char *p, char *r, ULONG n) { (void)p; if (n) r[0] = 0; return 0; }

/* ---- Hal -------------------------------------------------------------- */

void NTAPI HalReturnToFirmware(ULONG Routine)
{
    xlog("HalReturnToFirmware(%u): title asked to reboot or return to the dashboard", Routine);
    exit(0);
}

void NTAPI HalInitiateShutdown(void)
{
    xlog("HalInitiateShutdown");
    exit(0);
}

BOOLEAN NTAPI HalIsResetOrShutdownPending(void) { return 0; }

void NTAPI HalRegisterShutdownNotification(PVOID Registration, BOOLEAN Register)
{
    (void)Registration; (void)Register;
}

void NTAPI HalReadWritePCISpace(ULONG Bus, ULONG Slot, ULONG Reg, PVOID Buffer, ULONG Length, BOOLEAN Write)
{
    if (!Write) memset(Buffer, 0, Length);
    TRACE("HalReadWritePCISpace(%u, %u, %#x, %u, %s)", Bus, Slot, Reg, Length, Write ? "write" : "read");
}

NTSTATUS NTAPI HalReadSMBusValue(UCHAR Address, UCHAR Command, BOOLEAN WriteWord, ULONG *Data)
{
    (void)Address; (void)Command; (void)WriteWord;
    *Data = 0;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI HalWriteSMBusValue(UCHAR Address, UCHAR Command, BOOLEAN WriteWord, ULONG Data)
{
    (void)Address; (void)Command; (void)WriteWord; (void)Data;
    return STATUS_SUCCESS;
}

void NTAPI HalReadSMCTrayState(ULONG *State, ULONG *Count)
{
    *State = 0x60; /* media detected */
    if (Count) *Count = 1;
}

void NTAPI HalWriteSMCScratchRegister(ULONG v) { (void)v; }
void NTAPI HalEnableSecureTrayEject(void) {}
void FASTCALL HalRequestSoftwareInterrupt(KIRQL irql) { (void)irql; }
void FASTCALL HalClearSoftwareInterrupt(KIRQL irql) { (void)irql; }
void NTAPI HalEnableSystemInterrupt(ULONG v, ULONG mode) { (void)v; (void)mode; }
void NTAPI HalDisableSystemInterrupt(ULONG v) { (void)v; }

/* ---- Av: video mode programming -------------------------------------- */

static PVOID av_saved_data;

PVOID NTAPI AvGetSavedDataAddress(void) { return av_saved_data; }
void NTAPI AvSetSavedDataAddress(PVOID p) { av_saved_data = p; }

void NTAPI AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option, ULONG Param, ULONG *Result)
{
    (void)RegisterBase; (void)Param;
    TRACE("AvSendTVEncoderOption(%u)", Option);
    if (Result) {
        /* AV_QUERY_AV_CAPABILITIES (6): report an HDTV-capable component pack
           in 60 Hz NTSC-M so titles pick 640x480. */
        *Result = Option == 6 ? 0x00400101 : 0;
    }
}

ULONG NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode, ULONG Format, ULONG Pitch,
                             ULONG FrameBuffer)
{
    (void)RegisterBase;
    TRACE("AvSetDisplayMode(step %u, mode %#x, format %#x, pitch %u, fb %#x)", Step, Mode, Format, Pitch,
          FrameBuffer);
    return 0;
}

/* ---- Ex: nonvolatile settings (EEPROM) ------------------------------- */

NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG ValueIndex, ULONG *Type, PVOID Value, ULONG ValueLength,
                                         ULONG *ResultLength)
{
    ULONG v = 0, len = 4;
    switch (ValueIndex) {
    case 0x03: v = 0; break;               /* XC_TIMEZONE_BIAS */
    case 0x07: v = 1; break;               /* XC_LANGUAGE: English */
    case 0x08: v = 0x00400100; break;      /* XC_VIDEO_FLAGS */
    case 0x09: v = 0; break;               /* XC_AUDIO_FLAGS */
    case 0x0A: v = 0; break;               /* XC_PARENTAL_CONTROL_GAMES: allow all */
    case 0x0B: v = 0; break;               /* XC_PARENTAL_CONTROL_PASSWORD */
    case 0x0C: v = 0; break;               /* XC_PARENTAL_CONTROL_MOVIES */
    case 0x0E: v = 0; break;               /* XC_DVD_REGION */
    case 0x10: v = 0; break;               /* XC_MISC_FLAGS */
    case 0x103: v = 1; break;              /* XC_FACTORY_GAME_REGION: North America */
    case 0x104: v = 0x00400100; break;     /* XC_FACTORY_AV_REGION: NTSC-M */
    case 0xFFFF: {                         /* XC_MAX_ALL: whole EEPROM image */
        len = 256;
        if (ValueLength < len) return STATUS_BUFFER_TOO_SMALL;
        memset(Value, 0, len);
        if (Type) *Type = 3;
        if (ResultLength) *ResultLength = len;
        return STATUS_SUCCESS;
    }
    default:
        TRACE("ExQueryNonVolatileSetting(%#x): unknown, returning 0", ValueIndex);
    }
    if (ValueLength < len) return STATUS_BUFFER_TOO_SMALL;
    memcpy(Value, &v, len);
    if (Type) *Type = 4;  /* REG_DWORD */
    if (ResultLength) *ResultLength = len;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ExSaveNonVolatileSetting(ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    (void)Type; (void)Value; (void)ValueLength;
    TRACE("ExSaveNonVolatileSetting(%#x) ignored", ValueIndex);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ExReadWriteRefurbInfo(PVOID Info, ULONG Length, BOOLEAN Write)
{
    if (!Write) memset(Info, 0, Length);
    return STATUS_SUCCESS;
}

void NTAPI ExRaiseException(PVOID rec)
{
    extern void NTAPI RtlRaiseException(PVOID);
    RtlRaiseException(rec);
}

void NTAPI ExRaiseStatus(NTSTATUS st)
{
    extern void NTAPI RtlRaiseStatus(NTSTATUS);
    RtlRaiseStatus(st);
}

/* ---- Xe: XBE sections are all loaded up front ------------------------ */

typedef struct { ULONG SectionFlags, VirtualAddress, VirtualSize, PointerToRawData, SizeOfRawData,
                 SectionName, SectionReferenceCount; USHORT *Head, *Tail; } XBE_SECTION_HDR;

NTSTATUS NTAPI XeLoadSection(XBE_SECTION_HDR *Section)
{
    Section->SectionReferenceCount++;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI XeUnloadSection(XBE_SECTION_HDR *Section)
{
    if (Section->SectionReferenceCount) Section->SectionReferenceCount--;
    return STATUS_SUCCESS;
}

/* ---- Interlocked (fastcall) ------------------------------------------ */

LONG FASTCALL InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
LONG FASTCALL InterlockedDecrement(volatile LONG *p) { return __sync_sub_and_fetch(p, 1); }
LONG FASTCALL InterlockedExchange(volatile LONG *p, LONG v) { return __sync_lock_test_and_set(p, v); }
LONG FASTCALL InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __sync_fetch_and_add(p, v); }

/* InterlockedCompareExchange(Destination, ExChange, Comperand): fastcall
   passes the first two in ecx/edx and the third on the stack. */
LONG FASTCALL InterlockedCompareExchange(volatile LONG *p, LONG exchange, LONG comperand)
{
    return __sync_val_compare_and_swap(p, comperand, exchange);
}

typedef struct SLIST_ENTRY { struct SLIST_ENTRY *Next; } SLIST_ENTRY;
typedef union { ULONGLONG Alignment; struct { SLIST_ENTRY Next; USHORT Depth, Sequence; }; } SLIST_HEADER;

static pthread_mutex_t slist_lock = PTHREAD_MUTEX_INITIALIZER;

SLIST_ENTRY *FASTCALL InterlockedPushEntrySList(SLIST_HEADER *h, SLIST_ENTRY *e)
{
    pthread_mutex_lock(&slist_lock);
    SLIST_ENTRY *old = h->Next.Next;
    e->Next = old;
    h->Next.Next = e;
    h->Depth++;
    h->Sequence++;
    pthread_mutex_unlock(&slist_lock);
    return old;
}

SLIST_ENTRY *FASTCALL InterlockedPopEntrySList(SLIST_HEADER *h)
{
    pthread_mutex_lock(&slist_lock);
    SLIST_ENTRY *e = h->Next.Next;
    if (e) {
        h->Next.Next = e->Next;
        h->Depth--;
        h->Sequence++;
    }
    pthread_mutex_unlock(&slist_lock);
    return e;
}

SLIST_ENTRY *FASTCALL InterlockedFlushSList(SLIST_HEADER *h)
{
    pthread_mutex_lock(&slist_lock);
    SLIST_ENTRY *e = h->Next.Next;
    h->Next.Next = NULL;
    h->Depth = 0;
    pthread_mutex_unlock(&slist_lock);
    return e;
}

/* ExInterlockedAddLargeStatistic(Addend, Increment): ecx, edx. */
void FASTCALL ExInterlockedAddLargeStatistic(LARGE_INTEGER *Addend, ULONG Increment)
{
    __sync_fetch_and_add(&Addend->QuadPart, (LONGLONG)Increment);
}

/* ExInterlockedCompareExchange64(Destination, Exchange*, Comperand*): ecx, edx, stack. */
LONGLONG FASTCALL ExInterlockedCompareExchange64(LONGLONG *Dest, LONGLONG *Exchange, LONGLONG *Comperand)
{
    return __sync_val_compare_and_swap(Dest, *Comperand, *Exchange);
}

/* ---- crypto: titles only use these for save signatures --------------- */

void NTAPI XcSHAInit(UCHAR *ctx) { memset(ctx, 0, 116); }
void NTAPI XcSHAUpdate(UCHAR *ctx, const UCHAR *in, ULONG n) { (void)ctx; (void)in; (void)n; }
void NTAPI XcSHAFinal(UCHAR *ctx, UCHAR *digest) { (void)ctx; memset(digest, 0, 20); }
void NTAPI XcHMAC(const UCHAR *key, ULONG keylen, const UCHAR *a, ULONG alen, const UCHAR *b, ULONG blen,
                  UCHAR *digest)
{
    (void)key; (void)keylen; (void)a; (void)alen; (void)b; (void)blen;
    memset(digest, 0, 20);
}

/* ---- misc -------------------------------------------------------------- */

NTSTATUS NTAPI NtSetSystemTime(LARGE_INTEGER *New, LARGE_INTEGER *Old)
{
    (void)New;
    if (Old) Old->QuadPart = system_time_now();
    return STATUS_SUCCESS;
}

typedef struct { ULONG Length, ReturnValue; } IO_DUMMY;

NTSTATUS NTAPI IoSynchronousDeviceIoControlRequest(ULONG Code, PVOID Dev, PVOID In, ULONG InLen,
                                                   PVOID Out, ULONG OutLen, ULONG *Returned, BOOLEAN Internal)
{
    (void)Dev; (void)In; (void)InLen; (void)Internal;
    TRACE("IoSynchronousDeviceIoControlRequest(%#x)", Code);
    if (Out) memset(Out, 0, OutLen);
    if (Returned) *Returned = 0;
    return STATUS_SUCCESS;
}
