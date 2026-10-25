/*
 * xbcompat-gpu: the xbcompat side of the emulated NV2A.
 *
 * Titles run their own statically linked Direct3D, which drives the GPU
 * through its registers at 0xFD000000 and push buffers in contiguous
 * memory, as on the console.  This file
 *   - maps the register window (trapped) and the PRAMIN instance memory
 *     aperture (plain memory) and hands both to nv2a_host.c,
 *   - emulates each instruction that touches a register: the common
 *     mov/movzx/test/ALU forms are decoded and executed against the
 *     device, anything else is single-stepped over a page holding the
 *     register's value (like the NIC in kernel/nic.c),
 *   - delivers the GPU interrupt to the ISR D3D connected,
 *   - runs the display loop on the main thread: vblank, then the CRTC's
 *     framebuffer to the window,
 *   - stands in for the parts of hle/d3d8.c the rest of xbcompat calls.
 *
 * Built only into the GPL test executable xbcompat-gpu (make GPU=1).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>

#include "../xbcompat.h"
#include "../hle/hle.h"
#include "gpu_host.h"

#define GPU_IRQ 3

void gpu_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    xlog("%s", buf);
}

void gpu_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fatal("%s", buf);
}

void gpu_thread_started(const char *name) { (void)name; }

/* ---- the stand-ins for hle/d3d8.c --------------------------------------- */

const struct hle_func d3d8_funcs[] = { { NULL, NULL } };
void d3d_bind_globals(void) {}

static volatile ULONG frames_shown;
ULONG d3d_frame_count(void) { return frames_shown; }

/* ---- memory ------------------------------------------------------------- */

static uint8_t *pramin;

