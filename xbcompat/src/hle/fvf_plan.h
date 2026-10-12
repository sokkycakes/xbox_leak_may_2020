#ifndef XBCOMPAT_FVF_PLAN_H
#define XBCOMPAT_FVF_PLAN_H
/* Prepared pre-transformed FVF draws: the state such a draw's GL setup
   depends on, as one flat key, so a draw whose key matches the last one
   can reuse the program, textures and samplers already in place. */
typedef struct {
    ULONG rs[D3DRS_MAX], tss[4 * 32], fvf;
    D3DVIEWPORT8 viewport;
    ULONG rt_height, rt_texture, target_zmax;
    ULONG scissors[34];      /* d3d.scissors: count, exclusive, rects[8] */
    ULONG texture[4][6];     /* per stage: identity, then the D3DPixelContainer */
} fvf_plan_key;
static struct {
    fvf_plan_key key;
    unsigned epoch, serial, units;   /* gc_epoch and conv_serial when prepared; units bound */
    bool valid;
} fvf_plan;

static bool fvf_plan_capture(fvf_plan_key *key, const fvf_layout *l)
{
    /* Only the plain case: fixed function, no fog, vertex blending, point
       scaling, stencil or texture transforms, and no CPU writes to the frame. */
    if (!l->pretransformed || d3d.pixel_shader || d3d.bb_cpu_dirty || RS(118 /* VERTEXBLEND */)
        || RS(D3DRS_FOGENABLE) || RS(D3DRS_POINTSCALEENABLE)
        || RS(D3DRS_STENCILENABLE)) return false;
    for (int s = 0; s < 4; s++)
        if (TSS(s, 21 /* TEXTURETRANSFORMFLAGS */) || TSS(s, D3DTSS_TEXCOORDINDEX) >> 16) return false;
    memset(key, 0, sizeof(*key));
    for (int i = 0; i < D3DRS_MAX; i++) key->rs[i] = RS(i);
    memcpy(key->tss, d3d.texture_state, sizeof(key->tss));
    key->fvf = d3d.vertex_shader;
    key->viewport = d3d.viewport;
    key->rt_height = d3d.rt_height; key->rt_texture = d3d.rt_texture;
    memcpy(&key->target_zmax, &d3d.target_zmax, sizeof(key->target_zmax));
    /* The scissor rectangles as the title set them, not as last applied. */
    memcpy(key->scissors, &d3d.scissors, sizeof(key->scissors));
    for (int s = 0; s < 4; s++) {
        key->texture[s][0] = hud_packet_active ? hud_packet_texture_identity[s] : (ULONG)d3d.textures[s];
        if (d3d.textures[s]) memcpy(&key->texture[s][1], d3d.textures[s], sizeof(D3DPixelContainer));
    }
    return true;
}

/* XBCOMPAT_PREPARED_SAMPLER_REUSE=0 applies every sampler again on a
   prepared draw, to rule the reuse in fvf_plan_resources out. */
static bool fvf_sampler_reuse_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("XBCOMPAT_PREPARED_SAMPLER_REUSE");
        enabled = !value || value[0] != '0';
    }
    return enabled;
}

static void fvf_plan_resources(unsigned units)
{
    for (int s = 0; s < 4; s++) {
        if (!((units >> s) & 1)) continue;
        p_glActiveTexture(GL_TEXTURE0 + s);
        unsigned sampler_epoch = gc_epoch;
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        if (t) {
            GLenum target = tex_target(t);
            tex_use_palette(s);
            glBindTexture(target, texture_for(t));
            /* The plan's key covers every sampler state, so the stage's
               sampler object already holds them as long as it is the one
               bound and no GL call got past the cache since the unit was
               selected: the texture was bound already and nothing was
               uploaded.  apply_sampler would then only repeat parameters
               its own cache drops, at a cost on every HUD draw.  Anything
               else, or separate samplers off, applies the sampler again. */
            if (!fvf_sampler_reuse_enabled() || !separate_samplers || bound_sampler[s] != stage_sampler[s]
                || sampler_epoch != gc_epoch)
                apply_sampler(target, s);
        } else {
            if (separate_samplers) bind_stage_sampler(s, 0);
            glBindTexture(GL_TEXTURE_2D, white_texture());
        }
    }
    p_glActiveTexture(GL_TEXTURE0);
}
#endif
