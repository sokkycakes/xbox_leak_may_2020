/* XBCOMPAT_ASYNC_DRAWS=1 (with XBCOMPAT_RENDER_WORKER=1): the render worker
   draws while the title's thread goes on with its own work.

   The title's threads keep setting state on the front copy of the device
   (d3d_storage[0]); state setters run there at once, as they always did.
   Work that needs GL (draws, clears, render target switches, visibility
   tests, presenting) is queued instead, each item preceded by whatever
   front state changed since the last one (field by field, and the title's
   render and texture stage state arrays in blocks), and the worker replays
   the queue on the back copy (d3d_storage[1]).  Everything else (resource
   creation, locks, readbacks, anything that returns what the GPU made)
   still waits for the queue to drain and runs on the worker, which then
   hands its state back to the front.

   Vertex and index buffers are read when the worker gets to the draw, as
   the NV2A reads them when it gets there: a title that rewrites a buffer
   without locking it (locks wait for the queue) can show the newer data.
   The title runs at most one frame ahead of the worker.  Included by d3d8.c
   after the functions it queues. */

enum {
    AS_FIELD = 1, AS_RS, AS_TSS, AS_DRAW, AS_DRAW_UP, AS_DRAW_INDEXED, AS_DRAW_INDEXED_UP, AS_CLEAR,
    AS_TARGET, AS_VIS_BEGIN, AS_VIS_END, AS_SWAP, AS_RAW, AS_RELEASE, AS_FENCE,
};

#define ASYNC_DRAWS_PER_POST 8   /* draws queued before the batch goes to the worker */

/* Defined in renderer/worker.h.  The builtin renderer has no worker (these
   stand-ins only link it; queued drawing never turns on there). */
__attribute__((weak)) void renderer_post_async(uint32_t *buf, unsigned n)
{
    (void)buf; (void)n;
    fatal("XBCOMPAT_ASYNC_DRAWS needs the native renderer's worker");
}
__attribute__((weak)) void renderer_async_flush(void) {}
__attribute__((weak)) bool renderer_has_worker(void) { return false; }
__attribute__((weak)) void renderer_async_wait(volatile ULONG *counter, ULONG target) { (void)counter; (void)target; }

static pthread_mutex_t async_lock;   /* recursive: the title's D3D calls, one at a time */
static volatile ULONG async_frames_done;   /* frames the worker has presented */
static volatile bool async_quit;     /* the worker saw the window close */
static d3d_state async_sent;         /* the front as the worker last heard it */
static ULONG *async_sent_rs, async_sent_tss[4 * 32];
static ULONG *async_back_rs, async_back_tss[4 * 32];
static USHORT *async_back_index_data;
static uint32_t *as_buf;
static unsigned as_n, as_cap, as_draws;

/* Which state the title's thread may have changed since the last delta,
   by group: each front call marks the groups it writes (d3d_async_groups),
   so a draw compares only those. */
enum {
    G_VSC = 1, G_PSC = 2, G_RS = 4, G_TSS = 8, G_TEX = 16, G_STREAM = 32, G_XFORM = 64, G_VIEW = 128,
    G_OTHER = 256, G_ALL = 511,
};
static unsigned async_dirty = G_ALL;
const char *async_last_call;   /* XBCOMPAT_ASYNC_CHECK: the last front call, for its messages */
static uint32_t async_vs_rows[6];   /* vertex shader constant rows written, when G_VSC is set */

#define AF(f, g) { offsetof(d3d_state, f), sizeof(((d3d_state *)0)->f), g }
/* The fields the worker follows.  Not here: the GL objects and window
   (worker only), the title's array pointers (each copy has its own), the
   GL depth range (host_zmax: the worker works it out at each target
   switch), and flags the worker clears itself (bb_cpu_dirty, stipple_dirty,
   gamma_dirty). */
static const struct { unsigned short off, size, group; } async_fields[] = {
    AF(width, G_OTHER), AF(height, G_OTHER), AF(frame, G_OTHER), AF(transforms, G_XFORM),
    AF(viewport, G_VIEW), AF(streams, G_STREAM), AF(textures, G_TEX), AF(vertex_shader, G_STREAM),
    AF(indices, G_STREAM), AF(base_vertex_index, G_STREAM), AF(lights, G_OTHER), AF(material, G_OTHER),
    AF(material_power, G_OTHER), AF(back_material, G_OTHER), AF(back_material_power, G_OTHER),
    AF(backbuffer, G_OTHER), AF(depth, G_OTHER), AF(target, G_OTHER), AF(target_depth, G_OTHER),
    AF(rt_width, G_OTHER), AF(rt_height, G_OTHER), AF(rt_texture, G_OTHER), AF(target_zmax, G_OTHER),
    AF(scissors, G_VIEW), AF(screen_offset, G_OTHER), AF(backbuffer_scale, G_OTHER),
    AF(multisample_type, G_OTHER), AF(color_format, G_OTHER), AF(depth_format, G_OTHER),
    AF(flicker_filter, G_OTHER), AF(soft_display, G_OTHER), AF(gamma, G_OTHER), AF(gamma_set, G_OTHER),
    AF(constant_mode, G_OTHER), AF(vs_const, G_VSC), AF(ps_const, G_PSC), AF(eye_vector, G_OTHER),
    AF(pixel_shader, G_RS), AF(zscale, G_OTHER), AF(cpu_time, G_OTHER), AF(callbacks, G_OTHER),
    AF(ncallbacks, G_OTHER), AF(palettes, G_TEX), AF(stipple_on, G_RS), AF(stipple, G_RS),
    AF(direct_mv, G_XFORM),
};
#undef AF
_Static_assert(sizeof(d3d_state) <= 0xFFFF, "async_fields keeps offsets in 16 bits");

