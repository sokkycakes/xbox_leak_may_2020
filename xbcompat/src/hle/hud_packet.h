/*
 * Draw packets for the renderer worker: a pre-transformed textured draw
 * captured on the guest's thread, with the state and vertices it needs,
 * to run later on the worker that owns GL.  A front holds what was true
 * when the guest asked for the call (fvf_plan.h's key, the resident
 * textures, the push buffer's put pointer); a draw is captured only while
 * nothing it depends on has moved since.  Included by d3d8.c.
 */
/* Our own push buffer, when the title has none (see pusher_init). */
static ULONG *pusher_buf;
struct xbr_resident { ULONG data, format, size; GLuint id; };
struct xbr_catalog { unsigned refs, version, count; struct xbr_resident entries[]; };
static struct xbr_catalog *hud_catalog;   /* the texture cache's 2D entries, shared by fronts */
struct xbr_draw_front {
    fvf_plan_key state;
    ULONG *rs, *tss;
    unsigned rs_count, width, height;
    ULONG pixel_shader, bb_data, depth_data;
    bool certified, queue_state, no_cache, shader_ready, combine_ready;
    struct xbr_catalog *catalog;
    unsigned char xlat[D3DRS_MAX];
    const ULONG *put;            /* the title's push buffer put pointer, and where it must be */
    ULONG pusher;
    GLuint ids[4];
    const D3DResource *vb;       /* stream 0 when the draw is bound */
    D3DResource vb_header;
    const UCHAR *vb_data;
    ULONG vb_stride;
};
struct xbr_draw_packet {
    fvf_plan_key state;
    GLuint ids[4];
    ULONG rs[256], tss[128];
    D3DPixelContainer textures[4];
    ULONG count, stride;
    size_t size;
    UCHAR vertices[];
};

struct xbr_draw_front *d3d_draw_front_create(void *impl)
{
    bool bound = impl == (void *)D3DDevice_DrawVertices;
    if (!bound && impl != (void *)D3DDevice_DrawVerticesUP) return NULL;
    if (bound && !d3d.streams[0].vb) return NULL;
    if (!d3d.window || d3d.recording || hud_query_active || g_trace || getenv("XBCOMPAT_DUMP_DRAWS")
        || getenv("XBCOMPAT_PROFILE_SYNC")) { hud_front_reject[0]++; return NULL; }
    if (!fvf_plan.valid || fvf_plan.serial != conv_serial || fvf_plan.epoch != gc_epoch) { hud_front_reject[1]++; return NULL; }
    if (!hud_prepared_program(5 /* TRIANGLELIST */)
        || (hud_shader_supported() && (!hud_program || !hud_program->prog))) { hud_front_reject[2]++; return NULL; }
    if ((d3d.vertex_shader & ~0x100u) != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)) { hud_front_reject[3]++; return NULL; }
    if (RS(D3DRS_SPECULARENABLE) || RS(D3DRS_POINTSPRITEENABLE)) { hud_front_reject[4]++; return NULL; }
    /* A title writing its own push buffer may have commands queued there
       that the draw must come after; only our own empty one is safe. */
    if (d3d.device && d3d.device[0] && d3d.device[0] != (ULONG)pusher_buf) { hud_front_reject[5]++; return NULL; }
    struct xbr_draw_front front = { 0 };
    fvf_layout l = parse_fvf(d3d.vertex_shader);
    if (!fvf_plan_capture(&front.state, &l)
        || memcmp(&front.state, &fvf_plan.key, sizeof(front.state))) { hud_front_reject[6]++; return NULL; }
    if (!hud_resident_textures(front.ids, fvf_plan.units)) { hud_front_reject[7]++; return NULL; }
    front.certified = true;
    front.rs = d3d.render_state; front.tss = d3d.texture_state;
    /* Checked again at capture: the draw is queued only if the title hasn't put anything since. */
    front.put = d3d.device;
    front.pusher = (ULONG)pusher_buf;
    /* A bound draw reads the vertex buffer when it runs: keep its header,
       so a capture can tell the buffer wasn't replaced or moved. */
    front.vb = bound ? d3d.streams[0].vb : NULL;
    if (front.vb) {
        front.vb_header = *front.vb;
        front.vb_data = resource_data((D3DResource *)front.vb);
        front.vb_stride = d3d.streams[0].stride;
    }
    memcpy(front.xlat, rs_xlat, sizeof(front.xlat));
    struct xbr_draw_front *out = malloc(sizeof(*out));
    if (out) *out = front;
    return out;
}

/* The worker frees a front once it has captured (or given up on) its call. */
void d3d_draw_front_free(struct xbr_draw_front *front)
{
    if (front && front->catalog && !--front->catalog->refs) free(front->catalog);
    free(front);
}

