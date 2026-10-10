/*
 * A shadow of the GL state d3d8.c sets on every draw, so a call that would
 * set what is already set is skipped.  D3D state maps onto a few hundred GL
 * calls a draw, nearly all redundant; under box86 on a Raspberry Pi each one
 * crosses from x86 into the native libGL and Mesa, and that crossing, not the
 * GPU, was most of the frame.
 *
 * Included by d3d8.c only; the macros at the end route its calls through the
 * cache.  The cache only ever holds what it set itself, so anything that
 * changes GL state behind it (glPopAttrib, deleting a bound texture) calls
 * gc_reset() or the matching invalidation.
 */
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

/* The entry points it wraps that d3d8.c fetches at run time (declared again,
   with these types, where they are loaded). */
static void (APIENTRY *p_glActiveTexture)(GLenum);
static void (APIENTRY *p_glClientActiveTexture)(GLenum);
static void (APIENTRY *p_glUseProgram)(GLuint);
static void (APIENTRY *p_glEnableVertexAttribArray)(GLuint);
static void (APIENTRY *p_glDisableVertexAttribArray)(GLuint);
static void (APIENTRY *p_glVertexAttrib4fv)(GLuint, const GLfloat *);
static void (APIENTRY *p_glMultiTexCoord4fv)(GLenum, const GLfloat *);
static void (APIENTRY *p_glPointParameterfv)(GLenum, const GLfloat *);
static void (APIENTRY *p_glPointParameterf)(GLenum, GLfloat);

#define GC_UNITS 4
#define GC_UNKNOWN 0xFFFFFFFFu

/* Capabilities glEnable/glDisable switch: 0 unknown, 1 off, 2 on. */
enum {
    CAP_DEPTH_TEST, CAP_POLYGON_OFFSET_FILL, CAP_BLEND, CAP_ALPHA_TEST, CAP_POINT_SPRITE, CAP_CULL_FACE,
    CAP_LIGHTING, CAP_NORMALIZE, CAP_COLOR_MATERIAL, CAP_SCISSOR_TEST, CAP_STENCIL_TEST, CAP_FOG,
    CAP_COLOR_SUM, CAP_COLOR_LOGIC_OP, CAP_LIGHT0, CAP_CLIP_PLANE0 = CAP_LIGHT0 + 8, CAP_GLOBAL = CAP_CLIP_PLANE0 + 6
};
/* Per texture unit. */
enum { UCAP_2D, UCAP_CUBE, UCAP_3D, UCAP_GEN_S, UCAP_GEN_T, UCAP_GEN_R, UCAP_GEN_Q, UCAP_N };
/* Client arrays. */
enum { CA_VERTEX, CA_NORMAL, CA_COLOR, CA_SECONDARY, CA_GLOBAL };

/* glTexEnv parameters, per unit. */
enum {
    TE_MODE, TE_COMBINE_RGB, TE_COMBINE_ALPHA, TE_SRC_RGB0, TE_SRC_ALPHA0 = TE_SRC_RGB0 + 3,
    TE_OP_RGB0 = TE_SRC_ALPHA0 + 3, TE_OP_ALPHA0 = TE_OP_RGB0 + 3, TE_RGB_SCALE = TE_OP_ALPHA0 + 3,
    TE_ALPHA_SCALE, TE_COORD_REPLACE, TE_COLOR, TE_N = TE_COLOR + 4
};

/* glTexParameter values, per texture object. */
enum { TP_WRAP_S, TP_WRAP_T, TP_WRAP_R, TP_MAG, TP_MIN, TP_COMPARE_MODE, TP_COMPARE_FUNC, TP_BORDER, TP_N = TP_BORDER + 4 };

typedef struct { uint32_t v[TP_N]; } gc_texparams;