void gpu_init(void)
{
    gpu_cfg.trace_mmio = getenv("XBCOMPAT_GPU_TRACE_MMIO") != NULL;
    void *w = mmap((void *)GPU_MMIO_BASE, GPU_MMIO_SIZE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (w != (void *)GPU_MMIO_BASE) fatal("GPU register window unavailable: %s", strerror(errno));
    pramin = mmap((void *)(GPU_MMIO_BASE + GPU_PRAMIN_OFF), GPU_PRAMIN_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (pramin != (void *)(GPU_MMIO_BASE + GPU_PRAMIN_OFF)) fatal("PRAMIN aperture unavailable: %s", strerror(errno));
    /* The window's context first: xemu's contexts share with it, and the
       device's FIFO thread starts rendering as soon as it exists. */
    const char *fs = getenv("XBCOMPAT_FULLSCREEN");
    gpu_display_init(640, 480, fs && *fs);
    gpu_host_init((uint8_t *)CONTIG_BASE, CONTIG_SIZE, pramin, GPU_PRAMIN_SIZE);
    xlog("GPU: emulated NV2A (xemu's model); the title's own Direct3D drives it");
}

/* ---- register access emulation ----------------------------------------- */

static inline bool in_window(uint32_t a)
{
    return a >= GPU_MMIO_BASE && a < GPU_MMIO_BASE + GPU_MMIO_SIZE &&
           !(a >= GPU_MMIO_BASE + GPU_PRAMIN_OFF && a < GPU_MMIO_BASE + GPU_PRAMIN_OFF + GPU_PRAMIN_SIZE);
}

static uint32_t rd(uint32_t a, unsigned size)
{
    uint32_t off = a - GPU_MMIO_BASE;
    if (size == 4 && (off & 3)) {
        /* unaligned dword: two halves of neighbouring registers */
        uint32_t lo = gpu_mmio_read(off & ~3u, 4), hi = gpu_mmio_read((off & ~3u) + 4, 4);
        unsigned sh = (off & 3) * 8;
        return (lo >> sh) | (hi << (32 - sh));
    }
    return gpu_mmio_read(off, size);
}

static void wr(uint32_t a, uint32_t v, unsigned size)
{
    gpu_mmio_write(a - GPU_MMIO_BASE, v, size);
}

static greg_t *reg32(greg_t *r, int n)
{
    static const int map[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_UESP, REG_EBP, REG_ESI, REG_EDI };
    return &r[map[n & 7]];
}

static uint32_t get_reg(greg_t *r, int n, unsigned size)
{
    if (size == 1) {
        uint32_t v = (uint32_t)*reg32(r, n & 3);
        return n & 4 ? (v >> 8) & 0xff : v & 0xff;
    }
    uint32_t v = (uint32_t)*reg32(r, n);
    return size == 2 ? v & 0xffff : v;
}

static void set_reg(greg_t *r, int n, uint32_t v, unsigned size)
{
    if (size == 1) {
        greg_t *p = reg32(r, n & 3);
        if (n & 4) *p = (*p & ~0xff00) | ((v & 0xff) << 8);
        else *p = (*p & ~0xff) | (v & 0xff);
    } else if (size == 2) {
        greg_t *p = reg32(r, n);
        *p = (*p & ~0xffff) | (v & 0xffff);
    } else {
        *reg32(r, n) = v;
    }
}

#define F_CF 0x001
#define F_PF 0x004
#define F_ZF 0x040
#define F_SF 0x080
#define F_OF 0x800

static void set_flags(greg_t *r, uint32_t res, unsigned size, bool cf, bool of)
{
    uint32_t mask = size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
    res &= mask;
    uint32_t f = (uint32_t)r[REG_EFL] & ~(F_CF | F_PF | F_ZF | F_SF | F_OF | 0x10);
    if (cf) f |= F_CF;
    if (of) f |= F_OF;
    if (!res) f |= F_ZF;
    if (res & (1u << (size * 8 - 1))) f |= F_SF;
    if (!__builtin_parity(res & 0xff)) f |= F_PF;
    r[REG_EFL] = f;
}

/* add/or/adc/sbb/and/sub/xor/cmp on a, b; returns the result, sets flags. */
static uint32_t alu(greg_t *r, int op, uint32_t a, uint32_t b, unsigned size)
{
    uint32_t mask = size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1, sign = 1u << (size * 8 - 1);
    a &= mask; b &= mask;
    uint32_t res;
    switch (op) {
    case 0: res = (a + b) & mask;
        set_flags(r, res, size, res < a, (~(a ^ b) & (a ^ res) & sign) != 0); return res;
    case 1: res = a | b; set_flags(r, res, size, false, false); return res;
    case 4: res = a & b; set_flags(r, res, size, false, false); return res;
    case 6: res = a ^ b; set_flags(r, res, size, false, false); return res;
    case 5: case 7: res = (a - b) & mask;
        set_flags(r, res, size, a < b, ((a ^ b) & (a ^ res) & sign) != 0); return res;
    default: return a;   /* adc/sbb: not decoded */
    }
}

/* Length of the ModRM operand starting at p (ModRM byte included). */
static int modrm_len(const uint8_t *p)
{
    int mod = p[0] >> 6, rm = p[0] & 7, n = 1;
    if (mod == 3) return n;
    if (rm == 4) {
        int base = p[1] & 7;
        n++;
        if (mod == 0 && base == 5) n += 4;
    } else if (mod == 0 && rm == 5) {
        n += 4;
    }
    if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    return n;
}

/* Emulates the instruction at eip, which accessed register address a.
   False when it isn't one of the decoded forms. */
static bool emulate(greg_t *r, uint32_t a)
{
    const uint8_t *ip = (const uint8_t *)r[REG_EIP], *p = ip;
    unsigned osize = 4;
    for (;; p++) {
        if (*p == 0x66) osize = 2;
        else if (*p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36) continue;
        else break;
    }
    uint8_t op = *p++;
    if (op == 0x0F) {
        uint8_t op2 = *p++;
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            unsigned src = op2 & 1 ? 2 : 1;
            int reg = (p[0] >> 3) & 7;
            uint32_t v = rd(a, src);
            if (op2 >= 0xBE) v = src == 1 ? (uint32_t)(int32_t)(int8_t)v : (uint32_t)(int32_t)(int16_t)v;
            set_reg(r, reg, v, osize);
            r[REG_EIP] = (greg_t)(p + modrm_len(p));
            return true;
        }
        return false;
    }
    unsigned size = (op & 1) ? osize : 1;
    int reg = (p[0] >> 3) & 7, len;
    switch (op) {
    case 0x8B: case 0x8A:   /* mov r, m */
        set_reg(r, reg, rd(a, size), size);
        r[REG_EIP] = (greg_t)(p + modrm_len(p));
        return true;
    case 0x89: case 0x88:   /* mov m, r */
        wr(a, get_reg(r, reg, size), size);
        r[REG_EIP] = (greg_t)(p + modrm_len(p));
        return true;
    case 0xC7: case 0xC6: { /* mov m, imm */
        if (reg != 0) return false;
        len = modrm_len(p);
        uint32_t imm = size == 4 ? *(const uint32_t *)(p + len) : size == 2 ? *(const uint16_t *)(p + len) : p[len];
        wr(a, imm, size);
        r[REG_EIP] = (greg_t)(p + len + size);
        return true;
    }
    case 0xA1: case 0xA0:   /* mov eax, [moffs] */
        set_reg(r, 0, rd(a, size), size);
        r[REG_EIP] = (greg_t)(p + 4);
        return true;
    case 0xA3: case 0xA2:   /* mov [moffs], eax */
        wr(a, get_reg(r, 0, size), size);
        r[REG_EIP] = (greg_t)(p + 4);
        return true;
    case 0x85: case 0x84:   /* test m, r */
        alu(r, 4, rd(a, size), get_reg(r, reg, size), size);
        r[REG_EIP] = (greg_t)(p + modrm_len(p));
        return true;
    case 0xF7: case 0xF6: { /* test m, imm */
        if (reg != 0) return false;
        len = modrm_len(p);
        uint32_t imm = size == 4 ? *(const uint32_t *)(p + len) : size == 2 ? *(const uint16_t *)(p + len) : p[len];
        alu(r, 4, rd(a, size), imm, size);
        r[REG_EIP] = (greg_t)(p + len + size);
        return true;
    }
    case 0x81: case 0x83: case 0x80: { /* op m, imm */
        if (reg == 2 || reg == 3) return false;
        len = modrm_len(p);
        unsigned isz = op == 0x81 ? size : 1;
        uint32_t imm = isz == 4 ? *(const uint32_t *)(p + len) : isz == 2 ? *(const uint16_t *)(p + len) : p[len];
        if (op == 0x83) imm = (uint32_t)(int32_t)(int8_t)imm;
        uint32_t res = alu(r, reg, rd(a, size), imm, size);
        if (reg != 7) wr(a, res, size);
        r[REG_EIP] = (greg_t)(p + len + isz);
        return true;
    }
    case 0x01: case 0x09: case 0x21: case 0x29: case 0x31: case 0x39:
    case 0x00: case 0x08: case 0x20: case 0x28: case 0x30: case 0x38: { /* op m, r */
        int aop = op >> 3;
        uint32_t res = alu(r, aop, rd(a, size), get_reg(r, reg, size), size);
        if (aop != 7) wr(a, res, size);
        r[REG_EIP] = (greg_t)(p + modrm_len(p));
        return true;
    }
    case 0x03: case 0x0B: case 0x23: case 0x2B: case 0x33: case 0x3B:
    case 0x02: case 0x0A: case 0x22: case 0x2A: case 0x32: case 0x3A: { /* op r, m */
        int aop = op >> 3;
        uint32_t res = alu(r, aop, get_reg(r, reg, size), rd(a, size), size);
        if (aop != 7) set_reg(r, reg, res, size);
        r[REG_EIP] = (greg_t)(p + modrm_len(p));
        return true;
    }
    default:
        return false;
    }
}

/* Anything else: single-step it over the page with the register's value in
   place, then apply what it stored. */
static pthread_mutex_t step_lock = PTHREAD_MUTEX_INITIALIZER;
static bool stepping;
static uint32_t step_page, step_addr, step_before;

bool gpu_trap_fault(void *addr, void *ucv)
{
    uint32_t a = (uint32_t)(uintptr_t)addr;
    if (!in_window(a)) return false;
    greg_t *r = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    if (emulate(r, a)) return true;
    static unsigned noted;
    if (noted < 20) {
        noted++;
        const uint8_t *ip = (const uint8_t *)r[REG_EIP];
        xlog("GPU: stepping over %02x %02x %02x %02x at %08x (register %#x)", ip[0], ip[1], ip[2], ip[3],
             (unsigned)r[REG_EIP], a - GPU_MMIO_BASE);
    }
    pthread_mutex_lock(&step_lock);
    step_page = a & ~0xFFFu;
    step_addr = a & ~3u;
    mprotect((void *)step_page, 0x1000, PROT_READ | PROT_WRITE);
    step_before = rd(step_addr, 4);
    *(volatile uint32_t *)step_addr = step_before;
    stepping = true;
    r[REG_EFL] |= 0x100;   /* TF */
    return true;
}

bool gpu_trap_step(void *ucv)
{
    if (!stepping) return false;
    greg_t *r = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    r[REG_EFL] &= ~0x100;
    stepping = false;
    uint32_t v = *(volatile uint32_t *)step_addr;
    if (v != step_before) wr(step_addr, v, 4);
    mprotect((void *)step_page, 0x1000, PROT_NONE);
    pthread_mutex_unlock(&step_lock);
    return true;
}

/* ---- interrupt ---------------------------------------------------------- */

typedef BOOLEAN (NTAPI *isr_fn)(PVOID Interrupt, PVOID Context);
static pthread_mutex_t irq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t irq_cond = PTHREAD_COND_INITIALIZER;
static PVOID irq_object, irq_context;
static isr_fn irq_routine;
static bool irq_connected, irq_thread_started;

static void *irq_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "gpu-irq");
    thread_adopt_host("gpu-irq");
    pthread_mutex_lock(&irq_lock);
    for (;;) {
        while (!irq_connected || !gpu_irq_asserted())
            pthread_cond_wait(&irq_cond, &irq_lock);
        PVOID obj = irq_object, ctx = irq_context;
        isr_fn isr = irq_routine;
        pthread_mutex_unlock(&irq_lock);
        ke_dispatch_enter(26 - GPU_IRQ);
        isr(obj, ctx);
        ke_dispatch_leave();
        pthread_mutex_lock(&irq_lock);
        if (gpu_irq_asserted()) {
            /* still asserted (a source the ISR left pending): don't spin */
            pthread_mutex_unlock(&irq_lock);
            struct timespec ts = { 0, 200000 };
            nanosleep(&ts, NULL);
            pthread_mutex_lock(&irq_lock);
        }
    }
    return NULL;
}

