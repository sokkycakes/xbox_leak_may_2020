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
#include "kernel/net.h"

void misc_init(void);

int g_screenshot_frame = 60;
const char *g_screenshot_path;
int g_exit_after_frames;
volatile unsigned g_guest_traps;

#ifdef XBC_NATIVE
bool rtl_redirect_fault(void *ucontext, int sig, ULONG code, ULONG addr, ULONG write);
bool gpu_regs_trap_fault(void *addr, void *ucontext);
bool gpu_regs_trap_step(void *ucontext);
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
/* ---- an interrupt table for titles that hook it -------------------------

   Fable pages part of its memory itself: it reads the IDT's address with
   sidt, points the page fault vector at its own handler, and pages in on
   the faults (ending with iret, which works at any privilege level).  sidt
   isn't privileged (or the host kernel answers it with a dummy address), so
   the loader turns the cli; sidt pairs it finds in code into cli; hlt, and
   the hlt is answered with a table in a page of contiguous memory nothing
   else uses.  Every vector starts out pointing at a hlt of its own in the
   page after it; a guest page fault goes to vector 14's handler while a
   title has hooked it, and reaching the stub (the handler passing it on)
   unwinds the frame and raises the access violation as usual. */
#define IDT_ADDR  (CONTIG_BASE + 0xE000u)
#define IDT_STUBS (CONTIG_BASE + 0xD000u)
static uint32_t sidt_sites[16];
static int sidt_count;
static bool idt_ready;
static __thread uint32_t idt_cr2;

void idt_patch_section(uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i + 4 <= len && sidt_count < 16; i++) {
        if (p[i] != 0xFA || p[i + 1] != 0x0F || p[i + 2] != 0x01) continue;
        uint8_t m = p[i + 3];
        if (((m >> 3) & 7) != 1 || (m >> 6) == 3) continue;
        p[i + 1] = 0xF4;
        sidt_sites[sidt_count++] = (uint32_t)(uintptr_t)(p + i + 1);
        xlog("loader: sidt at %#x is answered with xbcompat's interrupt table", (uint32_t)(uintptr_t)(p + i + 1));
    }
}

static void idt_init(void)
{
    if (idt_ready) return;
    memset((void *)IDT_STUBS, 0xF4, 256);
    uint32_t *idt = (uint32_t *)IDT_ADDR;
    for (uint32_t v = 0; v < 256; v++) {
        uint32_t off = IDT_STUBS + v;
        idt[v * 2] = (off & 0xFFFF) | (0x8u << 16);         /* offset low, kernel code selector */
        idt[v * 2 + 1] = (off & 0xFFFF0000u) | 0x8E00u;     /* present 32-bit interrupt gate */
    }
    idt_ready = true;
}

static uint32_t idt_vector(unsigned v)
{
    const uint32_t *idt = (const uint32_t *)IDT_ADDR;
    return (idt[v * 2] & 0xFFFF) | (idt[v * 2 + 1] & 0xFFFF0000u);
}

static bool idt_hooked(unsigned v)
{
    return idt_ready && idt_vector(v) != IDT_STUBS + v;
}

/* Once a title runs its own page fault handler, cli and sti are a lock: its
   paging code counts on cli keeping every other thread out, as it does on
   the console's one processor, and it ran on several threads at once here
   (Fable hung paging the same page in and out).  A thread gives the lock up
   while it waits, as a context switch would turn interrupts back on.
   Other titles' cli and sti still do nothing. */
static pthread_mutex_t cli_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread bool cli_held;

static void idt_cli(bool disable)
{
    if (disable && !cli_held && idt_hooked(14)) {
        pthread_mutex_lock(&cli_lock);
        cli_held = true;
    } else if (!disable && cli_held) {
        cli_held = false;
        pthread_mutex_unlock(&cli_lock);
    }
}

bool idt_cli_release(void)
{
    if (!cli_held) return false;
    cli_held = false;
    pthread_mutex_unlock(&cli_lock);
    return true;
}

void idt_cli_reacquire(void)
{
    pthread_mutex_lock(&cli_lock);
    cli_held = true;
}

static const int gpr_index[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI };

/* The address a ModR/M memory operand names and the operand's length
   (ModR/M byte on): 32-bit addressing, no segment overrides. */