static bool async_front(void) { return async_on && !async_worker_thread; }

static uint32_t *as_op(unsigned op, unsigned payload_dwords)
{
    unsigned need = as_n + 1 + payload_dwords;
    if (need > as_cap) {
        while (as_cap < need) as_cap = as_cap ? as_cap * 2 : 4096;
        as_buf = realloc(as_buf, as_cap * 4);
        if (!as_buf) fatal("D3D: out of memory queueing draws");
    }
    uint32_t *p = as_buf + as_n;
    p[0] = op | (1 + payload_dwords) << 8;
    as_n = need;
    return p + 1;
}

/* XBCOMPAT_PROFILE_RENDERER: the title's thread's time spent finding and
   copying state changes, reported with the worker's numbers. */
static uint64_t async_delta_ns;
static int async_profiling = -1;
uint64_t d3d_async_take_delta_ns(void) { return __atomic_exchange_n(&async_delta_ns, 0, __ATOMIC_RELAXED); }

static void async_delta_(void);
static void async_delta(void)
{
    if (async_profiling < 0) async_profiling = getenv("XBCOMPAT_PROFILE_RENDERER") != NULL;
    if (!async_profiling) { async_delta_(); return; }
    uint64_t t0 = now_ns();
    async_delta_();
    __atomic_fetch_add(&async_delta_ns, now_ns() - t0, __ATOMIC_RELAXED);
}

static void async_touch(unsigned groups)
{
    async_dirty |= groups;
    if (groups & G_VSC) memset(async_vs_rows, 0xFF, sizeof async_vs_rows);
}

void d3d_async_touch(unsigned groups) { async_touch(groups); }

/* A vertex shader constant row the title's thread wrote. */
static void async_vs_touch(int slot)
{
    async_dirty |= G_VSC;
    async_vs_rows[slot >> 5] |= 1u << (slot & 31);
}

static void async_clean(void)
{
    async_dirty = 0;
    memset(async_vs_rows, 0, sizeof async_vs_rows);
}

