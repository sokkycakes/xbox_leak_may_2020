/*
 * Where guest code runs.
 *
 * On x86 (the i386 build) Xbox code runs natively in the same 32-bit
 * process: guest and host call each other directly, and these helpers are
 * thin wrappers.
 *
 * Everywhere else (the 32-bit ARM build for the Raspberry Pi) the guest's x86
 * code runs in Unicorn, QEMU's x86 CPU packaged as a library, while all of
 * xbcompat itself (kernel, D3D, DirectSound, input) stays native.  The process
 * is 32-bit too and maps guest memory at the same addresses as on x86, so a
 * guest pointer is a host pointer and the kernel and HLE code are unchanged.
 * What differs is crossing between the two:
 *   - guest -> host: each host function the guest may call gets a stub
 *     address in guest memory; executing it runs the function's adapter,
 *     which reads the arguments as the x86 calling convention placed them
 *     (generated per source file by tools/gen_adapters.py);
 *   - host -> guest: cpu_call() runs a guest function to its return.
 */
#ifndef XBCOMPAT_CPU_H
#define XBCOMPAT_CPU_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__i386__) && !defined(XBC_FORCE_TRANSLATED)
#define XBC_NATIVE 1
#else
#define XBC_TRANSLATED 1
#endif

enum cpu_conv { CONV_STD, CONV_FAST, CONV_CDECL };

#ifdef XBC_TRANSLATED

/* ---- guest -> host adapters ---------------------------------------- */

/* The guest's view of a call to a host function. */
struct xa_frame {
    const uint32_t *stack;   /* first stack argument (esp + 4 at the call) */
    uint32_t ecx, edx;       /* fastcall register arguments */
    uint32_t ret;            /* guest return address */
    int fast, nreg;          /* fastcall: registers taken so far */
    uint32_t used;           /* stack words taken so far */
    int fp;                  /* the result is floating point, in fret */
    double fret;
    void *ctx;               /* per-stub context (traps) */
};

typedef uint64_t (*xa_fn)(struct xa_frame *f);

struct xa_entry {
    void *host;              /* the C function */
    xa_fn adapter;
    int conv;                /* enum cpu_conv */
    const char *name;
};

void xa_register(const struct xa_entry *tbl, size_t n);

static inline void xa_arg(struct xa_frame *f, void *p, size_t n, int fp)
{
    if (f->fast && f->nreg < 2 && n <= 4 && !fp) {
        uint32_t r = f->nreg++ ? f->edx : f->ecx;
        memcpy(p, &r, n);
        return;
    }
    memcpy(p, f->stack + f->used, n);
    f->used += (uint32_t)((n + 3) / 4);
}

static inline uint64_t xa_ret(struct xa_frame *f, const void *p, size_t n, int fp)
{
    uint64_t v = 0;
    if (fp) {
        f->fp = 1;
        f->fret = n == sizeof(float) ? (double)*(const float *)p : *(const double *)p;
        return 0;
    }
    memcpy(&v, p, n);
    if (n == 1) v = *(const uint8_t *)p;          /* MSVC callers read al/ax; zero-extend */
    else if (n == 2) v = *(const uint16_t *)p;
    return v;
}

#define XA_ISFP(x) _Generic((x), float: 1, double: 1, long double: 1, default: 0)
#define XA_ARG(v) xa_arg(xa_f, &(v), sizeof(v), XA_ISFP(v))
#define XA_RET(e) do { __auto_type xa_r_ = (e); _Static_assert(sizeof(xa_r_) <= 8, "result too large"); \
                       return xa_ret(xa_f, &xa_r_, sizeof(xa_r_), XA_ISFP(xa_r_)); } while (0)

/* Guest varargs (DbgPrint and the Rtl printf family): format from the
   guest's stack words starting at args. */
int xa_format(char *out, size_t size, const char *fmt, const uint32_t *args);

/* ---- the interface the rest of xbcompat uses ------------------------- */

/* The address the guest calls to reach host function fn. */
uint32_t cpu_guest_entry(void *fn);
/* Same, for a host function with some arguments in registers (hle.c's
   regs= spec from LTCG builds, e.g. "eax,s,ecx"). */
uint32_t cpu_guest_entry_regs(void *fn, const char *spec);
/* A guest address that fails with msg (and the caller's address). */
uint32_t cpu_guest_trap(const char *msg);
/* Replace host function pointers in a table the guest calls through. */
void cpu_guest_table(void **tbl, size_t n);

/* Run guest function fn on this thread with n 32-bit arguments; returns edx:eax. */
uint64_t cpu_call(uint32_t fn, int conv, int n, const uint32_t *args);
#define CPU_CALL(fn, conv, ...) \
    cpu_call((uint32_t)(uintptr_t)(fn), conv, \
             sizeof((uint32_t[]){ __VA_ARGS__ }) / sizeof(uint32_t), (uint32_t[]){ __VA_ARGS__ })
#define CPU_CALL0(fn, conv) cpu_call((uint32_t)(uintptr_t)(fn), conv, 0, NULL)

void cpu_init(void);
/* This host thread runs guest code with fs:0 at pcr, on the guest stack [limit, base). */
void cpu_thread_attach(void *pcr, void *stack_limit, void *stack_base);
void cpu_thread_detach(void);
/* Host code rewrote guest code in [addr, addr+len) (section loads). */
void cpu_code_changed(uint32_t addr, uint32_t len);
/* For crash reports: the guest registers of this thread, if it is in guest code. */
void cpu_dump_guest(void);

#else  /* XBC_NATIVE */

static inline uint32_t cpu_guest_entry(void *fn) { return (uint32_t)fn; }
static inline void cpu_guest_table(void **tbl, size_t n) { (void)tbl; (void)n; }
static inline void cpu_code_changed(uint32_t addr, uint32_t len) { (void)addr; (void)len; }

#endif

#endif
