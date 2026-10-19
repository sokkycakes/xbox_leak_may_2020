/* Hal, Av, Dbg, Ex settings, Xe section loading and the kernel's data exports. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>

#include "../xbcompat.h"
#include "../cpu.h"

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

#ifdef XBC_TRANSLATED
/* The guest's variadic arguments are x86 stack words, which a host va_list
   can't describe: format them from the stack. */
static uint64_t xa_DbgPrint_varargs(struct xa_frame *f)
{
    char buf[2048];
    xa_format(buf, sizeof(buf), (const char *)(uintptr_t)f->stack[0], f->stack + 1);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    xlog("[guest] %s", buf);
    return 0;
}

static const struct xa_entry dbgprint_entry[] = {
    { (void *)DbgPrint, xa_DbgPrint_varargs, CONV_CDECL, "DbgPrint" },
};
__attribute__((constructor)) static void dbgprint_register(void) { xa_register(dbgprint_entry, 1); }
#endif

void NTAPI DbgBreakPoint(void) { xlog("DbgBreakPoint (ignored)"); }
void NTAPI DbgBreakPointWithStatus(ULONG st) { xlog("DbgBreakPointWithStatus(%#x) (ignored)", st); }
void NTAPI DbgLoadImageSymbols(PVOID a, PVOID b, ULONG c) { (void)a; (void)b; (void)c; }
void NTAPI DbgUnLoadImageSymbols(PVOID a, PVOID b, ULONG c) { (void)a; (void)b; (void)c; }
ULONG NTAPI DbgPrompt(const char *p, char *r, ULONG n) { (void)p; if (n) r[0] = 0; return 0; }

/* ---- Hal -------------------------------------------------------------- */

/* ---- title launches ----------------------------------------------------
   XLaunchNewImage writes the next title's path ("<D: target>;<path>") and
   its launch data into the LaunchDataPage, then quick-reboots.  xbcompat
   reboots by running itself again (through the XBCOMPAT_LAUNCHER command when
   set, so xbrun can build the new title's library map) with the D: target,
   the image and the launch page passed on the command line. */

NTSTATUS NTAPI IoCreateSymbolicLink(OBJECT_STRING *Link, OBJECT_STRING *Target);
PVOID NTAPI MmAllocateContiguousMemory(SIZE_T NumberOfBytes);

static int saved_argc;
static char **saved_argv;

void launch_init(int argc, char **argv, const char *d_path, const char *launch_data_file, const char *xbe_rel)
{
    saved_argc = argc;
    saved_argv = argv;
    {   /* the kernel's own link for the DVD drive (XAPI's "cdrom0:") */
        OBJECT_STRING link = { 12, 13, "\\??\\CdRom0:" }, target = { 14, 15, "\\Device\\CdRom0" };
        IoCreateSymbolicLink(&link, &target);
    }
    {   /* xbcompat's game card slot (see io.c) */
        OBJECT_STRING link = { 10, 11, "\\??\\CARD0:" }, target = { 17, 18, "\\Device\\GameCard0" };
        IoCreateSymbolicLink(&link, &target);
    }
    if (d_path) {
        static char name[600];
        snprintf(name, sizeof(name), "%s\\%s", d_path, xbe_rel ? xbe_rel : "default.xbe");
        XeImageFileName.Buffer = name;
        XeImageFileName.Length = strlen(name);
        XeImageFileName.MaximumLength = XeImageFileName.Length + 1;
        OBJECT_STRING link = { 6, 7, "\\??\\D:" }, target;
        target.Buffer = (char *)d_path;
        target.Length = strlen(d_path);
        target.MaximumLength = target.Length + 1;
        IoCreateSymbolicLink(&link, &target);
        xlog("launched with D: -> %s", d_path);
    }
    if (launch_data_file) {
        FILE *f = fopen(launch_data_file, "rb");
        if (f) {
            LaunchDataPage = MmAllocateContiguousMemory(4096);
            memset(LaunchDataPage, 0, 4096);
            if (fread(LaunchDataPage, 1, 4096, f) != 4096) xlog("launch data %s is short", launch_data_file);
            fclose(f);
        }
    }
}

