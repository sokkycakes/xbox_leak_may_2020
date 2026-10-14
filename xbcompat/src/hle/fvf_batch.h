/* HUD batching: runs of small pre-transformed, textured triangle lists that share a plan
   key go to GL as one glDrawArrays from a CPU vertex array, with the HUD shader. */
#include "fvf_plan.h"
#define HUD_BATCH_BYTES (256u * 1024)
typedef struct {
    fvf_plan_key state;
    uintptr_t target, depth;
    float screen_offset[2], backbuffer_scale[2];
    ULONG stride, units, program;
    GLuint textures[4];
} hud_batch_key;
static struct {
    hud_batch_key key;
    UCHAR *vertices;
    size_t bytes;
    ULONG count;
} hud_batch;
static unsigned hud_original_draws, hud_submitted_draws;
static unsigned hud_reject[5];   /* why hud_batch_capture_key said no, by reason (see hud_profile_log) */
static unsigned hud_front_reject[8], hud_capture_reject[7];   /* the same for the worker's fronts and packets */
static bool hud_query_active;    /* a visibility test is open: draws must not move */
/* The renderer worker's counters (src/renderer/worker.h); null when this
   build has no worker. */
void xbr_worker_stats(uint32_t *) __attribute__((weak));
void xbr_worker_profile(double) __attribute__((weak));

static bool hud_batch_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XBCOMPAT_HUD_BATCH");
        enabled = e && e[0] && e[0] != '0' && xbr_worker_stats && !getenv("XBCOMPAT_DUMP_DRAWS")
                  && !getenv("XBCOMPAT_PROFILE_SYNC");
    }
    return enabled;
}

/* The texture units the fixed-function stages use: those before the first
   disabled stage.  A batch binds only these, so only their textures belong
   in its key. */
static unsigned hud_active_units(const fvf_plan_key *state)
{
    unsigned units = 0;
    for (int s = 0; s < 4; s++) {
        if (state->tss[s * 32 + D3DTSS_COLOROP] == 1 /* DISABLE */) break;
        units |= 1u << s;
    }
    return units;
}

static bool hud_resident_textures(GLuint *ids, unsigned units)
{
    if (hud_packet_active) {
        memcpy(ids, hud_packet_texture_id, sizeof(hud_packet_texture_id));
        return true;
    }
    memset(ids, 0, 4 * sizeof(*ids));
    static int no_cache = -1;
    if (no_cache < 0) no_cache = getenv("XBCOMPAT_NO_TEXCACHE") != NULL;
    if (no_cache || d3d.rt_texture) return false;
    for (int s = 0; s < 4; s++) {
        if (!((units >> s) & 1)) continue;
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        if (!t) continue;
        ULONG fmt = (t->Format >> 8) & 0xFF;
        ULONG va = t->res.Data | CONTIG_BASE;
        if (fmt == 0x0B /* P8 */ || tex_target(t) != GL_TEXTURE_2D
            || (d3d.backbuffer && (d3d.backbuffer->Data | CONTIG_BASE) == va)
            || (d3d.depth && (d3d.depth->Data | CONTIG_BASE) == va)) return false;
        for (tex_entry *e = *tex_bucket(t->res.Data); e; e = e->next)
            if (e->data == t->res.Data && e->format == t->Format && e->size == t->Size
                && !e->pal) { ids[s] = e->id; break; }
        if (!ids[s]) return false;
    }
    return true;
}

