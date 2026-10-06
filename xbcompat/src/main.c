/*
 * xbcompat: run an original Xbox XBE on Linux by providing the kernel it
 * imports and replacing the statically linked libraries that touch hardware.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "xbcompat.h"

void misc_init(void);

int g_screenshot_frame = 60;
const char *g_screenshot_path;
int g_exit_after_frames;

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

static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    greg_t *r = uc->uc_mcontext.gregs;
    if (sig == SIGSEGV && debug_service(r)) return;
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
    _exit(128 + sig);
}

static ULONG NTAPI run_entry_point(PVOID entry)
{
    /* The kernel calls the XBE entry point as a plain cdecl function on its
       initialization thread, then terminates that thread. */
    ((void (CDECLAPI *)(void))entry)();
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
            "  --shot-frame N     which frame --screenshot captures\n"
            "  --frames N   exit after presenting N frames\n"
            "  --log FILE   write the log to FILE instead of stderr\n");
    exit(2);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "hle", required_argument, 0, 'h' }, { "hdd", required_argument, 0, 'd' },
        { "trace", no_argument, 0, 't' }, { "log", required_argument, 0, 'l' },
        { "screenshot", required_argument, 0, 's' }, { "shot-frame", required_argument, 0, 'n' },
        { "frames", required_argument, 0, 'f' }, { 0, 0, 0, 0 },
    };
    const char *hle = NULL, *hdd = NULL;
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

    struct sigaction sa = { 0 };
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);

    mem_init();
    thread_init_main();
    timers_init();
    misc_init();

    xbe_image img;
    xbe_load(xbe, &img);
    fs_init(xbe, hdd);
    kernel_resolve_imports(&img);
    hle_patch(&img, hle);

    thread_create(img.header->SizeOfStackCommit, 0, NULL, (PVOID)run_entry_point,
                  (PVOID)img.entry, false);
    for (;;) pause();
}