static struct {
    uint8_t cap[CAP_GLOBAL], ucap[GC_UNITS][UCAP_N], client[CA_GLOBAL], client_tc[GC_UNITS];
    uint32_t active, client_active, program;
    uint32_t bound[GC_UNITS][3];                  /* 2D, cube, 3D */
    uint32_t texenv[GC_UNITS][TE_N];
    uint32_t texgen[GC_UNITS][4];
    uint32_t texcoord[GC_UNITS][4];
    uint32_t depth_func, depth_mask, blend[2], alpha[2], shade, polygon_mode, cull, front;
    uint32_t offset[2], point_size, point_att[3], point_min, point_max;
    uint32_t viewport[4], scissor[4], color_mask[4];
    double depth_range[2];
    uint32_t matrix_mode;
    uint32_t matrix[2 + GC_UNITS][16];            /* modelview, projection, texture 0..3 */
    uint8_t matrix_known[2 + GC_UNITS];
    uint32_t fog[7];                              /* color, start, end, density */
    uint32_t attrib_on;                           /* generic arrays known on */
    uint32_t attrib_off;                          /* generic arrays known off */
    uint32_t attrib_val[16][4];
    uint16_t attrib_val_known;
} gc;

static gc_texparams *gc_tex;    /* indexed by texture name */
static uint32_t gc_ntex;
static unsigned gc_calls, gc_skipped;   /* this frame, for the fps log */

static void gc_reset(void)
{
    memset(&gc, 0, sizeof gc);
    /* 0 means unknown for the capability bytes; everything else uses GC_UNKNOWN. */
    uint8_t *caps_end = (uint8_t *)&gc.active;
    memset(caps_end, 0xFF, (uint8_t *)(&gc + 1) - caps_end);
    memset(gc.matrix_known, 0, sizeof gc.matrix_known);
    gc.attrib_on = gc.attrib_off = 0;
    gc.attrib_val_known = 0;
    if (gc_tex) memset(gc_tex, 0xFF, gc_ntex * sizeof *gc_tex);
}

static inline bool gc_same(const uint32_t *a, const void *b, int n)
{
    const uint32_t *p = b;
    for (int i = 0; i < n; i++)
        if (a[i] != p[i]) return false;
    return true;
}

/* Copy n words of b into the cache; true when they differed (the call is due). */
static inline bool gc_set(uint32_t *a, const void *b, int n)
{
    if (gc_same(a, b, n)) { gc_skipped++; return false; }
    memcpy(a, b, n * 4);
    gc_calls++;
    return true;
}

static inline bool gc_set1(uint32_t *a, uint32_t v) { return gc_set(a, &v, 1); }

static inline uint32_t gc_fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static unsigned gc_unit(void) { return gc.active < GC_UNITS ? gc.active : GC_UNITS; }

static int gc_cap_index(GLenum cap, bool *per_unit)
{
    *per_unit = false;
    switch (cap) {
    case GL_DEPTH_TEST: return CAP_DEPTH_TEST;
    case GL_POLYGON_OFFSET_FILL: return CAP_POLYGON_OFFSET_FILL;
    case GL_BLEND: return CAP_BLEND;
    case GL_ALPHA_TEST: return CAP_ALPHA_TEST;
    case 0x8861 /* GL_POINT_SPRITE */: return CAP_POINT_SPRITE;
    case GL_CULL_FACE: return CAP_CULL_FACE;
    case GL_LIGHTING: return CAP_LIGHTING;
    case GL_NORMALIZE: return CAP_NORMALIZE;
    case GL_COLOR_MATERIAL: return CAP_COLOR_MATERIAL;
    case GL_SCISSOR_TEST: return CAP_SCISSOR_TEST;
    case GL_STENCIL_TEST: return CAP_STENCIL_TEST;
    case GL_FOG: return CAP_FOG;
    case 0x8458 /* GL_COLOR_SUM */: return CAP_COLOR_SUM;
    case GL_COLOR_LOGIC_OP: return CAP_COLOR_LOGIC_OP;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) return CAP_LIGHT0 + (cap - GL_LIGHT0);
    if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + 6) return CAP_CLIP_PLANE0 + (cap - GL_CLIP_PLANE0);
    *per_unit = true;
    switch (cap) {
    case GL_TEXTURE_2D: return UCAP_2D;
    case 0x8513 /* GL_TEXTURE_CUBE_MAP */: return UCAP_CUBE;
    case 0x806F /* GL_TEXTURE_3D */: return UCAP_3D;
    case GL_TEXTURE_GEN_S: return UCAP_GEN_S;
    case GL_TEXTURE_GEN_T: return UCAP_GEN_T;
    case GL_TEXTURE_GEN_R: return UCAP_GEN_R;
    case GL_TEXTURE_GEN_Q: return UCAP_GEN_Q;
    }
    return -1;
}

