/* Render worker, included by backend.c only: guest D3D calls become requests
   run on one GL thread, so the guest thread pays only for packet capture. */
#include "../hle/draw_packet.h"
#include <time.h>
#define RENDERER_QUEUE_LIMIT (1u << 20) /* Captured bytes in flight. */
#define RENDERER_QUEUE_MAX 1024u        /* Requests in flight. */
/* Profile slots: one per d3d8 export, then one per xbr_control op. */
#define RENDERER_SLOTS (1024 + XBR_SHUTDOWN + 1)
enum xbr_request_kind { XBR_REQUEST_CALL, XBR_REQUEST_CONTROL, XBR_REQUEST_DRAW, XBR_REQUEST_STATE, XBR_REQUEST_ASYNC };
struct xbr_service_request { uint32_t op; uint32_t *args; bool done; };
struct xbr_request {
    struct xbr_request *next;
    enum xbr_request_kind kind;
    uint32_t op;
    struct xbr_call *call;
    uint32_t *args;
    struct xbr_draw_packet *packet;
    struct xbr_state_packet *state;
    struct xbr_service_request *service; /* Host call the worker wants run on this caller. */
    uint32_t *queued;                    /* XBR_REQUEST_ASYNC: work d3d8.c queued, or none */
    unsigned nqueued;
    size_t bytes;
    bool done, detached;
    pthread_t origin;
};
static pthread_mutex_t renderer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t renderer_cond = PTHREAD_COND_INITIALIZER;
static pthread_t renderer_thread;
static __thread bool renderer_is_owner;
static struct xbr_request *renderer_head, **renderer_tail = &renderer_head;
static struct xbr_request *renderer_current;
static struct xbr_draw_front *renderer_front; /* Device state as the guest thread sees it. */
static pthread_t renderer_front_origin;
static size_t renderer_queue_bytes;
static unsigned renderer_queue_requests, renderer_state_count[8], renderer_draw_count;
static bool renderer_queue_state, renderer_barriers;
static bool renderer_call_barrier[1024];
static unsigned renderer_async_count, renderer_sync_count;
static bool renderer_enabled, renderer_started, renderer_stopping, renderer_stopped;
static bool renderer_profile;
static unsigned renderer_op_count[RENDERER_SLOTS];
static uint64_t renderer_op_blocked[RENDERER_SLOTS];
/* XBCOMPAT_ASYNC_DRAWS (hle/async_draws.h): which calls run on the title's
   thread once queued drawing is on (1 state, 2 queued work), and a host
   service queued work wants, run by whichever title thread comes by next. */
bool d3d_async_wanted(void), d3d_async_ready(void), d3d_async_active(void);
void d3d_async_lock(void), d3d_async_unlock(void), d3d_async_before_sync(void), d3d_async_after_sync(void);
int d3d_async_class(const char *name, const void *impl);
uint64_t d3d_async_take_delta_ns(void);
void d3d_async_run(const uint32_t *buf, unsigned n);
void d3d_worker_thread_init(void);
static unsigned char renderer_async_class[1024];
static bool renderer_async_wanted;
static struct xbr_service_request *volatile renderer_async_service;
static unsigned renderer_async_posts;
static __thread bool renderer_servicing;   /* running a host service the worker waits on */
/* Defined in backend.c: runs one request on whichever thread drains it. */
static void renderer_execute(struct xbr_request *r);
static uint64_t renderer_clock_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}
/* Pops the queue head. Caller holds renderer_lock. */
static struct xbr_request *renderer_take_locked(void)
{
    struct xbr_request *r = renderer_head;
    if (!r) return NULL;
    renderer_head = r->next;
    if (!renderer_head) renderer_tail = &renderer_head;
    renderer_queue_bytes -= r->bytes;
    renderer_queue_requests--;
    pthread_cond_broadcast(&renderer_cond);
    return r;
}
/* Pops the oldest request from one guest thread. While that thread waits on a
   host service, its own queued work must still run, or the two would wait on
   each other. Caller holds renderer_lock. */