#ifdef XBC_TRANSLATED /* guest call frames exist only in the translated renderer */
struct xbr_draw_packet *d3d_draw_packet_capture(const struct xbr_draw_front *front, void *impl,
                                                struct xa_frame *call)
{
    bool bound = impl == (void *)D3DDevice_DrawVertices;
    if ((impl != (void *)D3DDevice_DrawVerticesUP && !bound) || call->fast) { __atomic_fetch_add(&hud_capture_reject[0], 1, __ATOMIC_RELAXED); return NULL; }
    if (!front->certified) return NULL;
    if (front->put && *front->put != front->pusher) { __atomic_fetch_add(&hud_capture_reject[1], 1, __ATOMIC_RELAXED); return NULL; }
    ULONG primitive, count, stride, first = 0;
    const UCHAR *vertices;
    xa_arg(call, &primitive, sizeof(primitive), 0);
    if (bound) {
        xa_arg(call, &first, sizeof(first), 0); xa_arg(call, &count, sizeof(count), 0);
        if (!front->vb || (!front->queue_state && memcmp(front->vb, &front->vb_header, sizeof(front->vb_header))))
            { __atomic_fetch_add(&hud_capture_reject[5], 1, __ATOMIC_RELAXED); return NULL; }
        /* With queued state the stream is the one the queue set, read now. */
        vertices = front->queue_state ? resource_data((D3DResource *)front->vb) : front->vb_data;
        stride = front->vb_stride;
    } else {
        xa_arg(call, &count, sizeof(count), 0); xa_arg(call, &vertices, sizeof(vertices), 0); xa_arg(call, &stride, sizeof(stride), 0);
    }
    if (primitive != 5 /* TRIANGLELIST */ || !count || count % 3
        || stride != (front->state.fvf == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE) ? 20 : 28)
        || count > HUD_BATCH_BYTES / stride || !vertices) { __atomic_fetch_add(&hud_capture_reject[2], 1, __ATOMIC_RELAXED); return NULL; }
    fvf_plan_key state = front->state;
    GLuint ids[4]; memcpy(ids, front->ids, sizeof(ids));
    if (front->queue_state) {
        for (int i = 0; i < D3DRS_MAX; i++) state.rs[i] = front->rs[front->xlat[i]];
        memcpy(state.tss, front->tss, sizeof(state.tss));
        /* The state is the queue's, as of this call, not what the front saw:
           check it again the way fvf_plan_capture and hud_batch_capture_key
           would, and find the textures in the catalog the front took, since
           the cache itself belongs to the worker. */
        if (state.rs[D3DRS_FOGENABLE] || state.rs[118 /* VERTEXBLEND */] || state.rs[D3DRS_POINTSCALEENABLE]
            || state.rs[D3DRS_STENCILENABLE] || state.rs[D3DRS_SPECULARENABLE] || state.rs[D3DRS_POINTSPRITEENABLE]
            || front->pixel_shader || ff_combiner_needs_program(state.tss, 0) || (state.fvf & ~0x100u) != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE) || (state.fvf == 0x144 && !front->shader_ready)) return NULL;
        for (int s = 0; s < 4; s++)
            if (state.tss[s * 32 + 21] || state.tss[s * 32 + D3DTSS_TEXCOORDINDEX] >> 16) return NULL;
        memset(ids, 0, sizeof(ids));
        unsigned units = hud_active_units(&state);
        if (units && !front->combine_ready) return NULL;
        for (int s = 0; s < 4; s++) {
            if (!state.texture[s][0]) continue;
            const D3DPixelContainer *t = (const D3DPixelContainer *)state.texture[s][0];
            memcpy(&state.texture[s][1], t, sizeof(*t));
            if (!((units >> s) & 1)) continue;
            if (front->no_cache || state.rt_texture || ((t->Format >> 8) & 0xFF) == 0x0B || tex_target(t) != GL_TEXTURE_2D) return NULL;
            ULONG va = t->res.Data | CONTIG_BASE;
            if ((front->bb_data && front->bb_data == va)
                || (front->depth_data && front->depth_data == va)) return NULL;
            for (unsigned i = 0; front->catalog && i < front->catalog->count; i++) {
                const struct xbr_resident *r = &front->catalog->entries[i];
                if (r->data == t->res.Data && r->format == t->Format && r->size == t->Size) { ids[s] = r->id; break; }
            }
            if (!ids[s]) return NULL;
        }
    } else {
        for (int i = 0; i < D3DRS_MAX; i++)
            if (front->rs[front->xlat[i]] != state.rs[i]) { __atomic_fetch_add(&hud_capture_reject[3], 1, __ATOMIC_RELAXED); return NULL; }
        if (memcmp(front->tss, state.tss, sizeof(state.tss))) { __atomic_fetch_add(&hud_capture_reject[4], 1, __ATOMIC_RELAXED); return NULL; }
        for (int s = 0; s < 4; s++)
            if (state.texture[s][0] && memcmp((const void *)state.texture[s][0], &state.texture[s][1], sizeof(D3DPixelContainer))) { __atomic_fetch_add(&hud_capture_reject[5], 1, __ATOMIC_RELAXED); return NULL; }
    }
    /* Everything matches: copy the vertices now, the title may rewrite them
       as soon as the call returns. */
    size_t bytes = (size_t)count * stride;
    /* first comes from the guest: the vertex range must lie inside the
       address space before any pointer is formed from it (forming one
       past the end is already undefined), so check in integers, with
       first * stride in 64 bits. */
    uint64_t offset = (uint64_t)first * stride;
    if (offset > UINTPTR_MAX - (uintptr_t)vertices
        || bytes > UINTPTR_MAX - ((uintptr_t)vertices + offset)) { __atomic_fetch_add(&hud_capture_reject[2], 1, __ATOMIC_RELAXED); return NULL; }
    /* Only now is the pointer known to be inside the address space
       (whether it is mapped is the guest's business, as on the device). */
    vertices += offset;
    struct xbr_draw_packet *packet = calloc(1, sizeof(*packet) + bytes);
    if (!packet) { __atomic_fetch_add(&hud_capture_reject[6], 1, __ATOMIC_RELAXED); return NULL; }
    packet->state = state;
    memcpy(packet->ids, ids, sizeof(ids));
    packet->count = count; packet->stride = stride; packet->size = sizeof(*packet) + bytes;
    for (int i = 0; i < D3DRS_MAX; i++) packet->rs[front->xlat[i]] = state.rs[i];
    memcpy(packet->tss, state.tss, sizeof(packet->tss));
    for (int s = 0; s < 4; s++)
        memcpy(&packet->textures[s], &state.texture[s][1], sizeof(packet->textures[s]));
    memcpy(packet->vertices, vertices, bytes);
    return packet;
}
#endif