static uint8_t *gc_cap_slot(GLenum cap)
{
    bool per_unit;
    int i = gc_cap_index(cap, &per_unit);
    if (i < 0) return NULL;
    if (!per_unit) return &gc.cap[i];
    unsigned u = gc_unit();
    return u < GC_UNITS ? &gc.ucap[u][i] : NULL;
}

static void gc_enable(GLenum cap, bool on)
{
    uint8_t *s = gc_cap_slot(cap);
    if (s && *s == (on ? 2 : 1)) { gc_skipped++; return; }
    if (s) *s = on ? 2 : 1;
    gc_calls++;
    if (on) (glEnable)(cap); else (glDisable)(cap);
}

static GLboolean gc_is_enabled(GLenum cap)
{
    uint8_t *s = gc_cap_slot(cap);
    if (s && *s) return *s == 2;
    return (glIsEnabled)(cap);
}

static int gc_client_index(GLenum a)
{
    switch (a) {
    case GL_VERTEX_ARRAY: return CA_VERTEX;
    case GL_NORMAL_ARRAY: return CA_NORMAL;
    case GL_COLOR_ARRAY: return CA_COLOR;
    case 0x845E /* GL_SECONDARY_COLOR_ARRAY */: return CA_SECONDARY;
    }
    return -1;
}

static void gc_client(GLenum a, bool on)
{
    uint8_t *s = NULL;
    if (a == GL_TEXTURE_COORD_ARRAY) {
        unsigned u = gc.client_active;
        if (u < GC_UNITS) {
            s = &gc.client_tc[u];
            /* A draw from the array leaves the current coordinate undefined. */
            if (on) gc.texcoord[u][0] = GC_UNKNOWN, gc.texcoord[u][3] = GC_UNKNOWN;
        }
    } else {
        int i = gc_client_index(a);
        if (i >= 0) s = &gc.client[i];
    }
    if (s && *s == (on ? 2 : 1)) { gc_skipped++; return; }
    if (s) *s = on ? 2 : 1;
    gc_calls++;
    if (on) (glEnableClientState)(a); else (glDisableClientState)(a);
}

static void gc_active_texture(GLenum unit)
{
    if (gc_set1(&gc.active, unit - GL_TEXTURE0)) p_glActiveTexture(unit);
}

static void gc_client_active_texture(GLenum unit)
{
    if (gc_set1(&gc.client_active, unit - GL_TEXTURE0)) p_glClientActiveTexture(unit);
}

static int gc_target_index(GLenum target)
{
    return target == GL_TEXTURE_2D ? 0 : target == 0x8513 ? 1 : target == 0x806F ? 2 : -1;
}

static void gc_bind_texture(GLenum target, GLuint id)
{
    unsigned u = gc_unit();
    int t = gc_target_index(target);
    if (u < GC_UNITS && t >= 0) {
        if (!gc_set1(&gc.bound[u][t], id)) return;
    } else {
        gc_calls++;
    }
    (glBindTexture)(target, id);
}

/* The parameters of the texture bound to `target`, or NULL if unknown. */
static gc_texparams *gc_bound_params(GLenum target)
{
    unsigned u = gc_unit();
    int t = gc_target_index(target);
    if (u >= GC_UNITS || t < 0) return NULL;
    uint32_t id = gc.bound[u][t];
    if (id == GC_UNKNOWN || id == 0) return NULL;
    if (id >= gc_ntex) {
        uint32_t n = id + 256;
        gc_texparams *p = realloc(gc_tex, n * sizeof *p);
        if (!p) return NULL;
        memset(p + gc_ntex, 0xFF, (n - gc_ntex) * sizeof *p);
        gc_tex = p;
        gc_ntex = n;
    }
    return &gc_tex[id];
}

