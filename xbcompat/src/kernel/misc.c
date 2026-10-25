/* Hal, Av, Dbg, Ex settings, Xe section loading and the kernel's data exports. */
#define _GNU_SOURCE
#include <sys/mman.h>
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

/* This install's console identity: the factory EEPROM values Xbox Live
   reads (serial number, Ethernet address, online key) and the hard disk key.
   Real consoles each have their own, so they are made up once per hard disk
   directory and kept in console-id there.  Nothing here comes from a real
   console's keys, and every value is nonzero. */
static UCHAR id_serial[12], id_mac[6], id_online_key[16];

void identity_init(const char *hdd)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/console-id", hdd);
    FILE *f = fopen(path, "r");
    bool ok = false;
    if (f) {
        char ser[16], m[16], ok16[40], hk[40];
        if (fscanf(f, "serial %12s\nmac %12s\nonline-key %32s\nhdd-key %32s", ser, m, ok16, hk) == 4 &&
            strlen(ser) == 12 && strlen(m) == 12 && strlen(ok16) == 32 && strlen(hk) == 32) {
            memcpy(id_serial, ser, 12);
            for (int i = 0; i < 6; i++) sscanf(m + 2 * i, "%2hhx", &id_mac[i]);
            for (int i = 0; i < 16; i++) sscanf(ok16 + 2 * i, "%2hhx", &id_online_key[i]);
            for (int i = 0; i < 16; i++) sscanf(hk + 2 * i, "%2hhx", &XboxHDKey[i]);
            ok = true;
        }
        fclose(f);
    }
    if (!ok) {
        UCHAR rnd[64];
        FILE *r = fopen("/dev/urandom", "rb");
        if (!r || fread(rnd, 1, sizeof rnd, r) != sizeof rnd) fatal("cannot read /dev/urandom");
        fclose(r);
        for (int i = 0; i < 12; i++) id_serial[i] = '0' + rnd[i] % 10;
        id_serial[0] = '1' + rnd[0] % 9;
        id_mac[0] = 0x00; id_mac[1] = 0x50; id_mac[2] = 0xF2;   /* Microsoft's prefix, as consoles use */
        memcpy(id_mac + 3, rnd + 12, 3);
        memcpy(id_online_key, rnd + 16, 16);
        memcpy(XboxHDKey, rnd + 32, 16);
        id_online_key[0] |= 1;
        XboxHDKey[0] |= 1;
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "serial %.12s\nmac ", (char *)id_serial);
            for (int i = 0; i < 6; i++) fprintf(f, "%02x", id_mac[i]);
            fprintf(f, "\nonline-key ");
            for (int i = 0; i < 16; i++) fprintf(f, "%02x", id_online_key[i]);
            fprintf(f, "\nhdd-key ");
            for (int i = 0; i < 16; i++) fprintf(f, "%02x", XboxHDKey[i]);
            fprintf(f, "\n");
            fclose(f);
            xlog("identity: made a new console identity in %s", path);
        }
    }
}

const UCHAR *identity_mac(void) { return id_mac; }

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

/* Where in the title a reboot came from: the return address and the saved
   EBP chain of the guest code that called in (titles built with frame
   pointers), for finding which of a title's exits fired. */
static void log_guest_callers(const char *what, uintptr_t ret, const uint32_t *frame)
{
#if defined(__i386__)
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s called from %#x", what, (unsigned)ret);
    for (int i = 0; i < 8 && n < (int)sizeof(buf) - 16; i++) {
        uintptr_t f = (uintptr_t)frame;
        if (f < ARENA_BASE || f >= ARENA_END || (f & 3)) break;
        uint32_t r = frame[1];
        if (r < 0x10000 || r >= IMAGE_REGION_END) break;
        n += snprintf(buf + n, sizeof(buf) - n, " <- %#x", (unsigned)r);
        const uint32_t *next = (const uint32_t *)(uintptr_t)frame[0];
        if (next <= frame) break;
        frame = next;
    }
    xlog("%s", buf);
#else
    (void)what; (void)ret; (void)frame;
#endif
}

/* Optimized titles often have no frame pointers: words on the guest stack
   that point into the image are candidate return addresses. */
static void log_guest_stack_scan(const char *what, const uint32_t *sp)
{
#if defined(__i386__)
    char buf[400];
    int n = snprintf(buf, sizeof(buf), "%s stack:", what);
    for (int i = 0; i < 256 && n < (int)sizeof(buf) - 12; i++) {
        uintptr_t a = (uintptr_t)&sp[i];
        if (i == 0 || !(a & 0xfff)) {
            /* stop at the end of the mapped stack */
            unsigned char in;
            if (mincore((void *)(a & ~(uintptr_t)0xfff), 4096, &in) != 0) break;
        }
        uint32_t v = sp[i];
        if (v >= 0x10000 && v < IMAGE_REGION_END) n += snprintf(buf + n, sizeof(buf) - n, " +%x:%#x", i * 4, (unsigned)v);
    }
    xlog("%s", buf);
#else
    (void)what; (void)sp;
#endif
}