void gpu_irq_changed(bool level)
{
    if (!level) return;
    pthread_mutex_lock(&irq_lock);
    pthread_cond_broadcast(&irq_cond);
    pthread_mutex_unlock(&irq_lock);
}

bool gpu_interrupt_init(PVOID Interrupt, PVOID ServiceRoutine, PVOID ServiceContext, ULONG Vector)
{
    if (Vector != 0x30 + GPU_IRQ) return false;
    pthread_mutex_lock(&irq_lock);
    irq_object = Interrupt;
    irq_routine = (isr_fn)ServiceRoutine;
    irq_context = ServiceContext;
    pthread_mutex_unlock(&irq_lock);
    return true;
}

bool gpu_interrupt_connect(PVOID Interrupt, bool connect)
{
    if (!irq_object || Interrupt != irq_object) return false;
    pthread_mutex_lock(&irq_lock);
    irq_connected = connect;
    if (!irq_thread_started) {
        pthread_t t;
        pthread_create(&t, NULL, irq_thread, NULL);
        irq_thread_started = true;
    }
    pthread_cond_broadcast(&irq_cond);
    pthread_mutex_unlock(&irq_lock);
    xlog("GPU: Direct3D %s its interrupt", connect ? "connected" : "disconnected");
    return true;
}

/* ---- the kernel's video mode call --------------------------------------- */