size_t d3d_draw_packet_size(const struct xbr_draw_packet *packet) { return packet->size; }
void d3d_draw_packet_free(struct xbr_draw_packet *packet) { free(packet); }

void d3d_draw_packet_run(struct xbr_draw_packet *packet)
{
    ULONG *rs = d3d.render_state, *tss = d3d.texture_state;
    D3DResource *textures[4];
    memcpy(textures, d3d.textures, sizeof(textures));
    d3d.render_state = packet->rs;
    d3d.texture_state = packet->tss;
    for (int s = 0; s < 4; s++) {
        hud_packet_texture_identity[s] = packet->state.texture[s][0];
        hud_packet_texture_id[s] = packet->ids[s];
        d3d.textures[s] = packet->state.texture[s][0] ? &packet->textures[s].res : NULL;
    }
    hud_packet_active = true;
    draw(5 /* TRIANGLELIST */, packet->vertices, packet->stride, 0, packet->count, NULL, false);
    hud_packet_active = false;
    d3d.render_state = rs; d3d.texture_state = tss;
    memcpy(d3d.textures, textures, sizeof(textures));
}

void d3d_backend_read_pixels(uint32_t *a)
{
    hud_batch_flush();
    GLenum prior = glGetError();
    if (prior) xlog("D3D: pending GL error %#x before renderer readback", prior);
    if (!d3d.rt_texture) {
        GLbitfield mask = a[4] == GL_DEPTH_COMPONENT ? GL_DEPTH_BUFFER_BIT
                        : a[4] == GL_STENCIL_INDEX ? GL_STENCIL_BUFFER_BIT
                        : a[4] == 0x84F9 /* DEPTH_STENCIL */ ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT
                        : GL_COLOR_BUFFER_BIT;
        backbuffer_read_begin(mask);
    }
    glReadPixels(a[0], a[1], a[2], a[3], a[4], a[5], (void *)(uintptr_t)a[6]);
    GLenum error = glGetError();
    if (error) xlog("D3D: renderer readback %ux%u format %#x type %#x failed: GL error %#x", a[2], a[3], a[4], a[5], error);

    if (!d3d.rt_texture) backbuffer_read_end();
}
void d3d_backend_stats(uint32_t *a)
{
    hud_batch_flush();
    a[0] = hud_original_draws; a[1] = hud_submitted_draws;
}
void d3d_backend_shutdown(void)
{
    __atomic_store_n(&g_vblank_hook, NULL, __ATOMIC_RELEASE);
    hud_batch_flush();
    free(hud_batch.vertices); hud_batch.vertices = NULL;
    if (hud_catalog && !--hud_catalog->refs) free(hud_catalog);
    hud_catalog = NULL;
    if (d3d.gl) SDL_GL_DeleteContext(d3d.gl);
    if (d3d.window) SDL_DestroyWindow(d3d.window);
    d3d.window = NULL; d3d.gl = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