static void async_delta_(void)
{
    unsigned dirty = async_dirty;
    const char *front = (const char *)&d3d_storage[0];
    char *sent = (char *)&async_sent;
    if (dirty & G_VSC) {
        /* Runs of written rows that differ from what the worker has. */
        const float (*fv)[4] = d3d_storage[0].vs_const;
        float (*sv)[4] = async_sent.vs_const;
        for (unsigned r = 0; r < 192;) {
            if (!(async_vs_rows[r >> 5] >> (r & 31) & 1) || same_bytes(fv[r], sv[r], 16)) { r++; continue; }
            unsigned e = r + 1;
            while (e < 192 && e - r < 16 && (async_vs_rows[e >> 5] >> (e & 31) & 1) && !same_bytes(fv[e], sv[e], 16)) e++;
            memcpy(sv[r], fv[r], (e - r) * 16);
            uint32_t *p = as_op(AS_FIELD, 2 + (e - r) * 4);
            p[0] = offsetof(d3d_state, vs_const) + r * 16;
            p[1] = (e - r) * 16;
            memcpy(p + 2, fv[r], (e - r) * 16);
            r = e;
        }
    }
    for (unsigned i = 0; i < sizeof async_fields / sizeof async_fields[0]; i++) {
        if (!(async_fields[i].group & dirty) || async_fields[i].group == G_VSC) continue;
        for (unsigned o = 0; o < async_fields[i].size; o += 64) {
            unsigned off = async_fields[i].off + o, n = async_fields[i].size - o < 64 ? async_fields[i].size - o : 64;
            if (same_bytes(front + off, sent + off, n)) continue;
            memcpy(sent + off, front + off, n);
            uint32_t *p = as_op(AS_FIELD, 2 + (n + 3) / 4);
            p[0] = off;
            p[1] = n;
            memcpy(p + 2, front + off, n);
        }
    }
    const ULONG *rs = d3d_storage[0].render_state, *tss = d3d_storage[0].texture_state;
    /* The render and texture stage state arrays are the title's own: its
       inlined setters store into them without a call, so they are always
       compared. */
    unsigned nrs = d3d_storage[0].rs_count;
    for (unsigned i = 0; i < nrs; i += 16) {
        unsigned n = nrs - i < 16 ? nrs - i : 16;
        if (same_bytes(rs + i, async_sent_rs + i, n * 4)) continue;
        memcpy(async_sent_rs + i, rs + i, n * 4);
        uint32_t *p = as_op(AS_RS, 2 + n);
        p[0] = i;
        p[1] = n;
        memcpy(p + 2, rs + i, n * 4);
    }
    for (unsigned i = 0; i < 4 * 32; i += 16) {
        if (same_bytes(tss + i, async_sent_tss + i, 64)) continue;
        memcpy(async_sent_tss + i, tss + i, 64);
        uint32_t *p = as_op(AS_TSS, 2 + 16);
        p[0] = i;
        p[1] = 16;
        memcpy(p + 2, tss + i, 64);
    }
    async_clean();
    static int check = -1;
    if (check < 0) check = getenv("XBCOMPAT_ASYNC_CHECK") != NULL;
    if (check) {
        /* Debug aid: anything the groups missed. */
        for (unsigned i = 0; i < sizeof async_fields / sizeof async_fields[0]; i++)
            if (memcmp(front + async_fields[i].off, sent + async_fields[i].off, async_fields[i].size))
                fprintf(stderr, "D3D async check: field at %u (group %u) missed, last front call %s\n",
                        async_fields[i].off, async_fields[i].group, async_last_call ? async_last_call : "?");
        if (memcmp(rs, async_sent_rs, d3d_storage[0].rs_count * 4))
            fprintf(stderr, "D3D async check: render state missed, last front call %s\n", async_last_call ? async_last_call : "?");
        if (memcmp(tss, async_sent_tss, sizeof async_sent_tss))
            fprintf(stderr, "D3D async check: texture state missed, last front call %s\n", async_last_call ? async_last_call : "?");
    }
}

/* Hand the queued batch to the worker. */
static void async_post(void)
{
    if (!as_n) return;
    renderer_post_async(as_buf, as_n);
    as_buf = NULL;
    as_n = as_cap = as_draws = 0;
}

/* Raw methods the title wrote into the device's push buffer (LTCG builds)
   go to the worker as they are; called by pusher_drain on the front.  They
   can set state the front cannot follow, so the front waits for the worker
   and takes its state back (titles that set state through calls rarely
   write methods themselves). */
void d3d_async_after_sync(void);
static void async_raw_front(const ULONG *p, unsigned n)
{
    async_delta();
    uint32_t *a = as_op(AS_RAW, 1 + n);
    a[0] = n;
    memcpy(a + 1, p, n * 4);
    async_post();
    renderer_async_flush();
    d3d_async_after_sync();
}

/* The title checks fences against the GPU's time (its inlined code reads
   the device's m_pGpuTime) to know when it may rewrite a buffer the GPU is
   reading.  Queued draws read buffers when the worker gets to them, so the
   GPU's time is the last fence the worker passed.  Sent at once: a title
   may spin on it. */
static void async_fence(ULONG f)
{
    uint32_t *p = as_op(AS_FENCE, 1);
    p[0] = f;
    async_post();
}

static void async_drawn(void)
{
    if (++as_draws >= ASYNC_DRAWS_PER_POST) async_post();
}

static void async_draw(ULONG prim, ULONG start, ULONG count)
{
    async_delta();
    uint32_t *p = as_op(AS_DRAW, 3);
    p[0] = prim; p[1] = start; p[2] = count;
    async_drawn();
}

static void async_draw_up(ULONG prim, ULONG count, const void *data, ULONG stride)
{
    async_delta();
    ULONG bytes = count * stride;
    uint32_t *p = as_op(AS_DRAW_UP, 3 + (bytes + 3) / 4);
    p[0] = prim; p[1] = count; p[2] = stride;
    memcpy(p + 3, data, bytes);
    async_drawn();
}

/* The indices stay where they are: the worker's index cache knows them by
   address and content. */
static void async_draw_indexed(ULONG prim, ULONG count, const USHORT *indices)
{
    async_delta();
    uint32_t *p = as_op(AS_DRAW_INDEXED, 3);
    p[0] = prim; p[1] = count; p[2] = (uint32_t)(uintptr_t)indices;
    async_drawn();
}

static void async_draw_indexed_up(ULONG prim, ULONG count, const USHORT *indices, const void *verts, ULONG stride)
{
    async_delta();
    ULONG nverts = 0;
    for (ULONG i = 0; i < count; i++) if (indices[i] + 1u > nverts) nverts = indices[i] + 1u;
    ULONG ib = (count * 2 + 3) & ~3u, vb = nverts * stride;
    uint32_t *p = as_op(AS_DRAW_INDEXED_UP, 3 + ib / 4 + (vb + 3) / 4);
    p[0] = prim; p[1] = count; p[2] = stride;
    memcpy(p + 3, indices, count * 2);
    memcpy((char *)(p + 3) + ib, verts, vb);
    async_drawn();
}