static int gc_texparam_index(GLenum pname)
{
    switch (pname) {
    case GL_TEXTURE_WRAP_S: return TP_WRAP_S;
    case GL_TEXTURE_WRAP_T: return TP_WRAP_T;
    case 0x8072 /* GL_TEXTURE_WRAP_R */: return TP_WRAP_R;
    case GL_TEXTURE_MAG_FILTER: return TP_MAG;
    case GL_TEXTURE_MIN_FILTER: return TP_MIN;
    case 0x884C /* GL_TEXTURE_COMPARE_MODE */: return TP_COMPARE_MODE;
    case 0x884D /* GL_TEXTURE_COMPARE_FUNC */: return TP_COMPARE_FUNC;
    }
    return -1;
}

static void gc_tex_parameteri(GLenum target, GLenum pname, GLint v)
{
    gc_texparams *p = gc_bound_params(target);
    int i = gc_texparam_index(pname);
    if (p && i >= 0) {
        if (!gc_set1(&p->v[i], (uint32_t)v)) return;
    } else {
        gc_calls++;
    }
    (glTexParameteri)(target, pname, v);
}

static void gc_tex_parameterfv(GLenum target, GLenum pname, const GLfloat *v)
{
    gc_texparams *p = gc_bound_params(target);
    if (p && pname == GL_TEXTURE_BORDER_COLOR) {
        if (!gc_set(&p->v[TP_BORDER], v, 4)) return;
    } else {
        gc_calls++;
    }
    (glTexParameterfv)(target, pname, v);
}

/* New or deleted names start with default parameters and are unbound. */
static void gc_forget_textures(GLsizei n, const GLuint *ids)
{
    for (GLsizei i = 0; i < n; i++) {
        if (ids[i] < gc_ntex) memset(&gc_tex[ids[i]], 0xFF, sizeof *gc_tex);
        for (int u = 0; u < GC_UNITS; u++)
            for (int t = 0; t < 3; t++)
                if (gc.bound[u][t] == ids[i]) gc.bound[u][t] = GC_UNKNOWN;
    }
}

static void gc_gen_textures(GLsizei n, GLuint *ids)
{
    (glGenTextures)(n, ids);
    gc_forget_textures(n, ids);
}

static void gc_delete_textures(GLsizei n, const GLuint *ids)
{
    (glDeleteTextures)(n, ids);
    gc_forget_textures(n, ids);
}

static int gc_texenv_index(GLenum target, GLenum pname)
{
    if (target == 0x8861 /* GL_POINT_SPRITE */) return pname == 0x8862 /* GL_COORD_REPLACE */ ? TE_COORD_REPLACE : -1;
    if (target != GL_TEXTURE_ENV) return -1;
    switch (pname) {
    case GL_TEXTURE_ENV_MODE: return TE_MODE;
    case 0x8571 /* GL_COMBINE_RGB */: return TE_COMBINE_RGB;
    case 0x8572 /* GL_COMBINE_ALPHA */: return TE_COMBINE_ALPHA;
    case 0x8573 /* GL_RGB_SCALE */: return TE_RGB_SCALE;
    case GL_ALPHA_SCALE: return TE_ALPHA_SCALE;
    }
    if (pname >= 0x8580 && pname <= 0x8582) return TE_SRC_RGB0 + (pname - 0x8580);     /* GL_SOURCE0..2_RGB */
    if (pname >= 0x8588 && pname <= 0x858A) return TE_SRC_ALPHA0 + (pname - 0x8588);   /* GL_SOURCE0..2_ALPHA */
    if (pname >= 0x8590 && pname <= 0x8592) return TE_OP_RGB0 + (pname - 0x8590);      /* GL_OPERAND0..2_RGB */
    if (pname >= 0x8598 && pname <= 0x859A) return TE_OP_ALPHA0 + (pname - 0x8598);    /* GL_OPERAND0..2_ALPHA */
    return -1;
}

static uint32_t *gc_texenv_slot(GLenum target, GLenum pname)
{
    unsigned u = gc_unit();
    int i = gc_texenv_index(target, pname);
    return u < GC_UNITS && i >= 0 ? &gc.texenv[u][i] : NULL;
}