static bool hud_batch_capture_key(hud_batch_key *key, const fvf_plan_key *state,
                                  ULONG primitive, ULONG stride, ULONG count,
                                  const USHORT *indices, bool eligible)
{
    if (!hud_batch_enabled()) return false;
    if (!eligible || hud_raw_active) { hud_reject[0]++; return false; }
    if (hud_query_active || primitive != 5 /* TRIANGLELIST */ || !count || count % 3 || indices
        || count > HUD_BATCH_BYTES / (stride ? stride : 1)) { hud_reject[1]++; return false; }
    if ((d3d.vertex_shader & ~0x100u /* TEX1 */) != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
        || stride != (d3d.vertex_shader == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE) ? 20 : 28)) { hud_reject[2]++; return false; }
    /* The HUD shader has neither a specular color nor point sprites. */
    if (RS(D3DRS_SPECULARENABLE) || RS(D3DRS_POINTSPRITEENABLE)) { hud_reject[3]++; return false; }
    memset(key, 0, sizeof(*key));
    key->state = *state;
    key->target = (uintptr_t)d3d.target; key->depth = (uintptr_t)d3d.target_depth;
    /* What the HUD shader maps XYZRHW positions to the window with. */
    memcpy(key->screen_offset, d3d.screen_offset, sizeof(key->screen_offset));
    memcpy(key->backbuffer_scale, d3d.backbuffer_scale, sizeof(key->backbuffer_scale));
    key->stride = stride;
    key->units = hud_active_units(state);
    if (hud_batch.count && !memcmp(&hud_batch.key.state, &key->state, sizeof(key->state)) && hud_batch.key.target == key->target
        && hud_batch.key.depth == key->depth
        && !memcmp(hud_batch.key.screen_offset, key->screen_offset, sizeof(key->screen_offset))
        && !memcmp(hud_batch.key.backbuffer_scale, key->backbuffer_scale, sizeof(key->backbuffer_scale)))
        { memcpy(key->textures, hud_batch.key.textures, sizeof(key->textures)); return true; }
    /* Otherwise the draw's textures must all be resident already: nothing
       may be uploaded between two draws of one batch. */
    if (!hud_resident_textures(key->textures, key->units)) { hud_reject[4]++; return false; }
    return true;
}

static void hud_batch_flush(void)
{
    /* A held run of programmable draws (d3d8.c) lands first: whatever made
       this call is about to change GL state or look at the frame. */
    merge_flush();
    /* Only the thread holding the context draws.  Anywhere else (the
       guest's thread while the worker owns GL) the batch stays queued for
       the worker's next flush; its barrier gets there before anything that
       must see the draws. */
    if (!d3d.gl || SDL_GL_GetCurrentContext() != d3d.gl) return;
    if (!hud_batch.count) return;
    /* The program, textures and render state are still the batch's: a draw
       that would change any of them flushes first. */
    ULONG stride = hud_batch.key.stride;
    gc_bind_buffer(GL_ARRAY_BUFFER, 0);
    gc_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    /* XYZRHW: the HUD shader maps x, y and z itself (screen_offset and
       backbuffer_scale), and doesn't use RHW. */
    glVertexPointer(3, GL_FLOAT, stride, hud_batch.vertices);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(rgba_vertex_colors ? 4 : GL_BGRA, GL_UNSIGNED_BYTE, stride,
                   hud_batch.vertices + 16);
    for (int u = 0; u < 4; u++) {
        ULONG tci = hud_batch.key.state.tss[u * 32 + D3DTSS_TEXCOORDINDEX] & 0xFFFF;
        p_glClientActiveTexture(GL_TEXTURE0 + u);
        if (((hud_batch.key.units >> u) & 1) && !tci && hud_batch.key.state.fvf == 0x144 /* XYZRHW | DIFFUSE | TEX1 */) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, stride, hud_batch.vertices + 20);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            p_glMultiTexCoord4fv(GL_TEXTURE0 + u, default_texcoord);
        }
    }
    p_glClientActiveTexture(GL_TEXTURE0);
    shader_diagnostics_mark(d3d.frame, ++shader_diag_draw, hud_batch.key.state.fvf, 0, hud_batch.key.program,
                            "fvf-batch");
    TIMED(t_gl, glDrawArrays(GL_TRIANGLES, 0, hud_batch.count));
    hud_submitted_draws++;

    hud_batch.bytes = 0; hud_batch.count = 0;
}

