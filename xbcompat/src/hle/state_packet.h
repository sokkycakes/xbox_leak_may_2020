/*
 * State packets for the renderer worker: the D3D state calls a HUD draw
 * depends on, captured on the guest's thread so they can be queued
 * instead of waiting for the worker.  A state front starts from d3d's
 * state as the worker last left it; each packet is committed to the front
 * as it is queued (so a later draw capture sees it) and run on the worker
 * in order.  Included by d3d8.c, after hud_packet.h.
 */
enum { XBR_STATE_VIEWPORT, XBR_STATE_TEXTURE, XBR_STATE_RS, XBR_STATE_TSS, XBR_STATE_STREAM, XBR_STATE_VERTEX, XBR_STATE_TRANSFORM, XBR_STATE_CONSTANT, XBR_STATE_CATEGORIES };

struct xbr_state_packet {
    unsigned kind;
    ULONG a[3];
    union { D3DVIEWPORT8 viewport; D3DMATRIX transform; } value;
    float constants[];           /* XBR_STATE_CONSTANT: a[1] registers of four */
};

struct xbr_draw_front *d3d_state_front_create(void *impl)
{
    if (!d3d.window || d3d.recording || hud_query_active || hud_raw_active || g_trace
        || getenv("XBCOMPAT_DUMP_DRAWS") || getenv("XBCOMPAT_PROFILE_SYNC")) return NULL;
    if (d3d.device && d3d.device[0] && d3d.device[0] != (ULONG)pusher_buf) return NULL;
    struct xbr_draw_front *f = d3d_draw_front_create(impl);
    if (!f) f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->queue_state = true;
    f->rs = d3d.render_state; f->tss = d3d.texture_state;
    f->rs_count = d3d.rs_count;
    f->width = d3d.width; f->height = d3d.height;
    f->put = d3d.device; f->pusher = (ULONG)pusher_buf;
    f->pixel_shader = d3d.pixel_shader;
    f->state.fvf = d3d.vertex_shader;
    f->state.viewport = d3d.viewport;
    memcpy(f->state.scissors, &d3d.scissors, sizeof(f->state.scissors));
    /* Only the identities: a draw capture copies the headers when it runs,
       after the state packets queued before it. */
    for (int s = 0; s < 4; s++) f->state.texture[s][0] = (ULONG)d3d.textures[s];
    f->bb_data = d3d.backbuffer ? d3d.backbuffer->Data | CONTIG_BASE : 0;
    f->depth_data = d3d.depth ? d3d.depth->Data | CONTIG_BASE : 0;
    memcpy(f->xlat, rs_xlat, sizeof(f->xlat));
    static int no_cache = -1, shader_enabled = -1;
    if (no_cache < 0) no_cache = getenv("XBCOMPAT_NO_TEXCACHE") != NULL;
    if (shader_enabled < 0) {
        const char *e = getenv("XBCOMPAT_HUD_SHADER");
        shader_enabled = e && e[0] && e[0] != '0';
    }
    f->no_cache = no_cache;
    f->combine_ready = texture_combine_initialized;
    /* The HUD shader is built on the worker; until it is, textured draws
       must take the ordinary path. */
    f->shader_ready = !shader_enabled || (hud_program && hud_program->prog);
    if (!hud_catalog || hud_catalog->version != tex_cache_version) {
        unsigned n = 0;
        for (unsigned b = 0; b < TEX_BUCKETS; b++)
            for (tex_entry *e = tex_buckets[b]; e && n < 512; e = e->next)
                if (e->target == GL_TEXTURE_2D && !e->pal) n++;
        struct xbr_catalog *c = malloc(sizeof(*c) + n * sizeof(c->entries[0]));
        if (c) {
            c->refs = 1; c->version = tex_cache_version; c->count = 0;
            for (unsigned b = 0; b < TEX_BUCKETS; b++)
                for (tex_entry *e = tex_buckets[b]; e && c->count < n; e = e->next)
                    if (e->target == GL_TEXTURE_2D && !e->pal)
                        c->entries[c->count++] = (struct xbr_resident){ e->data, e->format, e->size, e->id };
            if (hud_catalog && !--hud_catalog->refs) free(hud_catalog);
            hud_catalog = c;
        }
    }
    if (hud_catalog && hud_catalog->version == tex_cache_version) {
        hud_catalog->refs++; f->catalog = hud_catalog;
    }
    return f;
}