static struct xbr_request *renderer_take_reentrant_locked(pthread_t origin)
{
    struct xbr_request **link = &renderer_head;
    while (*link && (*link)->origin != origin) link = &(*link)->next;
    struct xbr_request *r = *link;
    if (!r) return NULL;
    *link = r->next;
    if (!*link) renderer_tail = link;
    renderer_queue_bytes -= r->bytes;
    renderer_queue_requests--;
    pthread_cond_broadcast(&renderer_cond);
    return r;
}
/* Host services are x86-side and must run on the guest thread that asked. */
static void renderer_host_service(uint32_t op, uint32_t *args)
{
    if (!renderer_enabled || !renderer_is_owner) {
        host_call(op, args);
        return;
    }
    if (!renderer_current) abort();
    struct xbr_service_request service = {.op = op, .args = args};
    if (renderer_current->detached) {
        /* Queued work: nobody waits on it.  Messages need no title thread;
           anything else waits for the next one to come by. */
        if (op == XBR_LOG || op == XBR_FATAL) {
            fprintf(stderr, "%s\n", (const char *)(uintptr_t)args[0]);
            return;
        }
        if (renderer_current->kind != XBR_REQUEST_ASYNC) abort();
        pthread_mutex_lock(&renderer_lock);
        while (renderer_async_service) pthread_cond_wait(&renderer_cond, &renderer_lock);
        renderer_async_service = &service;
        pthread_cond_broadcast(&renderer_cond);
        while (!service.done) pthread_cond_wait(&renderer_cond, &renderer_lock);
        pthread_mutex_unlock(&renderer_lock);
        return;
    }
    pthread_mutex_lock(&renderer_lock);
    renderer_current->service = &service;
    pthread_cond_broadcast(&renderer_cond);
    while (!service.done) {
        struct xbr_request *nested = renderer_take_reentrant_locked(renderer_current->origin);
        if (nested) {
            pthread_mutex_unlock(&renderer_lock);
            renderer_execute(nested);
            pthread_mutex_lock(&renderer_lock);
        } else pthread_cond_wait(&renderer_cond, &renderer_lock);
    }
    pthread_mutex_unlock(&renderer_lock);
}
/* The render thread: owns the GL context from the first request on. */
static void *renderer_main(void *unused)
{
    (void)unused;
    renderer_is_owner = true;
    d3d_worker_thread_init();
    pthread_setname_np(pthread_self(), "xbc-render");
    pthread_mutex_lock(&renderer_lock);
    for (;;) {
        while (!renderer_head && !renderer_stopping)
            pthread_cond_wait(&renderer_cond, &renderer_lock);
        if (!renderer_head) break;
        struct xbr_request *r = renderer_take_locked();
        pthread_mutex_unlock(&renderer_lock);
        renderer_execute(r);
        pthread_mutex_lock(&renderer_lock);
    }
    renderer_stopped = true;
    pthread_cond_broadcast(&renderer_cond);
    pthread_mutex_unlock(&renderer_lock);
    return NULL;
}
/* Requests run in order: a synchronous call waits behind every detached packet
   queued before it, so the guest never observes state from the future or the
   past. Caller holds renderer_lock. */
static void renderer_enqueue_locked(struct xbr_request *r)
{
    r->next = NULL;
    *renderer_tail = r;
    renderer_tail = &r->next;
    renderer_queue_bytes += r->bytes;
    renderer_queue_requests++;
    pthread_cond_broadcast(&renderer_cond);
}
/* Waits for r, running any host service the worker hands back meanwhile. */
static bool renderer_run_async_service_locked(void);
static void renderer_wait_locked(struct xbr_request *r)
{
    while (!r->done) {
        if (renderer_run_async_service_locked()) continue;
        if (r->service) {
            struct xbr_service_request *s = r->service;
            r->service = NULL;
            pthread_mutex_unlock(&renderer_lock);
            renderer_servicing = true;
            host_call(s->op, s->args);
            renderer_servicing = false;
            pthread_mutex_lock(&renderer_lock);
            s->done = true;
            pthread_cond_broadcast(&renderer_cond);
        } else pthread_cond_wait(&renderer_cond, &renderer_lock);
    }
}
/* Runs a host service queued work asked for, on this title thread.
   Caller holds renderer_lock; false when there was none. */
static bool renderer_run_async_service_locked(void)
{
    struct xbr_service_request *s = renderer_async_service;
    if (!s || renderer_is_owner) return false;
    renderer_async_service = NULL;
    pthread_mutex_unlock(&renderer_lock);
    renderer_servicing = true;
    host_call(s->op, s->args);
    renderer_servicing = false;
    pthread_mutex_lock(&renderer_lock);
    s->done = true;
    pthread_cond_broadcast(&renderer_cond);
    return true;
}
static void renderer_start_locked(void);
/* Queued work for the worker, from the title's thread; buf is the worker's
   to free. */
