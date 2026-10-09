/* Hal, Av, Dbg, Ex settings, Xe section loading and the kernel's data exports. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dirent.h>
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
        if (!strcmp(a, "--hle") || !strcmp(a, "--dvd") || !strcmp(a, "--d-path") || !strcmp(a, "--launch-data") ||
            !strcmp(a, "--xbe-path")) {
            i++;
            continue;
        }
        args[n++] = saved_argv[i];
    }
    extern char *g_dvd_root;
    if (g_dvd_root) { args[n++] = "--dvd"; args[n++] = g_dvd_root; }
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
    if (Routine == 2 && path && path[0]) relaunch(path);
    xlog("HalReturnToFirmware(%u): title asked to reboot or return to the dashboard", Routine);
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

/* An empty --dvd directory is an empty tray (the dashboard otherwise
   reports an unrecognized disc); anything else is a detected disc. */
bool dvd_tray_empty(void)
{
    extern char *g_dvd_root;
    static int empty = -1;
    if (empty < 0) {
        empty = 0;
        DIR *d = g_dvd_root ? opendir(g_dvd_root) : NULL;
        if (d) {
            struct dirent *e;
            empty = 1;
            while ((e = readdir(d)))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { empty = 0; break; }
            closedir(d);
        }
    }
    return empty;
}

void NTAPI HalReadSMCTrayState(ULONG *State, ULONG *Count)
{
    *State = dvd_tray_empty() ? 0x40 /* no media */ : 0x60; /* media detected */
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

/* ---- crypto: save signatures (SHA-1 / HMAC) and RC4 ------------------ */

/* The title's 116-byte SHA context holds this. */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha1_ctx;

static uint32_t rol32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void NTAPI XcSHAInit(UCHAR *ctx)
{
    sha1_ctx *c = (sha1_ctx *)ctx;
    memset(ctx, 0, 116);
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
}

void NTAPI XcSHAUpdate(UCHAR *ctx, const UCHAR *in, ULONG n)
{
    sha1_ctx *c = (sha1_ctx *)ctx;
    c->len += n;
    while (n) {
        ULONG take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, in, take);
        c->n += take; in += take; n -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

void NTAPI XcSHAFinal(UCHAR *ctx, UCHAR *digest)
{
    sha1_ctx *c = (sha1_ctx *)ctx;
    uint64_t bits = c->len * 8;
    static const UCHAR pad[64] = { 0x80 };
    ULONG padlen = c->n < 56 ? 56 - c->n : 120 - c->n;
    XcSHAUpdate(ctx, pad, padlen);
    UCHAR lenbe[8];
    for (int i = 0; i < 8; i++) lenbe[i] = (UCHAR)(bits >> (56 - 8 * i));
    XcSHAUpdate(ctx, lenbe, 8);
    for (int i = 0; i < 5; i++) {
        digest[4 * i] = c->h[i] >> 24; digest[4 * i + 1] = c->h[i] >> 16;
        digest[4 * i + 2] = c->h[i] >> 8; digest[4 * i + 3] = c->h[i];
    }
}

/* HMAC-SHA1 over the concatenation of two buffers (either may be empty). */
void NTAPI XcHMAC(const UCHAR *key, ULONG keylen, const UCHAR *a, ULONG alen, const UCHAR *b, ULONG blen,
                  UCHAR *digest)
{
    UCHAR k[64] = { 0 }, ctx[116], inner[20];
    if (keylen > 64) { XcSHAInit(ctx); XcSHAUpdate(ctx, key, keylen); XcSHAFinal(ctx, k); }
    else if (key) memcpy(k, key, keylen);
    UCHAR pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    XcSHAInit(ctx); XcSHAUpdate(ctx, pad, 64);
    if (a && alen) XcSHAUpdate(ctx, a, alen);
    if (b && blen) XcSHAUpdate(ctx, b, blen);
    XcSHAFinal(ctx, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    XcSHAInit(ctx); XcSHAUpdate(ctx, pad, 64); XcSHAUpdate(ctx, inner, 20);
    XcSHAFinal(ctx, digest);
}

/* RC4: the title's key structure is 256 state bytes plus the two indices. */
void NTAPI XcRC4Key(UCHAR *state, ULONG keylen, const UCHAR *key)
{
    for (int i = 0; i < 256; i++) state[i] = (UCHAR)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + state[i] + (keylen ? key[i % keylen] : 0)) & 0xFF;
        UCHAR t = state[i]; state[i] = state[j]; state[j] = t;
    }
    state[256] = state[257] = 0;
}

void NTAPI XcRC4Crypt(UCHAR *state, ULONG len, UCHAR *data)
{
    unsigned i = state[256], j = state[257];
    for (ULONG n = 0; n < len; n++) {
        i = (i + 1) & 0xFF;
        j = (j + state[i]) & 0xFF;
        UCHAR t = state[i]; state[i] = state[j]; state[j] = t;
        data[n] ^= state[(state[i] + state[j]) & 0xFF];
    }
    state[256] = (UCHAR)i; state[257] = (UCHAR)j;
}

/* DES keys carry odd parity in each byte's low bit. */
void NTAPI XcDESKeyParity(UCHAR *key, ULONG len)
{
    for (ULONG i = 0; i < len; i++) {
        UCHAR b = key[i] & 0xFE;
        key[i] = b | !(__builtin_popcount(b) & 1);
    }
}

/* Big numbers: n little-endian 32-bit limbs.  r = (r + r [+ b]) mod m. */
static int bn_cmp(const uint32_t *a, const uint32_t *b, ULONG n)
{
    for (ULONG i = n; i-- > 0;) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static void bn_sub(uint32_t *a, const uint32_t *b, ULONG n)
{
    uint64_t borrow = 0;
    for (ULONG i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
}

/* a = (a + b) mod m, with a, b < m; `carry` handles the bit past n limbs. */
static void bn_addmod(uint32_t *a, const uint32_t *b, const uint32_t *m, ULONG n)
{
    uint64_t c = 0;
    for (ULONG i = 0; i < n; i++) { c += (uint64_t)a[i] + b[i]; a[i] = (uint32_t)c; c >>= 32; }
    if (c || bn_cmp(a, m, n) >= 0) bn_sub(a, m, n);
}

/* r = a * b mod m by double-and-add over a's bits. */
static void bn_mulmod(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, ULONG n)
{
    uint32_t *acc = calloc(n, 4);
    for (ULONG i = n * 32; i-- > 0;) {
        bn_addmod(acc, acc, m, n);
        if ((a[i / 32] >> (i % 32)) & 1) bn_addmod(acc, b, m, n);
    }
    memcpy(r, acc, n * 4);
    free(acc);
}

/* A = B^C mod D, all n limbs. */
ULONG NTAPI XcModExp(ULONG *A, const ULONG *B, const ULONG *C, const ULONG *D, ULONG n)
{
    if (!n) return 0;
    uint32_t *base = calloc(n, 4), *res = calloc(n, 4);
    memcpy(base, B, n * 4);
    while (bn_cmp(base, (const uint32_t *)D, n) >= 0) bn_sub(base, (const uint32_t *)D, n);   /* B < D in practice */
    res[0] = 1;
    if (n == 1 && D[0] == 1) res[0] = 0;
    for (ULONG i = n * 32; i-- > 0;) {
        bn_mulmod(res, res, res, (const uint32_t *)D, n);
        if ((C[i / 32] >> (i % 32)) & 1) bn_mulmod(res, res, base, (const uint32_t *)D, n);
    }
    memcpy(A, res, n * 4);
    free(base);
    free(res);
    return 1;
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
