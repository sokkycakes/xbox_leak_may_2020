/* Native half: render code stays unchanged; only its runtime services cross. */
#include "../xbcompat.h"
#include "../cpu.h"
#include "../hle/hle.h"
#include "bridge.h"
#include <stdarg.h>
#include <stdlib.h>

static xbr_host_fn host_call;
static struct xa_entry entries[1024];
static size_t nentries;
static xa_fn adapters[1024];
static int conventions[1024];
static size_t nexports;
int g_trace, g_screenshot_frame, g_exit_after_frames;
const char *g_screenshot_path;
volatile unsigned g_guest_traps;
void (*g_vblank_hook)(void); /* Native hook, distinct from the x86 kernel thunk. */

void xa_register(const struct xa_entry *tbl, size_t n)
{
    if (n > 1024 - nentries) abort();
    memcpy(entries + nentries, tbl, n * sizeof(*tbl));
    nentries += n;
}
int xbr_init(uint32_t abi, xbr_host_fn host)
{
    if (abi != XBR_ABI || !host || (host_call && host_call != host)) return -1;
    host_call = host;
    for (nexports = 0; d3d8_funcs[nexports].name; nexports++) {
        if (nexports == 1024) return -2;
        size_t j;
        for (j = 0; j < nentries; j++)
            if (entries[j].host == d3d8_funcs[nexports].impl) break;
        if (j == nentries) return -3;
        adapters[nexports] = entries[j].adapter;
        conventions[nexports] = entries[j].conv;
    }
    return 0;
}
const char *xbr_name(uint32_t index)
{
    return index < nexports ? d3d8_funcs[index].name : NULL;
}
void xbr_invoke(uint32_t index, struct xbr_call *call)
{
    if (!host_call || index >= nexports || !call) abort();
    struct xa_frame f = {
        .stack = (const uint32_t *)(uintptr_t)call->stack,
        .ecx = call->ecx, .edx = call->edx,
        .fast = conventions[index] == CONV_FAST
    };
    uint64_t result = adapters[index](&f);
    call->lo = (uint32_t)result;
    call->hi = (uint32_t)(result >> 32);
    call->fp = f.fp;
    memcpy(&call->fret_lo, &f.fret, 8);
    call->pop_bytes = conventions[index] == CONV_CDECL ? 0 : f.used * 4;
}
void xbr_control(uint32_t op, uint32_t *a)
{
    if (!host_call) abort();
    switch (op) {
    case XBR_CONFIG: {
        const struct xbr_config *c = (const void *)a;
        g_trace = c->trace; g_screenshot_frame = c->shot_frame;
        g_exit_after_frames = c->exit_frames;
        g_screenshot_path = (const char *)(uintptr_t)c->shot_path;
        break;
    }
    case XBR_BIND_GLOBALS: d3d_bind_globals(); break;
    case XBR_FRAME_COUNT: a[0] = d3d_frame_count(); break;
    case XBR_VBLANK: {
        void (*hook)(void) = __atomic_load_n(&g_vblank_hook, __ATOMIC_ACQUIRE);
        if (hook) hook();
        break;
    }
    default: abort();
    }
}
static void message(uint32_t op, const char *fmt, va_list ap)
{
    char text[4096];
    vsnprintf(text, sizeof(text), fmt, ap);
    uint32_t a[] = {(uint32_t)(uintptr_t)text};
    host_call(op, a);
}
void xlog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); message(XBR_LOG, fmt, ap); va_end(ap);
}
void fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); message(XBR_FATAL, fmt, ap); va_end(ap);
    abort();
}
void xbc_exit(int code)
{
    uint32_t a[] = {(uint32_t)code}; host_call(XBR_EXIT, a); abort();
}
#define SERVICE0(fn, op) void fn(void) { host_call(op, NULL); }
void ke_frame_presented(void)
{
    uint32_t a[1] = {0}; host_call(XBR_FRAME_PRESENTED, a); g_guest_traps = a[0];
}
SERVICE0(xinput_check_reset_combo, XBR_RESET_COMBO)
SERVICE0(reset_check, XBR_RESET_CHECK)
LONG NTAPI KeSetEvent(KEVENT *event, LONG increment, BOOLEAN wait)
{
    uint32_t a[] = {(uint32_t)(uintptr_t)event, (uint32_t)increment, wait};
    host_call(XBR_SET_EVENT, a); return (LONG)a[0];
}
SERVICE0(install_fault_handlers, XBR_FAULT_HANDLERS)
SERVICE0(av_title_starting, XBR_AV_START)
SERVICE0(av_hand_over, XBR_AV_HANDOVER)
void cpu_block(void) {} /* Box86 owns guest scheduling. */
ULONG hle_lookup(const char *name)
{
    uint32_t a[] = {(uint32_t)(uintptr_t)name}; host_call(XBR_LOOKUP, a); return a[0];
}
ULONG hle_lookup_prefix(const char *name)
{
    uint32_t a[] = {(uint32_t)(uintptr_t)name}; host_call(XBR_LOOKUP_PREFIX, a); return a[0];
}
void *pool_alloc(size_t n)
{
    uint32_t a[] = {n}; host_call(XBR_POOL_ALLOC, a); return (void *)(uintptr_t)a[0];
}
void pool_free(void *p)
{
    uint32_t a[] = {(uint32_t)(uintptr_t)p}; host_call(XBR_POOL_FREE, a);
}
bool pool_owns(const void *p)
{
    uint32_t a[] = {(uint32_t)(uintptr_t)p}; host_call(XBR_POOL_OWNS, a); return a[0] != 0;
}
void *arena_alloc(size_t n, ULONG top)
{
    uint32_t a[] = {n, top}; host_call(XBR_ARENA_ALLOC, a); return (void *)(uintptr_t)a[0];
}
PVOID NTAPI MmAllocateContiguousMemoryEx(SIZE_T n, ULONG_PTR low, ULONG_PTR high,
                                        ULONG_PTR align, ULONG protect)
{
    uint32_t a[] = {n, low, high, align, protect};
    host_call(XBR_CONTIG_ALLOC, a); return (void *)(uintptr_t)a[0];
}
void NTAPI MmFreeContiguousMemory(PVOID p)
{
    uint32_t a[] = {(uint32_t)(uintptr_t)p}; host_call(XBR_CONTIG_FREE, a);
}
SIZE_T NTAPI MmQueryAllocationSize(PVOID p)
{
    uint32_t a[] = {(uint32_t)(uintptr_t)p}; host_call(XBR_ALLOC_SIZE, a); return a[0];
}
void av_persist(const unsigned char *px, unsigned w, unsigned h)
{
    uint32_t a[] = {(uint32_t)(uintptr_t)px, w, h}; host_call(XBR_AV_PERSIST, a);
}
uint64_t cpu_call(uint32_t fn, int conv, int n, const uint32_t *args)
{
    uint32_t a[] = {fn, (uint32_t)conv, (uint32_t)n, (uint32_t)(uintptr_t)args, 0, 0};
    host_call(XBR_GUEST_CALL, a);
    return (uint64_t)a[4] | ((uint64_t)a[5] << 32);
}