static void async_clear(ULONG count, const D3DRECT *rects, ULONG flags, ULONG color, float z, ULONG stencil)
{
    async_delta();
    if (!rects) count = 0;
    uint32_t *p = as_op(AS_CLEAR, 5 + count * sizeof(D3DRECT) / 4);
    p[0] = count; p[1] = flags; p[2] = color; memcpy(&p[3], &z, 4); p[4] = stencil;
    if (count) memcpy(p + 5, rects, count * sizeof(D3DRECT));
}

/* The state a render target switch leaves, as D3DDevice_SetRenderTarget
   sets it (the worker sets the same on its copy, and the GL depth range). */
static void async_target_state(D3DSurface *target, D3DSurface *z)
{
    if (target == d3d.backbuffer || (!target->Parent && !p_glGenFramebuffers)) {
        d3d.rt_width = d3d.width;
        d3d.rt_height = d3d.height;
        d3d.rt_texture = false;
        d3d.target_zmax = z ? depth_format_max((z->Format >> 8) & 0xFF) : d3d.zscale;
        target_viewport_reset(z, d3d.rt_width, d3d.rt_height);
        return;
    }
    if (!p_glGenFramebuffers) return;
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)target, &w, &h, &pitch);
    d3d.target_zmax = z ? depth_format_max((z->Format >> 8) & 0xFF) : d3d.zscale;
    d3d.rt_width = w;
    d3d.rt_height = h;
    d3d.rt_texture = true;
    target_viewport_reset(z, w, h);
}

static void async_target(D3DSurface *target, D3DSurface *z)
{
    async_delta();   /* the worker switches with everything set before it */
    if (!target) target = d3d.target;
    internal_addref_surface(target);
    if (z) internal_addref_surface(z);
    if (d3d.target) internal_release_surface(d3d.target);
    if (d3d.target_depth) internal_release_surface(d3d.target_depth);
    d3d.target = target;
    d3d.target_depth = z;
    sync_device_surfaces();
    async_target_state(target, z);
    uint32_t *p = as_op(AS_TARGET, 2);
    p[0] = (uint32_t)(uintptr_t)target; p[1] = (uint32_t)(uintptr_t)z;
    /* The worker's switch leaves its copy as the front's now is. */
    const char *front = (const char *)&d3d_storage[0];
    for (unsigned i = 0; i < sizeof async_fields / sizeof async_fields[0]; i++)
        memcpy((char *)&async_sent + async_fields[i].off, front + async_fields[i].off, async_fields[i].size);
}

/* A resource's last reference: the worker frees it after the draws queued
   before it are done with it. */
static void async_release(D3DResource *r)
{
    uint32_t *p = as_op(AS_RELEASE, 1);
    p[0] = (uint32_t)(uintptr_t)r;
}

/* Visibility test results, filled in by the worker as the GPU finishes
   them; GetVisibilityTestResult reads them on the title's thread. */
static struct { ULONG index; int state; ULONG result; } vis_shared[VIS_SLOTS];   /* state: 1 pending, 2 done */
static pthread_mutex_t vis_lock = PTHREAD_MUTEX_INITIALIZER;

static void async_vis_begin(void)
{
    async_delta();
    as_op(AS_VIS_BEGIN, 0);
}

static void async_vis_end(ULONG index)
{
    pthread_mutex_lock(&vis_lock);
    unsigned slot = index % VIS_SLOTS;
    vis_shared[slot].index = index;
    vis_shared[slot].state = 1;
    pthread_mutex_unlock(&vis_lock);
    uint32_t *p = as_op(AS_VIS_END, 1);
    p[0] = index;
}

static LONG async_vis_result(ULONG index, UINT_ *result)
{
    pthread_mutex_lock(&vis_lock);
    unsigned slot = index % VIS_SLOTS;
    LONG r = vis_shared[slot].index != index || !vis_shared[slot].state ? D3DERR_INVALIDCALL
             : vis_shared[slot].state == 1 ? 0x88760828 /* D3DERR_TESTINCOMPLETE */ : D3D_OK;
    if (result) *result = r == D3D_OK ? vis_shared[slot].result : 0;
    pthread_mutex_unlock(&vis_lock);
    return r;
}

/* Worker: collect the results the GPU has finished. */
static void async_vis_poll(void)
{
    pthread_mutex_lock(&vis_lock);
    for (unsigned slot = 0; slot < VIS_SLOTS; slot++) {
        if (vis_shared[slot].state != 1 || !vis[slot].used || vis[slot].index != vis_shared[slot].index
            || !vis[slot].query)
            continue;
        GLuint avail = 0, n = 0;
        p_glGetQueryObjectuiv(vis[slot].query, 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &avail);
        if (!avail) continue;
        p_glGetQueryObjectuiv(vis[slot].query, 0x8866 /* GL_QUERY_RESULT */, &n);
        vis_shared[slot].result = n;
        vis_shared[slot].state = 2;
    }
    pthread_mutex_unlock(&vis_lock);
}