/* glTexEnvi and glTexEnvf share a slot: the value is stored with its kind. */
static void gc_texenvi(GLenum target, GLenum pname, GLint v)
{
    uint32_t *s = gc_texenv_slot(target, pname);
    if (s) { if (!gc_set1(s, (uint32_t)v & 0x7FFFFFFFu)) return; }
    else gc_calls++;
    (glTexEnvi)(target, pname, v);
}

static void gc_texenvf(GLenum target, GLenum pname, GLfloat v)
{
    uint32_t *s = gc_texenv_slot(target, pname);
    if (s) { if (!gc_set1(s, gc_fbits(v) | 0x80000000u)) return; }
    else gc_calls++;
    (glTexEnvf)(target, pname, v);
}

static void gc_texenvfv(GLenum target, GLenum pname, const GLfloat *v)
{
    unsigned u = gc_unit();
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_COLOR && u < GC_UNITS) {
        if (!gc_set(&gc.texenv[u][TE_COLOR], v, 4)) return;
    } else {
        gc_calls++;
    }
    (glTexEnvfv)(target, pname, v);
}

static void gc_texgeni(GLenum coord, GLenum pname, GLint v)
{
    unsigned u = gc_unit(), c = coord - GL_S;
    if (u < GC_UNITS && c < 4 && pname == GL_TEXTURE_GEN_MODE) {
        if (!gc_set1(&gc.texgen[u][c], (uint32_t)v)) return;
    } else {
        gc_calls++;
    }
    (glTexGeni)(coord, pname, v);
}

static void gc_multitexcoord4fv(GLenum unit, const GLfloat *v)
{
    unsigned u = unit - GL_TEXTURE0;
    if (u < GC_UNITS) { if (!gc_set(gc.texcoord[u], v, 4)) return; }
    else gc_calls++;
    p_glMultiTexCoord4fv(unit, v);
}

static void gc_use_program(GLuint p)
{
    if (gc_set1(&gc.program, p)) p_glUseProgram(p);
}

static void gc_attrib_array(GLuint r, bool on)
{
    uint32_t bit = 1u << r;
    if (r < 16 && ((on ? gc.attrib_on : gc.attrib_off) & bit)) { gc_skipped++; return; }
    gc_calls++;
    if (r < 16) {
        if (on) { gc.attrib_on |= bit; gc.attrib_off &= ~bit; gc.attrib_val_known &= ~bit; }
        else { gc.attrib_off |= bit; gc.attrib_on &= ~bit; }
    }
    if (on) p_glEnableVertexAttribArray(r); else p_glDisableVertexAttribArray(r);
}

static void gc_vertex_attrib4fv(GLuint r, const GLfloat *v)
{
    if (r < 16) {
        uint16_t bit = 1u << r;
        if ((gc.attrib_val_known & bit) && gc_same(gc.attrib_val[r], v, 4)) { gc_skipped++; return; }
        memcpy(gc.attrib_val[r], v, 16);
        /* Only an attribute with its array off keeps the value through a draw. */
        if (gc.attrib_off & bit) gc.attrib_val_known |= bit;
    }
    gc_calls++;
    p_glVertexAttrib4fv(r, v);
}

/* Turn every generic array off (fixed-function draws read the conventional ones). */
static void gc_disable_attrib_arrays(void)
{
    for (int r = 0; r < 16; r++) gc_attrib_array(r, false);
}

static void gc_depth_func(GLenum f) { if (gc_set1(&gc.depth_func, f)) (glDepthFunc)(f); }
static void gc_depth_mask(GLboolean m) { if (gc_set1(&gc.depth_mask, m)) (glDepthMask)(m); }
static void gc_shade_model(GLenum m) { if (gc_set1(&gc.shade, m)) (glShadeModel)(m); }
static void gc_cull_face(GLenum m) { if (gc_set1(&gc.cull, m)) (glCullFace)(m); }
static void gc_front_face(GLenum m) { if (gc_set1(&gc.front, m)) (glFrontFace)(m); }
static void gc_point_size(GLfloat s) { if (gc_set1(&gc.point_size, gc_fbits(s))) (glPointSize)(s); }