static void relaunch(const char *path)
{
    char dpath[520], rel[520];
    /* "<D: target>;<image relative to it>", or a plain image path, whose
       directory becomes D: (titles launched from the hard disk). */
    const char *semi = strchr(path, ';');
    if (!semi) semi = strrchr(path, '\\');
    if (!semi) { xlog("launch path %s has no directory", path); return; }
    snprintf(dpath, sizeof(dpath), "%.*s", (int)(semi - path), path);
    snprintf(rel, sizeof(rel), "%s", semi + 1);
    char dir[4096], img[4096];
    if (!NT_SUCCESS(fs_host_path(dpath, dir, sizeof(dir)))) { xlog("cannot map %s", dpath); return; }
    char full[1100];
    snprintf(full, sizeof(full), "%s\\%s", dpath, rel);
    if (!NT_SUCCESS(fs_host_path(full, img, sizeof(img)))) { xlog("cannot map %s", full); return; }
    char page[4096];
    snprintf(page, sizeof(page), "%s/.launchdata", fs_hdd_root());
    FILE *f = fopen(page, "wb");
    if (!f || fwrite(LaunchDataPage, 1, 4096, f) != 4096) { xlog("cannot write %s", page); if (f) fclose(f); return; }
    fclose(f);

    /* The new command line: the launcher (or this binary), the options this
       run was given except --hle, --dvd and launch state, then the new ones. */
    char *args[64];
    int n = 0;
    const char *launcher = getenv("XBCOMPAT_LAUNCHER");
    char *lcopy = launcher ? strdup(launcher) : NULL;
    if (lcopy) {
        /* xbrun.py takes the image first, then xbcompat's options. */
        for (char *tok = strtok(lcopy, " "); tok && n < 8; tok = strtok(NULL, " ")) args[n++] = tok;
        args[n++] = img;
    } else {
        args[n++] = "/proc/self/exe";
    }
    for (int i = 1; i < saved_argc - 1 && n < 48; i++) {
        const char *a = saved_argv[i];
        if (!strcmp(a, "--hle") || !strcmp(a, "--dvd") || !strcmp(a, "--dvd-drive") || !strcmp(a, "--d-path") || !strcmp(a, "--launch-data") ||
            !strcmp(a, "--xbe-path")) {
            i++;
            continue;
        }
        args[n++] = saved_argv[i];
    }
    extern char *g_dvd_root, *g_dvd_drive;
    if (g_dvd_root) { args[n++] = "--dvd"; args[n++] = g_dvd_root; }
    if (g_dvd_drive) { args[n++] = "--dvd-drive"; args[n++] = g_dvd_drive; }
    args[n++] = "--d-path"; args[n++] = dpath;
    args[n++] = "--xbe-path"; args[n++] = rel;
    args[n++] = "--launch-data"; args[n++] = page;
    if (!lcopy) args[n++] = img;
    args[n] = NULL;
    xlog("XLaunchNewImage: %s (D: %s)", img, dpath);
    if (g_log) fflush(g_log);
    fflush(stderr);
    thread_untrap_tsc();   /* the trap survives exec: python and the next xbcompat would fault */
    execvp(args[0], args);
    xlog("relaunch failed: %s", strerror(errno));
}

void NTAPI HalReturnToFirmware(ULONG Routine)
{
    const char *path = LaunchDataPage ? (const char *)LaunchDataPage + 8 : NULL;
    av_hand_over();
    if (Routine == 2 && path && path[0]) relaunch(path);
    xlog("HalReturnToFirmware(%u): title asked to reboot or return to the dashboard", Routine);
#ifdef XBC_TRANSLATED
    if (Routine == 4 /* HalFatalErrorRebootRoutine */) cpu_dump_guest();
#endif
    if (LaunchDataPage && *(ULONG *)LaunchDataPage /* dwLaunchDataType */) {
        /* XLaunchNewImage(NULL, data): the dashboard gets the launch data
           (LDT_LAUNCH_DASHBOARD, e.g. "open the Memory screen"). Whoever
           starts the dashboard next passes it on with --launch-data. */
        char page[4096];
        snprintf(page, sizeof(page), "%s/.dashlaunch", fs_hdd_root());
        FILE *f = fopen(page, "wb");
        if (f) {
            fwrite(LaunchDataPage, 1, 4096, f);
            fclose(f);
            xlog("dashboard launch data saved to %s", page);
        }
    }
    xbc_exit(0);
}