/* AvSetDisplayMode: the real kernel programs the CRTC and the encoder; the
   display here only needs the size, pitch, depth and framebuffer. */
void gpu_av_set_display_mode(ULONG Mode, ULONG Format, ULONG Pitch, ULONG FrameBuffer)
{
    (void)Mode;
    /* D3DFMT_LIN_A8R8G8B8 0x12, LIN_X8R8G8B8 0x1E: 32 bpp; R5G6B5/X1R5G5B5: 16 */
    int bpp = Format == 0x12 || Format == 0x1E || Format == 0x3F || Format == 0x40 ? 32 : 16;
    int width = Pitch ? (int)(Pitch / (bpp / 8)) : 640, height = 480;
    if (width > 1280) width = 1920, height = 1080;
    else if (width > 720) width = 1280, height = 720;
    else if (width > 640) width = 720;
    xlog("GPU: AvSetDisplayMode(mode %#x, format %#x, pitch %u, fb %#x) -> %dx%d %d bpp", Mode, Format, Pitch,
         FrameBuffer, width, height, bpp);
    gpu_set_display_mode(width, height, Pitch, bpp, FrameBuffer);
}

/* ---- display loop (main thread) ---------------------------------------- */

extern int g_screenshot_frame, g_exit_after_frames;
extern const char *g_screenshot_path;