void renderer_post_async(uint32_t *buf, unsigned n)
{
    struct xbr_request *r = calloc(1, sizeof(*r));
    if (!r) abort();
    r->kind = XBR_REQUEST_ASYNC; r->queued = buf; r->nqueued = n; r->detached = true;
    r->bytes = n * 4; r->origin = pthread_self();
    pthread_mutex_lock(&renderer_lock);
    renderer_start_locked();
    /* Bounded like the other queue: wait (serving the worker) while full. */
    while (renderer_queue_bytes > RENDERER_QUEUE_LIMIT * 8 && !renderer_stopping)
        if (!renderer_run_async_service_locked()) pthread_cond_wait(&renderer_cond, &renderer_lock);
    renderer_enqueue_locked(r);
    renderer_async_count++; renderer_async_posts++;
    pthread_mutex_unlock(&renderer_lock);
}
/* Waits until the worker has run everything queued so far. */
void renderer_async_flush(void)
{
    struct xbr_request r = {.kind = XBR_REQUEST_ASYNC, .origin = pthread_self()};
    pthread_mutex_lock(&renderer_lock);
    renderer_start_locked();
    renderer_enqueue_locked(&r); renderer_wait_locked(&r);
    pthread_mutex_unlock(&renderer_lock);
}
/* Waits until *counter reaches target (the worker bumps it, then finishes
   the request it was running, which wakes this). */
void renderer_async_wait(volatile ULONG *counter, ULONG target)
{
    pthread_mutex_lock(&renderer_lock);
    while ((LONG)(__atomic_load_n(counter, __ATOMIC_ACQUIRE) - target) < 0 && !renderer_stopping)
        if (!renderer_run_async_service_locked()) pthread_cond_wait(&renderer_cond, &renderer_lock);
    pthread_mutex_unlock(&renderer_lock);
}
/* Guest memory is about to be freed: let queued drawing finish with it.
   Not from inside a host service the worker waits on: the drain would
   wait for the worker, which waits for the service. */