void NTAPI HalReturnToFirmware(ULONG Routine)
{
    const char *path = LaunchDataPage ? (const char *)LaunchDataPage + 8 : NULL;
#if defined(__i386__)
    {
        /* This function's frame holds the guest's EBP and return address;
           the guest's stack starts right after them. */
        const uint32_t *frame = __builtin_frame_address(0);
        log_guest_callers("HalReturnToFirmware", (uintptr_t)__builtin_return_address(0),
                          (const uint32_t *)(uintptr_t)frame[0]);
        log_guest_stack_scan("HalReturnToFirmware", frame + 2);
    }
#endif
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

/* The persisted frame (see D3D's persisted_load): D3D's GetPersistedSurface
   is this call, so titles built with it inlined ask the kernel directly. */
PVOID NTAPI AvGetSavedDataAddress(void)
{
    static unsigned logged;
    if (logged < 20) { logged++; xlog("AvGetSavedDataAddress: %p", av_saved_data); }
    return av_saved_data;
}
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
#ifdef XBC_GPU
    gpu_av_set_display_mode(Mode, Format, Pitch, FrameBuffer);
#endif
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
    /* XC_MISC_FLAG_DONT_USE_DST (plain UTC), and 0x8: the Xbox Live terms
       were accepted. The retail dashboard sets that on its legal screen; the
       retail online dashboard goes straight back to the dashboard without it. */
    .MiscFlags = 2 | 8,
};
#pragma pack(pop)
_Static_assert(sizeof(user_settings) == 96, "XBOX_USER_SETTINGS is 96 bytes");

/* The user settings persist in <hdd>/.eeprom-user, as the EEPROM does. */
static void user_settings_path(char *path, size_t len)
{
    snprintf(path, len, "%s/.eeprom-user", fs_hdd_root());
}

static void user_settings_load(void)
{
    static int loaded;
    char path[4096];
    FILE *f;

    if (loaded) return;
    loaded = 1;
    user_settings_path(path, sizeof(path));
    if (!(f = fopen(path, "rb"))) return;
    if (fread(&user_settings, 1, sizeof(user_settings), f) != sizeof(user_settings))
        TRACE("%s: short read, settings may be partly default", path);
    fclose(f);
    user_settings.VideoFlags &= ~0x80000000u;
}

static void user_settings_save(void)
{
    char path[4096], tmp[4200];
    FILE *f;

    user_settings_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (!(f = fopen(tmp, "wb"))) {
        TRACE("%s: %s", tmp, strerror(errno));
        return;
    }
    __typeof__(user_settings) saved = user_settings;
    saved.VideoFlags &= ~0x80000000u;      /* XBCOMPAT_DASH_VIEW's, not the title's */
    fwrite(&saved, 1, sizeof(saved), f);
    if (fclose(f) == 0) rename(tmp, path);
}

/* User settings by XC_VALUE_INDEX: offset and size in user_settings. */
static const struct { UCHAR off, len; } user_index[] = {
    { 4, 4 }, { 8, 4 }, { 24, 4 }, { 40, 4 }, { 12, 4 }, { 28, 4 }, { 44, 4 },   /* time zone */
    { 48, 4 }, { 52, 4 }, { 56, 4 }, { 60, 4 }, { 64, 4 }, { 68, 4 },           /* language .. movies */
    { 72, 4 }, { 76, 4 }, { 80, 4 }, { 84, 4 }, { 88, 4 }, { 92, 4 },           /* online, misc, DVD */
};