static uint32_t modrm_address(const greg_t *r, const uint8_t *m, int *len)
{
    int mod = m[0] >> 6, rm = m[0] & 7, n = 1;
    uint32_t a = 0;
    if (rm == 4) {
        int ss = m[1] >> 6, idx = (m[1] >> 3) & 7, base = m[1] & 7;
        n = 2;
        if (idx != 4) a += (uint32_t)r[gpr_index[idx]] << ss;
        if (base == 5 && mod == 0) { a += *(const uint32_t *)(m + n); n += 4; }
        else a += (uint32_t)r[gpr_index[base]];
    } else if (rm == 5 && mod == 0) {
        a = *(const uint32_t *)(m + 1);
        n += 4;
    } else {
        a = (uint32_t)r[gpr_index[rm]];
    }
    if (mod == 1) { a += (uint32_t)(int8_t)m[n]; n += 1; }
    else if (mod == 2) { a += *(const uint32_t *)(m + n); n += 4; }
    *len = n;
    return a;
}

static bool privileged_insn(greg_t *r)
{
    const uint8_t *ip = (const uint8_t *)r[REG_EIP];
    if (ip[0] == 0x0F && (ip[1] == 0x08 || ip[1] == 0x09)) { r[REG_EIP] += 2; return true; }   /* invd, wbinvd */
    if (ip[0] == 0xFA || ip[0] == 0xFB) { idt_cli(ip[0] == 0xFA); r[REG_EIP] += 1; return true; }   /* cli, sti */
    /* mov to and from control registers, and invlpg: titles that edit page
       tables themselves (memory.c). Other control registers read as a
       paging, write-protecting console's would and ignore writes. */
    if (ip[0] == 0x0F && (ip[1] == 0x20 || ip[1] == 0x22) && (ip[2] >> 6) == 3) {
        int cr = (ip[2] >> 3) & 7, reg = gpr_index[ip[2] & 7];
        if (ip[1] == 0x20) r[reg] = cr == 3 ? mem_read_cr3() : cr == 2 ? idt_cr2 : cr == 0 ? 0x8001003B : cr == 4 ? 0x610 : 0;
        else if (cr == 3) mem_load_cr3();
        r[REG_EIP] += 3;
        return true;
    }
    if (ip[0] == 0x0F && ip[1] == 0x01 && ((ip[2] >> 3) & 7) == 7 && (ip[2] >> 6) != 3) {
        int n;
        mem_invlpg(modrm_address(r, ip + 2, &n));
        r[REG_EIP] += 2 + n;
        return true;
    }
    if (ip[0] == 0xF4 && ip[1] == 0x01) {
        for (int i = 0; i < sidt_count; i++) {
            if (sidt_sites[i] != (uint32_t)r[REG_EIP]) continue;
            int n;
            uint8_t *d = (uint8_t *)(uintptr_t)modrm_address(r, ip + 2, &n);
            idt_init();
            *(uint16_t *)d = 256 * 8 - 1;
            *(uint32_t *)(d + 2) = IDT_ADDR;
            r[REG_EIP] += 2 + n;
            return true;
        }
    }
    return false;
}

/* A page fault in guest code while the title has hooked vector 14: push an
   interrupt frame (error code, eip, cs, eflags) on the thread's stack as
   the CPU does in ring 0, and run the title's handler. */
static bool idt_page_fault(greg_t *r, uint32_t addr, bool write, bool fetch)
{
    if (!idt_hooked(14)) return false;
    uint32_t *sp = (uint32_t *)(uintptr_t)(r[REG_ESP] - 16);
    sp[0] = (write ? 2 : 0) | (fetch ? 0x10 : 0);
    sp[1] = r[REG_EIP];
    sp[2] = r[REG_CS];
    sp[3] = r[REG_EFL];
    r[REG_ESP] = (greg_t)(uintptr_t)sp;
    idt_cr2 = addr;
    r[REG_EIP] = idt_vector(14);
    return true;
}

/* Host code is about to read or write guest memory (a file read or write):
   page in what the title's own tables leave out, by calling its handler the
   way the fault would have, since a host access can't take that fault.  The
   handler gives every register back and returns to the pushed address. */
void guest_page_in(const void *p, size_t len, bool write)
{
#if defined(__i386__)
    if (!len || !idt_hooked(14)) return;
    static __thread uint32_t handler, code;
    for (uint32_t va = (uint32_t)(uintptr_t)p & ~0xFFFu; va < (uint32_t)(uintptr_t)p + len; va += 0x1000) {
        if (!mem_page_absent(va)) {
            if (write) mem_fault_resync(va, true);
            continue;
        }
        handler = idt_vector(14);
        code = write ? 2 : 0;
        idt_cr2 = va < (uint32_t)(uintptr_t)p ? (uint32_t)(uintptr_t)p : va;
        __asm__ volatile("pushfl\n\t"
                         "pushl %%cs\n\t"
                         "pushl $1f\n\t"
                         "pushl %1\n\t"
                         "jmp *%0\n"
                         "1:"
                         : : "m"(handler), "m"(code) : "memory", "cc", "eax", "ecx", "edx", "esi", "edi");
        if (write) mem_fault_resync(va, true);
        if (va + 0x1000 < va) break;
    }
#else
    (void)p; (void)len; (void)write;
#endif
}