static void renderer_async_drain(void)
{
    if (!renderer_async_wanted || renderer_is_owner || renderer_servicing || !d3d_async_active()) return;
    static unsigned logged;
    if (logged < 4) { logged++; fprintf(stderr, "D3D: queued drawing finished before a free\n"); }
    d3d_async_lock();
    d3d_async_before_sync();
    renderer_async_flush();
    d3d_async_unlock();
}
/* Started lazily so the GL context is created on the render thread. */
static void renderer_start_locked(void)
{
    if (renderer_started) return;
    renderer_started = true;
    if (pthread_create(&renderer_thread, NULL, renderer_main, NULL)) abort();
}
/* Entry for every call and control from the guest side. */
static void renderer_dispatch(enum xbr_request_kind kind, uint32_t op,
                              struct xbr_call *call, uint32_t *args)
{
    if (!renderer_enabled) {
        struct xbr_request r = {.kind = kind, .op = op, .call = call, .args = args};
        /* Without the worker, XBCOMPAT_PROFILE_RENDERER counts the calls and
           their time per export (nested calls count in their callers too). */
        uint64_t t0 = renderer_profile ? renderer_clock_ns() : 0;
        renderer_execute(&r);
        unsigned slot = kind == XBR_REQUEST_CALL ? op : 1024 + op;
        if (t0 && slot < RENDERER_SLOTS) {
            renderer_op_count[slot]++;
            renderer_op_blocked[slot] += renderer_clock_ns() - t0;
        }
        return;
    }
    uint64_t started = renderer_profile ? renderer_clock_ns() : 0;
    if (renderer_async_wanted) {
        /* The DPC thread's vertical blank touches no GL. */
        if (kind == XBR_REQUEST_CONTROL && op == XBR_VBLANK && d3d_async_active()) {
            control_now(op, args);
            return;
        }
        if (renderer_async_service) {
            pthread_mutex_lock(&renderer_lock);
            renderer_run_async_service_locked();
            pthread_mutex_unlock(&renderer_lock);
        }
        if (kind == XBR_REQUEST_CALL && renderer_async_class[op] == 3 && d3d_async_active()) {
            invoke_now(op, call);
            return;
        }
        d3d_async_lock();
        if (kind == XBR_REQUEST_CALL && renderer_async_class[op] && d3d_async_ready()) {
            invoke_now(op, call);
            d3d_async_unlock();
            return;
        }
        if (d3d_async_active()) d3d_async_before_sync();
    }
    pthread_mutex_lock(&renderer_lock);
    if (renderer_stopping) {
        if (renderer_async_wanted) d3d_async_unlock();
        pthread_mutex_unlock(&renderer_lock);
        if (kind == XBR_REQUEST_CONTROL && (op == XBR_VBLANK || op == XBR_SHUTDOWN)) return;
        abort();
    }
    renderer_start_locked();
    /* Says why the first few draws did or did not take the asynchronous path. */
    static unsigned diagnostic_draws;
    if (kind == XBR_REQUEST_CALL && diagnostic_draws < 16
        && getenv("XBCOMPAT_PROFILE_RENDERER") && strstr(d3d8_funcs[op].name, "DrawVertices")) {
        diagnostic_draws++;
        fprintf(stderr, "D3D worker draw route: %s convention %d front %u queue %zu\n",
                d3d8_funcs[op].name, conventions[op], renderer_front != NULL, renderer_queue_bytes);
    }
    if (kind == XBR_REQUEST_CALL && renderer_queue_state && renderer_front && !renderer_async_wanted
        && renderer_front_origin == pthread_self() && renderer_queue_requests < RENDERER_QUEUE_MAX
        && renderer_queue_bytes < RENDERER_QUEUE_LIMIT
        && (conventions[op] == CONV_STD
            || conventions[op] == CONV_FAST)) {
        struct xa_frame f = {.stack = (const uint32_t *)(uintptr_t)call->stack, .ecx = call->ecx,
                             .edx = call->edx, .fast = conventions[op] == CONV_FAST};
        struct xbr_state_packet *p = d3d_state_packet_capture(renderer_front, d3d8_funcs[op].impl, &f);
        if (p) {
            size_t bytes = d3d_state_packet_size(p);
            struct xbr_request *r = bytes <= RENDERER_QUEUE_LIMIT - renderer_queue_bytes ? calloc(1, sizeof(*r)) : NULL;
            if (r) {
                r->kind = XBR_REQUEST_STATE; r->state = p; r->bytes = bytes; r->detached = true; r->origin = pthread_self();
                /* Fold the packet into the front first: the next capture builds on it. */
                d3d_state_packet_commit(renderer_front, p);
                renderer_enqueue_locked(r);
                renderer_async_count++;
                unsigned category = d3d_state_packet_category(p);
                if (category < 8) renderer_state_count[category]++;
                call->lo = call->hi = call->fp = call->fret_lo = call->fret_hi = 0;
                call->pop_bytes = f.used * 4;
                pthread_mutex_unlock(&renderer_lock);
                return;
            }
            d3d_state_packet_free(p);
        }
    }
    if (kind == XBR_REQUEST_CALL && renderer_front && !renderer_async_wanted && renderer_front_origin == pthread_self()
        && renderer_queue_bytes < RENDERER_QUEUE_LIMIT
        && renderer_queue_requests < RENDERER_QUEUE_MAX && conventions[op] == CONV_STD) {
        struct xa_frame f = {.stack = (const uint32_t *)(uintptr_t)call->stack};
        struct xbr_draw_packet *p = d3d_draw_packet_capture(renderer_front, d3d8_funcs[op].impl, &f);
        if (p) {
            size_t bytes = d3d_draw_packet_size(p);
            struct xbr_request *r = bytes <= RENDERER_QUEUE_LIMIT - renderer_queue_bytes ? calloc(1, sizeof(*r)) : NULL;
            if (r) {
                r->kind = XBR_REQUEST_DRAW; r->packet = p; r->bytes = bytes; r->detached = true; r->origin = pthread_self();
                /* Draws return nothing the guest reads, so it need not wait. */
                renderer_enqueue_locked(r);
                renderer_async_count++; renderer_draw_count++;
                call->lo = call->hi = call->fp = call->fret_lo = call->fret_hi = 0;
                call->pop_bytes = f.used * 4;
                pthread_mutex_unlock(&renderer_lock);
                return;
            }
            d3d_draw_packet_free(p);
        }
    }
    d3d_draw_front_free(renderer_front); /* Anything else may change state the front cannot model. */
    renderer_front = NULL;
    struct xbr_request r = {.kind = kind, .op = op, .call = call, .args = args, .origin = pthread_self()};
    renderer_sync_count++;
    renderer_enqueue_locked(&r); renderer_wait_locked(&r);
    /* Time the guest thread spent blocked, per call or control. */
    unsigned slot = kind == XBR_REQUEST_CALL ? op : 1024 + op;
    if (started && slot < RENDERER_SLOTS) {
        renderer_op_count[slot]++;
        renderer_op_blocked[slot] += renderer_clock_ns() - started;
    }
    bool shutdown = kind == XBR_REQUEST_CONTROL && op == XBR_SHUTDOWN;
    pthread_mutex_unlock(&renderer_lock);
    if (renderer_async_wanted) {
        if (d3d_async_active()) d3d_async_after_sync();
        d3d_async_unlock();
    }
    if (shutdown) pthread_join(renderer_thread, NULL);
}
/* async, sync, queued bytes, enabled: for the frame overlay. */
/* Same numbers XBR_RENDER_STATS reports to the client. */
void xbr_worker_stats(uint32_t *stats)
{
    pthread_mutex_lock(&renderer_lock);
    stats[0] = renderer_async_count; stats[1] = renderer_sync_count;
    stats[2] = renderer_queue_bytes; stats[3] = renderer_enabled;
    pthread_mutex_unlock(&renderer_lock);
}
/* XBCOMPAT_PROFILE_RENDERER: per-frame averages since the last report, then the
   five calls the guest spent longest blocked on. XBCOMPAT_PROFILE_WORKER_OPS
   also lists every synchronous op. Counters reset on each report. Called from
   d3d8.c on the guest thread. */