NTSTATUS NTAPI ExQueryNonVolatileSetting(ULONG ValueIndex, ULONG *Type, PVOID Value, ULONG ValueLength,
                                         ULONG *ResultLength)
{
    static const ULONG av_region = 0x00400100;    /* NTSC-M */
    static const ULONG game_region = 1;           /* North America */
    static UCHAR all[256];
    const void *src;
    ULONG len, type = 4;   /* REG_DWORD */

    user_settings_load();
    /* XBCOMPAT_DASH_VIEW=4:3 sets bit 31 of the video flags, which no Xbox
       flag uses (XGetVideoFlags masks it off, so titles never see it). The
       dashboard dashbuild makes reads it and builds its 3D view for 640x480
       instead of its usual 720x480, showing 1/8 less of the scene sideways.
       It is never saved to .eeprom-user (user_settings_save drops it). */
    static int dash_view = -1;
    if (dash_view < 0) {
        const char *v = getenv("XBCOMPAT_DASH_VIEW");
        dash_view = v && !strcmp(v, "4:3");
    }
    if (dash_view) user_settings.VideoFlags |= 0x80000000u;

    if (ValueIndex < sizeof(user_index) / sizeof(user_index[0])) {
        src = (const UCHAR *)&user_settings + user_index[ValueIndex].off;
        len = user_index[ValueIndex].len;
        if (ValueIndex == 1 || ValueIndex == 2 || ValueIndex == 4 || ValueIndex == 5) type = 3;
    } else switch (ValueIndex) {
    case 0xFF: src = &user_settings; len = sizeof(user_settings); type = 3; break;   /* XC_MAX_OS */
    case 0x100: src = id_serial; len = sizeof(id_serial); type = 3; break;         /* XC_FACTORY_SERIAL_NUMBER */
    case 0x101: src = id_mac; len = sizeof(id_mac); type = 3; break;               /* XC_FACTORY_ETHERNET_ADDR */
    case 0x102: src = id_online_key; len = sizeof(id_online_key); type = 3; break; /* XC_FACTORY_ONLINE_KEY */
    case 0x103: src = &av_region; len = 4; break;                                  /* XC_FACTORY_AV_REGION */
    case 0x104: src = &game_region; len = 4; break;                                /* XC_FACTORY_GAME_REGION */
    case 0xFFFF:                                                                   /* XC_MAX_ALL */
        /* The factory section starts at 0x30 of the 256-byte image (AV region
           at +40), the user section at 0x60. */
        memcpy(all + 0x30 + 4, id_serial, 12);
        memcpy(all + 0x30 + 16, id_mac, 6);
        memcpy(all + 0x30 + 24, id_online_key, 16);
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
    UCHAR *dst;
    ULONG len;

    (void)Type;
    user_settings_load();
    if (ValueIndex < sizeof(user_index) / sizeof(user_index[0])) {
        dst = (UCHAR *)&user_settings + user_index[ValueIndex].off;
        len = user_index[ValueIndex].len;
    } else if (ValueIndex == 0xFF) {                                     /* XC_MAX_OS */
        dst = (UCHAR *)&user_settings;
        len = sizeof(user_settings);
    } else {
        TRACE("ExSaveNonVolatileSetting(%#x) ignored", ValueIndex);
        return STATUS_SUCCESS;
    }
    if (ValueLength > len) return STATUS_INVALID_PARAMETER;
    memcpy(dst, Value, ValueLength);
    TRACE("ExSaveNonVolatileSetting(%#x, %u bytes)", ValueIndex, ValueLength);
    user_settings_save();
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

/* With the NIC emulated the cable is always in: 100 Mbps, full duplex. */
ULONG NTAPI PhyGetLinkState(BOOLEAN Update) { (void)Update; return g_nic_enabled ? 0x01 | 0x02 | 0x08 : 0; }
NTSTATUS NTAPI PhyInitialize(BOOLEAN ForceReset, PVOID Param) { (void)ForceReset; (void)Param; return STATUS_SUCCESS; }

/* ---- misc -------------------------------------------------------------- */

NTSTATUS NTAPI NtSetSystemTime(LARGE_INTEGER *New, LARGE_INTEGER *Old)
{
    (void)New;
    if (Old) Old->QuadPart = system_time_now();
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

/* The file system cache is the host's: nothing to drop. */
void NTAPI FscInvalidateIdleBlocks(void) {}

/* ---- port I/O (HAL READ_/WRITE_PORT_BUFFER_*) -------------------------
   There are no I/O ports behind these: reads see a floating bus (all ones),
   writes go nowhere. */

static void port_note(const char *what, ULONG port)
{
    static int once;
    if (!once++) xlog("%s(port %#x): no I/O ports here; reads give all ones, writes are dropped", what,
                      (unsigned)port);
}

void NTAPI READ_PORT_BUFFER_UCHAR(UCHAR *Port, UCHAR *Buffer, ULONG Count)
{
    port_note("READ_PORT_BUFFER_UCHAR", (ULONG)(uintptr_t)Port);
    memset(Buffer, 0xFF, Count);
}

void NTAPI READ_PORT_BUFFER_USHORT(USHORT *Port, USHORT *Buffer, ULONG Count)
{
    port_note("READ_PORT_BUFFER_USHORT", (ULONG)(uintptr_t)Port);
    memset(Buffer, 0xFF, Count * sizeof(USHORT));
}

void NTAPI READ_PORT_BUFFER_ULONG(ULONG *Port, ULONG *Buffer, ULONG Count)
{
    port_note("READ_PORT_BUFFER_ULONG", (ULONG)(uintptr_t)Port);
    memset(Buffer, 0xFF, Count * sizeof(ULONG));
}

void NTAPI WRITE_PORT_BUFFER_UCHAR(UCHAR *Port, UCHAR *Buffer, ULONG Count)
{
    (void)Buffer; (void)Count;
    port_note("WRITE_PORT_BUFFER_UCHAR", (ULONG)(uintptr_t)Port);
}

void NTAPI WRITE_PORT_BUFFER_USHORT(USHORT *Port, USHORT *Buffer, ULONG Count)
{
    (void)Buffer; (void)Count;
    port_note("WRITE_PORT_BUFFER_USHORT", (ULONG)(uintptr_t)Port);
}

void NTAPI WRITE_PORT_BUFFER_ULONG(ULONG *Port, ULONG *Buffer, ULONG Count)
{
    (void)Buffer; (void)Count;
    port_note("WRITE_PORT_BUFFER_ULONG", (ULONG)(uintptr_t)Port);
}

/* ---- profiling (xprof/xprofile.c, xbbt.cpp) ----------------------------
   No sampling profiler and no BBT sweeps: control calls succeed and the
   profile stays empty. */

/* XProfpGlobals: the sample buffer (empty) and the session's parameters. */
static struct {
    ULONG *bufstart, *bufend, *bufnext, *bufnext_stopped;
    ULONG start_type, start_param;
    ULONG reserved[16];
} xprof_data;

NTSTATUS NTAPI XProfpControl(ULONG Action, ULONG Param)
{
    static int once;
    if (!once++) xlog("XProfpControl(%u, %#x): the kernel profiler is not available", (unsigned)Action,
                      (unsigned)Param);
    return STATUS_SUCCESS;
}

PVOID NTAPI XProfpGetData(void) { return &xprof_data; }

void NTAPI IrtClientInitFast(PVOID irtp, ULONG *Counts, UCHAR *TobCounts, PVOID frtp, ULONG *SeqNums,
                             PVOID fid, PVOID unused)
{
    (void)irtp; (void)Counts; (void)TobCounts; (void)frtp; (void)SeqNums; (void)fid; (void)unused;
    xlog("IrtClientInitFast: BBT instrumentation data is not collected");
}

NTSTATUS NTAPI IrtSweep(HANDLE File)
{
    (void)File;
    return STATUS_INVALID_PARAMETER;   /* as when no instrumented client initialized */
}

/* ---- debugger memory (mmx/debugsup.c) ----------------------------------
   The development kit's extra memory for the debug monitor, from the
   arena; addresses are writable as they are. */

static ULONG mm_dbg_pages;

PVOID NTAPI MmDbgAllocateMemory(SIZE_T NumberOfBytes, ULONG Protect)
{
    (void)Protect;
    SIZE_T size = (NumberOfBytes + 0xFFF) & ~0xFFFu;
    PVOID p = size ? arena_alloc(size, 0) : NULL;
    if (p) __sync_add_and_fetch(&mm_dbg_pages, size >> 12);
    return p;
}

ULONG NTAPI MmDbgFreeMemory(PVOID BaseAddress, SIZE_T NumberOfBytes)
{
    ULONG pages = (NumberOfBytes + 0xFFF) >> 12;
    if (!BaseAddress) return 0;
    arena_free(BaseAddress);
    __sync_sub_and_fetch(&mm_dbg_pages, pages);
    return pages;
}

/* What is left of the 64 MB a development kit has above a retail console's. */
ULONG NTAPI MmDbgQueryAvailablePages(void)
{
    return mm_dbg_pages < 0x4000 ? 0x4000 - mm_dbg_pages : 0;
}

PVOID NTAPI MmDbgWriteCheck(PVOID VirtualAddress, PVOID Opaque)
{
    (void)Opaque;
    return VirtualAddress;
}

void NTAPI MmDbgReleaseAddress(PVOID VirtualAddress, PVOID Opaque) { (void)VirtualAddress; (void)Opaque; }

/* MmGlobalData (data export): pointers to the memory manager's state for
   the debug monitor.  Only the counters are filled. */
static ULONG mm_pfn_region[2], mm_pte_range[4], mm_available_pages = 0x2000, mm_pages_by_usage[16];
static ULONG mm_address_space_lock[8];
static PVOID mm_vad_root, mm_vad_hint, mm_vad_free_hint;
struct {
    PVOID RetailPfnRegion, SystemPteRange, AvailablePages, AllocatedPagesByUsage, AddressSpaceLock;
    PVOID VadRoot, VadHint, VadFreeHint;
} MmGlobalData = {
    mm_pfn_region, mm_pte_range, &mm_available_pages, mm_pages_by_usage, mm_address_space_lock,
    &mm_vad_root, &mm_vad_hint, &mm_vad_free_hint,
};