static void async_wait_frames(ULONG frames)
{
    renderer_async_wait(&async_frames_done, frames);
}

/* Swap on the title's thread: the frame's bookkeeping and callbacks here,
   presenting on the worker, at most one frame behind. */
static ULONG async_swap(void)
{
    pusher_drain();
    swap_begin_frame();
    ke_frame_presented();
    run_callbacks();
    async_touch(G_OTHER);   /* the frame number and the callbacks */
    async_delta();
    as_op(AS_SWAP, 0);
    async_post();
    swap_finish();
    swap_checks(async_quit);
    if (g_exit_after_frames && (int)d3d.frame >= g_exit_after_frames) async_wait_frames(d3d.frame);
    swap_exit_check();
    async_wait_frames(d3d.frame - 1);
    return d3d.frame;
}

/* ---- the worker's side ---- */

void d3d_async_run(const uint32_t *buf, unsigned n)
{
    d3d_state *back = &d3d_storage[1];
    async_replaying = true;
    bool batch_flushed = false;   /* a pending HUD batch drew with the state before this run's changes */
    for (unsigned i = 0; i < n;) {
        unsigned op = buf[i] & 0xFF, len = buf[i] >> 8;
        const uint32_t *a = buf + i + 1;
        if (op == AS_FIELD || op == AS_RS || op == AS_TSS) {
            if (!batch_flushed) hud_batch_flush();
            batch_flushed = true;
        } else batch_flushed = false;
        switch (op) {
        case AS_FIELD:
            memcpy((char *)back + a[0], a + 2, a[1]);
            if (a[0] < offsetof(d3d_state, vs_const) + sizeof back->vs_const &&
                a[0] + a[1] > offsetof(d3d_state, vs_const)) {
                unsigned lo = a[0] > offsetof(d3d_state, vs_const) ? (a[0] - offsetof(d3d_state, vs_const)) / 16 : 0;
                unsigned hi = (a[0] + a[1] - offsetof(d3d_state, vs_const) + 15) / 16;
                vsc_touch_in(back, lo, hi - lo);
            }
            if (a[0] < offsetof(d3d_state, stipple) + sizeof back->stipple && a[0] + a[1] > offsetof(d3d_state, stipple))
                back->stipple_dirty = true;
            break;
        case AS_RS: memcpy(back->render_state + a[0], a + 2, a[1] * 4); break;
        case AS_TSS: memcpy(back->texture_state + a[0], a + 2, a[1] * 4); break;
        case AS_DRAW: D3DDevice_DrawVertices(a[0], a[1], a[2]); break;
        case AS_DRAW_UP: D3DDevice_DrawVerticesUP(a[0], a[1], a + 3, a[2]); break;
        case AS_DRAW_INDEXED: D3DDevice_DrawIndexedVertices(a[0], a[1], (const USHORT *)(uintptr_t)a[2]); break;
        case AS_DRAW_INDEXED_UP: {
            ULONG ib = (a[1] * 2 + 3) & ~3u;
            D3DDevice_DrawIndexedVerticesUP(a[0], a[1], (const USHORT *)(a + 3), (const char *)(a + 3) + ib, a[2]);
            break;
        }
        case AS_CLEAR: {
            float z;
            memcpy(&z, &a[3], 4);
            D3DDevice_Clear(a[0], a[0] ? (const D3DRECT *)(a + 5) : NULL, a[1], a[2], z, a[4]);
            break;
        }
        case AS_TARGET: D3DDevice_SetRenderTarget((D3DSurface *)(uintptr_t)a[0], (D3DSurface *)(uintptr_t)a[1]); break;
        case AS_VIS_BEGIN: D3DDevice_BeginVisibilityTest(); break;
        case AS_VIS_END: D3DDevice_EndVisibilityTest(a[0]); break;
        case AS_SWAP:
            hud_batch_flush();
            swap_cpu_backbuffer();
            flush_cpu_backbuffer();
            front_buffer_overlay();
            front_buffer_update();
            swap_gpu_frame_end();
            swap_screenshot();
            swap_present();
            persisted_first_present();
            if (swap_window_events()) async_quit = true;
            /* The title's thread sees it when this batch is done. */
            __atomic_store_n(&async_frames_done, back->frame, __ATOMIC_RELEASE);
            break;
        case AS_RAW: pb_interpret(a + 1, a[0]); break;
        case AS_RELEASE: D3DResource_Release((D3DResource *)(uintptr_t)a[0]); break;
        case AS_FENCE:
            hud_batch_flush();   /* batched HUD draws have read their vertices */
            __atomic_store_n(&async_gpu_time, a[0], __ATOMIC_RELEASE);
            break;
        default: fprintf(stderr, "D3D: bad queued op %u\n", op); abort();
        }
        i += len;
    }
    async_replaying = false;
    async_vis_poll();
}