static bool hud_batch_append(const hud_batch_key *key, const UCHAR *base,
                             ULONG first, ULONG count)
{
    size_t bytes = (size_t)count * key->stride;
    if (count > HUD_BATCH_BYTES / key->stride
        || (uint64_t)first * key->stride > UINTPTR_MAX) return false;
    if (hud_batch.count && (memcmp(&hud_batch.key, key, sizeof(*key))
                            || HUD_BATCH_BYTES - hud_batch.bytes < bytes)) hud_batch_flush();
    if (!hud_batch.vertices) {
        hud_batch.vertices = malloc(HUD_BATCH_BYTES);
        if (!hud_batch.vertices) return false;
    }
    if (!hud_batch.count) hud_batch.key = *key;
    UCHAR *dst = hud_batch.vertices + hud_batch.bytes;
    memcpy(dst, base + (size_t)first * key->stride, bytes);
    if (rgba_vertex_colors)   /* drawn as RGBA (see rgba_colors): swap D3DCOLOR's blue and red */
        for (ULONG i = 0; i < count; i++) {
            UCHAR *c = dst + i * key->stride + 16;
            UCHAR b = c[0]; c[0] = c[2]; c[2] = b;
        }
    hud_batch.bytes += bytes;
    hud_batch.count += count;
    hud_original_draws++;
    return true;
}

/* The worker calls this before anything that must see every draw queued
   so far: a readback, a present, a call it can't order behind the batch. */
void d3d_renderer_barrier(void) { hud_batch_flush(); }

static void hud_profile_log(double frames)
{
    static unsigned original, submitted, async, sync;
    uint32_t transport[4] = { 0 };   /* xbr_worker_stats: async, sync, queue bytes, enabled */
    unsigned capture[7];
    for (unsigned i = 0; i < 7; i++)   /* counted on the guest's thread */
        capture[i] = __atomic_exchange_n(&hud_capture_reject[i], 0, __ATOMIC_RELAXED);
    if (xbr_worker_stats) xbr_worker_stats(transport);
    if (xbr_worker_profile) xbr_worker_profile(frames);
    xlog("D3D owned draws: batch source %.1f submits %.1f; worker async %.1f sync %.1f queue %u bytes enabled %u",
         (hud_original_draws - original) / frames, (hud_submitted_draws - submitted) / frames,
         (transport[0] - async) / frames, (transport[1] - sync) / frames, transport[2], transport[3]);
    xlog("D3D batch rejected: state %.1f primitive/query %.1f layout %.1f specular/sprite %.1f resources %.1f",
         hud_reject[0] / frames, hud_reject[1] / frames, hud_reject[2] / frames, hud_reject[3] / frames, hud_reject[4] / frames);
    if (transport[3]) {
        xlog("D3D worker front rejected: debug/query %.1f prepared %.1f program %.1f layout %.1f specular/sprite %.1f "
             "pusher %.1f key %.1f resources %.1f", hud_front_reject[0] / frames, hud_front_reject[1] / frames,
             hud_front_reject[2] / frames, hud_front_reject[3] / frames, hud_front_reject[4] / frames,
             hud_front_reject[5] / frames, hud_front_reject[6] / frames, hud_front_reject[7] / frames);
        xlog("D3D worker capture rejected: API %.1f pusher %.1f layout %.1f RS %.1f TSS %.1f header %.1f allocation %.1f",
             capture[0] / frames, capture[1] / frames, capture[2] / frames, capture[3] / frames, capture[4] / frames,
             capture[5] / frames, capture[6] / frames);
    }
    memset(hud_reject, 0, sizeof(hud_reject));
    memset(hud_front_reject, 0, sizeof(hud_front_reject));
    original = hud_original_draws; submitted = hud_submitted_draws;
    async = transport[0]; sync = transport[1];
}