void xbr_worker_profile(double frames)
{
    if (!renderer_profile || frames <= 0) return;
    if (!renderer_enabled) {
        /* Inline calls: the 25 that took longest, with their counts. */
        static const char *controls[] = {"config", "bind-globals", "frame-count", "vblank", "read-pixels", "stats", "shutdown"};
        for (unsigned rank = 0; rank < 25; rank++) {
            unsigned top = 0;
            for (unsigned i = 1; i < RENDERER_SLOTS; i++) if (renderer_op_blocked[i] > renderer_op_blocked[top]) top = i;
            if (!renderer_op_blocked[top]) break;
            const char *name = top < 1024 ? d3d8_funcs[top].name : top - 1024 < 7 ? controls[top - 1024] : "?";
            fprintf(stderr, "D3D calls: %-40s %7.1f calls/frame %7.3f ms/frame\n", name,
                    renderer_op_count[top] / frames, renderer_op_blocked[top] / (frames * 1e6));
            renderer_op_blocked[top] = 0;
        }
        memset(renderer_op_count, 0, sizeof(renderer_op_count));
        memset(renderer_op_blocked, 0, sizeof(renderer_op_blocked));
        return;
    }
    unsigned counts[RENDERER_SLOTS], state[8], draws; uint64_t blocked[RENDERER_SLOTS];
    pthread_mutex_lock(&renderer_lock);
    memcpy(state, renderer_state_count, sizeof(state));
    memset(renderer_state_count, 0, sizeof(renderer_state_count));
    draws = renderer_draw_count; renderer_draw_count = 0;
    unsigned posts = renderer_async_posts; renderer_async_posts = 0;
    memcpy(counts, renderer_op_count, sizeof(counts));
    memcpy(blocked, renderer_op_blocked, sizeof(blocked));
    memset(renderer_op_count, 0, sizeof(renderer_op_count));
    memset(renderer_op_blocked, 0, sizeof(renderer_op_blocked));
    pthread_mutex_unlock(&renderer_lock);
    fprintf(stderr, "D3D worker queued/frame: draw %.1f viewport %.1f texture %.1f RS %.1f TSS %.1f stream %.1f"
            " vertex %.1f transform %.1f constants %.1f state-enabled %u\n", draws / frames, state[0] / frames,
            state[1] / frames, state[2] / frames, state[3] / frames, state[4] / frames, state[5] / frames, state[6] / frames, state[7] / frames, renderer_queue_state);
    if (renderer_async_wanted)
        fprintf(stderr, "D3D queued drawing: %s, %.1f batches/frame, state copying %.2f ms/frame on the title's thread\n",
                d3d_async_active() ? "on" : "off", posts / frames, d3d_async_take_delta_ns() / (frames * 1e6));
    if (getenv("XBCOMPAT_PROFILE_WORKER_OPS"))
        for (unsigned i = 0; i < RENDERER_SLOTS; i++) if (counts[i])
            fprintf(stderr, "D3D worker synchronous op %u calls/frame %.1f\n", i, counts[i] / frames);
    static const char *controls[] = {"config", "bind-globals", "frame-count", "vblank", "read-pixels", "stats", "shutdown"};
    for (unsigned rank = 0; rank < 5; rank++) {
        unsigned top = 0;
        for (unsigned i = 1; i < RENDERER_SLOTS; i++) if (blocked[i] > blocked[top]) top = i;
        if (!blocked[top]) return;
        const char *name = top < 1024 ? d3d8_funcs[top].name : controls[top - 1024];
        fprintf(stderr, "D3D worker blocked: %s calls/frame %.1f blocked ms/frame %.3f\n",
                name, counts[top] / frames, blocked[top] / (frames * 1e6));
        blocked[top] = 0;
    }
}