/* ---- the switch-over and the calls that still wait ---- */

bool d3d_async_wanted(void)
{
    static int wanted = -1;
    if (wanted < 0) {
        const char *e = getenv("XBCOMPAT_ASYNC_DRAWS");
        wanted = e && *e && *e != '0' && renderer_has_worker();
    }
    return wanted;
}

/* Called on the title's thread before a call the queue may carry.  The
   first one after the device exists makes the front copy from the worker's
   (nothing is queued yet: every earlier call waited). */
bool d3d_async_ready(void)
{
    /* Push buffer recording runs on the worker, call by call. */
    if (async_on) return !d3d_storage[1].recording;
    if (!d3d_async_wanted()) return false;
    d3d_state *back = &d3d_storage[1], *front = &d3d_storage[0];
    if (!back->window || !back->render_state || !back->texture_state || back->recording) return false;
    *front = *back;
    if (back->render_state == back->render_state_fallback) front->render_state = front->render_state_fallback;
    if (back->texture_state == back->texture_state_fallback) front->texture_state = front->texture_state_fallback;
    unsigned nrs = back->rs_count ? back->rs_count : D3DRS_MAX;
    front->rs_count = back->rs_count = nrs;
    async_back_rs = calloc(nrs, 4);
    async_sent_rs = calloc(nrs, 4);
    async_back_index_data = NULL;
    memcpy(async_back_rs, front->render_state, nrs * 4);
    memcpy(async_sent_rs, front->render_state, nrs * 4);
    memcpy(async_back_tss, front->texture_state, sizeof async_back_tss);
    memcpy(async_sent_tss, front->texture_state, sizeof async_sent_tss);
    back->render_state = async_back_rs;
    back->texture_state = async_back_tss;
    back->index_data = &async_back_index_data;
    async_sent = *front;
    async_clean();
    async_frames_done = front->frame;
    /* From now on the GPU's time is the worker's (bind_miniport pointed it
       at the CPU's: m_pGpuTime follows m_CpuTime in the device). */
    if (front->cpu_time) {
        async_gpu_time = *front->cpu_time;
        front->cpu_time[1] = (ULONG)(uintptr_t)&async_gpu_time;
    }
    async_on = true;
    xlog("D3D: queued drawing on (frame %u)", front->frame);
    return true;
}

static void async_lock_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&async_lock, &attr);
}

void d3d_async_lock(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, async_lock_init);
    pthread_mutex_lock(&async_lock);
}
void d3d_async_unlock(void) { pthread_mutex_unlock(&async_lock); }
bool d3d_async_active(void) { return async_on; }

/* Before a call that waits: everything queued so far goes first, with the
   state it will run in. */
void d3d_async_before_sync(void)
{
    pusher_drain();
    async_delta();
    async_post();
}

/* After it: the worker's state (the call may have changed it) becomes the
   front's.  The worker is idle and the title's thread waited, so neither
   copy is moving. */
void d3d_async_after_sync(void)
{
    d3d_state *back = &d3d_storage[1], *front = &d3d_storage[0];
    __atomic_store_n(&async_gpu_time, fence, __ATOMIC_RELEASE);   /* everything has been drawn */
    for (unsigned i = 0; i < sizeof async_fields / sizeof async_fields[0]; i++)
        memcpy((char *)front + async_fields[i].off, (const char *)back + async_fields[i].off, async_fields[i].size);
    memcpy(front->render_state, back->render_state, front->rs_count * 4);
    memcpy(front->texture_state, back->texture_state, sizeof async_back_tss);
    async_sent = *front;
    memcpy(async_sent_rs, front->render_state, front->rs_count * 4);
    memcpy(async_sent_tss, front->texture_state, sizeof async_sent_tss);
    async_clean();
}

/* Which calls run on the title's thread: 1 state the front keeps, 2 work
   the front queues (the functions check async_front()), 3 waits that touch
   no device state and so run without the lock (a thread looping on the
   vertical blank would keep every other thread out), 0 the rest. */
