/*
 * xbcompat: run an original Xbox XBE on Linux by providing the kernel it
 * imports and replacing the statically linked libraries that touch hardware.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "xbcompat.h"
#include "cpu.h"

void misc_init(void);

int g_screenshot_frame = 60;
const char *g_screenshot_path;
int g_exit_after_frames;

#ifdef XBC_NATIVE
/* int 2Dh is the kernel debugger service (DebugService in the NT CRT):
   eax = service, ecx/edx = arguments, followed by an int 3 that the kernel
   skips when it handles the request.  xapilib's OutputDebugString uses it. */
static bool debug_service(greg_t *r)
{
    const uint8_t *ip = (const uint8_t *)r[REG_EIP];
    if (ip[0] != 0xCD || ip[1] != 0x2D) return false;
    ULONG service = r[REG_EAX];
    if (service == 1 /* BREAKPOINT_PRINT */) {
        const ANSI_STRING *s = (const ANSI_STRING *)r[REG_ECX];
        if (s && s->Buffer) {
            int n = s->Length;
            while (n && (s->Buffer[n - 1] == '\n' || s->Buffer[n - 1] == '\r')) n--;
            xlog("[guest] %.*s", n, s->Buffer);
        }
    } else if (service == 2 /* BREAKPOINT_PROMPT */) {
        r[REG_EAX] = 0;   /* no characters read */
    } else {
        TRACE("debug service %u ignored", service);
    }
    r[REG_EIP] += 2;
    if (*(const uint8_t *)r[REG_EIP] == 0xCC) r[REG_EIP] += 1;
    return true;
}

/* Ring 0 instructions titles run themselves (wbinvd before handing memory
   to the GPU, cli/sti around hardware access) fault in user mode; none of
   them has anything to do here. */
static bool privileged_insn(greg_t *r)
{
    const uint8_t *ip = (const uint8_t *)r[REG_EIP];
    if (ip[0] == 0x0F && (ip[1] == 0x08 || ip[1] == 0x09)) { r[REG_EIP] += 2; return true; }   /* invd, wbinvd */
    if (ip[0] == 0xFA || ip[0] == 0xFB) { r[REG_EIP] += 1; return true; }                    /* cli, sti */
    return false;
}

/* rdtsc and rdtscp, which fault on guest threads (ke.c: thread_trap_tsc). */
static bool read_tsc(greg_t *r)
{
    const uint8_t *ip = (const uint8_t *)r[REG_EIP];
    int len = ip[0] == 0x0F && ip[1] == 0x31 ? 2 : ip[0] == 0x0F && ip[1] == 0x01 && ip[2] == 0xF9 ? 3 : 0;
    if (!len) return false;
    ULONGLONG t = ke_guest_tsc();
    r[REG_EAX] = (ULONG)t;
    r[REG_EDX] = (ULONG)(t >> 32);
    if (len == 3) r[REG_ECX] = 0;   /* rdtscp's processor id */
    r[REG_EIP] += len;
    return true;
}