static void gc_blend_func(GLenum s, GLenum d)
{
    uint32_t v[2] = { s, d };
    if (gc_set(gc.blend, v, 2)) (glBlendFunc)(s, d);
}

static void gc_alpha_func(GLenum f, GLclampf ref)
{
    uint32_t v[2] = { f, gc_fbits(ref) };
    if (gc_set(gc.alpha, v, 2)) (glAlphaFunc)(f, ref);
}

static void gc_polygon_offset(GLfloat a, GLfloat b)
{
    uint32_t v[2] = { gc_fbits(a), gc_fbits(b) };
    if (gc_set(gc.offset, v, 2)) (glPolygonOffset)(a, b);
}

static void gc_polygon_mode(GLenum face, GLenum mode)
{
    if (face == GL_FRONT_AND_BACK) { if (gc_set1(&gc.polygon_mode, mode)) (glPolygonMode)(face, mode); return; }
    gc.polygon_mode = GC_UNKNOWN;
    gc_calls++;
    (glPolygonMode)(face, mode);
}

static void gc_point_parameterfv(GLenum p, const GLfloat *v)
{
    if (p == 0x8129 /* GL_POINT_DISTANCE_ATTENUATION */) { if (gc_set(gc.point_att, v, 3)) p_glPointParameterfv(p, v); return; }
    gc_calls++;
    p_glPointParameterfv(p, v);
}

static void gc_point_parameterf(GLenum p, GLfloat v)
{
    uint32_t *s = p == 0x8126 /* GL_POINT_SIZE_MIN */ ? &gc.point_min : p == 0x8127 /* MAX */ ? &gc.point_max : NULL;
    if (s) { if (gc_set1(s, gc_fbits(v))) p_glPointParameterf(p, v); return; }
    gc_calls++;
    p_glPointParameterf(p, v);
}

static void gc_viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    uint32_t v[4] = { x, y, w, h };
    if (gc_set(gc.viewport, v, 4)) (glViewport)(x, y, w, h);
}

static void gc_scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    uint32_t v[4] = { x, y, w, h };
    if (gc_set(gc.scissor, v, 4)) (glScissor)(x, y, w, h);
}

static void gc_color_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    uint32_t v[4] = { r, g, b, a };
    if (gc_set(gc.color_mask, v, 4)) (glColorMask)(r, g, b, a);
}

static void gc_depth_range(GLclampd n, GLclampd f)
{
    if (gc.depth_range[0] == n && gc.depth_range[1] == f) { gc_skipped++; return; }
    gc.depth_range[0] = n;
    gc.depth_range[1] = f;
    gc_calls++;
    (glDepthRange)(n, f);
}

static void gc_fogfv(GLenum p, const GLfloat *v)
{
    if (p == GL_FOG_COLOR) { if (gc_set(gc.fog, v, 4)) (glFogfv)(p, v); return; }
    gc_calls++;
    (glFogfv)(p, v);
}

static void gc_fogf(GLenum p, GLfloat v)
{
    uint32_t *s = p == GL_FOG_START ? &gc.fog[4] : p == GL_FOG_END ? &gc.fog[5] : p == GL_FOG_DENSITY ? &gc.fog[6] : NULL;
    if (s) { if (gc_set1(s, gc_fbits(v))) (glFogf)(p, v); return; }
    gc_calls++;
    (glFogf)(p, v);
}

/* Matrices: the stack index of the current mode, or -1 if unknown. */
static int gc_matrix_slot(void)
{
    switch (gc.matrix_mode) {
    case GL_MODELVIEW: return 0;
    case GL_PROJECTION: return 1;
    case GL_TEXTURE: return gc_unit() < GC_UNITS ? 2 + (int)gc_unit() : -1;
    }
    return -1;
}

static void gc_matrix_mode(GLenum m) { if (gc_set1(&gc.matrix_mode, m)) (glMatrixMode)(m); }

