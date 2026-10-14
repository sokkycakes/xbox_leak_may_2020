/* Native half: render code stays unchanged; only its runtime services cross. */
#define _GNU_SOURCE /* pthread_setname_np */
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

static void invoke_now(uint32_t index, struct xbr_call *call);
static void control_now(uint32_t op, uint32_t *a);
struct dsmix_job;
void renderer_ds_mix(struct dsmix_job *job);   /* dsmix.c */
#include "worker.h"
/* Runs one request where it was drained; nested runs restore renderer_current. */
static void renderer_execute(struct xbr_request *r)
{
    struct xbr_request *parent = renderer_enabled ? renderer_current : NULL;
    if (renderer_enabled) renderer_current = r;
    if (r->kind == XBR_REQUEST_DRAW) {
        d3d_draw_packet_run(r->packet);
        d3d_draw_packet_free(r->packet);
    } else if (r->kind == XBR_REQUEST_STATE) {
        unsigned category = d3d_state_packet_category(r->state);
        /* Render and texture-stage state (2, 3) only feed later draws; anything
           else may be read by work a pending HUD batch has not flushed. */
        if (category != 2 && category != 3) d3d_renderer_barrier();
        d3d_state_packet_run(r->state);
        d3d_state_packet_free(r->state);
    } else if (r->kind == XBR_REQUEST_ASYNC) {
        if (r->queued) {
            d3d_async_run(r->queued, r->nqueued);
            free(r->queued);
        }
    } else if (r->kind == XBR_REQUEST_CALL) {
        if (renderer_barriers && renderer_call_barrier[r->op]) d3d_renderer_barrier();
        invoke_now(r->op, r->call);
    } else {
        if (renderer_barriers && r->op != XBR_VBLANK) d3d_renderer_barrier();
        control_now(r->op, r->args);
    }
    if (renderer_enabled) renderer_current = parent;
    if (!renderer_enabled) return;
    pthread_mutex_lock(&renderer_lock);
    if (r->kind == XBR_REQUEST_CONTROL && r->op == XBR_SHUTDOWN) renderer_stopping = true;
    /* A call that drained an empty queue leaves the device as the guest will see
       it next: snapshot that as the front later captures build on. */
    if (r->kind == XBR_REQUEST_CALL && !parent && !renderer_head && !renderer_stopping
        && !renderer_front && !renderer_async_wanted) {
        renderer_front = renderer_queue_state ? d3d_state_front_create(d3d8_funcs[r->op].impl)
                                              : d3d_draw_front_create(d3d8_funcs[r->op].impl);
        renderer_front_origin = r->origin;
    }
    if (r->detached) free(r);
    else r->done = true;
    pthread_cond_broadcast(&renderer_cond);
    pthread_mutex_unlock(&renderer_lock);
}
#define host_call(op, a) renderer_host_service(op, a) /* Calls below go via the owner. */
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
        const char *name = d3d8_funcs[nexports].name; /* Batched draws need no flush. */
        renderer_call_barrier[nexports] = !strstr(name, "DrawVertices") && !strstr(name, "DrawIndexedVertices");
    }
    const char *worker = getenv("XBCOMPAT_RENDER_WORKER");
    renderer_enabled = worker && *worker && *worker != '0';
    renderer_profile = getenv("XBCOMPAT_PROFILE_RENDERER") != NULL;
    const char *state = getenv("XBCOMPAT_RENDER_QUEUE_STATE");
    renderer_queue_state = renderer_enabled && state && *state && *state != '0';
    const char *batch = getenv("XBCOMPAT_HUD_BATCH");
    renderer_barriers = renderer_enabled || (batch && *batch && *batch != '0');
    renderer_async_wanted = renderer_enabled && d3d_async_wanted();
    for (size_t i = 0; renderer_async_wanted && i < nexports; i++)
    {
        renderer_async_class[i] = (unsigned char)d3d_async_class(d3d8_funcs[i].name, d3d8_funcs[i].impl);
        renderer_async_groups[i] = renderer_async_class[i] == 1
            ? (unsigned short)d3d_async_groups(d3d8_funcs[i].name, d3d8_funcs[i].impl) : 0;
    }
    return 0;
}
const char *xbr_name(uint32_t index)
{
    return index < nexports ? d3d8_funcs[index].name : NULL;
}
static void invoke_now(uint32_t index, struct xbr_call *call)
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
static void control_now(uint32_t op, uint32_t *a)
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
    case XBR_READ_PIXELS: d3d_backend_read_pixels(a); break;
    case XBR_RENDER_STATS:
        d3d_backend_stats(a);
        pthread_mutex_lock(&renderer_lock);
        a[2] = renderer_async_count; a[3] = renderer_sync_count;
        a[4] = renderer_enabled; a[5] = renderer_queue_bytes;
        pthread_mutex_unlock(&renderer_lock);
        break;
    case XBR_SHUTDOWN: d3d_backend_shutdown(); break;
    default: abort();
    }
}
void xbr_invoke(uint32_t index, struct xbr_call *call)
{
    if (!host_call || index >= nexports || !call) abort();
    renderer_dispatch(XBR_REQUEST_CALL, index, call, NULL);
}
void d3d_flush_held(void);
void xbr_control(uint32_t op, uint32_t *args)
{
    if (!host_call) abort();
    if (op == XBR_FRAME_COUNT) { /* A plain counter: no need to wait for the queue. */
        args[0] = d3d_frame_count();
        return;
    }
    if (op == XBR_DRAIN) {   /* from the kernel, on whichever thread frees */
        d3d_flush_held();    /* a held draw run (d3d8.c); only the GL thread draws it */
        renderer_async_drain();
        return;
    }
    if (op == XBR_DS_MIX) {   /* The sound mixer: no GL, so not the render queue's business. */
        renderer_ds_mix((struct dsmix_job *)args);
        return;
    }
    renderer_dispatch(XBR_REQUEST_CONTROL, op, NULL, args);
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