static void save_bmp(const char *path, const uint8_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { xlog("GPU: cannot write %s", path); return; }
    uint32_t row = (uint32_t)w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * h;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v[] = { 54 + size, 0, 54, 40, (uint32_t)w, (uint32_t)h, 1 | (24 << 16), 0, size, 2835, 2835, 0, 0 };
    memcpy(hdr + 2, v, sizeof v);
    fwrite(hdr, 1, 54, f);
    static const uint8_t zero[3];
    for (int y = 0; y < h; y++) {   /* GL rows are bottom-up, like BMP's */
        const uint8_t *s = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) fwrite(s + x * 4, 1, 3, f);
        fwrite(zero, 1, pad, f);
    }
    fclose(f);
}

void gpu_display_loop(void)
{
    const char *hz = getenv("XBCOMPAT_GPU_HZ");
    double rate = hz ? atof(hz) : 60.0;
    int64_t period = (int64_t)(1e9 / (rate > 1 ? rate : 60.0));
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint32_t last_writes = gpu_crtc_start_writes();
    uint8_t *pixels = NULL;
    unsigned vblanks = 0;
    for (;;) {
        int64_t ns = next.tv_nsec + period;
        next.tv_sec += ns / 1000000000;
        next.tv_nsec = ns % 1000000000;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        gpu_vblank();
        vblanks++;
        if (gpu_poll_events()) {
            xlog("GPU: window closed");
            exit(0);
        }
        /* A frame is a flip: D3D's vblank handler pointed the CRTC at the
           new front buffer. */
        uint32_t writes = gpu_crtc_start_writes();
        bool flipped = writes != last_writes;
        last_writes = writes;
        if (flipped) frames_shown++;
        bool shoot = g_screenshot_path && flipped &&
                     (strchr(g_screenshot_path, '%') ? frames_shown % (g_screenshot_frame ? g_screenshot_frame : 60) == 0
                                                    : (int)frames_shown == g_screenshot_frame);
        int w = 0, h = 0;
        if (shoot && !pixels) pixels = malloc(1920 * 1080 * 4);
        gpu_display_frame(shoot ? pixels : NULL, &w, &h);
        if (shoot) {
            char path[512];
            snprintf(path, sizeof path, g_screenshot_path, frames_shown);
            save_bmp(path, pixels, w, h);
            xlog("GPU: saved frame %lu to %s", (unsigned long)frames_shown, path);
        }
        if (vblanks % 600 == 0) xlog("GPU: %u vblanks, %lu frames flipped", vblanks, (unsigned long)frames_shown);
        if (g_exit_after_frames && (int)frames_shown >= g_exit_after_frames) {
            xlog("GPU: %lu frames presented, exiting", (unsigned long)frames_shown);
            exit(0);
        }
    }
}