/* The handler passed the fault on to the vector's original entry. */
static bool idt_stub(ucontext_t *uc)
{
    greg_t *r = uc->uc_mcontext.gregs;
    uint32_t eip = r[REG_EIP];
    if (!idt_ready || eip < IDT_STUBS || eip >= IDT_STUBS + 256) return false;
    unsigned v = eip - IDT_STUBS;
    uint32_t *sp = (uint32_t *)(uintptr_t)r[REG_ESP];
    bool err = v == 8 || (v >= 10 && v <= 14) || v == 17;
    uint32_t code = err ? *sp++ : 0;
    r[REG_EIP] = sp[0];
    r[REG_EFL] = sp[2];
    r[REG_ESP] = (greg_t)(uintptr_t)(sp + 3);
    if (v == 14 && rtl_redirect_fault(uc, SIGSEGV, 0xC0000005, idt_cr2, (code & 2) ? 1 : 0)) return true;
    xlog("fatal: interrupt %u at eip=%08x reached the end of the title's handlers", v, r[REG_EIP]);
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
    /* A jump to unmapped code faults on the fetch: nothing to decode there. */
    bool fetch = sig == SIGSEGV && (greg_t)si->si_addr == r[REG_EIP];
    if (sig == SIGSEGV && !fetch && (read_tsc(r) || debug_service(r) || privileged_insn(r))) { g_guest_traps++; return; }
    if (sig == SIGSEGV && !fetch && idt_stub(uc)) return;
    if (sig == SIGSEGV && nic_trap_fault(si->si_addr, uc)) return;
    if (sig == SIGTRAP && nic_trap_step(uc)) return;
    if (sig == SIGSEGV && gpu_regs_trap_fault(si->si_addr, uc)) return;
    if (sig == SIGTRAP && gpu_regs_trap_step(uc)) return;
    if (sig == SIGTRAP && probe_trap(uc)) return;
    if (sig == SIGTRAP) {
        /* int 3 (DbgBreakPoint and friends): nobody is listening, carry on. */
        xlog("breakpoint at eip=%08x ignored", r[REG_EIP]);
        return;
    }
    /* A fault in guest code goes to the title's page fault handler if it
       hooked one, else to its SEH handlers (rtl.c). */
    Dl_info gi;
    if (sig == SIGSEGV && mem_fault_resync((uint32_t)(uintptr_t)si->si_addr, (r[REG_ERR] & 2) != 0)) return;
    if (sig == SIGSEGV && thread_current() && !dladdr((void *)r[REG_EIP], &gi)
        && idt_page_fault(r, (uint32_t)(uintptr_t)si->si_addr, (r[REG_ERR] & 2) != 0, fetch))
        return;
    if (sig != SIGTRAP && thread_current() && !dladdr((void *)r[REG_EIP], &gi)) {
        ULONG code = sig == SIGILL ? 0xC000001D
                   : sig == SIGFPE ? (si->si_code == FPE_INTOVF ? 0xC0000095 : 0xC0000094)
                   : 0xC0000005;
        if (rtl_redirect_fault(uc, sig, code, (ULONG)si->si_addr, (r[REG_ERR] & 2) ? 1 : 0)) return;
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
    if (cpu_fix_fault(sig, uc_)) return;
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
            "  --net nat|tunnel:HOST:PORT[:LOCALPORT]|none\n"
            "               emulate the Ethernet controller: NAT to the host's network, or\n"
            "               frames in UDP to another xbcompat (System Link); default none\n"
            "               (or XBCOMPAT_NET)\n"
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
        { "dvd-drive", required_argument, 0, 'R' }, { "net", required_argument, 0, 'N' },
        { 0, 0, 0, 0 },
    };
    const char *net = getenv("XBCOMPAT_NET");
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
        case 'N': net = optarg; break;
        default: usage();
        }
    }
    if (optind != argc - 1) usage();
    const char *xbe = argv[optind];
    wincart_exec(xbe);   /* a Windows cart's stub: wincart-run takes over this process */

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
    identity_init(hdd);
    nic_init(net, identity_mac());
    launch_init(argc, argv, d_path, launch_data, xbe_rel);
    kernel_resolve_imports(&img);
    reset_init(((XBE_CERTIFICATE *)img.header->Certificate)->TitleID);
    guide_launch_pending(((XBE_CERTIFICATE *)img.header->Certificate)->TitleID);
    hle_patch(&img, hle);
    probe_init();

    thread_create(img.header->SizeOfStackCommit, 0, NULL, (PVOID)run_entry_point,
                  (PVOID)img.entry, false);
    for (;;) pause();
}
