/*
 * Minimal stand-ins for the QEMU APIs that xemu's NV2A code uses, so the
 * device runs inside xbcompat-gpu without QEMU.  Only what the NV2A sources
 * need is here; the device model itself (nv2a_host.c) owns the state that
 * QEMU's object model, memory API and timers would normally hold.
 *
 * This directory and ../xemu are part of the GPL xbcompat-gpu test build only.
 */
#ifndef XBC_QEMU_OSDEP_H
#define XBC_QEMU_OSDEP_H

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <glib.h>

typedef uint64_t hwaddr;
typedef uint64_t ram_addr_t;
#define HWADDR_PRIx PRIx64

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#define ROUND_UP(n, d) (((n) + (d) - 1) & -(0 ? (n) : (d)))
#define ROUND_DOWN(n, d) ((n) & -(0 ? (n) : (d)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define QEMU_ALIGN_UP(n, m) (DIV_ROUND_UP(n, m) * (m))
#ifndef container_of
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), #x)
#define QEMU_BUILD_BUG_MSG(x, msg) _Static_assert(!(x), msg)
#define xstringify(s) tostring(s)
#define tostring(s) #s
#define stringify(s) tostring(s)
#define QEMU_PACKED __attribute__((packed))
#define QEMU_NORETURN __attribute__((noreturn))
#define G_NORETURN __attribute__((noreturn))
#define QEMU_FALLTHROUGH __attribute__((fallthrough))
#define NANOSECONDS_PER_SECOND 1000000000LL
#define KiB (1ULL << 10)
#define MiB (1ULL << 20)
#define GiB (1ULL << 30)

static inline int ctz32(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
static inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline int ctpop32(uint32_t v) { return __builtin_popcount(v); }

static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void stw_le_p(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void stq_le_p(void *p, uint64_t v) { memcpy(p, &v, 8); }

/* a * b / c with a 96-bit intermediate (QEMU's portable version). */
static inline uint64_t muldiv64_rounding(uint64_t a, uint32_t b, uint32_t c, bool round_up)
{
    union { uint64_t ll; struct { uint32_t low, high; } l; } u, res;
    uint64_t rl, rh;
    u.ll = a;
    rl = (uint64_t)u.l.low * (uint64_t)b;
    if (round_up) rl += c - 1;
    rh = (uint64_t)u.l.high * (uint64_t)b;
    rh += (rl >> 32);
    res.l.high = rh / c;
    res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
    return res.ll;
}
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c) { return muldiv64_rounding(a, b, c, false); }
static inline uint64_t muldiv64_round_up(uint64_t a, uint32_t b, uint32_t c) { return muldiv64_rounding(a, b, c, true); }

/* ---- atomics ---------------------------------------------------------- */
#define qatomic_read(p) __atomic_load_n(p, __ATOMIC_SEQ_CST)
#define qatomic_set(p, v) __atomic_store_n(p, v, __ATOMIC_SEQ_CST)
#define qatomic_xchg(p, v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)
#define qatomic_inc(p) __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST)
#define qatomic_dec(p) __atomic_fetch_sub(p, 1, __ATOMIC_SEQ_CST)
#define qatomic_add(p, v) __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST)
#define qatomic_or(p, v) __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST)
#define qatomic_and(p, v) __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST)
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)

/* ---- errors ----------------------------------------------------------- */
typedef struct Error { char *msg; } Error;
extern Error *error_fatal;
void error_setg_internal(Error **errp, const char *fmt, ...);
#define error_setg(errp, ...) error_setg_internal(errp, __VA_ARGS__)
static inline const char *error_get_pretty(const Error *e) { return e ? e->msg : ""; }
static inline void error_free(Error *e) { if (e) { g_free(e->msg); g_free(e); } }
void error_report(const char *fmt, ...);
#define warn_report error_report
#define error_report_err(e) error_report("%s", error_get_pretty(e))

/* ---- files ------------------------------------------------------------ */
static inline FILE *qemu_fopen(const char *p, const char *m) { return fopen(p, m); }
static inline int qemu_mkdir(const char *p) { return mkdir(p, 0755); }
static inline int qemu_unlink(const char *p) { return unlink(p); }

/* ---- the rest of the stand-ins ---------------------------------------- */
#include "qemu/thread.h"
#include "xbc_memory.h"
#include "xbc_timer.h"

/* xemu has two ways to keep GPU surfaces and guest RAM coherent: with the
   TCG it hears about CPU accesses to a surface and moves data only then;
   under KVM it can't, so it downloads every surface after drawing to it
   and re-uploads the displayed one each frame.  xbcompat-gpu answers "TCG"
   (fast, CPU writes into render targets go unseen) unless
   XBCOMPAT_GPU_COHERENT asks for the KVM behaviour (slow, coherent). */
extern bool xbc_gpu_lazy_surfaces;
static inline bool tcg_enabled(void) { return xbc_gpu_lazy_surfaces; }

#endif