static void gc_load_matrixf(const GLfloat *m)
{
    int s = gc_matrix_slot();
    if (s >= 0) {
        if (gc.matrix_known[s] && gc_same(gc.matrix[s], m, 16)) { gc_skipped++; return; }
        memcpy(gc.matrix[s], m, 64);
        gc.matrix_known[s] = 1;
    }
    gc_calls++;
    (glLoadMatrixf)(m);
}

/* Any other change to the current matrix. */
static void gc_matrix_changed(void)
{
    int s = gc_matrix_slot();
    if (s >= 0) gc.matrix_known[s] = 0;
    else memset(gc.matrix_known, 0, sizeof gc.matrix_known);
    gc_calls++;
}

static void gc_pop_attrib(void)
{
    (glPopAttrib)();
    gc_reset();
}

#define glEnable(c) gc_enable(c, true)
#define glDisable(c) gc_enable(c, false)
#define glIsEnabled(c) gc_is_enabled(c)
#define glEnableClientState(a) gc_client(a, true)
#define glDisableClientState(a) gc_client(a, false)
#define p_glActiveTexture(u) gc_active_texture(u)
#define p_glClientActiveTexture(u) gc_client_active_texture(u)
#define glBindTexture(t, id) gc_bind_texture(t, id)
#define glTexParameteri(t, p, v) gc_tex_parameteri(t, p, v)
#define glTexParameterfv(t, p, v) gc_tex_parameterfv(t, p, v)
#define glGenTextures(n, ids) gc_gen_textures(n, ids)
#define glDeleteTextures(n, ids) gc_delete_textures(n, ids)
#define glTexEnvi(t, p, v) gc_texenvi(t, p, v)
#define glTexEnvf(t, p, v) gc_texenvf(t, p, v)
#define glTexEnvfv(t, p, v) gc_texenvfv(t, p, v)
#define glTexGeni(c, p, v) gc_texgeni(c, p, v)
#define p_glMultiTexCoord4fv(u, v) gc_multitexcoord4fv(u, v)
#define p_glUseProgram(p) gc_use_program(p)
#define p_glEnableVertexAttribArray(r) gc_attrib_array(r, true)
#define p_glDisableVertexAttribArray(r) gc_attrib_array(r, false)
#define p_glVertexAttrib4fv(r, v) gc_vertex_attrib4fv(r, v)
#define glDepthFunc(f) gc_depth_func(f)
#define glDepthMask(m) gc_depth_mask(m)
#define glShadeModel(m) gc_shade_model(m)
#define glCullFace(m) gc_cull_face(m)
#define glFrontFace(m) gc_front_face(m)
#define glPointSize(s) gc_point_size(s)
#define glBlendFunc(s, d) gc_blend_func(s, d)
#define glAlphaFunc(f, r) gc_alpha_func(f, r)
#define glPolygonOffset(a, b) gc_polygon_offset(a, b)
#define glPolygonMode(f, m) gc_polygon_mode(f, m)
#define p_glPointParameterfv(p, v) gc_point_parameterfv(p, v)
#define p_glPointParameterf(p, v) gc_point_parameterf(p, v)
#define glViewport(x, y, w, h) gc_viewport(x, y, w, h)
#define glScissor(x, y, w, h) gc_scissor(x, y, w, h)
#define glColorMask(r, g, b, a) gc_color_mask(r, g, b, a)
#define glDepthRange(n, f) gc_depth_range(n, f)
#define glFogfv(p, v) gc_fogfv(p, v)
#define glFogf(p, v) gc_fogf(p, v)
#define glMatrixMode(m) gc_matrix_mode(m)
#define glLoadMatrixf(m) gc_load_matrixf(m)
#define glLoadIdentity() (gc_matrix_changed(), (glLoadIdentity)())
#define glPopMatrix() (gc_matrix_changed(), (glPopMatrix)())
#define glOrtho(l, r, b, t, n, f) (gc_matrix_changed(), (glOrtho)(l, r, b, t, n, f))
#define glTranslatef(x, y, z) (gc_matrix_changed(), (glTranslatef)(x, y, z))
#define glPopAttrib() gc_pop_attrib()