#ifdef XBC_TRANSLATED /* guest call frames exist only in the translated renderer */
struct xbr_state_packet *d3d_state_packet_capture(struct xbr_draw_front *f, void *impl,
                                                  struct xa_frame *call)
{
    bool constant = impl == (void *)SetVertexShaderConstantNotInline
                    || impl == (void *)SetVertexShaderConstant1 || impl == (void *)D3DDevice_SetVertexShaderConstant
                    || impl == (void *)SetVertexShaderConstant4;
    if (!f->queue_state || (!constant && call->fast) || (f->put && *f->put != f->pusher)) return NULL;
    struct xbr_state_packet p = { 0 };
    const float *data = NULL;
    size_t extra = 0;
    if (constant) {
        LONG reg;
        ULONG count;
        xa_arg(call, &reg, sizeof(reg), 0); xa_arg(call, &data, sizeof(data), 0);
        count = impl == (void *)SetVertexShaderConstant1 ? 1
              : impl == (void *)SetVertexShaderConstant4 ? 4 : 0;
        if (!count) xa_arg(call, &count, sizeof(count), 0);
        if (impl != (void *)D3DDevice_SetVertexShaderConstant) {
            int64_t translated = (int64_t)reg - 96;   /* the inline forms take the register biased by 96 */
            if (translated < INT32_MIN || translated > INT32_MAX) return NULL;
            reg = (LONG)translated;
            if (impl == (void *)SetVertexShaderConstantNotInline) count /= 4;   /* dwords */
        }
        /* The guest's count and pointer: bound them before sizing the
           packet, the way the call itself would. */
        if (count > 192 || (count && (!data || (int64_t)reg + count > INT32_MAX - 95))) return NULL;
        p.kind = XBR_STATE_CONSTANT; p.a[0] = reg; p.a[1] = count;
        extra = count * sizeof(float[4]);
        if (extra && (uintptr_t)data > UINTPTR_MAX - extra) return NULL;
    } else if (impl == (void *)D3DDevice_SetViewport) {
        const D3DVIEWPORT8 *v;
        xa_arg(call, &v, sizeof(v), 0);
        p.kind = XBR_STATE_VIEWPORT;
        p.value.viewport = v ? *v : (D3DVIEWPORT8){ 0, 0, f->width, f->height, 0, 1 };
    } else if (impl == (void *)D3DDevice_SetTexture) {
        p.kind = XBR_STATE_TEXTURE;
        xa_arg(call, &p.a[0], sizeof(p.a[0]), 0); xa_arg(call, &p.a[1], sizeof(p.a[1]), 0);
        /* Only the pointer: the header is read when a draw capture or the
           worker uses it, as the call itself would. */
    } else if (impl == (void *)SetRenderStateNotInline) {
        p.kind = XBR_STATE_RS;
        xa_arg(call, &p.a[0], sizeof(p.a[0]), 0); xa_arg(call, &p.a[1], sizeof(p.a[1]), 0);
    } else if (impl == (void *)D3DDevice_SetTextureStageStateNotInline) {
        p.kind = XBR_STATE_TSS;
        for (int i = 0; i < 3; i++) xa_arg(call, &p.a[i], sizeof(p.a[i]), 0);
    } else if (impl == (void *)D3DDevice_SetStreamSource) {
        p.kind = XBR_STATE_STREAM;
        for (int i = 0; i < 3; i++) xa_arg(call, &p.a[i], sizeof(p.a[i]), 0);
    } else if (impl == (void *)D3DDevice_SetVertexShader) {
        p.kind = XBR_STATE_VERTEX;
        xa_arg(call, &p.a[0], sizeof(p.a[0]), 0);
    } else if (impl == (void *)D3DDevice_SetTransform) {
        const D3DMATRIX *m;
        p.kind = XBR_STATE_TRANSFORM;
        xa_arg(call, &p.a[0], sizeof(p.a[0]), 0); xa_arg(call, &m, sizeof(m), 0);
        if (p.a[0] < 10) p.value.transform = *m;
    } else return NULL;
    struct xbr_state_packet *out = malloc(sizeof(p) + extra);
    if (!out) return NULL;
    memcpy(out, &p, sizeof(p));
    if (extra) memcpy(out->constants, data, extra);

    return out;
}
#endif

void d3d_state_packet_commit(struct xbr_draw_front *f, const struct xbr_state_packet *p)
{
    switch (p->kind) {
    case XBR_STATE_VIEWPORT:
        f->state.viewport = p->value.viewport;
        f->state.scissors[0] = 0;   /* SetViewport drops the scissor rectangles */
        break;
    case XBR_STATE_TEXTURE:
        if (p->a[0] < 4) f->state.texture[p->a[0]][0] = p->a[1];
        break;
    case XBR_STATE_RS:
        /* Straight into the arrays the worker draws from: it runs nothing but this queue. */
        if (p->a[0] < f->rs_count) f->rs[p->a[0]] = p->a[1];
        break;
    case XBR_STATE_TSS:
        if (p->a[0] < 4 && p->a[1] < 32) f->tss[p->a[0] * 32 + p->a[1]] = p->a[2];
        break;
    case XBR_STATE_STREAM:
        if (p->a[0] == 0) {
            f->vb = (const D3DResource *)p->a[1]; f->vb_stride = p->a[2];
        }
        break;
    case XBR_STATE_VERTEX:
        f->state.fvf = p->a[0];
    }
}
void d3d_state_packet_run(const struct xbr_state_packet *p)
{
    switch (p->kind) {
    case XBR_STATE_VIEWPORT: D3DDevice_SetViewport(&p->value.viewport); break;
    case XBR_STATE_TEXTURE: D3DDevice_SetTexture(p->a[0], (D3DResource *)p->a[1]); break;
    case XBR_STATE_STREAM: D3DDevice_SetStreamSource(p->a[0], (D3DResource *)p->a[1], p->a[2]); break;
    case XBR_STATE_VERTEX: D3DDevice_SetVertexShader(p->a[0]); break;
    case XBR_STATE_TRANSFORM:
        if (p->a[0] < 10) D3DDevice_SetTransform(p->a[0], &p->value.transform);
        break;
    case XBR_STATE_CONSTANT:
        D3DDevice_SetVertexShaderConstant((LONG)p->a[0], p->constants, p->a[1]);
        break;
    case XBR_STATE_RS:
    case XBR_STATE_TSS:
        /* Already in the arrays: d3d_state_packet_commit wrote them. */
        break;
    }
}
void d3d_state_packet_free(struct xbr_state_packet *p) { free(p); }
size_t d3d_state_packet_size(const struct xbr_state_packet *p)
{
    return sizeof(*p) + (p->kind == XBR_STATE_CONSTANT ? p->a[1] * sizeof(float[4]) : 0);
}
unsigned d3d_state_packet_category(const struct xbr_state_packet *p) { return p->kind; }