int d3d_async_class(const char *name, const void *impl)
{
    if (impl == (const void *)D3DDevice_BlockUntilVerticalBlank) return 3;
    static const void *const queued[] = {
        (const void *)D3DDevice_DrawVertices, (const void *)D3DDevice_DrawVerticesUP,
        (const void *)D3DDevice_DrawIndexedVertices, (const void *)D3DDevice_DrawIndexedVerticesUP,
        (const void *)D3DDevice_Clear, (const void *)D3DDevice_SetRenderTarget,
        (const void *)D3DDevice_BeginVisibilityTest, (const void *)D3DDevice_EndVisibilityTest,
        (const void *)D3DDevice_Swap,
    };
    static const void *const state[] = {
        (const void *)D3DDevice_SetTexture, (const void *)D3DDevice_SetStreamSource,
        (const void *)D3DDevice_SetVertexShader, (const void *)D3DDevice_SetTransform,
        (const void *)D3DDevice_SetViewport, (const void *)D3DDevice_SetPixelShaderConstant,
        (const void *)D3DDevice_SetVertexShaderConstant, (const void *)SetVertexShaderConstantNotInline,
        (const void *)SetVertexShaderConstant1, (const void *)SetVertexShaderConstant4,
        (const void *)D3DDevice_SetShaderConstantMode, (const void *)D3DDevice_SetScissors,
        (const void *)D3DDevice_SetPixelShader, (const void *)D3DDevice_SetPixelShaderProgram,
        (const void *)D3DDevice_SetIndices, (const void *)D3DDevice_SetTextureStageStateNotInline,
        (const void *)SetRenderStateNotInline, (const void *)D3DDevice_GetViewportOffsetAndScale,
        (const void *)D3DDevice_GetVisibilityTestResult,
        (const void *)D3DDevice_SetStipple,
        /* Vertex program memory is read only by RunVertexStateShader,
           which waits for the queue. */
        (const void *)D3DDevice_SelectVertexShader, (const void *)D3DDevice_LoadVertexShader,
        (const void *)D3DDevice_LoadVertexShaderProgram,
        /* Reference counts (the last Release is queued), and the device push
           buffer's entry points, which only drain it (pusher_drain). */
        (const void *)D3DResource_AddRef, (const void *)D3DResource_Release,
        (const void *)D3D_LazySetState, (const void *)CDevice_SetStateVB, (const void *)CDevice_SetStateUP,
        (const void *)CDevice_KickOff, (const void *)D3D_MakeRequestedSpace, (const void *)D3DDevice_MakeSpace,
        (const void *)D3DDevice_BeginPushLTCG,
        /* A fence number; waiting on one still drains the queue. */
        (const void *)D3D_SetFence, (const void *)D3DDevice_InsertFence,
        /* Exports that do nothing. */
        (const void *)D3D_Nop0, (const void *)D3D_Nop4, (const void *)D3D_Nop8, (const void *)D3D_Nop12,
        (const void *)D3D_FastNop, (const void *)D3DPERF_Zero, (const void *)D3DPERF_Zero4,
        (const void *)D3DPERF_Zero8, (const void *)D3DPERF_Zero12, (const void *)D3DPERF_Zero16,
        (const void *)D3DDevice_Nop,
        /* Fixed-function state and getters: front fields only. */
        (const void *)D3DDevice_SetLight,
        (const void *)D3DDevice_LightEnable,
        (const void *)D3DDevice_SetMaterial,
        (const void *)D3DDevice_SetBackMaterial,
        (const void *)D3DDevice_GetBackMaterial,
        (const void *)D3DDevice_SetPalette,
        (const void *)D3DDevice_SetVertexShaderInput,
        (const void *)D3DDevice_SetFlickerFilter,
        (const void *)D3DDevice_SetSoftDisplayFilter,
        (const void *)D3DDevice_SetScreenSpaceOffset,
        (const void *)D3DDevice_SetBackBufferScale,
        (const void *)D3DDevice_GetBackBufferScale,
        (const void *)D3DDevice_GetTransform,
        (const void *)D3DDevice_GetViewport,
        (const void *)D3DDevice_GetRenderState,
        (const void *)D3DDevice_GetTextureStageState,
        (const void *)D3DDevice_GetTexture,
        (const void *)D3DDevice_GetTexture2,
        (const void *)D3DDevice_GetStreamSource,
        (const void *)D3DDevice_GetStreamSource2,
        (const void *)D3DDevice_GetIndices,
        (const void *)D3DDevice_GetMaterial,
        (const void *)D3DDevice_GetLight,
        (const void *)D3DDevice_GetLightEnable,
        (const void *)D3DDevice_GetVertexShader,
        (const void *)D3DDevice_GetVertexShaderConstant,
        (const void *)D3DDevice_GetShaderConstantMode,
        (const void *)D3DDevice_GetPixelShader,
        (const void *)D3DDevice_GetPixelShaderConstant,
        (const void *)D3DDevice_GetScissors,
        (const void *)D3DDevice_GetProjectionViewportMatrix,
        (const void *)D3DDevice_GetModelView,
        (const void *)D3DDevice_GetDisplayFieldStatus,
        (const void *)D3DDevice_GetBackBuffer,
        (const void *)D3DDevice_GetBackBuffer2,
        (const void *)D3DDevice_GetRenderTarget,
        (const void *)D3DDevice_GetRenderTarget2,
        (const void *)D3DDevice_GetDepthStencilSurface,
        (const void *)D3DDevice_GetDepthStencilSurface2,
        (const void *)D3DDevice_GetCreationParameters,
        (const void *)D3DDevice_GetDisplayMode,
        (const void *)D3DDevice_GetDeviceCaps,
        (const void *)Direct3D_GetDeviceCaps,
        (const void *)D3DDevice_GetDirect3D,
        (const void *)D3DDevice_GetRasterStatus,
        (const void *)D3DDevice_SetDebugMarker,
    };
    for (unsigned i = 0; i < sizeof queued / sizeof queued[0]; i++) if (impl == queued[i]) return 2;
    for (unsigned i = 0; i < sizeof state / sizeof state[0]; i++) if (impl == state[i]) return 1;
    /* The render and texture stage state setters only store into the arrays. */
    if (strstr(name, "SetRenderState_") || strstr(name, "SetTextureState_")) return 1;
    return 0;
}