static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    greg_t *r = uc->uc_mcontext.gregs;
    if (sig == SIGSEGV && (read_tsc(r) || debug_service(r) || privileged_insn(r))) return;
    if (sig == SIGTRAP) {
        /* int 3 (DbgBreakPoint and friends): nobody is listening, carry on. */
        xlog("breakpoint at eip=%08x ignored", r[REG_EIP]);
        return;
    }
    xlog("fatal signal %d (%s) at eip=%08x accessing %p", sig, strsignal(sig), r[REG_EIP], si->si_addr);
    xlog("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
         r[REG_EAX], r[REG_EBX], r[REG_ECX], r[REG_EDX], r[REG_ESI], r[REG_EDI], r[REG_EBP], r[REG_ESP]);
    ULONG *sp = (ULONG *)r[REG_ESP];
    xlog("  stack: %08x %08x %08x %08x %08x %08x %08x %08x", sp[0], sp[1], sp[2], sp[3], sp[4], sp[5],
         sp[6], sp[7]);
    /* Name the host code at the fault and unwind through the signal frame so a
       crash inside the HLE layer or a host library is attributable. */
    Dl_info info;
    if (dladdr((void *)r[REG_EIP], &info) && info.dli_fname)
        xlog("  eip is in %s%s%s+%#lx", info.dli_fname, info.dli_sname ? " " : "", info.dli_sname ? info.dli_sname : "",
             (unsigned long)(r[REG_EIP] - (greg_t)(info.dli_saddr ? info.dli_saddr : info.dli_fbase)));
    void *frames[32];
    int n = backtrace(frames, 32);
    char **names = backtrace_symbols(frames, n);
    for (int i = 0; i < n; i++) xlog("  #%d %s", i, names ? names[i] : "?");
    _exit(128 + sig);
}
#else
/* Guest code runs in the CPU emulator, which reports its own faults
   (cpu_unicorn.c); a signal here is a host crash, possibly while the
   emulator was touching guest memory for the guest. */
static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    xlog("fatal signal %d (%s) accessing %p", sig, strsignal(sig), si->si_addr);
#if defined(__arm__)
    xlog("  pc=%08lx lr=%08lx sp=%08lx", (unsigned long)uc->uc_mcontext.arm_pc,
         (unsigned long)uc->uc_mcontext.arm_lr, (unsigned long)uc->uc_mcontext.arm_sp);
    Dl_info info;
    if (dladdr((void *)uc->uc_mcontext.arm_pc, &info) && info.dli_fname)
        xlog("  pc is in %s %s +%#lx", info.dli_fname, info.dli_sname ? info.dli_sname : "",
             (unsigned long)((uintptr_t)uc->uc_mcontext.arm_pc - (uintptr_t)info.dli_fbase));
    /* Libraries like Mesa have no unwind tables: list the return addresses
       left on the stack instead, as library offsets to look up later. */
    const uintptr_t *sp = (const uintptr_t *)uc->uc_mcontext.arm_sp;
    for (int i = 0, n = 0; i < 512 && n < 24; i++) {
        if (!(sp[i] & 1) && (sp[i] & 3)) continue;   /* code addresses: ARM (4-aligned) or Thumb (odd) */
        if (dladdr((void *)sp[i], &info) && info.dli_fname && info.dli_fbase) {
            const char *f = strrchr(info.dli_fname, '/');
            xlog("  stack[%d] %s+%#lx %s", i, f ? f + 1 : info.dli_fname,
                 (unsigned long)(sp[i] - (uintptr_t)info.dli_fbase), info.dli_sname ? info.dli_sname : "");
            n++;
        }
    }
#else
    (void)uc;
#endif
    cpu_dump_guest();
    void *frames[32];
    int n = backtrace(frames, 32);
    char **names = backtrace_symbols(frames, n);
    for (int i = 0; i < n; i++) xlog("  #%d %s", i, names ? names[i] : "?");
    _exit(128 + sig);
}
#endif

/* Guest faults xbcompat answers itself (DbgPrint's int 2Dh, privileged
   instructions, int 3) arrive as these signals. SDL's console keyboard on
   KMSDRM installs its own handlers for them when its video subsystem starts,
   and they end the process on the first DbgPrint of a debug build, so every
   SDL_Init/SDL_InitSubSystem is followed by another call to this. */
void install_fault_handlers(void)
{
    struct sigaction sa = { 0 };
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}

static ULONG NTAPI run_entry_point(PVOID entry)
{
    /* The kernel calls the XBE entry point as a plain cdecl function on its
       initialization thread, then terminates that thread. */
#ifdef XBC_TRANSLATED
    CPU_CALL0(entry, CONV_CDECL);
#else
    ((void (CDECLAPI *)(void))entry)();
#endif
    return 0;
}

__attribute__((weak)) void hle_patch(xbe_image *img, const char *sigfile)
{
    (void)img; (void)sigfile;
    xlog("built without HLE libraries");
}