void NTAPI HalInitiateShutdown(void)
{
    xlog("HalInitiateShutdown");
    av_hand_over();
    xbc_exit(0);
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

NTSTATUS NTAPI HalReadSMCTrayState(ULONG *State, ULONG *Count)
{
    static ULONG last, changes;
    *State = dvd_tray_state();   /* SMC_TRAY_STATE_*: open, no media, media detected */
    if (*State != last) {
        last = *State;
        changes++;
        xlog("HalReadSMCTrayState: %#x", *State);
    }
    if (Count) *Count = changes;
    return STATUS_SUCCESS;
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

/* The EEPROM's user settings section (XBOX_USER_SETTINGS, 96 bytes), which
   XC_MAX_OS (0xFF) returns whole: XAPI's time zone code reads it that way. */
#pragma pack(push, 1)
static struct {
    ULONG Checksum;
    LONG TimeZoneBias;
    char TimeZoneStdName[4], TimeZoneDltName[4];
    ULONG Reserved1[2];
    UCHAR TimeZoneStdDate[4], TimeZoneDltDate[4];   /* month, day, day of week, hour */
    ULONG Reserved2[2];
    LONG TimeZoneStdBias, TimeZoneDltBias;
    ULONG Language, VideoFlags, AudioFlags;
    ULONG ParentalControlGames, ParentalControlPassword, ParentalControlMovies;
    ULONG OnlineIpAddress, OnlineDnsAddress, OnlineDefaultGatewayAddress, OnlineSubnetMask;
    ULONG MiscFlags, DvdRegion;
} user_settings = {
    .TimeZoneStdName = "GMT", .TimeZoneDltName = "BST",
    .Language = 1,                         /* English */
    .MiscFlags = 2,                        /* XC_MISC_FLAG_DONT_USE_DST: plain UTC */
};
#pragma pack(pop)
_Static_assert(sizeof(user_settings) == 96, "XBOX_USER_SETTINGS is 96 bytes");

NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG ValueIndex, ULONG *Type, PVOID Value, ULONG ValueLength,
                                         ULONG *ResultLength)
{
    /* User settings by XC_VALUE_INDEX: offset and size in user_settings. */
    static const struct { UCHAR off, len; } user[] = {
        { 4, 4 }, { 8, 4 }, { 24, 4 }, { 40, 4 }, { 12, 4 }, { 28, 4 }, { 44, 4 },   /* time zone */
        { 48, 4 }, { 52, 4 }, { 56, 4 }, { 60, 4 }, { 64, 4 }, { 68, 4 },           /* language .. movies */
        { 72, 4 }, { 76, 4 }, { 80, 4 }, { 84, 4 }, { 88, 4 }, { 92, 4 },           /* online, misc, DVD */
    };
    static const UCHAR serial[12] = "000000000000", mac[6] = { 0x00, 0x50, 0xf2, 0x00, 0x00, 0x01 };
    static const ULONG av_region = 0x00400100;    /* NTSC-M */
    static const ULONG game_region = 1;           /* North America */
    static UCHAR all[256];
    const void *src;
    ULONG len, type = 4;   /* REG_DWORD */

    /* XBCOMPAT_DASH_VIEW=4:3 sets bit 31 of the video flags, which no Xbox
       flag uses (XGetVideoFlags masks it off, so titles never see it). The
       dashboard dashbuild makes reads it and builds its 3D view for 640x480
       instead of its usual 720x480, showing 1/8 less of the scene sideways. */
    static int dash_view = -1;
    if (dash_view < 0) {
        const char *v = getenv("XBCOMPAT_DASH_VIEW");
        dash_view = v && !strcmp(v, "4:3");
        if (dash_view) user_settings.VideoFlags |= 0x80000000u;
    }

    if (ValueIndex < sizeof(user) / sizeof(user[0])) {
        src = (const UCHAR *)&user_settings + user[ValueIndex].off;
        len = user[ValueIndex].len;
        if (ValueIndex == 1 || ValueIndex == 2 || ValueIndex == 4 || ValueIndex == 5) type = 3;
    } else switch (ValueIndex) {
    case 0xFF: src = &user_settings; len = sizeof(user_settings); type = 3; break;   /* XC_MAX_OS */
    case 0x100: src = serial; len = sizeof(serial); type = 3; break;               /* XC_FACTORY_SERIAL_NUMBER */
    case 0x101: src = mac; len = sizeof(mac); type = 3; break;                     /* XC_FACTORY_ETHERNET_ADDR */
    case 0x103: src = &av_region; len = 4; break;                                  /* XC_FACTORY_AV_REGION */
    case 0x104: src = &game_region; len = 4; break;                                /* XC_FACTORY_GAME_REGION */
    case 0xFFFF:                                                                   /* XC_MAX_ALL */
        /* The factory section starts at 0x30 of the 256-byte image (AV region
           at +40), the user section at 0x60. */
        memcpy(all + 0x30 + 40, &av_region, 4);
        memcpy(all + 0x60, &user_settings, sizeof(user_settings));
        src = all; len = sizeof(all); type = 3;
        break;
    default: {
        static const ULONG zero;
        TRACE("ExQueryNonVolatileSetting(%#x): unknown, returning 0", ValueIndex);
        src = &zero; len = 4;
    }
    }
    if (ResultLength) *ResultLength = len;
    if (ValueLength < len) return STATUS_BUFFER_TOO_SMALL;
    memcpy(Value, src, len);
    if (Type) *Type = type;
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

/* ---- Xe: XBE sections ------------------------------------------------ */

/* Every section is mapped up front, but one loaded with nobody holding it
   is read from the XBE again, as the console would: titles fix up resource
   bundles in place and expect a fresh copy after XFreeSection. */
static pthread_mutex_t section_lock = PTHREAD_MUTEX_INITIALIZER;

NTSTATUS NTAPI XeLoadSection(XBE_SECTION *Section)
{
    pthread_mutex_lock(&section_lock);
    if (Section->SectionReferenceCount++ == 0)
        xbe_reload_section(Section->VirtualAddress, Section->PointerToRawData, Section->SizeOfRawData,
                           Section->VirtualSize);
    TRACE("XeLoadSection(%s) refs %u", (char *)Section->SectionName, Section->SectionReferenceCount);
    pthread_mutex_unlock(&section_lock);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI XeUnloadSection(XBE_SECTION *Section)
{
    pthread_mutex_lock(&section_lock);
    if (Section->SectionReferenceCount) Section->SectionReferenceCount--;
    TRACE("XeUnloadSection(%s) refs %u", (char *)Section->SectionName, Section->SectionReferenceCount);
    pthread_mutex_unlock(&section_lock);
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

/* ExInterlockedAddLargeInteger(Addend, Increment, Lock) (ex/i386/intrlock.asm):
   adds and returns the old value; the spin lock argument is unused on the
   Xbox. The increment is taken as two stack slots, as KeSetTimerEx does. */
LONGLONG NTAPI ExInterlockedAddLargeInteger(LARGE_INTEGER *Addend, ULONG IncLow, LONG IncHigh, PVOID Lock)
{
    (void)Lock;
    LONGLONG inc = (LONGLONG)(((ULONGLONG)(ULONG)IncHigh << 32) | IncLow);
    return __sync_fetch_and_add(&Addend->QuadPart, inc);
}

/* ExfInterlocked{Insert,Remove}*List (ex/i386/intrlfst.asm): list edits
   under a lock; inserts return the old first (last) entry, NULL when the
   list was empty, and a remove returns NULL on an empty list. */
static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;

LIST_ENTRY *FASTCALL ExfInterlockedInsertHeadList(LIST_ENTRY *Head, LIST_ENTRY *Entry)
{
    pthread_mutex_lock(&list_lock);
    LIST_ENTRY *next = Head->Flink;
    Entry->Flink = next;
    Entry->Blink = Head;
    Head->Flink = Entry;
    next->Blink = Entry;
    pthread_mutex_unlock(&list_lock);
    return next == Head ? NULL : next;
}

LIST_ENTRY *FASTCALL ExfInterlockedInsertTailList(LIST_ENTRY *Head, LIST_ENTRY *Entry)
{
    pthread_mutex_lock(&list_lock);
    LIST_ENTRY *prev = Head->Blink;
    Entry->Flink = Head;
    Entry->Blink = prev;
    Head->Blink = Entry;
    prev->Flink = Entry;
    pthread_mutex_unlock(&list_lock);
    return prev == Head ? NULL : prev;
}

LIST_ENTRY *FASTCALL ExfInterlockedRemoveHeadList(LIST_ENTRY *Head)
{
    pthread_mutex_lock(&list_lock);
    LIST_ENTRY *e = Head->Flink;
    if (e == Head) e = NULL;
    else {
        Head->Flink = e->Flink;
        e->Flink->Blink = Head;
    }
    pthread_mutex_unlock(&list_lock);
    return e;
}

/* Reader/writer locks (ex/rwlock.c, ex/i386/rwlocka.asm). LockCount starts
   at -1 and counts holders plus waiters; the asm's cli/sti section is a
   mutex here, and waiting happens outside it on the lock's own event or
   semaphore, as there. */
typedef struct {
    LONG LockCount;
    ULONG WritersWaitingCount;
    ULONG ReadersWaitingCount;
    ULONG ReadersEntryCount;
    KEVENT WriterEvent;
    KSEMAPHORE ReaderSemaphore;
} ERWLOCK;

extern void NTAPI KeInitializeEvent(KEVENT *, ULONG, BOOLEAN);
extern LONG NTAPI KeSetEvent(KEVENT *, LONG, BOOLEAN);
extern void NTAPI KeInitializeSemaphore(KSEMAPHORE *, LONG, LONG);
extern LONG NTAPI KeReleaseSemaphore(KSEMAPHORE *, LONG, LONG, BOOLEAN);
extern NTSTATUS NTAPI KeWaitForSingleObject(PVOID, ULONG, KPROCESSOR_MODE, BOOLEAN, LARGE_INTEGER *);

static pthread_mutex_t rw_lock = PTHREAD_MUTEX_INITIALIZER;

void NTAPI ExInitializeReadWriteLock(ERWLOCK *l)
{
    l->LockCount = -1;
    l->WritersWaitingCount = 0;
    l->ReadersWaitingCount = 0;
    l->ReadersEntryCount = 0;
    KeInitializeEvent(&l->WriterEvent, 1 /* SynchronizationEvent */, 0);
    KeInitializeSemaphore(&l->ReaderSemaphore, 0, 0x7fffffff);
}

void NTAPI ExAcquireReadWriteLockExclusive(ERWLOCK *l)
{
    pthread_mutex_lock(&rw_lock);
    if (++l->LockCount == 0) {
        pthread_mutex_unlock(&rw_lock);
        return;
    }
    l->WritersWaitingCount++;
    pthread_mutex_unlock(&rw_lock);
    KeWaitForSingleObject(&l->WriterEvent, 0, 0, 0, NULL);
}

void NTAPI ExAcquireReadWriteLockShared(ERWLOCK *l)
{
    pthread_mutex_lock(&rw_lock);
    if (++l->LockCount == 0 || (l->ReadersEntryCount && !l->WritersWaitingCount)) {
        /* Free, or held by readers with no writer queued: join them. */
        l->ReadersEntryCount++;
        pthread_mutex_unlock(&rw_lock);
        return;
    }
    l->ReadersWaitingCount++;
    pthread_mutex_unlock(&rw_lock);
    KeWaitForSingleObject(&l->ReaderSemaphore, 0, 0, 0, NULL);
}

void NTAPI ExReleaseReadWriteLock(ERWLOCK *l)
{
    pthread_mutex_lock(&rw_lock);
    if (--l->LockCount < 0) {
        l->ReadersEntryCount = 0;
        pthread_mutex_unlock(&rw_lock);
        return;
    }
    if (l->ReadersEntryCount) {
        /* A reader leaves; the last one hands over to a waiting writer. */
        if (--l->ReadersEntryCount) {
            pthread_mutex_unlock(&rw_lock);
            return;
        }
    } else if (l->ReadersWaitingCount) {
        /* A writer leaves with readers queued: let all of them in. */
        ULONG n = l->ReadersWaitingCount;
        l->ReadersEntryCount = n;
        l->ReadersWaitingCount = 0;
        pthread_mutex_unlock(&rw_lock);
        KeReleaseSemaphore(&l->ReaderSemaphore, 1, (LONG)n, 0);
        return;
    }
    l->WritersWaitingCount--;
    pthread_mutex_unlock(&rw_lock);
    KeSetEvent(&l->WriterEvent, 1, 0);
}

/* ---- network PHY: there is no Ethernet link ------------------------------ */

ULONG NTAPI PhyGetLinkState(BOOLEAN Update) { (void)Update; return 0; }
NTSTATUS NTAPI PhyInitialize(BOOLEAN ForceReset, PVOID Param) { (void)ForceReset; (void)Param; return STATUS_SUCCESS; }

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

/* ---- small exports later XDKs import ------------------------------------ */

ULONG KeTimeIncrement = 10000;   /* 100 ns units per clock tick (1 ms) */

static ULONG fsc_pages = 16;

NTSTATUS NTAPI FscSetCacheSize(ULONG NumberOfCachePages)
{
    TRACE("FscSetCacheSize(%u)", NumberOfCachePages);
    fsc_pages = NumberOfCachePages;
    return STATUS_SUCCESS;
}

ULONG NTAPI FscGetCacheSize(void) { return fsc_pages; }

/* The host saves and restores the FPU and SSE state for every thread. */
NTSTATUS NTAPI KeSaveFloatingPointState(PVOID FloatSave) { (void)FloatSave; return STATUS_SUCCESS; }
NTSTATUS NTAPI KeRestoreFloatingPointState(PVOID FloatSave) { (void)FloatSave; return STATUS_SUCCESS; }

/* Kernel stacks are only for drivers; hand out ordinary memory. */
PVOID NTAPI MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)
{
    (void)DebuggerThread;
    char *p = calloc(1, NumberOfBytes);
    return p ? p + NumberOfBytes : NULL;
}

void NTAPI MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
{
    (void)StackBase;
    free(StackLimit);
}