/* The state groups a front call (class 1) may write; anything not listed
   may write anything. */
unsigned d3d_async_groups(const char *name, const void *impl)
{
    static const struct { const void *impl; unsigned groups; } known[] = {
        /* These mark the vertex shader constant rows they write themselves. */
        { (const void *)D3DDevice_SetVertexShaderConstant, 0 }, { (const void *)SetVertexShaderConstantNotInline, 0 },
        { (const void *)SetVertexShaderConstant1, 0 }, { (const void *)SetVertexShaderConstant4, 0 },
        { (const void *)D3DDevice_SetViewport, G_VIEW },
        { (const void *)D3DDevice_SetPixelShaderConstant, G_PSC | G_RS },
        { (const void *)D3DDevice_SetTexture, G_TEX }, { (const void *)D3DDevice_SetPalette, G_TEX },
        { (const void *)D3DDevice_SetStreamSource, G_STREAM }, { (const void *)D3DDevice_SetVertexShader, G_STREAM },
        { (const void *)D3DDevice_SetIndices, G_STREAM }, { (const void *)D3DDevice_SetVertexShaderInput, G_STREAM },
        { (const void *)D3DDevice_SelectVertexShader, G_STREAM },
        { (const void *)D3DDevice_SetTransform, G_XFORM }, { (const void *)D3DDevice_SetScissors, G_VIEW },
        { (const void *)D3DDevice_SetPixelShader, G_RS }, { (const void *)D3DDevice_SetPixelShaderProgram, G_RS },
        { (const void *)SetRenderStateNotInline, G_RS },
        { (const void *)SetRenderState_Simple, G_RS | G_TSS }, { (const void *)D3DDevice_SetStipple, G_RS | G_TSS },
        { (const void *)D3DDevice_SetTextureStageStateNotInline, G_TSS },
        { (const void *)D3DDevice_SetShaderConstantMode, G_OTHER }, { (const void *)D3DDevice_SetLight, G_OTHER },
        { (const void *)D3DDevice_LightEnable, G_OTHER }, { (const void *)D3DDevice_SetMaterial, G_OTHER },
        { (const void *)D3DDevice_SetBackMaterial, G_OTHER },
        /* Nothing on the device: counts, fences, program memory, and the
           push buffer entry points (pusher_drain marks what it applies). */
        { (const void *)D3DResource_AddRef, 0 }, { (const void *)D3DResource_Release, 0 },
        { (const void *)D3D_SetFence, 0 }, { (const void *)D3DDevice_InsertFence, 0 },
        { (const void *)D3DDevice_LoadVertexShader, 0 }, { (const void *)D3DDevice_LoadVertexShaderProgram, 0 },
        { (const void *)D3D_LazySetState, 0 }, { (const void *)CDevice_SetStateVB, 0 },
        { (const void *)CDevice_SetStateUP, 0 }, { (const void *)CDevice_KickOff, 0 },
        { (const void *)D3D_MakeRequestedSpace, 0 }, { (const void *)D3DDevice_MakeSpace, 0 },
        { (const void *)D3DDevice_BeginPushLTCG, 0 },
        { (const void *)D3D_Nop0, 0 }, { (const void *)D3D_Nop4, 0 }, { (const void *)D3D_Nop8, 0 },
        { (const void *)D3D_Nop12, 0 }, { (const void *)D3D_FastNop, 0 }, { (const void *)D3DPERF_Zero, 0 },
        { (const void *)D3DPERF_Zero4, 0 }, { (const void *)D3DPERF_Zero8, 0 }, { (const void *)D3DPERF_Zero12, 0 },
        { (const void *)D3DPERF_Zero16, 0 }, { (const void *)D3DDevice_Nop, 0 },
    };
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++) if (impl == known[i].impl) return known[i].groups;
    if (strstr(name, "SetRenderState_")) return G_RS;
    if (strstr(name, "SetTextureState_")) return G_TSS;
    if (strstr(name, "Get")) return 0;   /* the getters on the front list only read */
    return G_ALL;
}