static void usage(void)
{
    fprintf(stderr,
            "usage: xbcompat [options] game.xbe\n"
            "  --hle FILE   symbol map from tools/findsigs.py (library functions to replace)\n"
            "  --hdd DIR    directory backing the hard disk partitions (default ~/.local/share/xbcompat/hdd)\n"
            "  --trace      log every kernel call\n"
            "  --screenshot FILE  save frame --shot-frame (default 60) as a BMP\n"
            "  --shot-frame N     which frame --screenshot captures (every Nth when FILE has %%d)\n"
            "  --frames N   exit after presenting N frames\n"
            "  --log FILE   write the log to FILE instead of stderr\n"
            "  --dvd DIR    directory backing the DVD drive (default: the XBE's directory),\n"
            "               or an Xbox disc image (XISO), or an optical drive (/dev/sr0)\n"
            "  --dvd-drive DEV  an optical drive whose disc is the tray while the drive is\n"
            "               plugged in (discs can go in and out while titles run)\n"
            "  --card DIR   the game card (Kazeta cart media); default: the first card\n"
            "               with a .kzi or .kzp under /media or /run/media\n"
            "  --d-path P, --xbe-path P, --launch-data FILE\n"
            "               D: target, image path and launch page of a title started by\n"
            "               XLaunchNewImage (set when xbcompat relaunches itself)\n");
    exit(2);
}

char *g_dvd_root, *g_dvd_drive;

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "hle", required_argument, 0, 'h' }, { "hdd", required_argument, 0, 'd' },
        { "trace", no_argument, 0, 't' }, { "log", required_argument, 0, 'l' },
        { "screenshot", required_argument, 0, 's' }, { "shot-frame", required_argument, 0, 'n' },
        { "frames", required_argument, 0, 'f' }, { "dvd", required_argument, 0, 'v' },
        { "d-path", required_argument, 0, 'D' }, { "launch-data", required_argument, 0, 'L' },
        { "xbe-path", required_argument, 0, 'X' }, { "card", required_argument, 0, 'c' },
        { "dvd-drive", required_argument, 0, 'R' },
        { 0, 0, 0, 0 },
    };
    const char *hle = NULL, *hdd = NULL, *d_path = NULL, *launch_data = NULL, *xbe_rel = NULL;
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'h': hle = optarg; break;
        case 'd': hdd = optarg; break;
        case 't': g_trace = 1; break;
        case 'l': g_log = fopen(optarg, "w"); break;
        case 's': g_screenshot_path = optarg; break;
        case 'n': g_screenshot_frame = atoi(optarg); break;
        case 'f': g_exit_after_frames = atoi(optarg); break;
        case 'v': g_dvd_root = optarg; break;
        case 'D': d_path = optarg; break;
        case 'L': launch_data = optarg; break;
        case 'X': xbe_rel = optarg; break;
        case 'c': fs_set_card(optarg); break;
        case 'R': g_dvd_drive = optarg; break;
        default: usage();
        }
    }
    if (optind != argc - 1) usage();
    const char *xbe = argv[optind];

    char hddbuf[4096];
    if (!hdd) {
        const char *home = getenv("HOME");
        snprintf(hddbuf, sizeof(hddbuf), "%s/.local/share/xbcompat/hdd", home ? home : ".");
        hdd = hddbuf;
    }

    install_fault_handlers();
    prof_init();

    mem_init();
#ifdef XBC_TRANSLATED
    cpu_init();
#endif
    thread_init_main();
    timers_init();
    misc_init();

    xbe_image img;
    xbe_load(xbe, &img);
    fs_init(xbe, hdd, g_dvd_root, g_dvd_drive);
    launch_init(argc, argv, d_path, launch_data, xbe_rel);
    kernel_resolve_imports(&img);
    hle_patch(&img, hle);

    thread_create(img.header->SizeOfStackCommit, 0, NULL, (PVOID)run_entry_point,
                  (PVOID)img.entry, false);
    for (;;) pause();
}
