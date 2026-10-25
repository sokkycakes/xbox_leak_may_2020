#include <sys/mman.h>
/*
 * Direct3D 8 (Xbox) on SDL2 + OpenGL.
 *
 * The Xbox D3D8 library is a thin layer that writes NV2A push buffers.  We
 * replace its public functions with ones that drive OpenGL instead.  Objects
 * the title can see (vertex buffers, the device) keep their Xbox layout in
 * guest memory, because inline header code reads their fields directly, and
 * render states live in the title's own D3D__RenderState array, which the
 * inline SetRenderState code writes before calling into the library.
 *
 * Coverage is what the ATG tutorials need: device creation, clears,
 * pre-transformed and transformed FVF geometry, the fixed-function transform
 * stack, basic render states, and Present.
 */
#define _GNU_SOURCE
#include <GL/gl.h>
#include <SDL.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../xbcompat.h"
#include "hle.h"
#include "../cpu.h"
#include "nv2a_shaders.h"
#include "dxt_decode.h"
#include "ff_vsh.h"
#include "ff_combiner.h"
#include "shader_diagnostics.h"
#include "cpu_compare_audit.h"
static unsigned shader_diag_draw;   /* draws seen, for XBCOMPAT_SHADER_DIAGNOSTICS */

/* Set at device creation: see rgba_colors. */
static bool rgba_vertex_colors;
static bool vc4;   /* the Raspberry Pi's GPU */
static GLint host_vertex_attribs;   /* GL_MAX_VERTEX_ATTRIBS */

#include "glcache.h"

/* !memcmp(a, b, n) without a call, 16 bytes a step (plain loads, which
   may be unaligned).  The vertex and index caches and the queued-state
   deltas compare thousands of short runs a frame, mostly equal; glibc's
   ARM memcmp was a tenth of the render worker's time in Phantom Dust. */
static inline bool same_bytes(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    for (; n >= 16; n -= 16, p += 16, q += 16) {
        uint32_t x[4], y[4];
        memcpy(x, p, 16);
        memcpy(y, q, 16);
        if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) return false;
    }
    for (; n >= 4; n -= 4, p += 4, q += 4) {
        uint32_t x, y;
        memcpy(&x, p, 4);
        memcpy(&y, q, 4);
        if (x != y) return false;
    }
    for (; n; n--)
        if (*p++ != *q++) return false;
    return true;
}

typedef ULONG UINT_;

#define D3D_OK 0
#define D3DERR_NOTAVAILABLE ((LONG)0x8876086A)
#define D3DERR_INVALIDCALL ((LONG)0x8876086C)
#define D3DERR_NOTFOUND ((LONG)0x88760866)

/* ---- Xbox D3D types ---------------------------------------------------- */

typedef struct {
    DWORD Common;
    DWORD Data;
    DWORD Lock;
} D3DResource;

typedef struct D3DPixelContainer {
    D3DResource res;
    ULONG Format;
    ULONG Size;
} D3DPixelContainer;

static void tex_invalidate(ULONG data);

/* A captured HUD packet is drawing (see hud_packet.h): its textures are bound already. */
static bool hud_packet_active;
static bool hud_raw_active;
static ULONG hud_packet_texture_identity[4];
static GLuint hud_packet_texture_id[4];

typedef struct {
    ULONG BackBufferWidth;
    ULONG BackBufferHeight;
    ULONG BackBufferFormat;
    ULONG BackBufferCount;
    ULONG MultiSampleType;
    ULONG SwapEffect;
    HANDLE hDeviceWindow;
    ULONG Windowed;
    ULONG EnableAutoDepthStencil;
    ULONG AutoDepthStencilFormat;
    ULONG Flags;
    ULONG FullScreen_RefreshRateInHz;
    ULONG FullScreen_PresentationInterval;
    PVOID BufferSurfaces[3];
    PVOID DepthStencilSurface;
} D3DPRESENT_PARAMETERS;

typedef struct { float m[4][4]; } D3DMATRIX;
typedef struct { ULONG X, Y, Width, Height; float MinZ, MaxZ; } D3DVIEWPORT8;
typedef struct { LONG x1, y1, x2, y2; } D3DRECT;

#define D3DCOMMON_REFCOUNT_MASK     0x0000FFFF
#define D3DCOMMON_TYPE_VERTEXBUFFER 0x00000000
#define D3DCOMMON_TYPE_INDEXBUFFER  0x00010000
#define D3DCOMMON_TYPE_PUSHBUFFER   0x00020000
#define D3DCOMMON_TYPE_PALETTE      0x00030000
#define D3DCOMMON_TYPE_TEXTURE      0x00040000
#define D3DCOMMON_TYPE_SURFACE      0x00050000
#define D3DCOMMON_TYPE_FIXUP        0x00060000
#define D3DCOMMON_TYPE_MASK         0x00070000
#define D3DCOMMON_D3DCREATED        0x01000000

#define D3DFVF_POSITION_MASK 0x00E
#define D3DFVF_XYZ           0x002
#define D3DFVF_XYZRHW        0x004
#define D3DFVF_XYZB1         0x006
#define D3DFVF_NORMAL        0x010
#define D3DFVF_DIFFUSE       0x040
#define D3DFVF_SPECULAR      0x080
#define D3DFVF_TEXCOUNT_MASK 0xF00
#define D3DFVF_TEXCOUNT_SHIFT 8

enum {
    D3DRS_PSCONSTANT0_0 = 10, D3DRS_PSCONSTANT1_0 = 18, D3DRS_PSCONSTANT1_7 = 25,
    D3DRS_PSFINALCOMBINERCONSTANT0 = 43, D3DRS_PSFINALCOMBINERCONSTANT1 = 44, D3DRS_PS_MAX = 57,
    D3DRS_ZFUNC = 57, D3DRS_ALPHAFUNC = 58, D3DRS_ALPHABLENDENABLE = 59, D3DRS_ALPHATESTENABLE = 60,
    D3DRS_ALPHAREF = 61, D3DRS_SRCBLEND = 62, D3DRS_DESTBLEND = 63, D3DRS_ZWRITEENABLE = 64, D3DRS_DITHERENABLE = 65,
    D3DRS_SHADEMODE = 66, D3DRS_COLORWRITEENABLE = 67, D3DRS_STENCILZFAIL = 68, D3DRS_STENCILPASS = 69,
    D3DRS_STENCILFUNC = 70, D3DRS_STENCILREF = 71, D3DRS_STENCILMASK = 72, D3DRS_STENCILWRITEMASK = 73,
    D3DRS_BLENDOP = 74, D3DRS_BLENDCOLOR = 75, D3DRS_STENCILFAIL = 126,
    D3DRS_POLYGONOFFSETZSLOPESCALE = 77, D3DRS_POLYGONOFFSETZOFFSET = 78, D3DRS_SOLIDOFFSETENABLE = 81,
    D3DRS_FOGENABLE = 82, D3DRS_FOGTABLEMODE = 83, D3DRS_FOGSTART = 84, D3DRS_FOGEND = 85,
    D3DRS_FOGDENSITY = 86, D3DRS_LIGHTING = 92, D3DRS_SPECULARENABLE = 93, D3DRS_COLORVERTEX = 95,
    D3DRS_DIFFUSEMATERIALSOURCE = 101, D3DRS_AMBIENTMATERIALSOURCE = 102, D3DRS_AMBIENT = 105,
    D3DRS_POINTSIZE = 106, D3DRS_POINTSIZE_MIN = 107, D3DRS_POINTSPRITEENABLE = 108, D3DRS_POINTSCALEENABLE = 109,
    D3DRS_POINTSCALE_A = 110, D3DRS_POINTSCALE_B = 111, D3DRS_POINTSCALE_C = 112, D3DRS_POINTSIZE_MAX = 113,
    D3DRS_PSTEXTUREMODES = 117, D3DRS_FOGCOLOR = 119,
    D3DRS_FILLMODE = 120, D3DRS_NORMALIZENORMALS = 123, D3DRS_ZENABLE = 124,
    D3DRS_STENCILENABLE = 125, D3DRS_FRONTFACE = 127, D3DRS_CULLMODE = 128,
    D3DRS_TEXTUREFACTOR = 129, D3DRS_SHADOWFUNC = 137, D3DRS_MAX = 146,
};

/* ---- state ------------------------------------------------------------- */

typedef struct d3d_state {
    SDL_Window *window;
    SDL_GLContext gl;
    int width, height;
    ULONG frame;
    ULONG *render_state;         /* the title's D3D__RenderState[] */
    ULONG render_state_fallback[D3DRS_MAX];
    unsigned rs_count;           /* entries in the title's array */
    ULONG *device;               /* the title's g_Device (or our own) */
    ULONG *device_ptr;           /* the title's g_pDevice */
    D3DMATRIX transforms[10];    /* VIEW, PROJECTION, TEXTURE0-3, WORLD0-3 */
    D3DVIEWPORT8 viewport;
    struct { D3DResource *vb; ULONG stride; } streams[16];
    ULONG *texture_state;        /* the title's D3D__TextureState[4][32] */
    USHORT **index_data;         /* the title's D3D__IndexData, read by the DrawIndexedPrimitive inline */
    ULONG texture_state_fallback[4 * 32];
    D3DResource *textures[4];
    ULONG vertex_shader;
    D3DResource *indices;
    ULONG base_vertex_index;
    struct { bool set; float diffuse[4], ambient[4], specular[4], position[4], direction[4];
             ULONG type; float range, attenuation[3]; bool enabled; } lights[8];
    float material[4][4];        /* diffuse, ambient, specular, emissive */
    float material_power;
    float back_material[4][4];   /* SetBackMaterial: diffuse, ambient, specular, emissive */
    float back_material_power;
    ULONG device_refs;

    struct D3DSurface *backbuffer, *depth;        /* the device's own surfaces */
    struct D3DSurface *target, *target_depth;     /* current render target */
    int rt_width, rt_height;     /* size of the current render target */
    bool rt_texture;             /* rendering into a texture (GL rows run bottom-up) */
    float target_zmax;           /* largest value of the current depth buffer's format */
    float host_zmax;             /* the same for the GL depth buffer actually backing it */
    ULONG fbo;                   /* framebuffer object for texture targets */
    ULONG bb_proxy_tex;          /* the back buffer's stand-in while it pairs with a depth texture */
    bool bb_proxy;               /* d3d.fbo draws into bb_proxy_tex for the back buffer */
    struct { ULONG count; ULONG exclusive; D3DRECT rects[8]; } scissors;
    float screen_offset[2];
    float backbuffer_scale[2];
    ULONG multisample_type;      /* D3DPRESENT_PARAMETERS.MultiSampleType */
    ULONG color_format, depth_format;   /* the device's back buffer formats */
    ULONG flicker_filter;        /* SetFlickerFilter level, 0 (off) to 5 */
    bool soft_display;           /* SetSoftDisplayFilter (the encoder's luma filter) */
    unsigned char gamma[3][256]; /* SetGammaRamp: red, green, blue (D3DGAMMARAMP) */
    bool gamma_set;              /* the ramp isn't the identity */
    bool gamma_dirty;            /* the ramp changed since Present last loaded it */
    ULONG constant_mode;
    float vs_const[192][4];      /* vertex shader constants, hardware numbering */
    /* Bumped by each write to vs_const; a row's number is the write that
       last touched it (see vsc_touch), so a program finds the rows written
       since it last drew without comparing all 192. */
    unsigned vsc_gen, vsc_row_gen[192];
    float ps_const[16][4];
    float eye_vector[4];
    ULONG pixel_shader;
    float zscale;                /* depth range of the target: 2^24-1 or 2^16-1 */
    ULONG *cpu_time;             /* the device's m_CpuTime, when its layout is known */
    struct { PVOID fn; ULONG ctx; } callbacks[64];
    unsigned ncallbacks;
    struct D3DPalette *palettes[4];
    struct D3DPushBuffer *recording;   /* push buffer being recorded, if any */
    bool bb_cpu_dirty;           /* the title locked the back buffer and may have written it */
    bool stipple_on;             /* NV097_SET_STIPPLE_CONTROL: polygon stipple */
    bool stipple_dirty;          /* the pattern changed since GL last got it */
    ULONG stipple[32];           /* NV097_SET_STIPPLE_PATTERN: one row of 32 pixels each */
    /* Set[VertexBlend]ModelView: matrices given to the hardware directly in
       place of the ones derived from the transforms (the library's
       D3DDIRTYFLAG_DIRECT_MODELVIEW, m_ModelViewTransform and
       m_ProjectionViewport).  `composite` maps to the viewport's pixels. */
    struct { bool on, blend, has_inv; ULONG count; D3DMATRIX mv[4], inv[4], composite, proj_vp; } direct_mv;
} d3d_state;

/* The device state.  With XBCOMPAT_ASYNC_DRAWS the title's threads set
   state in one copy (the front) while the render worker draws from another
   (the back), brought up to date with each queued draw (async_draws.h);
   otherwise every thread uses the first. */
static d3d_state d3d_storage[2];
static __thread d3d_state *d3d_cur __attribute__((tls_model("initial-exec"))) = &d3d_storage[0];
#define d3d (*d3d_cur)

/* Rows lo .. lo + n - 1 of the vertex shader constants were written. */
static void vsc_touch_in(d3d_state *st, unsigned lo, unsigned n)
{
    unsigned g = ++st->vsc_gen;
    for (unsigned r = lo; r < lo + n && r < 192; r++) st->vsc_row_gen[r] = g;
}
#define vsc_touch(lo, n) vsc_touch_in(d3d_cur, lo, n)

/* XBCOMPAT_ASYNC_DRAWS (async_draws.h): on once the front copy exists.
   async_worker_thread marks the render worker, async_replaying its replay
   of queued work. */
static bool async_on;
static __thread bool async_worker_thread, async_replaying;
static bool async_front(void);
static ULONG async_swap(void);
static void async_raw_front(const ULONG *p, unsigned n);
static void async_draw(ULONG prim, ULONG start, ULONG count);
static void async_draw_up(ULONG prim, ULONG count, const void *data, ULONG stride);
static void async_draw_indexed(ULONG prim, ULONG count, const USHORT *indices);
static void async_draw_indexed_up(ULONG prim, ULONG count, const USHORT *indices, const void *verts, ULONG stride);
static void async_clear(ULONG count, const D3DRECT *rects, ULONG flags, ULONG color, float z, ULONG stencil);
static void async_target(struct D3DSurface *target, struct D3DSurface *z);
static void async_vis_begin(void);
static void async_vis_end(ULONG index);
static LONG async_vis_result(ULONG index, UINT_ *result);
static void async_release(D3DResource *r);
static bool pb_state_only(const ULONG *p, unsigned n);
static void async_touch(unsigned groups);
static void async_vs_touch(int slot);
static void pb_interpret_body(const ULONG *p, unsigned n);
bool d3d_async_wanted(void);

/* The render worker's own copy, when drawing is queued. */
void d3d_worker_thread_init(void)
{
    async_worker_thread = true;
    if (d3d_async_wanted()) d3d_cur = &d3d_storage[1];
}

/* Vertical blank and swap bookkeeping, shared by the title's threads, the
   kernel's DPC thread and the render worker. */
static struct {
    PVOID vblank_callback, swap_callback;
    ULONG *miniport_swap_cb;     /* the device's m_pSwapCallback when the title inlines its setters */
    volatile ULONG vblank_count, vblank_at_swap;
    volatile int swapped;        /* a Swap since the last vertical blank */
} d3dv;

/* Push buffer recording (see the push buffer section). */
static void pb_record(unsigned op, const void *payload, unsigned bytes);
static void pb_record2(unsigned op, const void *head, unsigned head_bytes, const void *data, unsigned data_bytes);
enum {
    OP_RS = 1, OP_TSS, OP_VS_CONST, OP_VERTEX_SHADER, OP_TEXTURE, OP_STREAM, OP_INDICES, OP_TRANSFORM,
    OP_VIEWPORT, OP_DRAW, OP_DRAW_INDEXED, OP_DRAW_UP, OP_DRAW_INDEXED_UP, OP_BEGIN_END, OP_PIXEL_SHADER,
    OP_PS_CONST, OP_VS_INPUT, OP_RENDER_TARGET, OP_PALETTE, OP_RUN, OP_CLEAR, OP_LIGHT, OP_LIGHT_ENABLE,
    OP_MATERIAL, OP_RAW, OP_NOP, OP_VIS_BEGIN, OP_VIS_END, OP_MODELVIEW, OP_BLEND_MODELVIEW, OP_SCISSORS,
};

/* An Xbox surface: a pixel container plus the texture it belongs to. */
typedef struct D3DSurface {
    DWORD Common, Data, Lock, Format, Size;
    struct D3DPixelContainer *Parent;
} D3DSurface;

typedef struct D3DPalette { DWORD Common, Data, Lock; } D3DPalette;

extern int g_screenshot_frame;
extern const char *g_screenshot_path;
extern int g_exit_after_frames;

/* Host code indexes render states with the 4400 D3DRS_* values below; later
   XDKs insert states, so rs_xlat[] gives each one's index in the title's
   own array (identity for 4400 titles; see bind_render_state_layout). */
static unsigned char rs_xlat[D3DRS_MAX];
#define RS(i) (d3d.render_state[rs_xlat[i]])
#define TSS(stage, i) (d3d.texture_state[(stage) * 32 + (i)])
static float rs_float(ULONG i) { float f; memcpy(&f, &RS(i), 4); return f; }
static float tss_float(int s, ULONG i) { float f; memcpy(&f, &d3d.texture_state[s * 32 + i], 4); return f; }
/* Fixed-function stages that need a fragment program: bump env mapping
   (ff_combiner.h).  Everything else stays on GL's texture environment. */
static bool ff_fragment_program(void)
{
    return !d3d.pixel_shader && ff_combiner_needs_program(d3d.texture_state, RS(D3DRS_POINTSPRITEENABLE) != 0);
}

enum {
    D3DTSS_ADDRESSU = 0, D3DTSS_ADDRESSV = 1, D3DTSS_ADDRESSW = 2, D3DTSS_MAGFILTER = 3, D3DTSS_MINFILTER = 4,
    D3DTSS_COLORKEYOP = 9, D3DTSS_ALPHAKILL = 11,
    D3DTSS_MIPFILTER = 5, D3DTSS_BUMPENVMAT00 = 22, D3DTSS_BUMPENVMAT01 = 23, D3DTSS_BUMPENVMAT11 = 24,
    D3DTSS_BUMPENVMAT10 = 25, D3DTSS_BUMPENVLSCALE = 26, D3DTSS_BUMPENVLOFFSET = 27,
    D3DTSS_COLOROP = 12, D3DTSS_COLORARG0 = 13, D3DTSS_COLORARG1 = 14, D3DTSS_COLORARG2 = 15,
    D3DTSS_ALPHAOP = 16, D3DTSS_ALPHAARG0 = 17, D3DTSS_ALPHAARG1 = 18, D3DTSS_ALPHAARG2 = 19,
    D3DTSS_TEXCOORDINDEX = 28, D3DTSS_BORDERCOLOR = 29, D3DTSS_COLORKEYCOLOR = 30,
};

/* ---- device ------------------------------------------------------------ */

static void load_fbo_functions(void);
static void load_shader_functions(void);
static void create_device_surfaces(ULONG format, ULONG depth_format);
static bool create_window_framebuffer(void);
static void present_window_framebuffer(void);
static void gamma_identity(void);
static void restore_window_framebuffer(void);
static void backbuffer_read_begin(GLbitfield mask);
static void backbuffer_read_end(void);
static void hud_batch_flush(void), hud_profile_log(double frames);   /* fvf_batch.h */
static GLuint cur_program;   /* the GL program bound for draws (see use_program) */
/* Set from /tmp/xbcompat-trace-boot (a frame count; the file is removed):
   the next title to create its device is traced from there until that frame,
   for catching what it shows before its first videos. */
static unsigned long trace_boot_frames;

static bool ff_lighting_candidate, ff_primary_present, ff_secondary_present;
static unsigned ff_texture_units, ff_weight_components;
static bool ff_pretransformed;
static void program_off(void);
static const float *vertex_diffuse(void);
static GLuint backbuffer_copy_texture(ULONG data, ULONG w, ULONG h);
static void swap_cpu_backbuffer(void);
static void front_buffer_update(void);
static void front_buffer_memory(void);
static void front_buffer_overlay(void);
static void front_snap_take(void);
/* GetBackBuffer(-1): see front_buffer_update.  snap holds its memory as
   xbcompat last left it (see front_buffer_overlay). */
static struct { struct D3DSurface *s; GLuint tex; volatile bool used; bool memory_stale; uint8_t *snap; } front;
static void persisted_first_present(void);
static void run_callbacks(void);
static void pusher_drain(void);
static void pusher_init(ULONG *dev);
static void sync_device_surfaces(void);
static bool device_layout_5849;   /* d3d.device is laid out like the 5849 CDevice */

/* ---- the GL context across guest threads -------------------------------- */

/* Each guest thread is a host thread, and the GL context is current on one
   thread at a time.  On the Xbox any thread may draw, so long as the title
   takes turns (D3D isn't thread safe there either): Splinter Cell draws its
   loading bar and Swaps from a thread of its own while the first thread
   loads, and those draws reached no context (black loading screens, no
   bar).  So a thread that draws while another holds the context asks for it
   (gl_require), and the holder lets it go at its next kernel call (a wait, a
   sleep, a file read: xbc_gl_yield) or when it exits, never inside a D3D
   call.  Threads that only set state or lock resources never ask.  With
   queued drawing the render worker owns the context and none of this
   applies. */
static pthread_mutex_t gl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gl_cond XBC_COND_ALIGN = PTHREAD_COND_INITIALIZER;
static bool gl_held;               /* current on some thread (under gl_lock) */
static __thread bool gl_mine;      /* current on this thread */
static __thread int gl_no_yield;   /* running a title callback from inside D3D: keep it */
static volatile int gl_wanted;     /* threads waiting in gl_require */
static pthread_t gl_creator;       /* made the window, and reads its events */
/* Textures a thread without the context let go of, for the next holder. */
static GLuint gl_deferred_deletes[256];
static unsigned gl_deferred_count;
void ke_wake_waiters(void) __attribute__((weak));   /* kernel/ke.c */

static bool gl_lost(void) { return d3d.gl && !async_on && !gl_mine; }

/* False when it couldn't get the context (GL calls then go nowhere, as
   before any of this). */
static bool gl_require(void)
{
    if (gl_mine || !d3d.gl || async_on) return true;
    cpu_block();   /* translated builds: the holder needs the guest CPU to get to a kernel call */
    pthread_mutex_lock(&gl_lock);
    struct timespec give_up;
    clock_gettime(CLOCK_REALTIME, &give_up);
    give_up.tv_sec += 2;
    __atomic_add_fetch(&gl_wanted, 1, __ATOMIC_SEQ_CST);
    while (gl_held) {
        /* A holder blocked in a kernel wait looks again. */
        pthread_mutex_unlock(&gl_lock);
        if (ke_wake_waiters) ke_wake_waiters();
        pthread_mutex_lock(&gl_lock);
        if (!gl_held) break;
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        if (t.tv_sec > give_up.tv_sec || (t.tv_sec == give_up.tv_sec && t.tv_nsec >= give_up.tv_nsec)) break;
        t.tv_nsec += 50000000;
        if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&gl_cond, &gl_lock, &t);
    }
    __atomic_sub_fetch(&gl_wanted, 1, __ATOMIC_SEQ_CST);
    if (gl_held || SDL_GL_MakeCurrent(d3d.window, d3d.gl) != 0) {
        static unsigned warned;
        if (warned++ < 4)
            xlog("D3D: a thread draws without the GL context (%s)",
                 gl_held ? "the thread holding it made no kernel call for 2 s" : SDL_GetError());
        pthread_mutex_unlock(&gl_lock);
        return false;
    }
    gl_held = gl_mine = true;
    if (gl_deferred_count) glDeleteTextures(gl_deferred_count, gl_deferred_deletes);
    gl_deferred_count = 0;
    pthread_mutex_unlock(&gl_lock);
    gc_reset();   /* anything set without the context never reached GL */
    static unsigned handoffs;
    if (handoffs++ < 8)
        xlog("D3D: the GL context goes to host thread %#lx, which draws now (frame %lu)",
             (unsigned long)pthread_self(), (unsigned long)d3d.frame);
    return true;
}

static void gl_delete_texture(GLuint id)
{
    if (!gl_lost()) { glDeleteTextures(1, &id); return; }
    pthread_mutex_lock(&gl_lock);
    if (gl_deferred_count < sizeof(gl_deferred_deletes) / sizeof(gl_deferred_deletes[0]))
        gl_deferred_deletes[gl_deferred_count++] = id;
    pthread_mutex_unlock(&gl_lock);
}

static void gl_release(void)
{
    pthread_mutex_lock(&gl_lock);
    SDL_GL_MakeCurrent(d3d.window, NULL);
    gl_held = gl_mine = false;
    pthread_cond_broadcast(&gl_cond);
    pthread_mutex_unlock(&gl_lock);
}

/* From kernel calls: true while this thread holds the context another
   thread waits for. */
bool xbc_gl_yield_due(void)
{
    return gl_mine && !gl_no_yield && __atomic_load_n(&gl_wanted, __ATOMIC_RELAXED);
}

void xbc_gl_yield(void)
{
    if (xbc_gl_yield_due()) gl_release();
}

void xbc_gl_thread_exit(void)
{
    if (gl_mine) gl_release();
}

static ULONG direct3d_object[4];

static PVOID NTAPI Direct3DCreate8(UINT_ SDKVersion)
{
    TRACE_ALL("Direct3DCreate8(%#x)", SDKVersion);
    return direct3d_object;
}

static void identity(D3DMATRIX *m)
{
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1;
}

static void default_render_states(void)
{
    /* The title's image carries the library's own defaults table. */
    ULONG init = hle_lookup_prefix("?g_InitialRenderStates@D3D@@");
    (void)init;
    RS(D3DRS_ZFUNC) = GL_LEQUAL;
    RS(D3DRS_ALPHAFUNC) = GL_ALWAYS;
    RS(D3DRS_ALPHABLENDENABLE) = 0;
    RS(D3DRS_ALPHATESTENABLE) = 0;
    RS(D3DRS_SRCBLEND) = GL_ONE;
    RS(D3DRS_DESTBLEND) = GL_ZERO;
    RS(D3DRS_ZWRITEENABLE) = 1;
    RS(D3DRS_SHADEMODE) = GL_SMOOTH;
    RS(D3DRS_COLORWRITEENABLE) = 0x01010101;
    RS(D3DRS_BLENDOP) = GL_FUNC_ADD;
    RS(D3DRS_STENCILFUNC) = GL_ALWAYS;
    RS(D3DRS_STENCILMASK) = RS(D3DRS_STENCILWRITEMASK) = 0xFF;
    RS(D3DRS_STENCILFAIL) = RS(D3DRS_STENCILZFAIL) = RS(D3DRS_STENCILPASS) = GL_KEEP;
    RS(D3DRS_LIGHTING) = 1;
    RS(D3DRS_COLORVERTEX) = 1;
    RS(D3DRS_DIFFUSEMATERIALSOURCE) = 1;   /* D3DMCS_COLOR1 */
    RS(D3DRS_AMBIENTMATERIALSOURCE) = 0;   /* D3DMCS_MATERIAL */
    RS(D3DRS_FILLMODE) = GL_FILL;
    RS(D3DRS_CULLMODE) = 0x901;            /* D3DCULL_CCW */
    RS(D3DRS_FRONTFACE) = GL_CW;

    for (int st = 0; st < 4; st++) {
        TSS(st, D3DTSS_COLOROP) = st == 0 ? 4 /* MODULATE */ : 1 /* DISABLE */;
        TSS(st, D3DTSS_COLORARG1) = 2;  /* D3DTA_TEXTURE */
        TSS(st, D3DTSS_COLORARG2) = 1;  /* D3DTA_CURRENT */
        TSS(st, D3DTSS_ALPHAOP) = st == 0 ? 2 /* SELECTARG1 */ : 1;
        TSS(st, D3DTSS_ALPHAARG1) = 2;
        TSS(st, D3DTSS_ALPHAARG2) = 1;
        TSS(st, D3DTSS_COLORARG0) = TSS(st, D3DTSS_ALPHAARG0) = 1;  /* D3DTA_CURRENT */
        TSS(st, D3DTSS_ADDRESSU) = TSS(st, D3DTSS_ADDRESSV) = 1;  /* WRAP */
        TSS(st, D3DTSS_MAGFILTER) = TSS(st, D3DTSS_MINFILTER) = 1;
        TSS(st, D3DTSS_TEXCOORDINDEX) = st;
    }
}

/* The title's index of a state that has a symbol of its own (XbSymbolDatabase
   maps them as _D3DRS_<Name>), or -1. */
static int title_rs_index(const char *name)
{
    char sym[64];
    snprintf(sym, sizeof(sym), "_D3DRS_%s", name);
    ULONG va = hle_lookup(sym);
    return va ? (int)((va - (ULONG)d3d.render_state) / 4) : -1;
}

/* XDK 4400's simple states end at 81, its deferred states run 82..116 and its
   complex states 117..145.  Later XDKs append unused slots to the first two
   groups and insert D3DRS_SAMPLEALPHA after D3DRS_LINEWIDTH, so each group
   moves as a block; the blocks are found from states the title has symbols
   for. */
static void bind_render_state_layout(void)
{
    for (int i = 0; i < D3DRS_MAX; i++) rs_xlat[i] = i;
    int fog = title_rs_index("FogEnable"), ps = title_rs_index("PSTextureModes");
    int dxt1 = title_rs_index("Dxt1NoiseEnable");
    if (fog < 0 && ps < 0) return;
    int d_shift = fog >= 0 ? fog - D3DRS_FOGENABLE : 0;
    int c_shift = ps >= 0 ? ps - D3DRS_PSTEXTUREMODES : d_shift;
    int t_shift = dxt1 >= 0 ? dxt1 - 139 /* D3DRS_DXT1NOISEENABLE */ : c_shift;
    for (int i = D3DRS_FOGENABLE; i < D3DRS_PSTEXTUREMODES; i++) rs_xlat[i] = i + d_shift;
    for (int i = D3DRS_PSTEXTUREMODES; i < 139; i++) rs_xlat[i] = i + c_shift;
    for (int i = 139; i < D3DRS_MAX; i++) rs_xlat[i] = i + t_shift;
    d3d.rs_count = D3DRS_MAX + t_shift;
    if (d_shift || c_shift || t_shift)
        xlog("D3D: render state layout shifted by %d/%d/%d (title has %u states)", d_shift, c_shift, t_shift,
             d3d.rs_count);
}

/* Frames presented so far (XBCOMPAT_INPUT_SCRIPT counts in these). The input
   thread reads it while the render worker presents, so Swap publishes it. */
static ULONG published_frame_count;
ULONG d3d_frame_count(void) { return __atomic_load_n(&published_frame_count, __ATOMIC_ACQUIRE); }

void d3d_bind_globals(void)
{
    d3d.render_state = (ULONG *)hle_lookup("_D3D__RenderState");
    if (!d3d.render_state) d3d.render_state = d3d.render_state_fallback;
    d3d.texture_state = (ULONG *)hle_lookup("_D3D__TextureState");
    if (!d3d.texture_state) d3d.texture_state = d3d.texture_state_fallback;
    d3d.index_data = (USHORT **)hle_lookup("_D3D__IndexData");
    d3d.device = (ULONG *)hle_lookup_prefix("?g_Device@D3D@@");
    d3d.device_ptr = (ULONG *)hle_lookup_prefix("?g_pDevice@D3D@@");
    d3d.rs_count = D3DRS_MAX;
    bind_render_state_layout();
    xlog("D3D: render states at %p, texture stage states at %p%s, device at %p", (void *)d3d.render_state,
         (void *)d3d.texture_state, d3d.texture_state == d3d.texture_state_fallback ? " (not the title's)" : "",
         (void *)d3d.device);
}

/* The D3D build the title links (its XBE library version table). */
static unsigned d3d_build(void)
{
    const uint8_t *hdr = (const uint8_t *)0x10000;
    ULONG n = *(const ULONG *)(hdr + 0x160);
    const uint8_t *lib = (const uint8_t *)*(const ULONG *)(hdr + 0x164);
    for (ULONG i = 0; i < n && lib; i++, lib += 16)
        if (!memcmp(lib, "D3D8", 4)) return *(const USHORT *)(lib + 12);
    return 0;
}

/* LTCG builds of D3D inline SetVerticalBlankCallback, SetSwapCallback and
   BlockUntilVerticalBlank, so the title stores its callbacks straight into
   the device's miniport and waits on its vertical blank event (mpintr.cpp,
   d3dbase.cpp).  The fields sit together: m_pSwapCallback,
   m_pVerticalBlankCallback, m_VerticalBlankEvent.  Their offset is per build
   (5849: from Phantom Dust's inlined code); XBCOMPAT_MINIPORT_OFFSET
   overrides it. */
/* The GPU's time while drawing is queued (async_draws.h): the fence the
   worker has got to.  Otherwise the device's GPU time is its CPU time. */
static volatile ULONG async_gpu_time;

static void bind_miniport(ULONG *dev)
{
    static const struct { unsigned build, swap_cb; } layouts[] = { { 5849, 0x1db4 } };
    unsigned build = d3d_build(), off = hle_lookup("_D3D_m_SwapCallback_OFFSET");
    for (unsigned i = 0; !off && i < sizeof(layouts) / sizeof(layouts[0]); i++)
        if (layouts[i].build == build) off = layouts[i].swap_cb;
    /* m_CpuTime and m_pGpuTime: inlined fence checks compare the two, so
       point the GPU's time at the CPU's (everything has always finished). */
    if (build == 5849) {
        d3d.cpu_time = dev + 0x2c / 4;
        dev[0x30 / 4] = (ULONG)d3d.cpu_time;
        /* The miniport (device+0x1c28) starts with the NV2A register base;
           library code that still pokes registers gets a dummy window
           instead of writing into guest memory near address 0. */
        static void *regs;
        if (!regs) regs = mmap(NULL, 16 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (regs != MAP_FAILED) dev[0x1c28 / 4] = (ULONG)regs;
        device_layout_5849 = true;
        sync_device_surfaces();
    }
    /* Library code that still runs reads the current vertex shader's flags
       through the device; give it a quiet, empty one. */
    ULONG vs_off = hle_lookup("_D3D_m_VertexShader_OFFSET");
    if (vs_off && vs_off + 4 <= 0x4000 && !dev[vs_off / 4]) {
        ULONG *dummy = pool_alloc(0x400);
        memset(dummy, 0, 0x400);
        dev[vs_off / 4] = (ULONG)dummy;
    }
    const char *e = getenv("XBCOMPAT_MINIPORT_OFFSET");
    if (e) off = strtoul(e, NULL, 0);
    if (!off || off + 12 + sizeof(KEVENT) > 0x4000 || (d3d.device && dev != d3d.device)) return;
    d3dv.miniport_swap_cb = (ULONG *)((uint8_t *)dev + off);
    KEVENT *ev = (KEVENT *)(d3dv.miniport_swap_cb + 2);
    ev->Header.Type = 0;   /* NotificationEvent, initially set (mpcore.cpp) */
    ev->Header.Size = sizeof(KEVENT) / 4;
    ev->Header.SignalState = 1;
    ev->Header.WaitListHead.Flink = ev->Header.WaitListHead.Blink = &ev->Header.WaitListHead;
    xlog("D3D: D3D build %u, miniport callbacks at device+%#x", build, off);
}

/* Where the 5849 CDevice keeps its surfaces (GetRenderTarget2 and friends
   read them; inlined copies of those run in the title). */

static void sync_device_surfaces(void)
{
    ULONG *dev = d3d.device;
    if (!dev || !device_layout_5849) return;
    dev[0x1a04 / 4] = (ULONG)d3d.target;
    dev[0x1a08 / 4] = (ULONG)d3d.target_depth;
    dev[0x1a14 / 4] = dev[0x1a18 / 4] = dev[0x1a1c / 4] = (ULONG)d3d.backbuffer;
}

/* Signalled at each vblank, for BlockUntilVerticalBlank (wait_vblank). */
static pthread_mutex_t vblank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vblank_cond XBC_COND_ALIGN = PTHREAD_COND_INITIALIZER;

/* A 60 Hz vertical blank, from the DPC thread (the GPU interrupt's DPC on an
   Xbox): sets the device's blank event and runs the title's callback. */
static void d3d_vblank(void)
{
    extern LONG NTAPI KeSetEvent(KEVENT *, LONG, BOOLEAN);
    pthread_mutex_lock(&vblank_lock);
    ULONG n = ++d3dv.vblank_count;
    pthread_cond_broadcast(&vblank_cond);
    pthread_mutex_unlock(&vblank_lock);
    ULONG flags = __atomic_exchange_n(&d3dv.swapped, 0, __ATOMIC_ACQ_REL) ? 1 /* D3DVBLANK_SWAPDONE */ : 0;
    PVOID cb = d3dv.vblank_callback;
    if (d3dv.miniport_swap_cb) {
        KeSetEvent((KEVENT *)(d3dv.miniport_swap_cb + 2), 1, 0);
        if (d3dv.miniport_swap_cb[1]) cb = (PVOID)d3dv.miniport_swap_cb[1];
    }
    if (cb) {
        ULONG data[3] = { n, d3d.frame, flags };
#ifdef XBC_TRANSLATED
        CPU_CALL(cb, CONV_CDECL, (uint32_t)data);
#else
        ((void (CDECLAPI *)(ULONG *))cb)(data);
#endif
    }
}

/* D3DPRESENT_INTERVAL_TWO (2), THREE (4), FOUR (8), alone or with IMMEDIATE
   (0x80000000: "or at once when late"): a frame stays up that many blanks.
   MechAssault asks for TWO_OR_IMMEDIATE and times its Bink movies by
   Present, so at one blank per frame they played twice as fast. */
static unsigned present_vblanks = 1;
static void set_present_interval(ULONG interval)
{
    ULONG n = interval & 0xF;
    unsigned v = n & 8 ? 4 : n & 4 ? 3 : n & 2 ? 2 : 1;
    if (v != present_vblanks) xlog("D3D: presentation interval %#lx: %u blanks per frame", (unsigned long)interval, v);
    present_vblanks = v;
}

static LONG NTAPI Direct3D_CreateDevice(UINT_ Adapter, ULONG DeviceType, PVOID pUnused, ULONG Flags,
                                       D3DPRESENT_PARAMETERS *pp, PVOID *ppDevice)
{
    d3d.width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    d3d.height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    d3d.multisample_type = pp->MultiSampleType;
    d3d.color_format = pp->BackBufferFormat;
    d3d.depth_format = pp->EnableAutoDepthStencil ? pp->AutoDepthStencilFormat : 0;
    d3d.flicker_filter = 5;   /* what the Xbox's device init sets */
    d3d.soft_display = false;
    gamma_identity();
    xlog("D3D: CreateDevice %ux%u, format %#x, depth %s, multisample %#x", d3d.width, d3d.height,
         pp->BackBufferFormat, pp->EnableAutoDepthStencil ? "yes" : "no", pp->MultiSampleType);
    set_present_interval(pp->FullScreen_PresentationInterval);

    av_title_starting();
    const char *boot_path = "/tmp/xbcompat-trace-boot";
    FILE *boot = fopen(boot_path, "r");
    if (!boot) boot = fopen(boot_path = "/data/xbox/xbcompat-trace-boot", "r");
    if (boot) {
        unsigned long n = 0;
        if (fscanf(boot, "%lu", &n) != 1 || !n) n = 30;
        fclose(boot);
        if (!unlink(boot_path)) {
            trace_boot_frames = d3d.frame + n;
            g_trace = 1;
            xlog("D3D: tracing from device creation to frame %lu", trace_boot_frames);
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) fatal("SDL_Init: %s", SDL_GetError());
    install_fault_handlers();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /* KMSDRM switches the display to the mode closest to a window's size
       (640x480 when the display has it). A fullscreen-desktop window keeps
       the display's own mode (KMS_MODE, e.g. 720x480 NTSC), and the back
       buffer is stretched over it (create_window_framebuffer). */
    Uint32 flags = SDL_WINDOW_OPENGL;
    const char *driver = SDL_GetCurrentVideoDriver();
    if ((driver && !strcasecmp(driver, "kmsdrm")) || getenv("XBCOMPAT_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    d3d.window = SDL_CreateWindow("xbcompat", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  d3d.width, d3d.height, flags);
    if (!d3d.window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    d3d.gl = SDL_GL_CreateContext(d3d.window);
    if (!d3d.gl) fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    gl_held = gl_mine = true;
    gl_creator = pthread_self();
    shader_diagnostics_init();
    gc_reset();
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &host_vertex_attribs);
    if (host_vertex_attribs > 16) host_vertex_attribs = 16;
    const char *swap_env = getenv("XBCOMPAT_SWAP_INTERVAL");
    int swap_interval = swap_env && !strcmp(swap_env, "0") ? 0 : 1;
    int swap_result = SDL_GL_SetSwapInterval(swap_interval);
    xlog("D3D: swap interval %d (%s)", swap_interval, swap_result ? SDL_GetError() : "accepted");
    xlog("D3D: OpenGL %s on %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    /* The Raspberry Pi's VC4 can't fetch B,G,R,A vertex colors; Mesa then
       converts them on every draw, and that path crashes in vc4 (indexed
       draws from client arrays). Swizzle them ourselves instead. */
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    vc4 = renderer && strstr(renderer, "VC4");
    rgba_vertex_colors = vc4 || getenv("XBCOMPAT_RGBA_VERTEX_COLORS");

    for (int i = 0; i < 10; i++) identity(&d3d.transforms[i]);
    d3d.viewport = (D3DVIEWPORT8){ 0, 0, d3d.width, d3d.height, 0, 1 };
    default_render_states();
    /* The library's device init turns the z test on when it made a depth
       buffer (dxgcreate.cpp); Torque titles (Marble Blast) never set it. */
    RS(D3DRS_ZENABLE) = pp->EnableAutoDepthStencil ? 1 /* D3DZB_TRUE */ : 0;
    d3d.material[0][0] = d3d.material[0][1] = d3d.material[0][2] = d3d.material[0][3] = 1;
    d3d.back_material[0][0] = d3d.back_material[0][1] = d3d.back_material[0][2] = d3d.back_material[0][3] = 1;
    d3d.device_refs = 1;

    load_fbo_functions();
    if (!create_window_framebuffer()) {
        SDL_GL_DeleteContext(d3d.gl); d3d.gl = NULL;
        gl_held = gl_mine = false;
        SDL_DestroyWindow(d3d.window); d3d.window = NULL;
        if (ppDevice) *ppDevice = NULL;
        return D3DERR_NOTAVAILABLE;
    }
    load_shader_functions();
    create_device_surfaces(pp->BackBufferFormat, pp->EnableAutoDepthStencil ? pp->AutoDepthStencilFormat : 0);
    d3d.backbuffer_scale[0] = d3d.backbuffer_scale[1] = 1;

    ULONG *dev = d3d.device;
    if (!dev) {
        /* Big enough for the 5849 CDevice, whose miniport fields LTCG titles
           reach into (see bind_miniport). */
        /* 16-byte aligned like the library's: its SSE code reads the
           device's matrices with movaps. */
        dev = (ULONG *)(((ULONG)pool_alloc(0x4000 + 16) + 15) & ~15u);
        memset(dev, 0, 0x4000);
        d3d.device = dev;
    }
    bind_miniport(dev);
    pusher_init(dev);
    __atomic_store_n(&g_vblank_hook, d3d_vblank, __ATOMIC_RELEASE);
    if (d3d.device_ptr) *d3d.device_ptr = (ULONG)dev;
    *ppDevice = dev;
    return D3D_OK;
}

/* ---- present and clears ---------------------------------------------- */

/* A 32-bit BMP of BGRA pixels, bottom row first as GL reads them. Written
   by hand: SDL_SaveBMP's RWops crash under box86 (x86 on a Raspberry Pi). */
static int write_bmp(const char *path, int w, int h, const uint8_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t size = (uint32_t)w * h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v[] = { 54 + size, 0, 54, 40, (uint32_t)w, (uint32_t)h };
    memcpy(hdr + 2, v, sizeof v);
    hdr[26] = 1; hdr[28] = 32;
    memcpy(hdr + 34, &size, 4);
    int ok = fwrite(hdr, 1, 54, f) == 54 && fwrite(px, 1, size, f) == size;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static void save_screenshot(const char *path)
{
    hud_batch_flush();
    int w = d3d.width, h = d3d.height;
    uint8_t *px = malloc(w * h * 4);
    backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    backbuffer_read_end();
    if (d3d.gamma_set)   /* as the TV shows it */
        for (int i = 0; i < w * h; i++)
            for (int c = 0; c < 3; c++) px[i * 4 + c] = d3d.gamma[2 - c][px[i * 4 + c]];
    if (write_bmp(path, w, h, px) != 0) xlog("screenshot failed: %s", strerror(errno));
    else xlog("D3D: saved frame %u to %s", d3d.frame, path);
    free(px);
}

/* Debug aid: XBCOMPAT_DUMP_DRAWS=dir saves the current target after every
   draw of one frame (XBCOMPAT_DUMP_FRAME, default the first) as
   dir/drawNNN.bmp. */
/* Work counted for XBCOMPAT_LOG_FPS and PROFILE_WORK: what a slow frame spends its time on. */
static struct { unsigned draws, uploads, readbacks, shaders; } stats;
/* Push buffer replay time per frame (draws included), in performance-counter
   ticks; log_fps times the rest. XBCOMPAT_PROFILE_SYNC=1 makes each draw
   wait for the GPU, so its time lands on the draw. */
static struct { Uint64 drain; } prof;
static int fps_on = -1, detail_on, work_on, prof_sync;
static unsigned fvf_plan_hits, fvf_plan_misses;

/* XBCOMPAT_LOG_FPS or XBCOMPAT_PROFILE_WORK is set: count and time the
   frame's work (the log has only the rate without PROFILE_WORK).
   XBCOMPAT_PROFILE_RENDERER adds the per-call timers (detail_on). */
static bool profiling(void)
{
    if (fps_on < 0) {
        const char *e = getenv("XBCOMPAT_LOG_FPS"), *w = getenv("XBCOMPAT_PROFILE_WORK");
        const char *p = getenv("XBCOMPAT_PROFILE_RENDERER"), *s = getenv("XBCOMPAT_PROFILE_SYNC");
        detail_on = p && *p && *p != '0';
        work_on = w && *w && *w != '0';
        fps_on = e && *e && *e != '0';
        prof_sync = s && *s && *s != '0';
    }
    return fps_on || work_on;
}

static Uint64 prof_draw_begin(void)
{
    return profiling() ? SDL_GetPerformanceCounter() : 0;
}

/* Draws and clears that reached the device's own depth buffer, so a
   texture header over its memory knows when to read the GPU's copy back. */
static unsigned long depth_gpu_writes;
/* The depth memory's hash and depth_gpu_writes when they last agreed. */
static struct { uint64_t hash; unsigned long writes; } zsync;
/* The back buffer's memory as it was when it last matched GL (an upload of
   CPU writes, or a read back): sampled, since views of the back buffer are
   made several times a frame. */
static void container_size(const D3DPixelContainer *t, ULONG *w, ULONG *h, ULONG *pitch);
static struct { ULONG data; uint64_t hash; } bbsync;
static uint64_t backbuffer_memory_hash(void)
{
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)d3d.backbuffer, &w, &h, &pitch);
    if (w > (ULONG)d3d.width) w = d3d.width;
    if (h > (ULONG)d3d.height) h = d3d.height;
    const uint8_t *px = (const uint8_t *)(d3d.backbuffer->Data | CONTIG_BASE);
    uint64_t x = 0xcbf29ce484222325ull;
    for (ULONG y = 0; y < h; y += 2) {
        const uint64_t *row = (const uint64_t *)(px + y * pitch);
        for (ULONG i = y & 6; i < w * 4 / 8; i += 8) x = (x ^ row[i]) * 0x100000001b3ull;
    }
    return x;
}
static void backbuffer_synced(void)
{
    if (!d3d.backbuffer) return;
    bbsync.data = d3d.backbuffer->Data;
    bbsync.hash = backbuffer_memory_hash();
}

static unsigned long draw_serial;   /* every draw: see swap_cpu_backbuffer */
static void debug_dump_draw(void)
{
    stats.draws++;
    draw_serial++;
    /* Only draws that can write depth or stencil count: Splinter Cell draws
       its loading picture (decoded into this memory) and progress bar with
       z writes off, and counting those read the GPU's depth back over the
       picture, so no level's loading screen showed. */
    if (d3d.target_depth && d3d.target_depth == d3d.depth &&
        ((RS(D3DRS_ZENABLE) && RS(D3DRS_ZWRITEENABLE)) || RS(D3DRS_STENCILENABLE)))
        depth_gpu_writes++;
    static const char *dir; static int checked, n; static ULONG frame;
    if (!checked) {
        dir = getenv("XBCOMPAT_DUMP_DRAWS");
        const char *f = getenv("XBCOMPAT_DUMP_FRAME");
        frame = f ? strtoul(f, NULL, 0) : 0;
        checked = 1;
    }
    if (!dir || d3d.frame != frame) return;
    char path[512];
    snprintf(path, sizeof path, "%s/draw%03d.bmp", dir, n++);
    save_screenshot(path);
}

/* Debug aid: XBCOMPAT_DUMP_TEXTURES=dir saves every 2D texture as it is
   uploaded, as dir/tex_<data>_<format>_<n>.bmp. */
static void debug_dump_texture(GLenum target, ULONG data, ULONG fmt, ULONG w, ULONG h)
{
    static const char *dir; static int checked, n;
    if (!checked) { dir = getenv("XBCOMPAT_DUMP_TEXTURES"); checked = 1; }
    if (!dir || target != GL_TEXTURE_2D) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, s->pixels);
    char path[512];
    snprintf(path, sizeof path, "%s/tex_%08x_%02x_%04d.bmp", dir, data, fmt, n++);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

/* The title wrote the back buffer through LockRect (XFONT text, software
   effects): put its memory back on screen before GL draws over it. */
static void (APIENTRY *p_glWindowPos2i)(GLint, GLint);
static void (APIENTRY *p_glBindFramebuffer)(GLenum, GLuint);
static void (APIENTRY *p_glActiveTexture)(GLenum);
static void draw_window_pixels_format(const uint8_t *tmp, GLenum format, GLenum type);
static bool surface_color_transfer(ULONG fmt, GLenum *format, GLenum *type, unsigned *bytes);
static void present_overlay(void);
static void restore_overlay(void);
static LONG NTAPI D3DDevice_PersistDisplay(void);

static void flush_cpu_backbuffer(void)
{
    if (!d3d.bb_cpu_dirty || !d3d.backbuffer) return;
    d3d.bb_cpu_dirty = false;
    ULONG w = d3d.width, h = d3d.height;
    GLenum format, type; unsigned bytes;
    if (!surface_color_transfer((d3d.backbuffer->Format >> 8) & 0xFF, &format, &type, &bytes)) {
        xlog("D3D: unsupported CPU back-buffer transfer format"); return;
    }
    ULONG pitch = d3d.backbuffer->Size ? ((d3d.backbuffer->Size >> 24) + 1) * 64 : w * bytes;
    const uint8_t *px = (const uint8_t *)(d3d.backbuffer->Data | CONTIG_BASE);
    uint8_t *tmp = malloc(w * h * bytes);
    if (!tmp) return;
    for (ULONG y = 0; y < h; y++) memcpy(tmp + (h - 1 - y) * w * bytes, px + y * pitch, w * bytes);
    draw_window_pixels_format(tmp, format, type);
    free(tmp);
    backbuffer_synced();
}

/* Replace the window's color buffer with BGRA pixels, bottom row first. */
static void draw_window_pixels(const uint8_t *tmp)
{ draw_window_pixels_format(tmp, GL_BGRA, GL_UNSIGNED_BYTE); }

static void draw_window_pixels_masked(const uint8_t *tmp, GLenum format, GLenum type, bool masked);
static void draw_window_pixels_format(const uint8_t *tmp, GLenum format, GLenum type)
{ draw_window_pixels_masked(tmp, format, type, false); }

/* masked: only pixels with alpha above 0 are drawn. */
static void draw_window_pixels_masked(const uint8_t *tmp, GLenum format, GLenum type, bool masked)
{
    shader_diagnostics_mark(d3d.frame, ++shader_diag_draw, 0, 0, 0, "cpu-pixels");
    if (!p_glWindowPos2i) p_glWindowPos2i = SDL_GL_GetProcAddress("glWindowPos2i");
    if (!p_glWindowPos2i) return;
    ULONG w = d3d.width, h = d3d.height;
    program_off();
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    for (int u = 3; u >= 0; u--) {
        p_glActiveTexture(GL_TEXTURE0 + u);
        glDisable(GL_TEXTURE_2D); glDisable(GL_TEXTURE_3D); glDisable(GL_TEXTURE_CUBE_MAP);
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_FOG);
    glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_LIGHTING);
    glDisable(GL_DITHER); glDisable(GL_POLYGON_STIPPLE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (masked) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.0f); }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glWindowPos2i(0, 0);
    glDrawPixels(w, h, format, type, tmp);
    if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, d3d.rt_texture ? d3d.fbo : 0);
    glPopAttrib();
}

/* 60 Hz vertical blank timing.  The host's vsync may not throttle (a
   headless X server, a compositor that ignores the swap interval), and
   titles that time video or animation by the wall clock need frames to take
   real time.  BlockUntilVerticalBlank waits for the next blank of a free
   running 60 Hz clock; Present queues a flip, so it only holds a title that
   gets more than a frame ahead of the display.  XBCOMPAT_UNPACED=1 turns
   both off for quick headless runs. */
static bool unpaced(void)
{
    static int u = -1;
    if (u < 0) u = getenv("XBCOMPAT_UNPACED") != NULL;
    return u;
}

/* SDL_Delay takes whole milliseconds; rounding down woke wait_vblank up to
   1 ms before the blank, and the next call then counted that same blank
   again, so a title looping on BlockUntilVerticalBlank saw several blanks
   per 60th of a second (Ms. Pac-Man's game logic ran 4-5x too fast). */
static void sleep_until(Uint64 t)
{
    Uint64 freq = SDL_GetPerformanceFrequency(), now;
    cpu_block();
    while ((now = SDL_GetPerformanceCounter()) < t)
        SDL_Delay((Uint32)(((t - now) * 1000 + freq - 1) / freq));
}

/* BlockUntilVerticalBlank: wait for the next vblank the kernel raises
   (d3d_vblank), which follows the display's own blanks where it can see
   them. A clock of its own here drifted against the display's. With no
   blanks arriving (before the kernel's thread runs, or a stalled fixed-step
   clock), fall back to a free 60 Hz grid. */

static uint64_t t_vbwait;   /* PROFILE_WORK: time in wait_vblank */
static uint64_t now_ns(void);
static void wait_vblank_(void);

static void wait_vblank(void)
{
    uint64_t t0 = work_on ? now_ns() : 0;
    wait_vblank_();
    if (t0) t_vbwait += now_ns() - t0;
}

static void wait_vblank_(void)
{
    static Uint64 base;
    if (unpaced()) return;
    cpu_block();
    pthread_mutex_lock(&vblank_lock);
    ULONG n = d3dv.vblank_count;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 50000000;
    if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
    int r = 0;
    while (d3dv.vblank_count == n && r == 0) r = pthread_cond_timedwait(&vblank_cond, &vblank_lock, &until);
    pthread_mutex_unlock(&vblank_lock);
    if (d3dv.vblank_count != n) return;
    Uint64 now = SDL_GetPerformanceCounter(), period = SDL_GetPerformanceFrequency() / 60;
    if (!base) base = now;
    sleep_until(base + ((now - base) / period + 1) * period);
}

/* XBCOMPAT_LOG_FPS: log the frame rate every 5 seconds, with where the
   frame time went: draws (xbcompat's state/constant/vertex setup, then the
   GL draw calls themselves), presenting (the filter pass and the buffer
   swap, which waits for the display), and the rest (the title's own code). */
static uint64_t t_draw, t_gl, t_present, t_pace;
/* With PROFILE_WORK, the presenting thread's CPU time in draws and present:
   wall time beyond it was spent blocked (a GPU sync in the driver, a lock). */
static uint64_t c_draw, c_present;
static uint64_t t_resolve, t_filter, t_swap, t_restore;
static uint64_t t_state, t_program, t_constants, t_attributes, t_indices, t_buffer_upload;
static uint64_t t_fshader, t_textures;   /* parts of t_program */
static unsigned n_draws;
static unsigned cache_hits, cache_misses, cache_evicts, cache_uploads;
static uint64_t cache_upload_bytes;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

static uint64_t thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

static void program_profile_log(double frames);
/* XBCOMPAT_WHATIF=1 (with XBCOMPAT_LOG_FPS): a measuring aid that cycles
   through ways of leaving work out, two fps reports each, so one play
   session shows what each costs.  The picture is wrong on purpose. */
enum { WHATIF_NORMAL, WHATIF_NO_BBVIEW, WHATIF_FLAT_FS, WHATIF_BOTH, WHATIF_NO_DRAWS, WHATIF_MODES };
static const char *const whatif_name[] = {
    "normal", "no back-buffer views (draws sampling the frame skipped)", "flat pixel shaders",
    "flat pixel shaders and no back-buffer views", "no draws reach the GPU"
};
static int whatif = -1;          /* the mode, -1 when off */
/* XBCOMPAT_FLUSH_DRAWS=n: hand the GPU the frame so far after every n draws
   into the back buffer (128 by default on the Pi's GPUs, 0 = never).  Mesa's VC4 driver otherwise starts on a frame only
   when something flushes it, then makes the CPU wait for it once a burst of
   small jobs (Phantom Dust's glow passes) follows; flushing early lets the
   GPU draw the scene while the CPU is still producing it, at the cost of
   storing and reloading the tiles at each split.  XBCOMPAT_WHATIF=flush
   cycles through settings instead, like the other what-if modes. */
static unsigned flush_every, flush_count;
/* Draws in this frame and the last: only a busy frame gains from splitting
   (the dashboard's ~200 draws lost 2 fps to it on the Pi). */
#define FLUSH_MIN_DRAWS 400
static unsigned frame_draws, last_frame_draws;
static const unsigned whatif_flush_steps[] = { 0, 64, 128, 256 };
static int whatif_flush = -1;    /* index into whatif_flush_steps, -1 when off */
static bool whatif_skip_draw;    /* this draw sampled a back-buffer view under WHATIF_NO_BBVIEW */
/* XBCOMPAT_WHATIF=cuts: turns the per-draw savings that skip unchanged
   constants and attribute pointers off and back on, two reports each. */
static bool whatif_cuts, cuts_off;
static void log_fps(void)
{
    static uint64_t since, cpu_since;
    static unsigned frames;
    profiling();
    static bool whatif_read;
    if (!whatif_read) {
        const char *w = getenv("XBCOMPAT_WHATIF");
        whatif_read = true;
        const char *f = getenv("XBCOMPAT_FLUSH_DRAWS");
        if (f) flush_every = (unsigned)atoi(f);
        else {
            /* Measured on the Pi 3 in a Phantom Dust fight: never 5.9 fps,
               every 64 draws 6.7, 128 7.1, 256 6.8.  Only tiling GPUs (the
               Pi's VC4 and V3D) gain from it. */
            const char *r = (const char *)glGetString(GL_RENDERER);
            if (r && (strstr(r, "VC4") || strstr(r, "V3D"))) flush_every = 128;
        }
        if (w && !strcmp(w, "cuts") && work_on) {
            whatif_cuts = true;
            xlog("D3D what-if: now per-draw cuts on");
        } else if (w && !strcmp(w, "flush") && work_on) {
            whatif_flush = 0;
            flush_every = whatif_flush_steps[0];
            xlog("D3D what-if: now flushing every %u draws (0 = never)", flush_every);
        } else if (w && *w && *w != '0' && work_on) {
            whatif = WHATIF_NORMAL;
            xlog("D3D what-if: now %s", whatif_name[0]);
        }
    }
    if (!fps_on) return;
    uint64_t now = now_ns();
    if (!since) { since = now; cpu_since = work_on ? thread_cpu_ns() : 0; }
    frames++;
    if (now - since >= 5000000000u) {
        double f = frames, ms = (now - since) / 1e6 / f;
        if (!work_on) {   /* the rate alone: counting the work costs frames */
            xlog("D3D: %.1f fps: %.2f ms a frame, %u draws (rate only)", 1000.0 / ms, ms,
                 (unsigned)(n_draws / f));
            since = now;
            n_draws = frames = 0;
            return;
        }
        if (whatif >= 0) xlog("D3D what-if: the report below ran with %s", whatif_name[whatif]);
        if (whatif_flush >= 0) xlog("D3D what-if: the report below ran flushing every %u draws (0 = never)", flush_every);
        if (whatif_cuts) xlog("D3D what-if: the report below ran with per-draw cuts %s", cuts_off ? "off" : "on");
        xlog("D3D: %.1f fps: %.1f ms a frame, %u draws %.1f ms (GL calls %.1f), present+swap %.1f ms, "
             "pacing %.1f ms, other %.1f ms; GL state calls %u, %u skipped", 1000.0 / ms, ms, (unsigned)(n_draws / f),
             t_draw / 1e6 / f, t_gl / 1e6 / f, t_present / 1e6 / f, t_pace / 1e6 / f,
             ms - (t_draw + t_present + t_pace) / 1e6 / f, (unsigned)(gc_calls / f), (unsigned)(gc_skipped / f));
        uint64_t cpu_now = thread_cpu_ns();
        double cpu = (cpu_now - cpu_since) / 1e6 / f, other = ms - (t_draw + t_present + t_pace) / 1e6 / f;
        xlog("D3D: presenting thread on the CPU %.0f%% of the frame: draws %.1f of %.1f ms, present %.1f of %.1f ms, "
             "other %.1f of %.1f ms (the rest blocked; %.1f ms of it in BlockUntilVerticalBlank)", 100.0 * cpu / ms,
             c_draw / 1e6 / f, t_draw / 1e6 / f, c_present / 1e6 / f, t_present / 1e6 / f,
             cpu - (c_draw + c_present) / 1e6 / f, other, t_vbwait / 1e6 / f);
        t_vbwait = 0;
        cpu_since = cpu_now;
        c_draw = c_present = 0;
        static unsigned traps;
        double tick = 1000.0 / (double)SDL_GetPerformanceFrequency() / f;
        xlog("D3D: per frame %u texture uploads, %u readbacks, %u shader compiles, %u guest traps; "
             "push buffer %.1f ms (draws included)%s", (unsigned)(stats.uploads / f), (unsigned)(stats.readbacks / f),
             (unsigned)(stats.shaders / f), (unsigned)((g_guest_traps - traps) / f), prof.drain * tick,
             prof_sync ? " [GPU synced per draw]" : "");
        traps = g_guest_traps;
        hud_profile_log(f);
        if (detail_on) program_profile_log(f);
        memset(&prof, 0, sizeof(prof));
        memset(&stats, 0, sizeof(stats));
        if (detail_on) {
            xlog("D3D detail: resolve %.2f filter %.2f swap %.2f restore %.2f; state %.2f program %.2f constants %.2f attributes %.2f indices %.2f ms/frame",
                 t_resolve / 1e6 / f, t_filter / 1e6 / f, t_swap / 1e6 / f, t_restore / 1e6 / f,
                 t_state / 1e6 / f, t_program / 1e6 / f, t_constants / 1e6 / f, t_attributes / 1e6 / f, t_indices / 1e6 / f);
            xlog("D3D cache: hits %.1f misses %.1f evictions %.1f uploads %.1f bytes %.0f per frame",
                 cache_hits / f, cache_misses / f, cache_evicts / f, cache_uploads / f, cache_upload_bytes / f);
            cache_hits = cache_misses = cache_evicts = cache_uploads = 0; cache_upload_bytes = 0;
            t_resolve = t_filter = t_swap = t_restore = 0;
            xlog("D3D buffers: upload calls %.2f ms/frame", t_buffer_upload / 1e6 / f);
            xlog("D3D program parts: fragment shader lookup %.2f, textures %.2f ms/frame", t_fshader / 1e6 / f,
                 t_textures / 1e6 / f);
            t_fshader = t_textures = 0;
            xlog("D3D prepared state: reused %.1f rebuilt %.1f per frame", fvf_plan_hits / f, fvf_plan_misses / f);
            t_buffer_upload = 0;
            t_state = t_program = t_constants = t_attributes = t_indices = 0;
        }
        fvf_plan_hits = fvf_plan_misses = 0;
        gc_calls = gc_skipped = 0;
        static unsigned whatif_reports;
        if (whatif >= 0 && ++whatif_reports % 2 == 0) {
            whatif = (whatif + 1) % WHATIF_MODES;
            xlog("D3D what-if: now %s", whatif_name[whatif]);
        }
        if (whatif_flush >= 0 && ++whatif_reports % 2 == 0) {
            whatif_flush = (whatif_flush + 1) % (int)(sizeof whatif_flush_steps / sizeof *whatif_flush_steps);
            flush_every = whatif_flush_steps[whatif_flush];
            xlog("D3D what-if: now flushing every %u draws (0 = never)", flush_every);
        }
        if (whatif_cuts && ++whatif_reports % 2 == 0) {
            cuts_off = gc_vap_bypass = !cuts_off;
            xlog("D3D what-if: now per-draw cuts %s", cuts_off ? "off" : "on");
        }
        since = now;
        frames = 0;
        t_draw = t_gl = t_present = t_pace = 0;
        n_draws = 0;
    }
}
#define DETAIL_TIMED(acc, ...) do { if (detail_on) { uint64_t td_ = now_ns(); __VA_ARGS__; acc += now_ns() - td_; } \
                                    else { __VA_ARGS__; } } while (0)
#define TIMED(acc, ...) do { if (profiling()) { uint64_t t0_ = now_ns(); __VA_ARGS__; acc += now_ns() - t0_; } \
                             else { __VA_ARGS__; } } while (0)
/* TIMED, and the thread's CPU time into cacc when PROFILE_WORK is on. */
#define TIMED_CPU(acc, cacc, ...) do { if (profiling()) { uint64_t c0_ = work_on ? thread_cpu_ns() : 0; \
                                           uint64_t t0_ = now_ns(); __VA_ARGS__; acc += now_ns() - t0_; \
                                           if (work_on) cacc += thread_cpu_ns() - c0_; } \
                                       else { __VA_ARGS__; } } while (0)

static void pace_present(void)
{
    static Uint64 next;
    log_fps();
    if (unpaced()) return;
    Uint64 now = SDL_GetPerformanceCounter(), period = SDL_GetPerformanceFrequency() / 60 * present_vblanks;
    if (!next || now > next + period) next = now;   /* first frame, or fell behind: resync */
    TIMED(t_pace, sleep_until(next));
    next += period;
}

#include "gpu_counters.h"

/* Swap in three parts, so that with XBCOMPAT_ASYNC_DRAWS the title's thread
   can run the first and last while the render worker presents. */
static void swap_begin_frame(void)
{
    d3d.frame++;
    __atomic_store_n(&published_frame_count, d3d.frame, __ATOMIC_RELEASE);
    if (d3d.frame == 1) xlog("D3D: first Present");
    {
        /* Debug aid: XBCOMPAT_TRACE_FRAME=n turns --trace on for frame n only. */
        static long trace_frame = -2;
        if (trace_frame == -2) { const char *e = getenv("XBCOMPAT_TRACE_FRAME"); trace_frame = e ? atol(e) : -1; }
        if (trace_frame >= 0) g_trace = (long)d3d.frame == trace_frame;
        /* Or trace the next frame whenever this file appears (it is removed):
           for catching a frame in the middle of play. */
        static int traced_by_file;
        if (traced_by_file) { g_trace = 0; traced_by_file = 0; }
        if (trace_boot_frames && d3d.frame >= trace_boot_frames) {
            trace_boot_frames = 0;
            g_trace = 0;
            xlog("D3D: boot trace ends at frame %lu", (unsigned long)d3d.frame);
        }
        if (trace_frame < 0 && !access("/tmp/xbcompat-trace-frame", F_OK) && !unlink("/tmp/xbcompat-trace-frame")) {
            g_trace = 1;
            traced_by_file = 1;
            xlog("D3D: tracing frame %lu", (unsigned long)d3d.frame);
        }
    }
}

/* The GPU's part: the frame goes to the screen. */
static void guide_overlay(void);

static void swap_present(void)
{
    TIMED_CPU(t_present, c_present, present_window_framebuffer(); guide_overlay(); cpu_block(); DETAIL_TIMED(t_swap, SDL_GL_SwapWindow(d3d.window));
          DETAIL_TIMED(t_restore, restore_window_framebuffer()));
    restore_overlay();
    pace_present();
}

/* XBCOMPAT_SHOT_ON_REQUEST=BASE: while BASE.req exists, the next frame is
   saved to BASE.bmp and BASE.req removed, so a run can be looked at over ssh
   (`touch BASE.req`, wait for it to go, fetch BASE.bmp). */
static void shot_on_request(void)
{
    static const char *base;
    static int checked;
    if (!checked) {
        checked = 1;
        base = getenv("XBCOMPAT_SHOT_ON_REQUEST");
        if (base && !*base) base = NULL;
    }
    if (!base) return;
    char req[512], tmp[512], out[512];
    snprintf(req, sizeof req, "%s.req", base);
    if (access(req, F_OK) != 0) return;
    snprintf(tmp, sizeof tmp, "%s.tmp.bmp", base);
    snprintf(out, sizeof out, "%s.bmp", base);
    save_screenshot(tmp);
    rename(tmp, out);
    unlink(req);
}

static void swap_screenshot(void)
{
    present_overlay();
    shot_on_request();
    if (g_screenshot_path && strstr(g_screenshot_path, "%d")) {
        /* A path with %d saves every --shot-frame frames, numbered by frame. */
        if (g_screenshot_frame > 0 && d3d.frame % g_screenshot_frame == 0) {
            char path[512];
            snprintf(path, sizeof path, g_screenshot_path, (int)d3d.frame);
            save_screenshot(path);
        }
    } else if (g_screenshot_path && (int)d3d.frame == g_screenshot_frame) {
        save_screenshot(g_screenshot_path);
    }
}

static void swap_gpu_frame_end(void)
{
    last_frame_draws = frame_draws;
    frame_draws = flush_count = 0;
    gpu_counters_frame();
    cpu_compare_audit_frame(d3d.frame);
}

/* Window events; true when the window was closed.  Only the thread that
   made the window reads them (SDL wants that); another thread's Swap leaves
   them for the next. */
static bool swap_window_events(void)
{
    SDL_Event e;
    bool quit = false;
    if (!pthread_equal(pthread_self(), gl_creator)) return false;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) quit = true;
    return quit;
}

/* The title's part after the frame is presented. */
static void swap_finish(void)
{
    d3dv.swapped = 1;
    PVOID swap_cb = d3dv.miniport_swap_cb && d3dv.miniport_swap_cb[0] ? (PVOID)d3dv.miniport_swap_cb[0]
                                                                     : d3dv.swap_callback;
    if (swap_cb) {
        /* D3DSWAPDATA: Swap, SwapVBlank, MissedVBlanks, TimeUntilSwapVBlank, TimeBetweenSwapVBlanks */
        ULONG vb = d3dv.vblank_count, missed = vb - d3dv.vblank_at_swap > 1 ? vb - d3dv.vblank_at_swap - 1 : 0;
        ULONG data[5] = { d3d.frame, vb, missed, 0, 16667 };
        d3dv.vblank_at_swap = vb;
        gl_no_yield++;
#ifdef XBC_TRANSLATED
        CPU_CALL(swap_cb, CONV_CDECL, (uint32_t)data);
#else
        ((void (CDECLAPI *)(ULONG *))swap_cb)(data);
#endif
        gl_no_yield--;
    }
}

static void swap_checks(bool quit)
{
    if (quit) {
        xlog("window closed");
        xbc_exit(0);
    }
    /* A keyboard plugged in mid-game makes SDL take the fault signals back. */
    install_fault_handlers();
    xinput_check_reset_combo();
    reset_check();
}

static void swap_exit_check(void)
{
    if (g_exit_after_frames && (int)d3d.frame >= g_exit_after_frames) {
        xlog("D3D: %u frames presented, exiting", d3d.frame);
        /* Leave the way a title does: XBCOMPAT_PERSIST=1 persists the frame first. */
        if (getenv("XBCOMPAT_PERSIST")) D3DDevice_PersistDisplay();
        av_hand_over();
        xbc_exit(0);
    }
}

static ULONG NTAPI D3DDevice_Swap(ULONG Flags)
{
    gl_require();
    (void)Flags;
    if (async_front()) return async_swap();
    pusher_drain();
    hud_batch_flush();
    swap_cpu_backbuffer();
    flush_cpu_backbuffer();
    front_buffer_overlay();
    front_buffer_update();
    swap_begin_frame();
    swap_gpu_frame_end();
    ke_frame_presented();
    run_callbacks();
    swap_screenshot();
    swap_present();
    persisted_first_present();
    swap_finish();
    swap_checks(swap_window_events());
    swap_exit_check();
    return d3d.frame;
}

/* The window clip (SetScissors) as GL scissor boxes: none (n < 0) with no
   rectangles set, which leaves the viewport; else disjoint boxes covering
   the union of the rectangles (inclusive) or the target outside all of them
   (exclusive).  GL has one scissor box, so a draw runs once per box
   (scissor_pass); the boxes don't overlap, so blending sees each pixel once. */
#define SCISSOR_BOXES (17 * 17)
static struct { int n; GLint box[SCISSOR_BOXES][4]; } scissor_boxes = { -1, { { 0 } } };

static int long_cmp(const void *a, const void *b)
{
    LONG x = *(const LONG *)a, y = *(const LONG *)b;
    return x < y ? -1 : x > y;
}

static void scissor_box_add(LONG x1, LONG y1, LONG x2, LONG y2)
{
    /* Grow a box from the band above when it spans the same columns. */
    for (int i = 0; i < scissor_boxes.n; i++) {
        GLint *b = scissor_boxes.box[i];
        if (b[0] == x1 && b[2] == x2 - x1 && b[1] + b[3] == y1) { b[3] = y2 - b[1]; return; }
    }
    if (scissor_boxes.n == SCISSOR_BOXES) return;
    GLint *b = scissor_boxes.box[scissor_boxes.n++];
    b[0] = x1; b[1] = y1; b[2] = x2 - x1; b[3] = y2 - y1;
}

static void scissor_build(void)
{
    ULONG count = d3d.scissors.count > 8 ? 8 : d3d.scissors.count;
    const D3DRECT *r = d3d.scissors.rects;
    scissor_boxes.n = -1;
    if (!count) return;
    scissor_boxes.n = 0;
    if (count == 1 && !d3d.scissors.exclusive) {
        scissor_boxes.n = 1;
        GLint *b = scissor_boxes.box[0];
        b[0] = r->x1; b[1] = r->y1; b[2] = r->x2 - r->x1; b[3] = r->y2 - r->y1;
    } else {
        /* Cut the target along every rectangle edge; each cell is wholly
           inside or outside each rectangle.  Rows of wanted cells become
           spans, and spans repeating down the bands one box. */
        LONG w = d3d.rt_width, h = d3d.rt_height, xs[18], ys[18];
        unsigned nx = 0, ny = 0;
        xs[nx++] = 0; xs[nx++] = w; ys[ny++] = 0; ys[ny++] = h;
        for (ULONG i = 0; i < count; i++) {
            xs[nx++] = r[i].x1 < 0 ? 0 : r[i].x1 > w ? w : r[i].x1;
            xs[nx++] = r[i].x2 < 0 ? 0 : r[i].x2 > w ? w : r[i].x2;
            ys[ny++] = r[i].y1 < 0 ? 0 : r[i].y1 > h ? h : r[i].y1;
            ys[ny++] = r[i].y2 < 0 ? 0 : r[i].y2 > h ? h : r[i].y2;
        }
        qsort(xs, nx, sizeof xs[0], long_cmp);
        qsort(ys, ny, sizeof ys[0], long_cmp);
        for (unsigned j = 0; j + 1 < ny; j++) {
            if (ys[j] == ys[j + 1]) continue;
            LONG start = -1;
            for (unsigned i = 0; i + 1 < nx; i++) {
                if (xs[i] == xs[i + 1]) continue;
                bool inside = false;
                for (ULONG k = 0; k < count && !inside; k++)
                    inside = xs[i] >= r[k].x1 && xs[i + 1] <= r[k].x2 && ys[j] >= r[k].y1 && ys[j + 1] <= r[k].y2;
                bool want = inside != (d3d.scissors.exclusive != 0);
                if (want && start < 0) start = xs[i];
                if (!want && start >= 0) {
                    scissor_box_add(start, ys[j], xs[i], ys[j + 1]);
                    start = -1;
                }
            }
            if (start >= 0) scissor_box_add(start, ys[j], xs[nx - 1], ys[j + 1]);
        }
    }
    /* To GL's window coordinates (rows bottom-up) unless drawing into a
       texture, which is stored flipped. */
    if (!d3d.rt_texture)
        for (int i = 0; i < scissor_boxes.n; i++) {
            GLint *b = scissor_boxes.box[i];
            b[1] = d3d.rt_height - (b[1] + b[3]);
        }
}

/* Before each GL draw call: false once the draw has gone to every box. */
static bool scissor_pass(unsigned pass)
{
    if (scissor_boxes.n < 0) return pass == 0;
    if ((int)pass >= scissor_boxes.n) return false;
    const GLint *b = scissor_boxes.box[pass];
    glScissor(b[0], b[1], b[2], b[3]);
    return true;
}

static void apply_viewport(void)
{
    hud_batch_flush();
    D3DVIEWPORT8 *v = &d3d.viewport;
    /* GL rows run bottom-up: flip the window viewport; a texture target is
       drawn upside down instead (see apply_render_states). */
    if (d3d.rt_texture) glViewport(v->X, v->Y, v->Width, v->Height);
    else glViewport(v->X, d3d.rt_height - (v->Y + v->Height), v->Width, v->Height);
    glDepthRange(v->MinZ, v->MaxZ);
    scissor_build();
    if (scissor_boxes.n >= 0) {
        glEnable(GL_SCISSOR_TEST);
        scissor_pass(0);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

static unsigned clear_serial;   /* bumped per Clear: see backbuffer_copy_texture */

/* A frame with no draws or clears whose back buffer memory changed since it
   last matched GL: the CPU wrote it through a pointer kept from an earlier
   lock, and the hardware would show that.  Splinter Cell presents for
   seconds without drawing while a level loads. */
static bool frame_idle;   /* this Swap's frame had no draws or clears */

/* Every byte of the back buffer's memory, for runs of frames with no draws:
   the sampled hash can miss a few rows of progress bar. */
static uint64_t backbuffer_memory_hash_full(void)
{
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)d3d.backbuffer, &w, &h, &pitch);
    if (h > (ULONG)d3d.height) h = d3d.height;
    const uint64_t *q = (const uint64_t *)(d3d.backbuffer->Data | CONTIG_BASE);
    uint64_t x = 0xcbf29ce484222325ull;
    for (size_t i = 0, n = (size_t)pitch * h / 8; i < n; i++) x = (x ^ q[i]) * 0x100000001b3ull;
    return x;
}

static void swap_cpu_backbuffer(void)
{
    static unsigned long draws;
    static unsigned clears;
    static bool was_idle;
    static uint64_t idle_hash;
    bool idle = draws == draw_serial && clears == clear_serial;
    draws = draw_serial;
    clears = clear_serial;
    /* Idle for the front buffer copy too, unless CPU writes go out. */
    frame_idle = idle && !d3d.bb_cpu_dirty;
    bool prev_idle = was_idle;
    was_idle = false;
    if (!idle || d3d.bb_cpu_dirty || !d3d.backbuffer || d3d.backbuffer->Data != bbsync.data) return;
    uint64_t full = backbuffer_memory_hash_full();
    bool changed = backbuffer_memory_hash() != bbsync.hash || (prev_idle && full != idle_hash);
    was_idle = true;
    idle_hash = full;
    TRACE_ALL("D3D: frame with no draws, back buffer memory %s", changed ? "written by the CPU, shown" : "unchanged");
    if (changed) d3d.bb_cpu_dirty = true, frame_idle = false;
}
static void NTAPI D3DDevice_Clear(ULONG Count, const D3DRECT *pRects, ULONG Flags, ULONG Color, float Z,
                                  ULONG Stencil)
{
    gl_require();
    pusher_drain();
    if (async_front()) { async_clear(Count, pRects, Flags, Color, Z, Stencil); return; }
    hud_batch_flush();
    clear_serial++;
    if (d3d.recording) {
        ULONG h[5] = { Count, Flags, Color, 0, Stencil };
        memcpy(&h[3], &Z, 4);
        pb_record2(OP_CLEAR, h, sizeof(h), pRects, Count && pRects ? Count * sizeof(D3DRECT) : 0);
        return;
    }
    TRACE_ALL("D3D: Clear(%u rects, flags %#x, color %#x, z %g)", Count, Flags, Color, Z);
    if ((Flags & 3) && d3d.target_depth && d3d.target_depth == d3d.depth) depth_gpu_writes++;
    flush_cpu_backbuffer();   /* earlier CPU writes go under the clear, not over it at Present */
    gc_enable(GL_DITHER, RS(D3DRS_DITHERENABLE) != 0);   /* the clear dithers as draws do */
    GLbitfield mask = 0;
    if (Flags & 0xF0) {
        glClearColor(((Color >> 16) & 255) / 255.0f, ((Color >> 8) & 255) / 255.0f, (Color & 255) / 255.0f,
                     (Color >> 24) / 255.0f);
        glColorMask(!!(Flags & 0x10), !!(Flags & 0x20), !!(Flags & 0x40), !!(Flags & 0x80));
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (Flags & 1) { glClearDepth(Z); glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    if (Flags & 2) { glClearStencil(Stencil); glStencilMask(0xFF); mask |= GL_STENCIL_BUFFER_BIT; }
    /* The library clips each rectangle, and the whole-target clear without
       any, to the viewport; the scissors play no part.  GL's clear ignores
       the viewport, so without this a clear of one viewport (a shadow
       buffer tile, a split screen) wiped the whole target, and a clear
       after a draw kept that draw's scissor. */
    const D3DVIEWPORT8 *v = &d3d.viewport;
    D3DRECT whole = { (LONG)v->X, (LONG)v->Y, (LONG)(v->X + v->Width), (LONG)(v->Y + v->Height) };
    if (!Count || !pRects) { Count = 1; pRects = &whole; }
    for (ULONG i = 0; i < Count; i++) {
        D3DRECT r = pRects[i];
        if (r.x1 < whole.x1) r.x1 = whole.x1;
        if (r.y1 < whole.y1) r.y1 = whole.y1;
        if (r.x2 > whole.x2) r.x2 = whole.x2;
        if (r.y2 > whole.y2) r.y2 = whole.y2;
        if (r.x1 >= r.x2 || r.y1 >= r.y2) continue;
        if (r.x1 <= 0 && r.y1 <= 0 && r.x2 >= (LONG)d3d.rt_width && r.y2 >= (LONG)d3d.rt_height) {
            glDisable(GL_SCISSOR_TEST);   /* the whole target: a plain (fast) clear */
        } else {
            glEnable(GL_SCISSOR_TEST);
            if (d3d.rt_texture) glScissor(r.x1, r.y1, r.x2 - r.x1, r.y2 - r.y1);
            else glScissor(r.x1, d3d.rt_height - r.y2, r.x2 - r.x1, r.y2 - r.y1);
        }
        glClear(mask);
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
}

/* ---- resources --------------------------------------------------------- */

static void color4(float *out, ULONG c);

static PVOID resource_data(D3DResource *r)
{
    /* Like the real library, Data holds a physical address. */
    return (PVOID)(r->Data | CONTIG_BASE);
}

static LONG NTAPI D3DDevice_CreateVertexBuffer(UINT_ Length, ULONG Usage, ULONG FVF, ULONG Pool,
                                               D3DResource **ppVB)
{
    (void)Usage; (void)FVF; (void)Pool;
    D3DResource *vb = pool_alloc(sizeof(*vb));
    PVOID mem = MmAllocateContiguousMemoryEx(Length, 0, 0x7FFFFFFF, 0, 4);
    if (!vb || !mem) return 0x8007000E; /* E_OUTOFMEMORY */
    vb->Common = 1 | D3DCOMMON_TYPE_VERTEXBUFFER | D3DCOMMON_D3DCREATED;
    vb->Data = (ULONG)mem & 0x7FFFFFFF;
    vb->Lock = 0;
    *ppVB = vb;
    TRACE_ALL("CreateVertexBuffer(%u) = %p", Length, (void *)vb);
    return D3D_OK;
}

static void note_vb_lock(const UCHAR *p, size_t len);

static void NTAPI D3DVertexBuffer_Lock(D3DResource *vb, UINT_ Offset, UINT_ Size, PVOID *ppbData, ULONG Flags)
{
    *ppbData = (UCHAR *)resource_data(vb) + Offset;
    /* Size 0 locks to the end of the buffer.  The cached GL copies of what
       a write lock covers are compared in full at their next draw. */
    if (!(Flags & 0x80 /* D3DLOCK_READONLY */)) note_vb_lock(*ppbData, Size);
}

static ULONG NTAPI D3DResource_AddRef(D3DResource *r)
{
    /* A surface holds one reference on its parent while anyone holds it. */
    if (!(r->Common & D3DCOMMON_REFCOUNT_MASK) && (r->Common & D3DCOMMON_TYPE_MASK) == D3DCOMMON_TYPE_SURFACE &&
        ((D3DSurface *)r)->Parent)
        D3DResource_AddRef(&((D3DSurface *)r)->Parent->res);
    r->Common++;
    return r->Common & D3DCOMMON_REFCOUNT_MASK;
}

static void pushbuffer_free(struct D3DPushBuffer *pb);

/* Headers the title allocated itself (an LTCG build's inlined create
   functions) stay its own; only the data goes back. */
static void res_free_header(D3DResource *r)
{
    if (pool_owns(r)) pool_free(r);
}

/* Free a resource whose references are all gone.  A surface's reference on
   its parent has already been dropped by whoever released it. */
static void destroy_resource(D3DResource *r)
{
    hud_batch_flush();   /* a queued HUD draw may still read it */
    if (!(r->Common & D3DCOMMON_D3DCREATED)) return;
    ULONG type = r->Common & D3DCOMMON_TYPE_MASK;
    if (type == D3DCOMMON_TYPE_VERTEXBUFFER || type == D3DCOMMON_TYPE_PALETTE) {
        MmFreeContiguousMemory(resource_data(r));
        res_free_header(r);
    } else if (type == D3DCOMMON_TYPE_INDEXBUFFER) {
        MmFreeContiguousMemory((PVOID)r->Data);
        res_free_header(r);
    } else if (type == D3DCOMMON_TYPE_TEXTURE) {
        tex_invalidate(r->Data);
        MmFreeContiguousMemory(resource_data(r));
        res_free_header(r);
    } else if (type == D3DCOMMON_TYPE_PUSHBUFFER) {
        pushbuffer_free((struct D3DPushBuffer *)r);
    } else if (type == D3DCOMMON_TYPE_FIXUP) {
        res_free_header(r);
    } else if (type == D3DCOMMON_TYPE_SURFACE) {
        D3DSurface *s = (D3DSurface *)r;
        if (s == d3d.backbuffer || s == d3d.depth) return;
        if (!s->Parent) {
            tex_invalidate(r->Data);   /* its GL copy, if it was a render target */
            MmFreeContiguousMemory(resource_data(r));
        }
        res_free_header(r);
    }
}

/* The library's own reference counts (D3DCOMMON_INTREFCOUNT): a render
   target is held this way, so the title can release its surface while the
   device still draws into it. */
#define D3DCOMMON_INTREFCOUNT_MASK 0x00780000
#define D3DCOMMON_INTREFCOUNT_1    0x00080000

static void internal_release(D3DResource *r)
{
    r->Common -= D3DCOMMON_INTREFCOUNT_1;
    if (!(r->Common & (D3DCOMMON_INTREFCOUNT_MASK | D3DCOMMON_REFCOUNT_MASK))) destroy_resource(r);
}

static void internal_addref_surface(D3DSurface *s)
{
    if (!(s->Common & D3DCOMMON_INTREFCOUNT_MASK) && s->Parent) s->Parent->res.Common += D3DCOMMON_INTREFCOUNT_1;
    s->Common += D3DCOMMON_INTREFCOUNT_1;
}

static void internal_release_surface(D3DSurface *s)
{
    if ((s->Common & D3DCOMMON_INTREFCOUNT_MASK) == D3DCOMMON_INTREFCOUNT_1) {
        if (s->Parent) internal_release(&s->Parent->res);
        if (!(s->Common & D3DCOMMON_REFCOUNT_MASK)) { destroy_resource((D3DResource *)s); return; }
    }
    s->Common -= D3DCOMMON_INTREFCOUNT_1;
}

static ULONG NTAPI D3DResource_Release(D3DResource *r)
{
    TRACE_ALL("D3D: Release(%p) type %#x refs %u from %p", (void *)r, r->Common & D3DCOMMON_TYPE_MASK,
          (r->Common & D3DCOMMON_REFCOUNT_MASK) - 1, __builtin_return_address(0));
    /* Queued drawing: the last reference goes in the queue, after the draws
       that may still use the resource; any other is only a count. */
    if (async_front() && (r->Common & D3DCOMMON_REFCOUNT_MASK) == 1) {
        async_release(r);
        return 0;
    }
    if ((r->Common & D3DCOMMON_REFCOUNT_MASK) == 1) {
        /* The last outside reference on a surface drops its parent's. */
        if ((r->Common & D3DCOMMON_TYPE_MASK) == D3DCOMMON_TYPE_SURFACE && ((D3DSurface *)r)->Parent)
            D3DResource_Release(&((D3DSurface *)r)->Parent->res);
        if (!(r->Common & D3DCOMMON_INTREFCOUNT_MASK)) {
            r->Common--;
            destroy_resource(r);
            return 0;
        }
    }
    if (!(r->Common & D3DCOMMON_REFCOUNT_MASK)) return 0;
    return --r->Common & D3DCOMMON_REFCOUNT_MASK;
}

static ULONG NTAPI D3DResource_GetType(D3DResource *r)
{
    if ((r->Common & D3DCOMMON_TYPE_MASK) == 0x00040000) {
        ULONG format = ((ULONG *)r)[3];
        if (format & 0x4) return 5;                     /* D3DRTYPE_CUBETEXTURE */
        if (((format >> 4) & 0xF) == 3) return 4;       /* D3DRTYPE_VOLUMETEXTURE */
        return 3;                                       /* D3DRTYPE_TEXTURE */
    }
    static const ULONG types[] = { 6 /* VERTEXBUFFER */, 7 /* INDEXBUFFER */, 10 /* PUSHBUFFER */,
                                   9 /* PALETTE */, 3 /* TEXTURE */, 1 /* SURFACE */, 11 /* FIXUP */ };
    return types[(r->Common & D3DCOMMON_TYPE_MASK) >> 16];
}


static PVOID NTAPI D3D_AllocContiguousMemory(ULONG Size, ULONG Alignment)
{
    return MmAllocateContiguousMemoryEx(Size, 0, 0x7FFFFFFF, Alignment, 4);
}

static void NTAPI D3D_FreeContiguousMemory(PVOID p)
{
    MmFreeContiguousMemory(p);
}

/* Texture data registered again, for the next texture lookup (on the GL
   thread: titles load bundles on threads of their own) to drop the GL
   copies of. */
#define TEX_STALE 4096
static ULONG tex_stale[TEX_STALE];
static unsigned tex_stale_put, tex_stale_got;

static void tex_stale_note(ULONG data)
{
    unsigned n = __atomic_load_n(&tex_stale_put, __ATOMIC_RELAXED);
    tex_stale[n % TEX_STALE] = data;
    __atomic_store_n(&tex_stale_put, n + 1, __ATOMIC_RELEASE);
}

static void tex_stale_drop(void);

static void NTAPI D3DResource_Register(D3DResource *r, PVOID base)
{
    ULONG mem = (ULONG)base + r->Data, type = r->Common & D3DCOMMON_TYPE_MASK;
    /* Push and index buffers keep a virtual address (the CPU reads them);
       everything else a GPU (physical) one. */
    r->Data = type == D3DCOMMON_TYPE_PUSHBUFFER || type == D3DCOMMON_TYPE_INDEXBUFFER ? mem : (mem & 0x7FFFFFFF);
    /* A bundle (XPR) loaded again into the same memory registers the same
       addresses over new contents: the GL copies made from the old ones go. */
    if (type == D3DCOMMON_TYPE_TEXTURE || type == D3DCOMMON_TYPE_SURFACE) tex_stale_note(r->Data);
    else if (type == D3DCOMMON_TYPE_VERTEXBUFFER || type == D3DCOMMON_TYPE_INDEXBUFFER)
        note_vb_lock((const UCHAR *)resource_data(r), 0);
}

/* ---- textures ---------------------------------------------------------- */


typedef struct tex_entry {
    ULONG data, format, size;
    ULONG pal;           /* P8 textures: a hash of the palette they were expanded with */
    GLuint id;
    GLenum target;
    struct tex_entry *next;
} tex_entry;

/* Kept in buckets by data address: a draw looks up each stage's texture,
   and walking one list of every texture a level loaded (hundreds) per stage
   per draw was a measurable part of the Pi's frame. */
#define TEX_BUCKETS 1024
static tex_entry *tex_buckets[TEX_BUCKETS];
static inline tex_entry **tex_bucket(ULONG data) { return &tex_buckets[(data * 2654435761u) >> 22]; }
static unsigned tex_cache_version;   /* bumped whenever a cached texture goes away */

/* The palette of the stage whose texture is being bound (P8 textures). */
static const uint32_t *tex_palette;
static ULONG tex_palette_count;

static void tex_use_palette(int stage)
{
    if (hud_packet_active) return;   /* the packet carries its own textures */
    D3DPalette *p = stage < 4 ? d3d.palettes[stage] : NULL;
    tex_palette = p ? (const uint32_t *)(p->Data | CONTIG_BASE) : NULL;
    tex_palette_count = p ? 256u >> (p->Common >> 30) : 0;
}

static ULONG palette_hash(void)
{
    ULONG h = 2166136261u;
    for (ULONG i = 0; tex_palette && i < tex_palette_count; i++) h = (h ^ tex_palette[i]) * 16777619u;
    return h | 1;
}

/* Forget the GL copy of a texture the title is about to modify. */
static void tex_invalidate(ULONG data)
{
    tex_cache_version++;
    hud_batch_flush();
    tex_entry **pp = tex_bucket(data);
    while (*pp) {
        tex_entry *e = *pp;
        if (e->data == data) {
            gl_delete_texture(e->id);
            *pp = e->next;
            free(e);
        } else {
            pp = &e->next;
        }
    }
}

/* NV2A swizzled textures store texels in Morton order: the bits of x and y
   interleave (x first) until the smaller dimension runs out. */
static void unswizzle(const uint8_t *src, uint8_t *dst, ULONG w, ULONG h, ULONG bpp)
{
    ULONG xmask = 0, ymask = 0;
    for (ULONG bit = 1, i = 1; i < w || i < h; i <<= 1) {
        if (i < w) { xmask |= bit; bit <<= 1; }
        if (i < h) { ymask |= bit; bit <<= 1; }
    }
    ULONG sy = 0;
    for (ULONG y = 0; y < h; y++) {
        ULONG sx = 0;
        for (ULONG x = 0; x < w; x++) {
            memcpy(dst + (y * w + x) * bpp, src + (sx | sy) * bpp, bpp);
            sx = (sx - xmask) & xmask;
        }
        sy = (sy - ymask) & ymask;
    }
}

/* The inverse of unswizzle: linear rows into Morton order. */
static void swizzle(const uint8_t *src, ULONG src_pitch, uint8_t *dst, ULONG w, ULONG h, ULONG bpp)
{
    ULONG xmask = 0, ymask = 0;
    for (ULONG bit = 1, i = 1; i < w || i < h; i <<= 1) {
        if (i < w) { xmask |= bit; bit <<= 1; }
        if (i < h) { ymask |= bit; bit <<= 1; }
    }
    ULONG sy = 0;
    for (ULONG y = 0; y < h; y++) {
        ULONG sx = 0;
        for (ULONG x = 0; x < w; x++) {
            memcpy(dst + (sx | sy) * bpp, src + y * src_pitch + x * bpp, bpp);
            sx = (sx - xmask) & xmask;
        }
        sy = (sy - ymask) & ymask;
    }
}

/* Volume textures swizzle x, y and z bits in turn while each axis has bits left. */
static void unswizzle3d(const uint8_t *src, uint8_t *dst, ULONG w, ULONG h, ULONG d, ULONG bpp)
{
    ULONG xmask = 0, ymask = 0, zmask = 0;
    for (ULONG bit = 1, i = 1; i < w || i < h || i < d; i <<= 1) {
        if (i < w) { xmask |= bit; bit <<= 1; }
        if (i < h) { ymask |= bit; bit <<= 1; }
        if (i < d) { zmask |= bit; bit <<= 1; }
    }
    ULONG sz = 0;
    for (ULONG z = 0; z < d; z++) {
        ULONG sy = 0;
        for (ULONG y = 0; y < h; y++) {
            ULONG sx = 0;
            for (ULONG x = 0; x < w; x++) {
                memcpy(dst + ((z * h + y) * w + x) * bpp, src + (sx | sy | sz) * bpp, bpp);
                sx = (sx - xmask) & xmask;
            }
            sy = (sy - ymask) & ymask;
        }
        sz = (sz - zmask) & zmask;
    }
}

/* GL 1.3 / 2.0 entry points, fetched at device creation. */
static GLuint (APIENTRY *p_glCreateShader)(GLenum);
static void (APIENTRY *p_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (APIENTRY *p_glCompileShader)(GLuint);
static void (APIENTRY *p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (APIENTRY *p_glCreateProgram)(void);
static void (APIENTRY *p_glAttachShader)(GLuint, GLuint);
static void (APIENTRY *p_glBindAttribLocation)(GLuint, GLuint, const char *);
static GLint (APIENTRY *p_glGetAttribLocation)(GLuint, const char *);
static void (APIENTRY *p_glLinkProgram)(GLuint);
static void (APIENTRY *p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static void (APIENTRY *p_glUseProgram)(GLuint);
static void (APIENTRY *p_glDeleteShader)(GLuint);
static void (APIENTRY *p_glDeleteProgram)(GLuint);
static GLint (APIENTRY *p_glGetUniformLocation)(GLuint, const char *);
static void (APIENTRY *p_glUniform4fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *p_glUniform1f)(GLint, GLfloat);
static void (APIENTRY *p_glUniform1i)(GLint, GLint);
static void (APIENTRY *p_glEnableVertexAttribArray)(GLuint);
static void (APIENTRY *p_glDisableVertexAttribArray)(GLuint);
static void (APIENTRY *p_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (APIENTRY *p_glVertexAttrib4fv)(GLuint, const GLfloat *);
static void (APIENTRY *p_glActiveTexture)(GLenum);
static void (APIENTRY *p_glClientActiveTexture)(GLenum);
static void (APIENTRY *p_glMultiTexCoord4fv)(GLenum, const GLfloat *);
static void (APIENTRY *p_glPointParameterfv)(GLenum, const GLfloat *);
static void (APIENTRY *p_glGenQueries)(GLsizei, GLuint *);
static void (APIENTRY *p_glSecondaryColorPointer)(GLint, GLenum, GLsizei, const void *);
static void (APIENTRY *p_glBlendEquation)(GLenum);
static void (APIENTRY *p_glBlendColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (APIENTRY *p_glBeginQuery)(GLenum, GLuint);
static void (APIENTRY *p_glEndQuery)(GLenum);
static void (APIENTRY *p_glGetQueryObjectuiv)(GLuint, GLenum, GLuint *);
static void (APIENTRY *p_glPointParameterf)(GLenum, GLfloat);
static void (APIENTRY *p_glUniform2fv)(GLint, GLsizei, const GLfloat *);
static void (APIENTRY *p_glGenBuffers)(GLsizei, GLuint *);
static void (APIENTRY *p_glGetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
static void (APIENTRY *p_glGetShaderSource)(GLuint, GLsizei, GLsizei *, char *);
static void (APIENTRY *p_glDeleteBuffers)(GLsizei, const GLuint *);
static void (APIENTRY *p_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
static void (APIENTRY *p_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);

static void container_size(const D3DPixelContainer *t, ULONG *w, ULONG *h, ULONG *pitch);
static ULONG level_offset(const D3DPixelContainer *t, ULONG level);
static ULONG level_count(const D3DPixelContainer *t);

/* The GL texture target a pixel container needs. */
static GLenum tex_target(const D3DPixelContainer *t)
{
    if (t->Format & 0x4) return GL_TEXTURE_CUBE_MAP;
    if (((t->Format >> 4) & 0xF) == 3) return GL_TEXTURE_3D;
    return GL_TEXTURE_2D;
}

/* Upload one image (a texture level, one cube map face, or a whole volume
   when target is GL_TEXTURE_3D and d > 1) to `target`. */
/* D24S8, F24S8, D16, F16 and their linear variants. */
static bool is_depth_format(ULONG fmt) { return fmt >= 0x2A && fmt <= 0x31; }
static float depth_format_max(ULONG fmt) { return (fmt & ~4u) == 0x2A || (fmt & ~4u) == 0x2B ? 16777215.0f : 65535.0f; }

static void upload_image3(GLenum target, GLint level, ULONG fmt, ULONG w, ULONG h, ULONG d, ULONG pitch, const uint8_t *src)
{
    if (is_depth_format(fmt) && target == GL_TEXTURE_2D) {
        /* A depth texture (a shadow buffer): sampled with a depth compare.
           The float variants are uploaded as if they were integer depth. */
        bool d24 = depth_format_max(fmt) > 65535.0f, swz = fmt <= 0x2D;
        ULONG bpp = d24 ? 4 : 2;
        uint8_t *px = malloc(w * h * bpp);
        if (swz) unswizzle(src, px, w, h, bpp);
        else for (ULONG y = 0; y < h; y++) memcpy(px + y * w * bpp, src + y * (pitch ? pitch : w * bpp), w * bpp);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (d24) glTexImage2D(target, level, 0x88F0 /* GL_DEPTH24_STENCIL8 */, w, h, 0, 0x84F9 /* GL_DEPTH_STENCIL */,
                              0x84FA /* GL_UNSIGNED_INT_24_8 */, px);
        else glTexImage2D(target, level, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, px);
        free(px);
        return;
    }
    /* conv: 1 = V8U8 (bump), 2 = L6V5U5 (bump), both expanded to RGBA8 with
       du/dv in r/g as signed bytes + 128 and luminance in b: the hardware
       filters these channels as signed values (the bump stage forces
       D3DTSIGN_GSIGNED | BSIGNED), and with the bias GL's unsigned
       filtering interpolates them the same way (psh bump_biased);
       3 = YUY2 and 4 = UYVY (video frames), converted to RGB with BT.601;
       5 = A8, which the NV2A samples as white with that alpha;
       6 = AL8, one byte that is both luminance and alpha;
       7 = P8, an index into the stage's palette of A8R8G8B8 colors;
       8 = V16U16, kept to its high bytes and stored like V8U8;
       9 = R8B8, red in the high byte and blue in the low one (the XDK's
       pixel descriptions, e.g. xfont's g_Formats and the D3D test
       framework's masks 0xFF00 / 0x00FF): no green, and opaque. */
    struct { ULONG fmt; int bpp; bool swizzled; GLenum gl_fmt, gl_type; bool force_alpha; int conv; } table[] = {
        { 0x3A, 4, true,  GL_RGBA, GL_UNSIGNED_BYTE, false, 0 },               /* A8B8G8R8 / Q8W8V8U8 */
        { 0x3F, 4, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 0 },               /* LIN_A8B8G8R8 */
        { 0x3B, 4, true,  GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, false, 0 },        /* B8G8R8A8 */
        { 0x40, 4, false, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, false, 0 },        /* LIN_B8G8R8A8 */
        { 0x3C, 4, true,  GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, false, 0 },        /* R8G8B8A8 */
        { 0x41, 4, false, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, false, 0 },        /* LIN_R8G8B8A8 */
        { 0x38, 2, true,  GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, false, 0 },      /* R5G5B5A1 */
        { 0x3D, 2, false, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, false, 0 },      /* LIN_R5G5B5A1 */
        { 0x39, 2, true,  GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, false, 0 },      /* R4G4B4A4 */
        { 0x3E, 2, false, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, false, 0 },      /* LIN_R4G4B4A4 */
        { 0x32, 2, true,  GL_LUMINANCE, GL_UNSIGNED_SHORT, false, 0 },         /* L16 */
        { 0x35, 2, false, GL_LUMINANCE, GL_UNSIGNED_SHORT, false, 0 },         /* LIN_L16 */
        { 0x28, 2, true,  GL_RGBA, GL_UNSIGNED_BYTE, false, 1 },            /* V8U8 / G8B8 */
        { 0x17, 2, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 1 },            /* LIN_V8U8 / LIN_G8B8 */
        { 0x27, 2, true,  GL_RGBA, GL_UNSIGNED_BYTE, false, 2 },            /* L6V5U5 / R6G5B5 */
        { 0x37, 2, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 2 },            /* LIN_L6V5U5 */
        { 0x33, 4, true,  GL_RGBA, GL_UNSIGNED_BYTE, false, 8 },            /* V16U16 */
        { 0x36, 4, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 8 },            /* LIN_V16U16 */
        { 0x06, 4, true,  GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, false, 0 },   /* A8R8G8B8 */
        { 0x07, 4, true,  GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, true, 0 },    /* X8R8G8B8 */
        { 0x12, 4, false, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, false, 0 },   /* LIN_A8R8G8B8 */
        { 0x1E, 4, false, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, true, 0 },    /* LIN_X8R8G8B8 */
        { 0x05, 2, true,  GL_RGB,  GL_UNSIGNED_SHORT_5_6_5, false, 0 },       /* R5G6B5 */
        { 0x11, 2, false, GL_RGB,  GL_UNSIGNED_SHORT_5_6_5, false, 0 },       /* LIN_R5G6B5 */
        { 0x02, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, false, 0 }, /* A1R5G5B5 */
        { 0x03, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, true, 0 },  /* X1R5G5B5 */
        { 0x10, 2, false, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, false, 0 }, /* LIN_A1R5G5B5 */
        { 0x1C, 2, false, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, true, 0 },  /* LIN_X1R5G5B5 */
        { 0x04, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, false, 0 }, /* A4R4G4B4 */
        { 0x1D, 2, false, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, false, 0 }, /* LIN_A4R4G4B4 */
        { 0x00, 1, true,  GL_LUMINANCE, GL_UNSIGNED_BYTE, false, 0 },         /* L8 */
        { 0x13, 1, false, GL_LUMINANCE, GL_UNSIGNED_BYTE, false, 0 },         /* LIN_L8 */
        { 0x19, 1, true,  GL_RGBA, GL_UNSIGNED_BYTE, false, 5 },              /* A8 */
        { 0x1F, 1, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 5 },              /* LIN_A8 */
        { 0x01, 1, true,  GL_RGBA, GL_UNSIGNED_BYTE, false, 6 },              /* AL8 */
        { 0x0B, 1, true,  GL_BGRA, GL_UNSIGNED_BYTE, false, 7 },              /* P8 */
        { 0x1B, 1, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 6 },              /* LIN_AL8 */
        { 0x1A, 2, true,  GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, false, 0 },   /* A8L8 */
        { 0x20, 2, false, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, false, 0 },   /* LIN_A8L8 */
        { 0x29, 2, true,  GL_RGBA, GL_UNSIGNED_BYTE, false, 9 },            /* R8B8 */
        { 0x16, 2, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 9 },            /* LIN_R8B8 */
        { 0x24, 2, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 3 },            /* YUY2 */
        { 0x25, 2, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 4 },            /* UYVY */
    };

    if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) {
        if (target == GL_TEXTURE_3D) {
            /* S3TC is not a legal compressed GL_TEXTURE_3D format.
               Decode the NV2A slab order before uploading a normal volume. */
            size_t packed_bytes, rgba_bytes;
            if (!dxt_volume_sizes(fmt, w, h, d, &packed_bytes, &rgba_bytes)) {
                xlog("D3D: invalid DXT volume dimensions %ux%ux%u", w, h, d);
                return;
            }
            uint8_t *rgba = malloc(rgba_bytes);
            if (!rgba) {
                xlog("D3D: cannot allocate %zu bytes for DXT volume", rgba_bytes);
                return;
            }
            if (dxt_decode_volume(fmt, w, h, d, src, packed_bytes, rgba, rgba_bytes)) {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage3D(target, level, GL_RGBA, w, h, d, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            }
            free(rgba);
            return;
        }
        /* DXT1/3/5 are stored linearly in 4x4 blocks, like on the PC. */
        static const GLenum dxt[] = { 0x83F1, 0x83F2, 0x83F3 };  /* GL_COMPRESSED_RGBA_S3TC_DXT{1,3,5}_EXT */
        GLenum internal = dxt[fmt == 0x0C ? 0 : fmt == 0x0E ? 1 : 2];
        ULONG block = fmt == 0x0C ? 8 : 16;
        ULONG bytes = ((w + 3) / 4) * ((h + 3) / 4) * block;
        glCompressedTexImage2D(target, level, internal, w, h, 0, bytes, src);
        return;
    }
    unsigned i = 0;
    while (i < sizeof(table) / sizeof(table[0]) && table[i].fmt != fmt) i++;
    if (i == sizeof(table) / sizeof(table[0])) {
        xlog("D3D: texture format %#x is not supported yet%s", fmt, target == GL_TEXTURE_3D ? " for volumes" : "");
        uint32_t magenta = 0xFFFF00FF;
        if (target == GL_TEXTURE_3D) glTexImage3D(target, level, GL_RGBA, 1, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &magenta);
        else glTexImage2D(target, level, GL_RGBA, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &magenta);
        return;
    }
    int bpp = table[i].bpp;
    if (target != GL_TEXTURE_3D) d = 1;
    uint8_t *px = malloc(w * h * d * bpp);
    if (table[i].swizzled && d > 1) {
        unswizzle3d(src, px, w, h, d, bpp);
    } else if (table[i].swizzled) {
        unswizzle(src, px, w, h, bpp);
    } else {
        if (!pitch) pitch = w * bpp;
        for (ULONG y = 0; y < h; y++) memcpy(px + y * w * bpp, src + y * pitch, w * bpp);
    }
    if (table[i].force_alpha && bpp == 4)
        for (ULONG k = 0; k < w * h * d; k++) px[k * 4 + 3] = 0xFF;
    if (table[i].conv == 8) {
        for (ULONG k = 0; k < w * h * d; k++) {
            uint8_t *o = px + k * 4;
            o[0] = o[1] ^ 0x80; o[1] = o[3] ^ 0x80; o[2] = o[3] = 0xFF;
        }
    } else if (table[i].conv == 9) {
        uint8_t *rgba = malloc(w * h * d * 4);
        for (ULONG k = 0; k < w * h * d; k++) {
            uint8_t *o = rgba + k * 4;
            o[0] = px[k * 2 + 1]; o[1] = 0; o[2] = px[k * 2]; o[3] = 0xFF;
        }
        free(px);
        px = rgba;
        bpp = 4;
    } else if (table[i].conv == 7) {
        uint8_t *bgra = malloc(w * h * d * 4);
        for (ULONG k = 0; k < w * h * d; k++) {
            uint32_t c = tex_palette && px[k] < tex_palette_count ? tex_palette[px[k]] : 0xFFFF00FF;
            memcpy(bgra + k * 4, &c, 4);
        }
        free(px);
        px = bgra;
        bpp = 4;
    } else if (table[i].conv >= 5) {
        uint8_t *rgba = malloc(w * h * d * 4);
        for (ULONG k = 0; k < w * h * d; k++) {
            rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = table[i].conv == 6 ? px[k] : 0xFF;
            rgba[k * 4 + 3] = px[k];
        }
        free(px);
        px = rgba;
        bpp = 4;
    } else if (table[i].conv >= 3) {
        /* Each 4-byte pair of texels shares U and V. */
        uint8_t *rgba = malloc(w * h * d * 4);
        bool uyvy = table[i].conv == 4;
        for (ULONG k = 0; k < w * h * d; k++) {
            const uint8_t *q = px + (k & ~1u) * 2;
            int y = uyvy ? q[1 + (k & 1) * 2] : q[(k & 1) * 2];
            int u = (uyvy ? q[0] : q[1]) - 128, v = (uyvy ? q[2] : q[3]) - 128;
            int c = (y - 16) * 298 + 128;
            int rgb[3] = { (c + 409 * v) >> 8, (c - 100 * u - 208 * v) >> 8, (c + 516 * u) >> 8 };
            for (int j = 0; j < 3; j++) rgba[k * 4 + j] = rgb[j] < 0 ? 0 : rgb[j] > 255 ? 255 : rgb[j];
            rgba[k * 4 + 3] = 0xFF;
        }
        free(px);
        px = rgba;
        bpp = 4;
    } else if (table[i].conv) {
        uint8_t *rgba = malloc(w * h * d * 4);
        for (ULONG k = 0; k < w * h * d; k++) {
            uint16_t v = px[k * 2] | px[k * 2 + 1] << 8;
            uint8_t *o = rgba + k * 4;
            if (table[i].conv == 1) {
                o[0] = (v & 0xFF) ^ 0x80; o[1] = (v >> 8) ^ 0x80; o[2] = 0xFF;
            } else {
                int du = (int)(v << 27) >> 27, dv = (int)((v >> 5) << 27) >> 27;   /* signed 5-bit */
                o[0] = (uint8_t)(128 + (du * 127 / 15 < -128 ? -128 : du * 127 / 15));
                o[1] = (uint8_t)(128 + (dv * 127 / 15 < -128 ? -128 : dv * 127 / 15));
                o[2] = (uint8_t)(((v >> 10) & 0x3F) * 255 / 63);
            }
            o[3] = 0xFF;
        }
        free(px);
        px = rgba;
        bpp = 4;
    }
    const char *dump = getenv("XBCOMPAT_DUMP_TEXTURES");
    if (dump) {
        /* Debug aid: XBCOMPAT_DUMP_TEXTURES=dir writes every upload as dir/texN_WxH_fmt.bin (tools/texdump.py renders them). */
        static int n; char path[512];
        snprintf(path, sizeof path, "%s/tex%d_%lux%lu_%lx.bin", dump, n++, (unsigned long)w, (unsigned long)h, (unsigned long)fmt);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(px, 1, w * h * bpp, f); fclose(f); }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (target == GL_TEXTURE_3D)
        glTexImage3D(target, level, table[i].force_alpha ? GL_RGB : GL_RGBA, w, h, d, 0, table[i].gl_fmt, table[i].gl_type, px);
    else   /* 5:6:5 stays 16-bit on the GPU: half the texture memory and upload bandwidth */
        glTexImage2D(target, level, fmt == 0x05 || fmt == 0x11 ? 0x8D62 /* GL_RGB565 */ : table[i].force_alpha ? GL_RGB : GL_RGBA,
                     w, h, 0, table[i].gl_fmt, table[i].gl_type, px);
    free(px);
}


#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
static void (APIENTRY *p_glBindFramebuffer)(GLenum, GLuint);   /* loaded with the other FBO functions */

/* Copy the window's color or depth/stencil buffer into a device surface's
   memory (top row first), the way the NV2A keeps them in RAM. */
static void readback_surface(D3DSurface *s, bool depth)
{
    hud_batch_flush();
    stats.readbacks++;
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)s, &w, &h, &pitch);
    if (w > (ULONG)d3d.width) w = d3d.width;
    if (h > (ULONG)d3d.height) h = d3d.height;
    uint8_t *px = (uint8_t *)(s->Data | CONTIG_BASE);
    uint8_t *tmp = malloc(w * h * 4);
    backbuffer_read_begin(depth ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (depth) glReadPixels(0, 0, w, h, 0x84F9 /* GL_DEPTH_STENCIL */, 0x84FA /* GL_UNSIGNED_INT_24_8 */, tmp);
    else glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    backbuffer_read_end();
    ULONG row = pitch < w * 4 ? pitch : w * 4;
    for (ULONG y = 0; y < h; y++) memcpy(px + y * pitch, tmp + (h - 1 - y) * w * 4, row);
    free(tmp);
}

static void upload_depth_memory(void);

/* FNV-1a over the device depth buffer's visible rows. */
static uint64_t depth_memory_hash(void)
{
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)d3d.depth, &w, &h, &pitch);
    if (w > (ULONG)d3d.width) w = d3d.width;
    if (h > (ULONG)d3d.height) h = d3d.height;
    const uint8_t *px = (const uint8_t *)(d3d.depth->Data | CONTIG_BASE);
    uint64_t x = 0xcbf29ce484222325ull;
    for (ULONG y = 0; y < h; y++) {
        const uint64_t *row = (const uint64_t *)(px + y * pitch);
        for (ULONG i = 0; i < w * 4 / 8; i++) x = (x ^ row[i]) * 0x100000001b3ull;
    }
    return x;
}

static void tex_stale_drop(void)
{
    unsigned put = __atomic_load_n(&tex_stale_put, __ATOMIC_ACQUIRE);
    if (put == tex_stale_got) return;
    if (put - tex_stale_got > TEX_STALE) {
        xlog("D3D: %u textures registered again at once; the oldest %u keep their GL copies",
             put - tex_stale_got, put - tex_stale_got - TEX_STALE);
        tex_stale_got = put - TEX_STALE;
    }
    for (unsigned i = tex_stale_got; i != put; i++) tex_invalidate(tex_stale[i % TEX_STALE]);
    tex_stale_got = put;
}

static GLuint texture_for(D3DPixelContainer *t)
{
    tex_stale_drop();
    if (hud_packet_active)   /* a captured HUD packet bound its textures already */
        for (int s = 0; s < 4; s++)
            if (d3d.textures[s] == (D3DResource *)t) return hud_packet_texture_id[s];
    /* A texture header over the back buffer or the depth buffer (titles
       build these with XGSetTextureHeader to filter the frame): read the
       current pixels back and upload them fresh. */
    ULONG va = t->res.Data | CONTIG_BASE;
    bool on_color = d3d.backbuffer && va == (d3d.backbuffer->Data | CONTIG_BASE) && t != (D3DPixelContainer *)d3d.backbuffer;
    bool on_depth = d3d.depth && va == (d3d.depth->Data | CONTIG_BASE) && t != (D3DPixelContainer *)d3d.depth;
    if (on_color || on_depth) hud_batch_flush();   /* queued draws land in the frame first */
    if (front.s && va == (front.s->Data | CONTIG_BASE) && t != (D3DPixelContainer *)front.s) {
        ULONG fmt = (t->Format >> 8) & 0xFF;
        if (!front.tex) front_buffer_update();   /* asked for before a Swap: the frame so far */
        if (front.tex && (fmt == 0x12 || fmt == 0x1E)) {
            TRACE_ALL("D3D: view of the front buffer at %#x", (unsigned)t->res.Data);
            return front.tex;
        }
        front_buffer_memory();
    }
    if (on_color && (whatif == WHATIF_NO_BBVIEW || whatif == WHATIF_BOTH)) { whatif_skip_draw = true; return 0; }
    if (on_color && d3d.backbuffer->Data == bbsync.data && (d3d.bb_cpu_dirty || backbuffer_memory_hash() != bbsync.hash)) {
        /* The CPU wrote the back buffer's memory, locked or through a
           pointer kept from an earlier lock: Splinter Cell copies each
           loading and menu picture in that way and cross-fades from a view
           of it, which sampled GL's stale frame (all white after the boot
           quad).  Load the memory into the frame first. */
        GLuint program = cur_program;
        d3d.bb_cpu_dirty = true;
        flush_cpu_backbuffer();
        gc_reset();
        if (program) { p_glUseProgram(program); cur_program = program; }
        TRACE_ALL("D3D: back buffer memory written by the CPU loaded before a view of it");
    }
    if (on_color) {
        /* A 32-bit view of the back buffer (Phantom Dust filters the frame
           through three every frame): copy it on the GPU, no readback. */
        ULONG fmt = (t->Format >> 8) & 0xFF, w, h, pitch;
        container_size(t, &w, &h, &pitch);
        GLuint id = fmt == 0x12 || fmt == 0x1E ? backbuffer_copy_texture(t->res.Data, w, h) : 0;
        if (id) return id;
    }
    if (on_color) {
        readback_surface(d3d.backbuffer, false);
        backbuffer_synced();
        tex_invalidate(t->res.Data);
    }
    if (on_depth) {
        /* The depth buffer's memory is the GPU's only while the GPU wrote it
           last.  Splinter Cell decodes its loading screens into that memory
           with the CPU between levels and draws them from a header over it;
           reading the GPU's depth back would overwrite the picture (a flat
           yellow or green screen).  So read back only when the GPU has
           drawn into it since the last sync and the CPU has not changed the
           memory since then. */
        uint64_t h = depth_memory_hash();
        TRACE_ALL("D3D: depth view %#x: memory %s, gpu writes %lu (synced at %lu)%s", (unsigned)t->res.Data,
                  h != zsync.hash ? "changed by the CPU" : "unchanged", depth_gpu_writes, zsync.writes,
                  h == zsync.hash && zsync.writes != depth_gpu_writes ? ", reading the GPU's depth back" : "");
        if (h != zsync.hash) {
            upload_depth_memory();
            zsync.hash = h;
            zsync.writes = depth_gpu_writes;
            tex_invalidate(t->res.Data);
        } else if (zsync.writes != depth_gpu_writes) {
            readback_surface(d3d.depth, true);
            zsync.hash = depth_memory_hash();
            zsync.writes = depth_gpu_writes;
            tex_invalidate(t->res.Data);
        }
    }
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("XBCOMPAT_NO_TEXCACHE") != NULL;
    if (nocache) tex_invalidate(t->res.Data);
    ULONG fmt = (t->Format >> 8) & 0xFF, w, h, pitch;
    ULONG pal = fmt == 0x0B ? palette_hash() : 0;
    for (tex_entry *e = *tex_bucket(t->res.Data); e; e = e->next)
        if (e->data == t->res.Data && e->format == t->Format && e->size == t->Size && e->pal == pal) return e->id;

    hud_batch_flush();   /* uploading rebinds texture units a queued draw uses */
    container_size(t, &w, &h, &pitch);
    if (!t->Size) pitch = 0;
    const uint8_t *src = (const uint8_t *)(t->res.Data | CONTIG_BASE);
    GLenum target = tex_target(t);

    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(target, id);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ULONG levels = level_count(t);
    /* Titles may provide a partial chain. Bound GL's completeness check to
       the supplied levels instead of requiring every level down to 1x1. */
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
    /* Cube faces each contain a whole mip chain, 128-byte aligned. */
    ULONG face_bytes = target == GL_TEXTURE_CUBE_MAP ? (level_offset(t, levels) + 127) & ~127u : 0;
    int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        ULONG lw = w, lh = h;
        ULONG ld = target == GL_TEXTURE_3D ? 1u << ((t->Format >> 28) & 0xF) : 1;
        GLenum image_target = target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : target;
        for (ULONG level = 0; level < levels; level++) {
            upload_image3(image_target, level, fmt, lw, lh, ld, pitch,
                          src + f * face_bytes + level_offset(t, level));
            if (lw > 1) lw >>= 1;
            if (lh > 1) lh >>= 1;
            if (ld > 1) ld >>= 1;
        }
    }
    stats.uploads++;
    TRACE_ALL("D3D: uploaded %ux%u texture format %#x%s", w, h, fmt, target == GL_TEXTURE_CUBE_MAP ? " (cube)" : "");
    debug_dump_texture(target, t->res.Data, fmt, w, h);

    tex_entry *e = malloc(sizeof(*e));
    *e = (tex_entry){ t->res.Data, t->Format, t->Size, pal, id, target, *tex_bucket(t->res.Data) };
    *tex_bucket(t->res.Data) = e;
    tex_cache_version++;
    return id;
}

static void NTAPI D3DDevice_SetTexture(ULONG Stage, D3DResource *t)
{
    if (d3d.recording) { ULONG a[2] = { Stage, (ULONG)t }; pb_record(OP_TEXTURE, a, sizeof(a)); }
    TRACE_ALL("D3D: SetTexture(%u, %p) data %#x", Stage, (void *)t, t ? (unsigned)t->Data : 0);
    if (Stage < 4) d3d.textures[Stage] = t;
}

static void NTAPI D3DDevice_SetTextureStageStateNotInline(ULONG Stage, ULONG Type, ULONG Value)
{
    if (Stage < 4 && Type < 32) TSS(Stage, Type) = Value;
}

static void NTAPI SetTextureState_TexCoordIndex(ULONG Stage, ULONG Value) { TSS(Stage, D3DTSS_TEXCOORDINDEX) = Value; }
static void NTAPI SetTextureState_BorderColor(ULONG Stage, ULONG Value) { TSS(Stage, D3DTSS_BORDERCOLOR) = Value; }
static void NTAPI SetTextureState_ColorKeyColor(ULONG Stage, ULONG Value) { TSS(Stage, D3DTSS_COLORKEYCOLOR) = Value; }
static void NTAPI SetTextureState_BumpEnv(ULONG Stage, ULONG Type, ULONG Value) { TSS(Stage, Type) = Value; }
static LONG NTAPI SetTextureState_ParameterCheck(ULONG Stage, ULONG Type, ULONG Value)
{
    (void)Stage; (void)Type; (void)Value;
    return D3D_OK;
}

static GLenum combine_source(ULONG arg)
{
    switch (arg & 0xF) {
    case 0: return GL_PRIMARY_COLOR;   /* D3DTA_DIFFUSE */
    case 1: return GL_PREVIOUS;        /* D3DTA_CURRENT */
    case 2: return GL_TEXTURE;         /* D3DTA_TEXTURE */
    case 3: return GL_CONSTANT;        /* D3DTA_TFACTOR */
    default: return GL_PREVIOUS;
    }
}

#ifndef GL_MODULATE_ADD_ATI
#define GL_MODULATE_ADD_ATI 0x8744
#endif

/* GL_ATI_texture_env_combine3 adds MODULATE_ADD (s0 * s2 + s1), which the
   triadic and "add smooth" ops need. */
static bool texture_combine_initialized;   /* the GL_COMBINE state below has been set once */
static bool have_combine3(void)
{
    static int have = -1;
    if (have < 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        have = ext && (strstr(ext, "GL_ATI_texture_env_combine3") || strstr(ext, "GL_NV_texture_env_combine4"));
        if (!have) xlog("D3D: no GL_ATI_texture_env_combine3; triadic texture ops fall back to MODULATE");
        texture_combine_initialized = true;
    }
    return have;
}

/* Combiner input n from a D3DTA_* argument.  `use_alpha` reads its alpha
   (as D3DTA_ALPHAREPLICATE does), `invert` flips D3DTA_COMPLEMENT. */
static void combine_arg(bool alpha, int n, ULONG arg, bool use_alpha, bool invert)
{
    static const GLenum src_rgb[3] = { GL_SOURCE0_RGB, GL_SOURCE1_RGB, GL_SOURCE2_RGB };
    static const GLenum src_a[3] = { GL_SOURCE0_ALPHA, GL_SOURCE1_ALPHA, GL_SOURCE2_ALPHA };
    static const GLenum op_rgb[3] = { GL_OPERAND0_RGB, GL_OPERAND1_RGB, GL_OPERAND2_RGB };
    static const GLenum op_a[3] = { GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA, GL_OPERAND2_ALPHA };
    bool comp = ((arg & 0x10) != 0) != invert;
    GLenum operand;
    if (alpha || use_alpha || (arg & 0x20)) operand = comp ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    else operand = comp ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;
    glTexEnvi(GL_TEXTURE_ENV, alpha ? src_a[n] : src_rgb[n], combine_source(arg));
    glTexEnvi(GL_TEXTURE_ENV, alpha ? op_a[n] : op_rgb[n], operand);
}

/* Map one D3D texture op onto GL_COMBINE for either RGB or alpha.
   `no_texture`: the stage has no texture set. */
static void combine(bool alpha, ULONG op, ULONG arg0, ULONG arg1, ULONG arg2, bool no_texture)
{
    GLenum mode_p = alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB;
    GLenum scale = alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE;
    GLenum mode = GL_MODULATE;
    float s = 1;
    /* Sources: s0/s1/s2 as D3DTA args, with "read alpha" and "complement" flags. */
    ULONG a[3] = { arg1, arg2, 1 };
    bool ua[3] = { false, false, false }, inv[3] = { false, false, false };
    bool c3 = have_combine3();
    switch (op) {
    case 1: mode = GL_REPLACE; a[0] = 1; break;   /* DISABLE: pass the current color */
    case 2: mode = GL_REPLACE; break;
    case 3: mode = GL_REPLACE; a[0] = arg2; break;
    case 4: break;
    case 5: s = 2; break;
    case 6: s = 4; break;
    case 7: mode = GL_ADD; break;
    case 8: mode = GL_ADD_SIGNED; break;
    case 9: mode = GL_ADD_SIGNED; s = 2; break;
    case 10: mode = GL_SUBTRACT; break;
    case 12: case 13: case 14: case 15:   /* BLEND{DIFFUSE,CURRENT,TEXTURE,FACTOR}ALPHA */
        mode = GL_INTERPOLATE;
        a[2] = op == 12 ? 0 : op == 13 ? 1 : op == 14 ? 2 : 3;
        ua[2] = true;
        break;
    case 17: mode = GL_MODULATE; break;   /* PREMODULATE: approximated */
    case 22:   /* DOTPRODUCT3: replicated to all four channels */
        mode = alpha ? GL_MODULATE : GL_DOT3_RGBA;
        break;
    case 24:   /* LERP: arg0 * arg1 + (1 - arg0) * arg2 */
        mode = GL_INTERPOLATE;
        a[2] = arg0;
        break;
    case 25: case 26:   /* BUMPENVMAP*: the stage itself passes the current color */
        mode = GL_REPLACE; a[0] = 1;
        break;
    default:
        if (!c3) break;
        mode = GL_MODULATE_ADD_ATI;   /* s0 * s2 + s1 */
        switch (op) {
        case 11:   /* ADDSMOOTH: arg1 + (1 - arg1) * arg2 */
            a[0] = arg1; inv[0] = true; a[2] = arg2; a[1] = arg1; break;
        case 16:   /* BLENDTEXTUREALPHAPM: arg1 + arg2 * (1 - texture alpha) */
            a[0] = arg2; a[2] = 2; ua[2] = inv[2] = true; a[1] = arg1; break;
        case 18:   /* MODULATEALPHA_ADDCOLOR: arg1 + arg1.a * arg2 */
            a[0] = arg1; ua[0] = true; a[2] = arg2; a[1] = arg1; break;
        case 19:   /* MODULATECOLOR_ADDALPHA: arg1 * arg2 + arg1.a */
            a[0] = arg1; a[2] = arg2; a[1] = arg1; ua[1] = true; break;
        case 20:   /* MODULATEINVALPHA_ADDCOLOR: (1 - arg1.a) * arg2 + arg1 */
            a[0] = arg1; ua[0] = inv[0] = true; a[2] = arg2; a[1] = arg1; break;
        case 21:   /* MODULATEINVCOLOR_ADDALPHA: (1 - arg1) * arg2 + arg1.a */
            a[0] = arg1; inv[0] = true; a[2] = arg2; a[1] = arg1; ua[1] = true; break;
        case 23:   /* MULTIPLYADD: arg0 + arg1 * arg2 */
            a[0] = arg1; a[2] = arg2; a[1] = arg0; break;
        default:
            mode = GL_MODULATE;
        }
    }
    if (no_texture) {
        /* A stage reading a texture it doesn't have selects CURRENT instead
           (combiner.cpp, TSS_USES_NULL_TEXTURE): the diffuse colour in stage
           0.  Splinter Cell draws its loading bar through its fade's stages
           with the textures taken away; a white stand-in texture made the
           bar white and the fade's last frame white. */
        unsigned used = mode == GL_REPLACE ? 1 : mode == GL_INTERPOLATE || mode == GL_MODULATE_ADD_ATI ? 3 : 2;
        for (unsigned n = 0; n < used; n++)
            if ((a[n] & 0xF) == 2 /* D3DTA_TEXTURE */) { mode = GL_REPLACE; a[0] = 1; s = 1; break; }
    }
    glTexEnvi(GL_TEXTURE_ENV, mode_p, mode);
    for (int n = 0; n < 3; n++) combine_arg(alpha, n, a[n], ua[n], inv[n]);
    glTexEnvf(GL_TEXTURE_ENV, scale, s);
}

static GLenum gl_wrap(ULONG mode)
{
    switch (mode) {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: case 5: return GL_CLAMP_TO_EDGE;
    case 4: return GL_CLAMP_TO_BORDER;
    default: return GL_REPEAT;
    }
}

static GLenum gl_filter(ULONG f)
{
    return f == 1 ? GL_NEAREST : GL_LINEAR;   /* D3DTEXF_POINT */
}

static GLenum gl_min_filter(ULONG min, ULONG mip)
{
    bool point = gl_filter(min) == GL_NEAREST;
    switch (mip) {
    case 1: return point ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
    case 2: return point ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
    default: return gl_filter(min);   /* D3DTEXF_NONE */
    }
}

/* Wrap and filter modes of stage `s`, applied to the texture bound to `target`. */
/* Sampler state belongs to a stage, not to the shared texture image.
   Use independent sampler objects where the host exposes them. */
static void (APIENTRY *p_glGenSamplers)(GLsizei, GLuint *);
static void (APIENTRY *p_glBindSampler)(GLuint, GLuint);
static void (APIENTRY *p_glSamplerParameteri)(GLuint, GLenum, GLint);
static void (APIENTRY *p_glSamplerParameterf)(GLuint, GLenum, GLfloat);
static void (APIENTRY *p_glSamplerParameterfv)(GLuint, GLenum, const GLfloat *);
static bool separate_samplers;
static GLuint stage_sampler[4], bound_sampler[4];

static void bind_stage_sampler(unsigned stage, GLuint sampler)
{
    if (bound_sampler[stage] == sampler) return;
    p_glBindSampler(stage, sampler);
    bound_sampler[stage] = sampler;
}

static bool sampler_changed(int stage, GLenum name, const void *value, unsigned words)
{
    static struct { GLenum name; unsigned words; uint32_t value[4]; } cache[4][16];
    for (unsigned i = 0; i < 16; i++) {
        if (cache[stage][i].name && cache[stage][i].name != name) continue;
        if (cache[stage][i].name == name && cache[stage][i].words == words
            && same_bytes(cache[stage][i].value, value, words * 4)) return false;
        cache[stage][i].name = name; cache[stage][i].words = words;
        memcpy(cache[stage][i].value, value, words * 4);
        return true;
    }
    return true;
}

static void sampler_i(GLenum target, int s, GLenum name, GLint value)
{
    if (separate_samplers) {
        if (sampler_changed(s, name, &value, 1)) p_glSamplerParameteri(stage_sampler[s], name, value);
    }
    else glTexParameteri(target, name, value);
}

static void sampler_f(GLenum target, int s, GLenum name, GLfloat value)
{
    if (separate_samplers) {
        if (sampler_changed(s, name, &value, 1)) p_glSamplerParameterf(stage_sampler[s], name, value);
    }
    else glTexParameterf(target, name, value);
}

static void apply_sampler(GLenum target, int s)
{
    if (separate_samplers) {
        if (!stage_sampler[0]) p_glGenSamplers(4, stage_sampler);
        bind_stage_sampler(s, stage_sampler[s]);
    }
    sampler_i(target, s, GL_TEXTURE_WRAP_S, gl_wrap(TSS(s, D3DTSS_ADDRESSU)));
    sampler_i(target, s, GL_TEXTURE_WRAP_T, gl_wrap(TSS(s, D3DTSS_ADDRESSV)));
    if (target != GL_TEXTURE_2D) sampler_i(target, s, GL_TEXTURE_WRAP_R, gl_wrap(TSS(s, D3DTSS_ADDRESSW)));
    sampler_i(target, s, GL_TEXTURE_MAG_FILTER, gl_filter(TSS(s, D3DTSS_MAGFILTER)));
    sampler_i(target, s, GL_TEXTURE_MIN_FILTER, gl_min_filter(TSS(s, D3DTSS_MINFILTER), TSS(s, D3DTSS_MIPFILTER)));
    float border[4];
    color4(border, TSS(s, D3DTSS_BORDERCOLOR));
    if (separate_samplers) {
        if (sampler_changed(s, GL_TEXTURE_BORDER_COLOR, border, 4))
            p_glSamplerParameterfv(stage_sampler[s], GL_TEXTURE_BORDER_COLOR, border);
    }
    else glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, border);
    D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
    /* API LOD controls do not change the authored image chain. Linear
       resources have a single level regardless of the requested filter. */
    ULONG last = t ? level_count(t) - 1 : 0;
    ULONG first = TSS(s, 7 /* MAXMIPLEVEL */);
    if (first > last) first = last;
    sampler_f(target, s, GL_TEXTURE_MIN_LOD, (GLfloat)first);
    float bias = t && !t->Size ? tss_float(s, 6 /* MIPMAPLODBIAS */) : 0.0f;
    glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, separate_samplers ? 0.0f : bias);
    if (separate_samplers) sampler_f(target, s, GL_TEXTURE_LOD_BIAS, bias);
    bool depth_compare = target == GL_TEXTURE_2D && t && is_depth_format((t->Format >> 8) & 0xFF);
    sampler_i(target, s, GL_TEXTURE_COMPARE_MODE, depth_compare ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
    if (depth_compare) {
        /* The NV2A compares the stage's r/q with the stored depth as
           "stored SHADOWFUNC r"; GL compares "r FUNC stored", so mirror it.
           The D3DCMP values are the GL enums. */
        static const GLenum mirror[8] = { GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL,
                                          GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS };
        sampler_i(target, s, GL_TEXTURE_COMPARE_FUNC, mirror[RS(D3DRS_SHADOWFUNC) & 7]);
    }
}

static GLuint white_texture(void)
{
    static GLuint id;
    if (!id) {
        static const unsigned char white[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    return id;
}

/* Fixed-function texturing: one GL unit per stage, GL_COMBINE for the ops.
   Returns the mask of units in use; a disabled stage ends the chain. */
static unsigned apply_textures(void)
{
    unsigned mask = 0;
    bool ended = false;
    for (int s = 0; s < 4; s++) {
        p_glActiveTexture(GL_TEXTURE0 + s);
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        bool sprite = s == 3 && RS(D3DRS_POINTSPRITEENABLE);   /* point sprites always use stage 3 */
        /* Only the stage's own target stays on; switching the others off
           rather than all three first keeps unchanged stages free of calls. */
        bool used = !(TSS(s, D3DTSS_COLOROP) == 1 || (ended && !sprite));
        GLenum want = !used ? 0 : t ? tex_target(t) : GL_TEXTURE_2D;
        if (want != GL_TEXTURE_2D) glDisable(GL_TEXTURE_2D);
        if (want != GL_TEXTURE_CUBE_MAP) glDisable(GL_TEXTURE_CUBE_MAP);
        if (want != GL_TEXTURE_3D) glDisable(GL_TEXTURE_3D);
        glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, sprite);
        TRACE_ALL("D3D: stage %d texture %p colorop %u(%u,%u) alphaop %u(%u,%u) tfactor %#x", s, (void *)t,
              TSS(s, D3DTSS_COLOROP), TSS(s, D3DTSS_COLORARG1), TSS(s, D3DTSS_COLORARG2), TSS(s, D3DTSS_ALPHAOP),
              TSS(s, D3DTSS_ALPHAARG1), TSS(s, D3DTSS_ALPHAARG2), RS(D3DRS_TEXTUREFACTOR));
        if (TSS(s, D3DTSS_COLOROP) == 1 || (ended && !sprite)) { ended = true; continue; }
        GLenum target = t ? tex_target(t) : GL_TEXTURE_2D;
        glEnable(target);
        if (t) {
            tex_use_palette(s);
            glBindTexture(target, texture_for(t));
            apply_sampler(target, s);
        } else {
            /* A stage with no texture still combines DIFFUSE, CURRENT and
               TFACTOR on the Xbox; run it with a white texture. */
            if (separate_samplers) bind_stage_sampler(s, 0);
            glBindTexture(GL_TEXTURE_2D, white_texture());
        }
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        float tf[4];
        color4(tf, RS(D3DRS_TEXTUREFACTOR));
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tf);
        combine(false, TSS(s, D3DTSS_COLOROP), TSS(s, D3DTSS_COLORARG0), TSS(s, D3DTSS_COLORARG1), TSS(s, D3DTSS_COLORARG2), !t);
        combine(true, TSS(s, D3DTSS_ALPHAOP), TSS(s, D3DTSS_ALPHAARG0), TSS(s, D3DTSS_ALPHAARG1), TSS(s, D3DTSS_ALPHAARG2), !t);
        mask |= 1u << s;
    }
    p_glActiveTexture(GL_TEXTURE0);
    return mask;
}


/* Fixed-function texture coordinate generation (D3DTSS_TEXCOORDINDEX high
   word) and texture transforms (D3DTS_TEXTUREn with
   D3DTSS_TEXTURETRANSFORMFLAGS).  `tsize[u]` is the number of components the
   vertex supplies for unit u.  D3D pads two-component coordinates to
   (u, v, 1) and divides only with D3DTTFF_PROJECTED; GL pads to (s, t, 0, 1)
   and always divides by q, so the matrix is rearranged to match. */
static void apply_texture_transforms(unsigned units, const int tsize[4])
{
    ff_texture_units = units;
    static const float sx[4] = { 1, 0, 0, 0 }, sy[4] = { 0, 1, 0, 0 }, sz[4] = { 0, 0, 1, 0 };
    /* Eye-linear planes are given in eye space: set them under an identity
       modelview (only then, as the push and pop cost calls every draw). */
    bool eye = false;
    for (int u = 0; u < 4; u++)
        if (((units >> u) & 1) && (TSS(u, D3DTSS_TEXCOORDINDEX) >> 16) == 2) eye = true;
    glMatrixMode(GL_MODELVIEW);
    if (eye) {
        glPushMatrix();
        glLoadIdentity();
    }
    for (int u = 0; u < 4; u++) {
        p_glActiveTexture(GL_TEXTURE0 + u);
        ULONG gen = TSS(u, D3DTSS_TEXCOORDINDEX) >> 16, ttff = TSS(u, 21 /* TEXTURETRANSFORMFLAGS */);
        bool on = (units >> u) & 1;
        GLint mode = 0;
        switch (on ? gen : 0) {
        case 1: mode = GL_NORMAL_MAP; break;        /* D3DTSS_TCI_CAMERASPACENORMAL */
        case 2: mode = GL_EYE_LINEAR; break;        /* CAMERASPACEPOSITION */
        case 3: mode = GL_REFLECTION_MAP; break;    /* CAMERASPACEREFLECTIONVECTOR */
        case 4: mode = GL_OBJECT_LINEAR; break;     /* D3DTSS_TCI_OBJECT (Xbox) */
        case 5: mode = GL_SPHERE_MAP; break;        /* D3DTSS_TCI_SPHERE (Xbox) */
        }
        static const GLenum coord[3] = { GL_S, GL_T, GL_R };
        static const GLenum en[3] = { GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R };
        const float *planes[3] = { sx, sy, sz };
        for (int k = 0; k < 3; k++) {
            if (!mode || (mode == GL_SPHERE_MAP && k == 2)) { glDisable(en[k]); continue; }
            glTexGeni(coord[k], GL_TEXTURE_GEN_MODE, mode);
            if (mode == GL_EYE_LINEAR) glTexGenfv(coord[k], GL_EYE_PLANE, planes[k]);
            if (mode == GL_OBJECT_LINEAR) glTexGenfv(coord[k], GL_OBJECT_PLANE, planes[k]);
            glEnable(en[k]);
        }
        glDisable(GL_TEXTURE_GEN_Q);

        float m[4][4];
        ULONG count = ttff & 0xFF;
        bool projected = (ttff & 0x100) != 0;
        if (!on || (!count && !projected)) {
            static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            memcpy(m, ident, sizeof m);
        } else {
            memcpy(m, &d3d.transforms[2 + u], sizeof m);   /* m[input][output] */
            int n_in = mode ? 3 : tsize[u];
            if (n_in <= 2) {           /* D3D's constant 1 sits in row 2, GL's in row 3 */
                memcpy(m[3], m[2], sizeof m[3]);
                memset(m[2], 0, sizeof m[2]);
            }
            if (projected && count >= 1 && count < 4) {
                for (int i = 0; i < 4; i++) m[i][3] = m[i][count - 1];
            } else if (!projected) {
                for (int i = 0; i < 4; i++) m[i][3] = i == 3;
            }
        }
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[u];
        if (on && t && t->Size && !d3d.pixel_shader && !ff_fragment_program()) {
            /* Only legacy fragment sampling needs vertex-side normalization.
               Pixel shaders normalize at the texture instruction instead. */
            ULONG w, h, pitch;
            container_size(t, &w, &h, &pitch);
            for (int i = 0; i < 4; i++) { m[i][0] /= w; m[i][1] /= h; }
        }
        glMatrixMode(GL_TEXTURE);
        glLoadMatrixf(&m[0][0]);
        glMatrixMode(GL_MODELVIEW);
    }
    if (eye) glPopMatrix();
    p_glActiveTexture(GL_TEXTURE0);
}

/* --trace detail for a draw: what each stage samples and how, and the
   vertices of a small pre-transformed draw (full-screen passes). */
/* On a traced frame with the dump flag set: each stage's texture as GL holds
   it for this draw (.ppm, GL row 0 first) and the guest memory behind it
   (.raw, pitch x height bytes), at most 32 a frame and each image once. */
static void trace_dump_sampled(void)
{
    static unsigned long frame;
    static unsigned n;
    static ULONG seen[32];
    if (access("/data/xbox/xbcompat-dump-targets", F_OK) && access("/tmp/xbcompat-dump-targets", F_OK)) return;
    if (frame != d3d.frame) { frame = d3d.frame; n = 0; }
    GLint active;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    for (int s = 0; s < 4 && n < 32; s++) {
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        if (!t) continue;
        unsigned i = 0;
        while (i < n && seen[i] != t->res.Data) i++;
        if (i < n) continue;
        seen[n++] = t->res.Data;
        p_glActiveTexture(GL_TEXTURE0 + s);
        GLint id = 0, gw = 0, gh = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &id);
        if (id) {
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &gw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &gh);
        }
        ULONG w = 0, h = 0, pitch = 0;
        container_size(t, &w, &h, &pitch);
        mkdir("/tmp/xbc-dump", 0755);
        char path[128];
        if (id && gw > 0 && gh > 0 && gw <= 2048 && gh <= 2048) {
            uint8_t *px = malloc((size_t)gw * gh * 4);
            if (px) {
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                snprintf(path, sizeof path, "/tmp/xbc-dump/%lu-s%d-%08x-%dx%d-gl.ppm", frame, s,
                         (unsigned)t->res.Data, gw, gh);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6 %d %d 255\n", gw, gh);
                    for (long k = 0; k < (long)gw * gh; k++) fwrite(px + k * 4, 1, 3, f);
                    fclose(f);
                }
                free(px);
            }
        }
        ULONG bytes = pitch * h;
        if (bytes && bytes <= (4u << 20)) {
            snprintf(path, sizeof path, "/tmp/xbc-dump/%lu-s%d-%08x-%ux%u-p%u-f%02x.raw", frame, s,
                     (unsigned)t->res.Data, (unsigned)w, (unsigned)h, (unsigned)pitch,
                     (unsigned)((t->Format >> 8) & 0xFF));
            FILE *f = fopen(path, "wb");
            if (f) { fwrite((const void *)(uintptr_t)(t->res.Data | CONTIG_BASE), 1, bytes, f); fclose(f); }
        }
        xlog("D3D:   dumped stage %d data %#x (gl texture %d, %dx%d)", s, (unsigned)t->res.Data, id, gw, gh);
    }
    p_glActiveTexture(active);
}

static void trace_draw_detail(const uint8_t *base, ULONG stride, ULONG count, bool rhw)
{
    if (g_trace != 1) return;
    for (int s = 0; s < 4; s++) {
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        if (!t) continue;
        ULONG w = 0, h = 0, pitch = 0;
        container_size(t, &w, &h, &pitch);
        xlog("D3D:   stage %d tex %p data %#x fmt %#x %ux%u pitch %u size %#x addr %u/%u filt %u/%u/%u border %#x%s%s",
             s, (void *)t, (unsigned)t->res.Data, (unsigned)((t->Format >> 8) & 0xFF), (unsigned)w, (unsigned)h,
             (unsigned)pitch, (unsigned)t->Size, TSS(s, D3DTSS_ADDRESSU), TSS(s, D3DTSS_ADDRESSV),
             TSS(s, D3DTSS_MAGFILTER), TSS(s, D3DTSS_MINFILTER), TSS(s, D3DTSS_MIPFILTER), TSS(s, D3DTSS_BORDERCOLOR),
             d3d.backbuffer && (t->res.Data | CONTIG_BASE) == (d3d.backbuffer->Data | CONTIG_BASE) ? " backbuffer" : "",
             d3d.depth && (t->res.Data | CONTIG_BASE) == (d3d.depth->Data | CONTIG_BASE) ? " depth" : "");
    }
    trace_dump_sampled();
    if (d3d.target) {
        ULONG w = 0, h = 0, pitch = 0;
        container_size((D3DPixelContainer *)d3d.target, &w, &h, &pitch);
        xlog("D3D:   target data %#x fmt %#x %ux%u blend %u %u/%u op %u alpha %u/%u viewport %u,%u %ux%u rt %dx%d"
             " scissors %d vs %#x", (unsigned)d3d.target->Data,
             (unsigned)((d3d.target->Format >> 8) & 0xFF), (unsigned)w, (unsigned)h, RS(D3DRS_ALPHABLENDENABLE),
             RS(D3DRS_SRCBLEND), RS(D3DRS_DESTBLEND), RS(D3DRS_BLENDOP), RS(D3DRS_ALPHATESTENABLE), RS(D3DRS_ALPHAREF),
             (unsigned)d3d.viewport.X, (unsigned)d3d.viewport.Y, (unsigned)d3d.viewport.Width,
             (unsigned)d3d.viewport.Height, d3d.rt_width, d3d.rt_height, d3d.scissors.count, (unsigned)d3d.vertex_shader);
    }
    if (rhw && base && count <= 6 && stride >= 16 && stride <= 64) {
        for (ULONG v = 0; v < count; v++) {
            const float *f = (const float *)(base + v * stride);
            char line[256];
            int n = 0;
            for (ULONG i = 0; i < stride / 4 && n < (int)sizeof(line) - 16; i++)
                n += snprintf(line + n, sizeof(line) - n, " %g", f[i]);
            xlog("D3D:   v%u%s", (unsigned)v, line);
        }
    }
}

/* ---- state setters ----------------------------------------------------- */

/* SetVertexShaderInput's state (with the vertex shader objects below). */
static bool vs_direct_input(void);
static void vs_set_aside_stream(UINT_ Stream, D3DResource *vb, ULONG Stride);
static void vs_set_program_ext(ULONG Handle);
static void vs_note_slot0(ULONG Handle);

static void NTAPI D3DDevice_SetStreamSource(UINT_ Stream, D3DResource *vb, UINT_ Stride)
{
    if (d3d.recording) { ULONG a[3] = { Stream, (ULONG)vb, Stride }; pb_record(OP_STREAM, a, sizeof(a)); }
    if (Stream >= 16) return;
    if (vs_direct_input()) {
        /* SetVertexShaderInput's streams stand until it is turned off. */
        vs_set_aside_stream(Stream, vb, Stride);
        return;
    }
    d3d.streams[Stream].vb = vb;
    d3d.streams[Stream].stride = Stride;
}

static void NTAPI D3DDevice_SetVertexShader(ULONG Handle)
{
    if (d3d.recording) pb_record(OP_VERTEX_SHADER, &Handle, 4);
    if (vs_direct_input()) { vs_set_program_ext(Handle); return; }
    d3d.vertex_shader = Handle;
    /* A program goes to slot 0 of program memory (see vp_flush_pending). */
    if (Handle & 1) vs_note_slot0(Handle);
}

static void NTAPI D3DDevice_SetTransform(ULONG State, const D3DMATRIX *m)
{
    if (d3d.recording) pb_record2(OP_TRANSFORM, &State, 4, m, sizeof(*m));
    if (State < 10) d3d.transforms[State] = *m;
    if (State == 1)
        TRACE_ALL("D3D: projection %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g",
                  m->m[0][0], m->m[0][1], m->m[0][2], m->m[0][3], m->m[1][0], m->m[1][1], m->m[1][2], m->m[1][3],
                  m->m[2][0], m->m[2][1], m->m[2][2], m->m[2][3], m->m[3][0], m->m[3][1], m->m[3][2], m->m[3][3]);
}

/* The viewport scale and offset the library writes to
   NV097_SET_VIEWPORT_SCALE/OFFSET, which take a program's oPos back from
   screen space.  The library also keeps them in the reserved constants
   c[-38] / c[-37] (slots 58 and 59).  With 96 constants a title can't reach
   those slots, so draws simply refill them; in the 192 constant modes the
   library writes them only at SetViewport and the title may overwrite them
   afterwards (Half-Life 2 keeps bone matrices there), so draws must not. */
static void viewport_constants(float vp[2][4])
{
    const D3DVIEWPORT8 *v = &d3d.viewport;
    float sx = 0.5f * v->Width, sy = -0.5f * v->Height;
    vp[0][0] = sx; vp[0][1] = sy; vp[0][2] = d3d.zscale * (v->MaxZ - v->MinZ); vp[0][3] = 0;
    /* The library's m_ScreenSpaceOffset carries KELVIN_BORDER (0.53125):
       CommonSetViewport writes it into the reserved constants, and a
       program that builds oPos from them lands half a pixel further right
       and down than one that doesn't add it. */
    vp[1][0] = v->X + sx + d3d.screen_offset[0] + 0.53125f;
    vp[1][1] = v->Y - sy + d3d.screen_offset[1] + 0.53125f;
    vp[1][2] = d3d.zscale * v->MinZ; vp[1][3] = 0;
}

static void NTAPI D3DDevice_SetViewport(const D3DVIEWPORT8 *v)
{
    if (d3d.recording) { ULONG has = v != NULL; pb_record2(OP_VIEWPORT, &has, 4, v, v ? sizeof(*v) : 0); }
    if (v) d3d.viewport = *v;
    else d3d.viewport = (D3DVIEWPORT8){ 0, 0, d3d.width, d3d.height, 0, 1 };
    d3d.scissors.count = 0;   /* the library's SetViewport ends with SetScissors(0, 0, NULL) */
    if (!(d3d.constant_mode & 0x10)) {   /* D3DSCM_NORESERVEDCONSTANTS */
        float vp[2][4];
        viewport_constants(vp);
        memcpy(d3d.vs_const[58], vp, sizeof(vp));
        vsc_touch(58, 2);
        if (async_front()) { async_vs_touch(58); async_vs_touch(59); }
    }
}

/* The end of the library's SetRenderTarget: the depth scale (m_ZScale)
   follows the new depth buffer, kept when there is none, and SetViewport
   over the whole w x h target resets the scissors and rewrites the reserved
   viewport constants.  Without this a scissor set for a pass on the back
   buffer stayed on into the next target: a shadow buffer rendered (and
   cleared) after a per-light scissor kept only that rectangle, which moves
   with the camera. */
static void target_viewport_reset(D3DSurface *z, ULONG w, ULONG h)
{
    if (z) d3d.zscale = depth_format_max((z->Format >> 8) & 0xFF);
    d3d.viewport = (D3DVIEWPORT8){ 0, 0, w, h, 0, 1 };
    d3d.scissors.count = 0;
    if (!(d3d.constant_mode & 0x10)) {   /* D3DSCM_NORESERVEDCONSTANTS */
        float vp[2][4];
        viewport_constants(vp);
        memcpy(d3d.vs_const[58], vp, sizeof(vp));
        if (async_front()) { async_vs_touch(58); async_vs_touch(59); }
    }
}

/* The NV097 method each simple state (D3DRS_PSALPHAINPUTS0 to
   D3DRS_SOLIDOFFSETENABLE) writes, the library's D3DSIMPLERENDERSTATEENCODE
   (public/xdk/inc/d3d8.h); their values go to the GPU unchanged. */
static const USHORT simple_state_method[] = {
    0x260, 0x264, 0x268, 0x26C, 0x270, 0x274, 0x278, 0x27C,                     /* 0: PS alpha inputs */
    0x288, 0x28C,                                                               /* PS final combiner inputs */
    0xA60, 0xA64, 0xA68, 0xA6C, 0xA70, 0xA74, 0xA78, 0xA7C,                     /* 10: PS constants 0 */
    0xA80, 0xA84, 0xA88, 0xA8C, 0xA90, 0xA94, 0xA98, 0xA9C,                     /* 18: PS constants 1 */
    0xAA0, 0xAA4, 0xAA8, 0xAAC, 0xAB0, 0xAB4, 0xAB8, 0xABC,                     /* 26: PS alpha outputs */
    0xAC0, 0xAC4, 0xAC8, 0xACC, 0xAD0, 0xAD4, 0xAD8, 0xADC,                     /* 34: PS RGB inputs */
    0x17F8, 0x1E20, 0x1E24,                                                     /* compare mode, final constants */
    0x1E40, 0x1E44, 0x1E48, 0x1E4C, 0x1E50, 0x1E54, 0x1E58, 0x1E5C,             /* 45: PS RGB outputs */
    0x1E60, 0x1D90, 0x1E74, 0x1E78,                                             /* combiner count .. input texture */
    0x354, 0x33C, 0x304, 0x300, 0x340, 0x344, 0x348, 0x35C, 0x310, 0x37C, 0x358,  /* 57: D3DRS_ZFUNC .. */
    0x374, 0x378, 0x364, 0x368, 0x36C, 0x360, 0x350, 0x34C, 0x9F8, 0x384, 0x388,  /* 68: STENCILZFAIL .. */
    0x330, 0x334, 0x338,                                                        /* 79: .. SOLIDOFFSETENABLE */
};

static bool pb_render_state(ULONG m, ULONG d);

static void FASTCALL SetRenderState_Simple(ULONG Method, ULONG Value)
{
    /* Inline header code usually stores the value in D3D__RenderState
       itself, but LTCG builds can drop or defer that store; record it from
       the method too. */
    Method &= 0x1FFC;
    if (pb_render_state(Method, Value)) return;
    static ULONG seen[16];
    static unsigned nseen;
    unsigned i = 0;
    while (i < nseen && seen[i] != Method) i++;
    if (i == nseen && nseen < 16) {
        seen[nseen++] = Method;
        xlog("D3D: SetRenderState_Simple method %#x is not supported", Method);
    }
}

/* A render state method LTCG code wrote into the push buffer itself, or
   handed to SetRenderState_Simple. */
static bool pb_render_state(ULONG m, ULONG d)
{
    /* Phantom Dust keeps its own render state cache and sets the pixel
       shader constants (its menu, HUD and dialog colors) this way. */
    for (unsigned i = 0; i < sizeof(simple_state_method) / sizeof(simple_state_method[0]); i++)
        if (simple_state_method[i] == m) { RS(i) = d; return true; }
    if (m == 0x30C) {   /* NV097_SET_DEPTH_TEST_ENABLE */
        RS(D3DRS_ZENABLE) = d ? (RS(D3DRS_ZENABLE) ? RS(D3DRS_ZENABLE) : 1) : 0;
        return true;
    }
    if (m == 0x32C) { RS(D3DRS_STENCILENABLE) = d; return true; }   /* NV097_SET_STENCIL_TEST_ENABLE */
    if (m == 0x370) { RS(D3DRS_STENCILFAIL) = d; return true; }     /* NV097_SET_STENCIL_OP_FAIL */
    /* Polygon stipple (Phantom Dust's arenas): the control word turns it on,
       then 32 rows of pattern. */
    if (m == 0x147C) { d3d.stipple_on = d & 1; return true; }       /* NV097_SET_STIPPLE_CONTROL */
    if (m >= 0x1480 && m < 0x1500) {                                  /* NV097_SET_STIPPLE_PATTERN(i) */
        unsigned i = (m - 0x1480) / 4;
        if (d3d.stipple[i] != d) { d3d.stipple[i] = d; d3d.stipple_dirty = true; }
        return true;
    }
    if (m >= 0x1B0C && m < 0x1B0C + 4 * 0x40 && (m - 0x1B0C) % 0x40 == 0) {   /* NV097_SET_TEXTURE_CONTROL0(i) */
        /* Phantom Dust sets its cut-out textures' alpha kill this way. */
        unsigned i = (m - 0x1B0C) / 0x40;
        TSS(i, D3DTSS_ALPHAKILL) = d & 4 ? 4 /* D3DTALPHAKILL_ENABLE */ : 0;
        TSS(i, D3DTSS_COLORKEYOP) = d & 3;
        return true;
    }
    return false;
}

static void NTAPI D3DDevice_SetStipple(const ULONG *pattern)
{
    for (unsigned i = 0; i < 32; i++) pb_render_state(0x1480 + 4 * i, pattern[i]);
}

static LONG NTAPI SetRenderState_ParameterCheck(ULONG State, ULONG Value)
{
    (void)State; (void)Value;
    return D3D_OK;
}

static void NTAPI SetRenderStateNotInline(ULONG State, ULONG Value)
{
    if (State < d3d.rs_count) d3d.render_state[State] = Value;
}

#define COMPLEX_STATE(name, index) \
    static void NTAPI SetRenderState_##name(ULONG Value) { RS(index) = Value; }
COMPLEX_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_STATE(BackFillMode, 121)
COMPLEX_STATE(TwoSidedLighting, 122)
COMPLEX_STATE(StencilFail, 126)
COMPLEX_STATE(ZBias, 130)
COMPLEX_STATE(LogicOp, 131)
COMPLEX_STATE(EdgeAntiAlias, 132)
COMPLEX_STATE(MultiSampleAntiAlias, 133)
COMPLEX_STATE(MultiSampleMask, 134)
COMPLEX_STATE(MultiSampleMode, 135)
COMPLEX_STATE(MultiSampleRenderTargetMode, 136)
COMPLEX_STATE(ShadowFunc, 137)
COMPLEX_STATE(LineWidth, 138)
COMPLEX_STATE(Dxt1NoiseEnable, 139)
COMPLEX_STATE(YuvEnable, 140)
COMPLEX_STATE(OcclusionCullEnable, 141)
COMPLEX_STATE(StencilCullEnable, 142)
COMPLEX_STATE(RopZCmpAlwaysRead, 143)
COMPLEX_STATE(RopZRead, 144)
COMPLEX_STATE(DoNotCullUncompressed, 145)
COMPLEX_STATE(VertexBlend, 118)
COMPLEX_STATE(FogColor, 119)
COMPLEX_STATE(PSTextureModes, 117)

typedef struct {
    ULONG Type;
    float Diffuse[4], Specular[4], Ambient[4];
    float Position[3], Direction[3];
    float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
} D3DLIGHT8;

static LONG NTAPI D3DDevice_SetLight(ULONG Index, const D3DLIGHT8 *l)
{
    if (d3d.recording) pb_record2(OP_LIGHT, &Index, 4, l, sizeof(*l));
    if (Index >= 8) return D3D_OK;
    typeof(d3d.lights[0]) *L = &d3d.lights[Index];
    L->set = true;
    L->type = l->Type;
    memcpy(L->diffuse, l->Diffuse, 16);
    memcpy(L->ambient, l->Ambient, 16);
    memcpy(L->specular, l->Specular, 16);
    memcpy(L->position, l->Position, 12);
    L->position[3] = 1;
    memcpy(L->direction, l->Direction, 12);
    L->direction[3] = 0;
    L->range = l->Range;
    L->attenuation[0] = l->Attenuation0;
    L->attenuation[1] = l->Attenuation1;
    L->attenuation[2] = l->Attenuation2;
    return D3D_OK;
}

static LONG NTAPI D3DDevice_LightEnable(ULONG Index, BOOLEAN Enable)
{
    if (d3d.recording) { ULONG a[2] = { Index, Enable }; pb_record(OP_LIGHT_ENABLE, a, sizeof(a)); }
    if (Index < 8) d3d.lights[Index].enabled = Enable;
    return D3D_OK;
}

static void NTAPI D3DDevice_SetMaterial(const float *m)
{
    if (d3d.recording) pb_record(OP_MATERIAL, m, 17 * 4);
    /* D3DMATERIAL8: Diffuse, Ambient, Specular, Emissive, Power */
    memcpy(d3d.material, m, 64);
    d3d.material_power = m[16];
}

static void NTAPI D3DDevice_BlockUntilIdle(void) { pusher_drain(); run_callbacks(); }
static void NTAPI D3DDevice_BlockUntilVerticalBlank(void) { wait_vblank(); }
static BOOLEAN NTAPI D3DDevice_IsBusy(void) { return 0; }
/* The video encoder's filters, applied when the frame goes to the screen
   (present_window_framebuffer). */
static void NTAPI D3DDevice_SetFlickerFilter(ULONG v) { d3d.flicker_filter = v > 5 ? 5 : v; }
static void NTAPI D3DDevice_SetSoftDisplayFilter(ULONG v) { d3d.soft_display = v != 0; }

/* ---- drawing ---------------------------------------------------------- */

static void mat_mul(D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b)
{
    D3DMATRIX r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] +
                        a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
    *out = r;
}

/* UpdateProjectionViewportTransform's viewport matrix: clip space to the
   viewport's pixels and the depth buffer's range, all of it scaled by
   zscale / wfar with D3DZB_USEW (the library then w-buffers). */
static void viewport_transform(D3DMATRIX *vp)
{
    const D3DVIEWPORT8 *v = &d3d.viewport;
    const D3DMATRIX *p = &d3d.transforms[1];
    float s = 1;
    if (RS(D3DRS_ZENABLE) == 2 /* D3DZB_USEW */) {
        float wfar = p->m[2][2] == p->m[2][3] || p->m[2][2] == 0 ? 1
                     : (p->m[3][3] - p->m[3][2]) / (p->m[2][2] - p->m[2][3]) * p->m[2][3] + p->m[3][3];
        if (wfar != 0) s = d3d.zscale / wfar;
    }
    memset(vp, 0, sizeof(*vp));
    vp->m[0][0] = vp->m[3][0] = 0.5f * v->Width * s;
    vp->m[1][1] = -0.5f * v->Height * s;
    vp->m[3][1] = 0.5f * v->Height * s;
    vp->m[2][2] = d3d.zscale * (v->MaxZ - v->MinZ) * s;
    vp->m[3][2] = d3d.zscale * v->MinZ * s;
    vp->m[3][3] = s;
}

/* The inverse of a general 4x4 matrix; false when it is singular. */
static bool mat_inverse(D3DMATRIX *out, const D3DMATRIX *m)
{
    const float *a = &m->m[0][0];
    float inv[16];
    inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det == 0 || !isfinite(det)) return false;
    for (int i = 0; i < 16; i++) (&out->m[0][0])[i] = inv[i] / det;
    return true;
}

/* With Set[VertexBlend]ModelView in effect: the modelview and the D3D clip
   space projection that reproduce its matrices.  The hardware takes
   positions straight to the viewport's pixels with the composite matrix
   (with skinning, the blended eye position times it); undoing the viewport
   transform gives the projection, and undoing the modelview too what GL
   applies after its modelview. */
static bool direct_modelview(D3DMATRIX *modelview, D3DMATRIX *proj)
{
    if (!d3d.direct_mv.on) return false;
    D3DMATRIX vp, unview;
    viewport_transform(&vp);
    if (!mat_inverse(&unview, &vp)) return false;
    *modelview = d3d.direct_mv.mv[0];
    mat_mul(proj, &d3d.direct_mv.composite, &unview);
    if (!d3d.direct_mv.blend) {
        D3DMATRIX unmodel;
        if (!mat_inverse(&unmodel, modelview)) return false;
        mat_mul(proj, &unmodel, proj);
    }
    return true;
}

static GLenum gl_primitive(ULONG type)
{
    static const GLenum map[] = { 0, GL_POINTS, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES,
                                  GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP, GL_POLYGON };
    return type < 11 ? map[type] : GL_TRIANGLES;
}

static void color4(float *out, ULONG c)
{
    out[0] = ((c >> 16) & 255) / 255.0f;
    out[1] = ((c >> 8) & 255) / 255.0f;
    out[2] = (c & 255) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

static GLenum stencil_op(ULONG op)
{
    switch (op) {
    case GL_ZERO: case GL_KEEP: case GL_REPLACE: case GL_INCR: case GL_DECR: case GL_INVERT:
    case GL_INCR_WRAP: case GL_DECR_WRAP:
        return op;
    default:
        return GL_KEEP;
    }
}

static void apply_render_states(bool pretransformed, bool has_normal)
{
    ff_lighting_candidate = !pretransformed && ((has_normal && RS(D3DRS_LIGHTING)) || RS(118));
    ff_weight_components = 0;
    ff_pretransformed = pretransformed;
    for (int i = 0; i < 4; i++)
        if (TSS(i, 21 /* TEXTURETRANSFORMFLAGS */) & 0xFF) ff_lighting_candidate = true;
    if (!pretransformed)
        for (int i = 0; i < 4; i++) {
            ULONG gen = TSS(i, D3DTSS_TEXCOORDINDEX) >> 16;
            if (gen == 1 || gen == 2 || gen == 4) ff_lighting_candidate = true;
        }
    ff_primary_present = ff_secondary_present = false;
    flush_cpu_backbuffer();
    apply_viewport();
    /* The Pi's 16-bit back buffer bands without dithering; follow the title. */
    gc_enable(GL_DITHER, RS(D3DRS_DITHERENABLE) != 0);

    if (RS(D3DRS_ZENABLE)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(RS(D3DRS_ZFUNC));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(RS(D3DRS_ZWRITEENABLE) ? GL_TRUE : GL_FALSE);
    {
        /* D3DCOLORWRITEENABLE_BLUE/GREEN/RED/ALPHA are bytes 0-3 (stencil
           shadow volumes draw with all four off). */
        ULONG cw = RS(D3DRS_COLORWRITEENABLE);
        glColorMask(!!(cw & 0xFF0000), !!(cw & 0xFF00), !!(cw & 0xFF), !!(cw & 0xFF000000));
    }
    if (RS(D3DRS_SOLIDOFFSETENABLE)) {
        /* The offset counts steps of the Xbox depth format; GL's of the
           host depth buffer, which may be 16 or 24 bits. */
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(rs_float(D3DRS_POLYGONOFFSETZSLOPESCALE),
                        rs_float(D3DRS_POLYGONOFFSETZOFFSET) * d3d.host_zmax / d3d.target_zmax);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (RS(D3DRS_ALPHABLENDENABLE)) {
        glEnable(GL_BLEND);
        glBlendFunc(RS(D3DRS_SRCBLEND), RS(D3DRS_DESTBLEND));
        /* D3DBLENDOP_* are the GL equations; the Xbox's signed ones
           (0xF005 REVSUBTRACTSIGNED, 0xF006 ADDSIGNED) have no GL
           counterpart and fall back to their unsigned forms. */
        ULONG op = RS(D3DRS_BLENDOP);
        if (op == 0xF005) op = GL_FUNC_REVERSE_SUBTRACT;
        else if (op != GL_FUNC_ADD && op != GL_FUNC_SUBTRACT && op != GL_FUNC_REVERSE_SUBTRACT
                 && op != GL_MIN && op != GL_MAX) op = GL_FUNC_ADD;
        if (p_glBlendEquation) p_glBlendEquation(op);
        if (p_glBlendColor) {
            float bc[4];
            color4(bc, RS(D3DRS_BLENDCOLOR));
            p_glBlendColor(bc[0], bc[1], bc[2], bc[3]);
        }
    } else {
        glDisable(GL_BLEND);
    }
    /* Stencil: the D3DCMP_* and D3DSTENCILOP_* values are GL's. */
    if (RS(D3DRS_STENCILENABLE)) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(RS(D3DRS_STENCILFUNC), RS(D3DRS_STENCILREF) & 0xFF, RS(D3DRS_STENCILMASK) & 0xFF);
        glStencilOp(stencil_op(RS(D3DRS_STENCILFAIL)), stencil_op(RS(D3DRS_STENCILZFAIL)),
                    stencil_op(RS(D3DRS_STENCILPASS)));
        glStencilMask(RS(D3DRS_STENCILWRITEMASK) & 0xFF);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (RS(D3DRS_ALPHATESTENABLE)) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(RS(D3DRS_ALPHAFUNC), RS(D3DRS_ALPHAREF) / 255.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    glShadeModel(RS(D3DRS_SHADEMODE) == GL_FLAT ? GL_FLAT : GL_SMOOTH);
    glPolygonMode(GL_FRONT_AND_BACK, RS(D3DRS_FILLMODE) ? RS(D3DRS_FILLMODE) : GL_FILL);

    /* Points: D3D scales the size by the viewport height and the distance
       attenuation 1/sqrt(A + B*d + C*d^2) when POINTSCALEENABLE is set. */
    float psize = rs_float(D3DRS_POINTSIZE), pmin = rs_float(D3DRS_POINTSIZE_MIN), pmax = rs_float(D3DRS_POINTSIZE_MAX);
    if (!(psize > 0)) psize = 1;
    if (!(pmax > 0)) pmax = 64;
    if (RS(D3DRS_POINTSCALEENABLE)) {
        float att[3] = { rs_float(D3DRS_POINTSCALE_A), rs_float(D3DRS_POINTSCALE_B), rs_float(D3DRS_POINTSCALE_C) };
        p_glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, att);
        glPointSize(psize * d3d.viewport.Height);
    } else {
        float att[3] = { 1, 0, 0 };
        p_glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, att);
        glPointSize(psize);
    }
    p_glPointParameterf(GL_POINT_SIZE_MIN, pmin > 0 ? pmin : 1);
    p_glPointParameterf(GL_POINT_SIZE_MAX, pmax);
    if (RS(D3DRS_POINTSPRITEENABLE)) glEnable(GL_POINT_SPRITE);
    else glDisable(GL_POINT_SPRITE);
    TRACE_ALL("D3D: points size %g scale %u sprite %u min %g max %g", psize, RS(D3DRS_POINTSCALEENABLE),
          RS(D3DRS_POINTSPRITEENABLE), pmin, pmax);

    if (d3d.stipple_on && d3d.stipple_dirty) {
        /* Bit 0 of a row is its leftmost pixel; rows count down the screen
           as the title sees it, while GL counts them up from the bottom of
           the window (and of a texture target, which is stored flipped). */
        GLubyte rows[32][4];
        for (int y = 0; y < 32; y++) {
            ULONG v = d3d.stipple[d3d.rt_texture ? y : 31 - y];
            for (int b = 0; b < 4; b++) rows[y][b] = (GLubyte)(v >> (8 * b));
        }
        GLint align;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        glPixelStorei(GL_UNPACK_LSB_FIRST, GL_TRUE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPolygonStipple(&rows[0][0]);
        glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        d3d.stipple_dirty = false;
    }
    gc_enable(GL_POLYGON_STIPPLE, d3d.stipple_on);

    /* D3DCULL_CCW culls triangles that appear counter-clockwise on screen,
       so the GL front face is the opposite winding. */
    ULONG cull = RS(D3DRS_CULLMODE);
    if (cull == 0) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace((cull == 0x900) != d3d.rt_texture ? GL_CCW : GL_CW);
    }

    bool lighting = !pretransformed && RS(D3DRS_LIGHTING) && has_normal;
    if (lighting) {
        glEnable(GL_LIGHTING);
        float amb[4];
        color4(amb, RS(D3DRS_AMBIENT));
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
        /* D3DRS_TWOSIDEDLIGHTING (Xbox) lights back faces with the back material. */
        bool two_sided = RS(122) != 0;
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, two_sided);
        GLenum front = two_sided ? GL_FRONT : GL_FRONT_AND_BACK;
        glMaterialfv(front, GL_DIFFUSE, d3d.material[0]);
        glMaterialfv(front, GL_AMBIENT, d3d.material[1]);
        /* D3D adds specular after the texture stages, and only with
           D3DRS_SPECULARENABLE; GL's separate specular color matches. */
        static const float no_specular[4] = { 0, 0, 0, 0 };
        bool specular = RS(D3DRS_SPECULARENABLE) != 0;
        glLightModeli(0x81F8 /* GL_LIGHT_MODEL_COLOR_CONTROL */, specular ? 0x81FA /* SEPARATE_SPECULAR */ : 0x81F9);
        glMaterialfv(front, GL_SPECULAR, specular ? d3d.material[2] : no_specular);
        glMaterialfv(front, GL_EMISSION, d3d.material[3]);
        glMaterialf(front, GL_SHININESS, d3d.material_power > 128 ? 128 : d3d.material_power);
        if (two_sided) {
            glMaterialfv(GL_BACK, GL_DIFFUSE, d3d.back_material[0]);
            glMaterialfv(GL_BACK, GL_AMBIENT, d3d.back_material[1]);
            glMaterialfv(GL_BACK, GL_SPECULAR, specular ? d3d.back_material[2] : no_specular);
            glMaterialfv(GL_BACK, GL_EMISSION, d3d.back_material[3]);
            glMaterialf(GL_BACK, GL_SHININESS, d3d.back_material_power > 128 ? 128 : d3d.back_material_power);
        }
        if (RS(D3DRS_NORMALIZENORMALS)) glEnable(GL_NORMALIZE); else glDisable(GL_NORMALIZE);

        /* Lights are specified in world space: load the view matrix alone. */
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(&d3d.transforms[0].m[0][0]);
        for (int i = 0; i < 8; i++) {
            GLenum L = GL_LIGHT0 + i;
            if (!d3d.lights[i].enabled) { glDisable(L); continue; }
            glEnable(L);
            glLightfv(L, GL_DIFFUSE, d3d.lights[i].diffuse);
            glLightfv(L, GL_AMBIENT, d3d.lights[i].ambient);
            glLightfv(L, GL_SPECULAR, d3d.lights[i].specular);
            if (d3d.lights[i].type == 3) {   /* D3DLIGHT_DIRECTIONAL: GL wants the direction to the light */
                float d[4] = { -d3d.lights[i].direction[0], -d3d.lights[i].direction[1],
                               -d3d.lights[i].direction[2], 0 };
                glLightfv(L, GL_POSITION, d);
                glLightf(L, GL_SPOT_CUTOFF, 180);
            } else {
                glLightfv(L, GL_POSITION, d3d.lights[i].position);
                glLightf(L, GL_CONSTANT_ATTENUATION, d3d.lights[i].attenuation[0]);
                glLightf(L, GL_LINEAR_ATTENUATION, d3d.lights[i].attenuation[1]);
                glLightf(L, GL_QUADRATIC_ATTENUATION, d3d.lights[i].attenuation[2]);
                glLightf(L, GL_SPOT_CUTOFF, 180);
            }
        }
    } else {
        glDisable(GL_LIGHTING);
    }

    if (pretransformed) {
        /* Screen-space vertices: map pixels (y down) and D3D's [0,1] z. */
        /* glOrtho(0, w, h, 0, 0, -1) (0 and h swapped for a texture
           target) and a translation by the viewport origin, loaded whole
           so unchanged matrices cost no calls. */
        float w = d3d.viewport.Width, h = d3d.viewport.Height, top = d3d.rt_texture ? h : 0, bottom = h - top;
        float ortho[16] = { 2 / w, 0, 0, 0,  0, 2 / (top - bottom), 0, 0,  0, 0, 2, 0,
                            -1, -(top + bottom) / (top - bottom), -1, 1 };
        /* The NV2A puts pixel centres on whole screen positions (the
           library's 0.53125 border), GL on halves: without the half pixel a
           full-target quad from -0.5 to w-0.5 missed its last column and
           row and sampled half a texel up and left, so Splinter Cell's
           blur chain left stale bright edges and crept to the top left. */
        float move[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                           0.5f - (float)d3d.viewport.X, 0.5f - (float)d3d.viewport.Y, 0, 1 };
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(ortho);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(move);
    } else {
        /* D3D row-vector matrices loaded as-is are the GL column-vector
           transforms.  D3D clip space has z in [0,w]; GL wants [-w,w]. */
        static const D3DMATRIX zfix = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 2, 0 }, { 0, 0, -1, 1 } } };
        D3DMATRIX proj, modelview, clip;
        if (direct_modelview(&modelview, &clip)) {
            mat_mul(&proj, &clip, &zfix);
        } else {
            mat_mul(&proj, &d3d.transforms[1], &zfix);
            mat_mul(&modelview, &d3d.transforms[6], &d3d.transforms[0]);
        }
        /* The half pixel of the pretransformed path, in clip space: the
           library's fixed-function viewport offset carries KELVIN_BORDER
           too.  Splinter Cell's glow pass draws its full-target quad
           through the transforms. */
        if (d3d.viewport.Width && d3d.viewport.Height) {
            float kx = 1.0f / d3d.viewport.Width, ky = -1.0f / d3d.viewport.Height;
            for (int i = 0; i < 4; i++) {
                proj.m[i][0] += kx * proj.m[i][3];
                proj.m[i][1] += ky * proj.m[i][3];
            }
        }
        if (d3d.rt_texture) for (int i = 0; i < 4; i++) proj.m[i][1] = -proj.m[i][1];
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(&proj.m[0][0]);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(&modelview.m[0][0]);
    }
}

typedef struct {
    int pos_size, pos_off, normal_off, diffuse_off, specular_off, tex_off[4], tex_size[4], ntex, stride;
    bool pretransformed;
} fvf_layout;

static fvf_layout parse_fvf(ULONG fvf)
{
    fvf_layout l = { 0 };
    int off = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: l.pos_size = 3; off = 12; break;
    case D3DFVF_XYZRHW: l.pos_size = 3; off = 16; l.pretransformed = true; break;
    default:
        /* XYZB1..B4: position plus blend weights. */
        l.pos_size = 3;
        off = 12 + 4 * (((fvf & D3DFVF_POSITION_MASK) - D3DFVF_XYZB1) / 2 + 1);
    }
    l.normal_off = l.diffuse_off = l.specular_off = -1;
    if (fvf & D3DFVF_NORMAL) { l.normal_off = off; off += 12; }
    if (fvf & D3DFVF_DIFFUSE) { l.diffuse_off = off; off += 4; }
    if (fvf & D3DFVF_SPECULAR) { l.specular_off = off; off += 4; }
    l.ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (int i = 0; i < l.ntex && i < 4; i++) {
        static const int sizes[4] = { 2, 3, 4, 1 };
        int fmt = (fvf >> (16 + i * 2)) & 3;
        l.tex_off[i] = off;
        l.tex_size[i] = sizes[fmt];
        off += 4 * sizes[fmt];
    }
    l.stride = off;
    return l;
}

/* ---- programmable vertex shaders ----------------------------------- */


#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#endif

static void load_shader_functions(void)
{
#define LOAD(n) p_##n = SDL_GL_GetProcAddress(#n)
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glBindAttribLocation); LOAD(glGetAttribLocation);
    LOAD(glLinkProgram); LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog); LOAD(glUseProgram);
    LOAD(glDeleteShader); LOAD(glDeleteProgram); LOAD(glGetUniformLocation); LOAD(glUniform4fv);
    LOAD(glUniform1f); LOAD(glUniform1i); LOAD(glEnableVertexAttribArray); LOAD(glDisableVertexAttribArray);
    LOAD(glVertexAttribPointer); LOAD(glVertexAttrib4fv); LOAD(glActiveTexture); LOAD(glClientActiveTexture);
    LOAD(glMultiTexCoord4fv); LOAD(glUniform2fv); LOAD(glPointParameterfv); LOAD(glPointParameterf);
    LOAD(glSecondaryColorPointer); LOAD(glBlendEquation); LOAD(glBlendColor);
    LOAD(glGetActiveUniform); LOAD(glGetShaderSource); LOAD(glGenBuffers); LOAD(glDeleteBuffers); LOAD(glBindBuffer); LOAD(glBufferData); LOAD(glBufferSubData);
    LOAD(glGenQueries); LOAD(glBeginQuery); LOAD(glEndQuery); LOAD(glGetQueryObjectuiv);
    LOAD(glGenSamplers); LOAD(glBindSampler); LOAD(glSamplerParameteri);
    LOAD(glSamplerParameterf); LOAD(glSamplerParameterfv);
    separate_samplers = SDL_GL_ExtensionSupported("GL_ARB_sampler_objects")
        && p_glGenSamplers && p_glBindSampler && p_glSamplerParameteri
        && p_glSamplerParameterf && p_glSamplerParameterfv;
#undef LOAD
}

static GLuint compile_shader(GLenum kind, const char *src)
{
    GLuint sh = p_glCreateShader(kind);
    shader_diagnostics_source(kind, sh, src);
    TRACE_ALL("D3D: compiling %s shader %u", kind == GL_VERTEX_SHADER ? "vertex" : "fragment", sh);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    stats.shaders++;
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        xlog("D3D: %s shader failed to compile:\n%s\n--- source ---\n%s", kind == GL_VERTEX_SHADER ? "vertex" : "fragment",
             log, src);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

/* Link a vertex and/or fragment shader (0 = fixed function for that stage). */
static GLuint link_program(GLuint vs, GLuint fs)
{
    GLuint prog = p_glCreateProgram();
    TRACE_ALL("D3D: linking program %u (vs %u, fs %u)", prog, vs, fs);
    if (vs) p_glAttachShader(prog, vs);
    if (fs) p_glAttachShader(prog, fs);
    /* Only position, diffuse and the fixed-function secondary color need
       fixed slots; the rest take what the linker gives them (looked up with
       glGetAttribLocation), so programs fit VC4's 8 attributes. */
    p_glBindAttribLocation(prog, 0, "v0");
    p_glBindAttribLocation(prog, 1, "v1");
    p_glBindAttribLocation(prog, 6, "ff_color2");
    p_glLinkProgram(prog);
    shader_diagnostics_link(prog, vs, fs);
    GLint ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        p_glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        xlog("D3D: program failed to link:\n%s", log);
        p_glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* One vertex attribute as the declaration describes it. */
typedef struct {
    int stream;        /* -1: not in the declaration */
    ULONG offset;      /* bytes from the start of a vertex */
    ULONG type;        /* D3DVSDT_* */
    int components;
    GLenum gl_type;
    bool normalized;
    int bytes;
} vattr;

typedef struct {
    vattr attr[16];
    uint32_t *code;          /* NV2A microcode, NULL for a fixed-function declaration */
    unsigned ninstr;
    ULONG header;            /* the function blob's first dword, for GetVertexShaderFunction */
    ULONG *decl;             /* a copy of the declaration, for GetVertexShaderDeclaration */
    ULONG decl_dwords;
    float (*consts)[4];      /* D3DVSD_CONST data: slot, then values */
    int *const_slots;
    unsigned nconsts;
    GLuint vs;               /* compiled vertex shader, 0 until first use */
    GLuint vs_z;             /* its XBCOMPAT_VSH_Z_VARIANT twin, 0 until first use */
    bool failed;
    bool vs_z_attempted;     /* the variant was tried (it may not compile) */
    ULONG stream_mask;
    int tess_normal_in, tess_normal_out, tess_uv_out;   /* tessellator outputs, -1 if unused */
    ULONG user_handle;       /* vs_compose: the handle GetVertexShader reports (0: this one) */
} vshader;

static vshader *vp_slot0_pending;
static void vp_flush_pending(void);

/* D3DVSDT: high nibble = component count (7 = FLOAT2H), low nibble = NV2A type. */
static bool decode_vsdt(ULONG t, vattr *a)
{
    int n = t >> 4, kind = t & 0xF;
    a->type = t;
    a->components = n == 7 ? 3 : n;
    switch (kind) {
    case 0: a->gl_type = GL_UNSIGNED_BYTE; a->normalized = true; a->bytes = 4; a->components = 4; break;  /* D3DCOLOR */
    case 1: a->gl_type = GL_SHORT; a->normalized = true; a->bytes = 2 * n; break;                         /* NORMSHORT */
    case 2: a->gl_type = GL_FLOAT; a->normalized = false; a->bytes = 4 * a->components; break;            /* FLOAT */
    case 4: a->gl_type = GL_UNSIGNED_BYTE; a->normalized = true; a->bytes = n; break;                     /* PBYTE */
    case 5: a->gl_type = GL_SHORT; a->normalized = false; a->bytes = 2 * n; break;                        /* SHORT */
    case 6: a->gl_type = GL_INT; a->normalized = true; a->bytes = 4; a->components = 3; break;           /* NORMPACKED3 */
    default: return false;
    }
    if (t == 0x02) return false;   /* D3DVSDT_NONE */
    return true;
}

static vshader *parse_declaration(const ULONG *decl)
{
    vshader *sh = calloc(1, sizeof(*sh));
    for (int i = 0; i < 16; i++) sh->attr[i].stream = -1;
    sh->tess_normal_in = sh->tess_normal_out = sh->tess_uv_out = -1;
    int stream = 0;
    ULONG offset = 0;
    for (const ULONG *p = decl; p && *p != 0xFFFFFFFF; p++) {   /* state shaders have no declaration */
        ULONG tok = *p, type = tok >> 29;
        if (type == 1) {                     /* D3DVSD_STREAM, or STREAM_TESS (bit 28) */
            stream = (tok & 0x10000000) ? -1 : (int)(tok & 0xF);
            offset = 0;
        } else if (type == 3) {              /* D3DVSD_TESSUV / D3DVSD_TESSNORMAL */
            if (tok & 0x10000000) sh->tess_uv_out = tok & 0xF;
            else { sh->tess_normal_in = (tok >> 20) & 0xF; sh->tess_normal_out = tok & 0xF; }
        } else if (type == 2 && stream >= 0) {              /* D3DVSD_STREAMDATA */
            if (tok & 0x10000000) {          /* SKIP (dwords) or SKIPBYTES */
                ULONG n = (tok >> 16) & 0xF;
                offset += (tok & 0x08000000) ? n : n * 4;
            } else {
                ULONG reg = tok & 0x1F;
                vattr a = { 0 };
                if (reg < 16 && decode_vsdt((tok >> 16) & 0xFF, &a)) {
                    a.stream = stream;
                    a.offset = offset;
                    sh->attr[reg] = a;
                    sh->stream_mask |= 1u << stream;
                }
                ULONG t = (tok >> 16) & 0xFF;
                vattr tmp = { 0 };
                offset += decode_vsdt(t, &tmp) ? (ULONG)tmp.bytes : 0;
            }
        } else if (type == 4) {              /* D3DVSD_CONST: count vec4s follow */
            ULONG count = (tok >> 25) & 0xF, addr = tok & 0xFF;
            sh->consts = realloc(sh->consts, (sh->nconsts + count) * sizeof(*sh->consts));
            sh->const_slots = realloc(sh->const_slots, (sh->nconsts + count) * sizeof(int));
            for (ULONG i = 0; i < count; i++) {
                sh->const_slots[sh->nconsts] = addr + i;
                memcpy(sh->consts[sh->nconsts], p + 1 + i * 4, 16);
                sh->nconsts++;
            }
            p += count * 4;
        }
        /* NOP and extension tokens are ignored */
    }
    return sh;
}

static LONG NTAPI D3DDevice_CreateVertexShader(const ULONG *decl, const ULONG *func, ULONG *handle, ULONG usage)
{
    vshader *sh = parse_declaration(decl);
    if (decl) {
        ULONG n = 0;
        while (decl[n] != 0xFFFFFFFF) n++;
        sh->decl_dwords = n + 1;
        sh->decl = malloc(sh->decl_dwords * 4);
        memcpy(sh->decl, decl, sh->decl_dwords * 4);
    }
    if (func) {
        /* The blob starts with one header dword whose high word counts instructions. */
        unsigned n = func[0] >> 16;
        if (!n || n > 136) {
            static int warned;
            if (warned++ < 4) xlog("D3D: vertex program header %#x has no usable length", func[0]);
            n = 136;
        }
        sh->ninstr = n;
        sh->header = (func[0] & 0xFFFF) | n << 16;
        sh->code = malloc(n * 16);
        memcpy(sh->code, func + 1, n * 16);
    }
    *handle = (ULONG)sh | 1;
    TRACE_ALL("CreateVertexShader: %u instructions, usage %#x = %#x", sh->ninstr, usage, *handle);
    return D3D_OK;
}

/* A linked program: a vertex shader object and a fragment shader object
   (0 = fixed function for that stage) with its uniform locations. */
typedef struct program_entry {
    GLuint vs, fs, prog;
    GLint attr[16];          /* the location of each v<n> input, -1 if unused */
    unsigned attr_mask;      /* the locations in attr */
    GLint loc_ff_lighting, loc_fog_vertex_mode, loc_ff_transform, loc_ff_texcount, loc_zw_scale;
    GLint loc_shadow_func, loc_shadow_dim;
    GLint loc_c, loc_flip_y, loc_vp_scale, loc_vp_offset, loc_wdepth;
    GLint loc_tex[4], loc_cube[4], loc_vol[4], loc_tex_scale, loc_c0, loc_c1, loc_fc0, loc_fc1,
          loc_bump_env, loc_bump_lum, loc_eye_vector, loc_key_color;
    struct { GLint location; unsigned words; uint32_t *value; } uniforms[32];   /* last values set */
    unsigned uniform_count;
    float (*last_c)[4];   /* the constants last uploaded to the program */
    unsigned c_gen;       /* d3d.vsc_gen when they were */
    const d3d_state *c_state;   /* the state copy they came from */
    bool c_partial;
    int c_size;           /* active elements of c (the compiler drops unused trailing ones) */
    /* XBCOMPAT_PROFILE_RENDERER: this program's draws since the last report. */
    uint64_t prof_all_ns, prof_gl_ns;
    unsigned prof_draws, prof_indices;
    struct program_entry *next;
} program_entry;

/* GL keeps uniform values per program, and setting one costs a call that
   the VC4 driver makes expensive; remember what each program was last given
   and skip uploads that change nothing. A uniform the table has no room for
   is always uploaded. */
static bool program_uniform_changed(program_entry *e, GLint location, unsigned words, const void *value)
{
    if (location < 0) return false;
    unsigned i;
    for (i = 0; i < e->uniform_count; i++)
        if (e->uniforms[i].location == location) break;
    if (i == e->uniform_count) {
        if (i == 32) return true;
        e->uniforms[i].location = location;
        e->uniform_count++;
    }
    __typeof__(e->uniforms[0]) *u = &e->uniforms[i];
    if (u->value && u->words == words
        && gc_same(u->value, value, words)) return false;
    uint32_t *p = realloc(u->value, words * 4);
    if (!p) { free(u->value); u->value = NULL; u->words = 0; return true; }
    u->value = p;
    u->words = words;

    memcpy(p, value, words * 4);
    return true;
}

static void program_uniform4fv(program_entry *e, GLint location, GLsizei count, const GLfloat *value)
{
    if (program_uniform_changed(e, location, count * 4, value)) p_glUniform4fv(location, count, value);
}
static void program_uniform2fv(program_entry *e, GLint location, GLsizei count, const GLfloat *value)
{
    if (program_uniform_changed(e, location, count * 2, value)) p_glUniform2fv(location, count, value);
}
static void program_uniform1f(program_entry *e, GLint location, GLfloat value)
{
    if (program_uniform_changed(e, location, 1, &value)) p_glUniform1f(location, value);
}
static void program_uniform1i(program_entry *e, GLint location, GLint value)
{
    if (program_uniform_changed(e, location, 1, &value)) p_glUniform1i(location, value);
}

static program_entry *programs;
static program_entry *program_lookup[256];

static void NTAPI D3DDevice_DeleteVertexShader(ULONG handle)
{
    if (!(handle & 1)) return;
    gl_require();
    vshader *sh = (vshader *)(handle & ~1u);
    if (vp_slot0_pending == sh) vp_flush_pending();   /* program memory keeps its copy */
    if (sh->vs || sh->vs_z) {
        /* GL reuses the names, so programs linked with them must go too. */
        for (program_entry **pp = &programs; *pp;) {
            program_entry *e = *pp;
            if (!e->vs || (e->vs != sh->vs && e->vs != sh->vs_z)) { pp = &e->next; continue; }
            if (e->prog && e->prog == cur_program) program_off();
            if (e->prog) p_glDeleteProgram(e->prog);
            *pp = e->next;
            for (unsigned i = 0; i < e->uniform_count; i++) free(e->uniforms[i].value);
            free(e->last_c);
            free(e);
        }
        memset(program_lookup, 0, sizeof program_lookup);
        if (sh->vs) p_glDeleteShader(sh->vs);
        if (sh->vs_z) p_glDeleteShader(sh->vs_z);
    }
    free(sh->code);
    free(sh->decl);
    free(sh->consts);
    free(sh->const_slots);
    free(sh);
}

static LONG NTAPI D3DDevice_GetVertexShaderSize(ULONG handle, UINT_ *size)
{
    *size = handle & 1 ? ((vshader *)(handle & ~1u))->ninstr : 0;
    return D3D_OK;
}

static LONG NTAPI D3DDevice_GetVertexShaderType(ULONG handle, ULONG *type)
{
    *type = handle & 1 ? 1 /* D3DVST_NORMAL */ : 0;
    return D3D_OK;
}

/* Both copy into `data` when it is big enough and report the size in bytes;
   a NULL `data` only asks for the size.  Half-Life 2 reads its shaders back
   this way and creates new ones from the copies. */
static LONG copy_out(void *data, ULONG *size, const void *a, ULONG a_bytes, const void *b, ULONG b_bytes)
{
    ULONG need = a_bytes + b_bytes;
    if (!data) { *size = need; return D3D_OK; }
    if (*size < need) { *size = need; return 0x887600A3; /* D3DERR_MOREDATA */ }
    memcpy(data, a, a_bytes);
    if (b_bytes) memcpy((char *)data + a_bytes, b, b_bytes);
    *size = need;
    return D3D_OK;
}

static LONG NTAPI D3DDevice_GetVertexShaderDeclaration(ULONG h, void *data, ULONG *size)
{
    if (!(h & 1) || !((vshader *)(h & ~1u))->decl) return D3DERR_INVALIDCALL;
    vshader *sh = (vshader *)(h & ~1u);
    return copy_out(data, size, sh->decl, sh->decl_dwords * 4, NULL, 0);
}

static LONG NTAPI D3DDevice_GetVertexShaderFunction(ULONG h, void *data, ULONG *size)
{
    if (!(h & 1) || !((vshader *)(h & ~1u))->code) return D3DERR_INVALIDCALL;
    vshader *sh = (vshader *)(h & ~1u);
    return copy_out(data, size, &sh->header, 4, sh->code, sh->ninstr * 16);
}

/* Vertex program memory (136 instruction slots), used for state shaders and
   for SelectVertexShader(NULL, address); ordinary programs are compiled from
   their own copy of the microcode. */
static uint32_t vp_mem[136][4];
/* SetVertexShader loads its program at slot 0, as the library does; the
   copy is made only when program memory is next read or written. */
static void vp_flush_pending(void)
{
    vshader *sh = vp_slot0_pending;
    vp_slot0_pending = NULL;
    if (sh && sh->code) memcpy(vp_mem[0], sh->code, (sh->ninstr < 136 ? sh->ninstr : 136) * 16);
}

static void vp_load(const uint32_t *code, unsigned n, ULONG address)
{
    vp_flush_pending();
    if (address >= 136) return;
    if (n > 136 - address) n = 136 - address;
    memcpy(vp_mem[address], code, n * 16);
}

/* Instructions from `address` through the one marked final (NV2A word 3 bit 0). */
static unsigned vp_program_length(ULONG address)
{
    vp_flush_pending();
    unsigned n = 0;
    while (address + n < 136) if (vp_mem[address + n++][3] & 1) break;
    return n;
}

/* Vertex shader objects made by combining one shader's attribute layout
   with another's (or program memory's) microcode: SelectVertexShader runs
   the program at an address with the handle's declaration, and
   SetVertexShaderInput reads the vertex arrays with its handle's layout
   while the current program runs.  Kept for the life of the title (a title
   has a handful), found again by content. */
static vshader **vs_composed;
static unsigned vs_ncomposed;

static bool vs_same_layout(const vshader *a, const vshader *b)
{
    return !memcmp(a->attr, b->attr, sizeof a->attr) && a->tess_normal_in == b->tess_normal_in &&
           a->tess_normal_out == b->tess_normal_out && a->tess_uv_out == b->tess_uv_out;
}

/* A handle drawing with `layout`'s attributes and `code` (ninstr
   instructions, NULL for none), with `program`'s declared constants;
   GetVertexShader reports `user`. */
static ULONG vs_compose(const vshader *layout, const uint32_t *code, unsigned ninstr, const vshader *program, ULONG user)
{
    if (layout == program && program && program->code == code) return (ULONG)program | 1;
    if (program && program->code == code && program->ninstr == ninstr && vs_same_layout(layout, program))
        return (ULONG)program | 1;
    unsigned nconsts = program ? program->nconsts : 0;
    for (unsigned i = 0; i < vs_ncomposed; i++) {
        vshader *c = vs_composed[i];
        if (c->user_handle == user && c->ninstr == ninstr && vs_same_layout(c, layout) &&
            (!ninstr || !memcmp(c->code, code, ninstr * 16)) && c->nconsts == nconsts &&
            (!nconsts || (!memcmp(c->consts, program->consts, nconsts * 16) &&
                          !memcmp(c->const_slots, program->const_slots, nconsts * sizeof(int)))))
            return (ULONG)c | 1;
    }
    vshader *c = calloc(1, sizeof(*c));
    if (!c) return user;
    memcpy(c->attr, layout->attr, sizeof c->attr);
    c->stream_mask = layout->stream_mask;
    c->tess_normal_in = layout->tess_normal_in;
    c->tess_normal_out = layout->tess_normal_out;
    c->tess_uv_out = layout->tess_uv_out;
    if (ninstr) {
        c->code = malloc(ninstr * 16);
        memcpy(c->code, code, ninstr * 16);
        c->ninstr = ninstr;
        c->header = ninstr << 16;
    }
    if (nconsts) {   /* the declaration's constants, which the program's draws apply */
        c->consts = malloc(nconsts * 16);
        c->const_slots = malloc(nconsts * sizeof(int));
        if (c->consts && c->const_slots) {
            memcpy(c->consts, program->consts, nconsts * 16);
            memcpy(c->const_slots, program->const_slots, nconsts * sizeof(int));
            c->nconsts = nconsts;
        }
    }
    c->user_handle = user;
    vshader **n = realloc(vs_composed, (vs_ncomposed + 1) * sizeof(*n));
    if (!n) { free(c->code); free(c); return user; }
    vs_composed = n;
    vs_composed[vs_ncomposed++] = c;
    if (vs_ncomposed == 256 || vs_ncomposed == 4096)
        xlog("D3D: %u combined vertex shader layouts/programs", vs_ncomposed);
    return (ULONG)c | 1;
}

static void NTAPI D3DDevice_LoadVertexShader(ULONG handle, ULONG address)
{
    if (!(handle & 1)) return;
    vshader *sh = (vshader *)(handle & ~1u);
    if (sh->code) vp_load(sh->code, sh->ninstr, address);
}

static void NTAPI D3DDevice_LoadVertexShaderProgram(const ULONG *func, ULONG address)
{
    if (func) vp_load(func + 1, func[0] >> 16, address);
}

/* SetVertexShaderInput state: the layout the vertex arrays follow (0: the
   current shader's, from SetStreamSource), the streams it set, and the
   SetStreamSource bindings it set aside. */
static ULONG vs_input_handle;
static UINT_ vs_input_count;
static ULONG vs_input_streams[16][3];
static struct { D3DResource *vb; ULONG stride; } vs_saved_streams[16];
static ULONG vs_program;   /* while vs_input_handle is set: the shader draws would use without it */

/* The handle draws use for `prog` with the direct input layout, if any. */
static ULONG vs_with_input(ULONG prog)
{
    if (!vs_input_handle || !(prog & 1)) return prog;
    vshader *p = (vshader *)(prog & ~1u), *in = (vshader *)(vs_input_handle & ~1u);
    return vs_compose(in, p->code, p->ninstr, p, p->user_handle ? p->user_handle : prog);
}

/* Make `h` the current shader, under the direct input layout if one is set. */
static void vs_set_program(ULONG h)
{
    if (vs_input_handle) {
        vs_program = h;
        d3d.vertex_shader = (h & 1) ? vs_with_input(h) : vs_input_handle;
    } else {
        d3d.vertex_shader = h;
    }
}

static void NTAPI D3DDevice_SelectVertexShader(ULONG handle, ULONG address)
{
    /* With a handle, its own program runs (the program it was loaded with
       is at `address`); xbcompat's program memory copy is not exact enough
       to trust over it: Splinter Cell's models vanished when the program
       was read back from there.  With no handle, the program at `address`
       runs with the current shader's declaration. */
    if (handle) {
        vs_set_program(handle);
        if (d3d.recording) pb_record(OP_VERTEX_SHADER, &d3d.vertex_shader, 4);
        return;
    }
    ULONG layout_h = vs_input_handle ? vs_program : d3d.vertex_shader;
    if (layout_h & 1) {
        vshader *layout = (vshader *)(layout_h & ~1u);
        ULONG user = layout->user_handle ? layout->user_handle : layout_h;
        const vshader *program = layout->user_handle ? (const vshader *)(layout->user_handle & ~1u) : layout;
        unsigned n = address < 136 ? vp_program_length(address) : 0;
        ULONG h;
        if (program->code && program->ninstr == n && !memcmp(vp_mem[address], program->code, n * 16))
            h = (ULONG)program | 1;   /* its own program: the usual case */
        else if (n)
            h = vs_compose(program, vp_mem[address], n, program, user);
        else
            h = layout_h;
        vs_set_program(h);
    }
    if (d3d.recording && d3d.vertex_shader) pb_record(OP_VERTEX_SHADER, &d3d.vertex_shader, 4);
}
static void NTAPI D3DDevice_RunVertexStateShader(ULONG address, const float *data)
{
    vp_flush_pending();
    if (address >= 136) return;
    vsh_run_state(vp_mem[address], 136 - address, data, d3d.vs_const);
    vsc_touch(0, 192);
}

/* A shim vertex buffer per (buffer, offset) pair: SetVertexShaderInput's
   offset moves where the stream starts, and draws read stream data through
   the resource's Data.  Kept (not reused) so queued drawing never sees one
   change under it. */
static D3DResource *vs_input_shim(D3DResource *vb, ULONG offset)
{
    static struct shim { D3DResource *vb; ULONG data, offset; D3DResource r; } *shims;
    static unsigned nshims;
    if (!vb || !offset) return vb;
    for (unsigned i = 0; i < nshims; i++)
        if (shims[i].vb == vb && shims[i].data == vb->Data && shims[i].offset == offset) return &shims[i].r;
    if (nshims % 64 == 0) {
        /* Grow by a fresh block: pointers into older blocks stay valid. */
        struct shim *blk = calloc(64, sizeof(*blk));
        if (!blk) return vb;
        static struct shim **blocks;
        static unsigned nblocks;
        struct shim **nb = realloc(blocks, (nblocks + 1) * sizeof(*nb));
        if (!nb) { free(blk); return vb; }
        blocks = nb;
        blocks[nblocks++] = blk;
        shims = blk;
        nshims = 0;
    }
    struct shim *s = &shims[nshims++];
    s->vb = vb;
    s->data = vb->Data;
    s->offset = offset;
    s->r = *vb;
    s->r.Data = vb->Data + offset;
    return &s->r;
}

static void NTAPI D3DDevice_SetVertexShaderInput(ULONG handle, UINT_ count, const ULONG *inputs)
{
    if (d3d.recording) { ULONG h[2] = { handle, count }; pb_record2(OP_VS_INPUT, h, 8, inputs, count * 12); }
    if (!handle) {
        /* Back to the streams SetStreamSource set and the shader's own layout. */
        if (vs_input_handle) {
            for (int i = 0; i < 16; i++) {
                d3d.streams[i].vb = vs_saved_streams[i].vb;
                d3d.streams[i].stride = vs_saved_streams[i].stride;
            }
            d3d.vertex_shader = vs_program;
        }
        vs_input_handle = 0;
        vs_input_count = 0;
        return;
    }
    if (!vs_input_handle) {
        for (int i = 0; i < 16; i++) {
            vs_saved_streams[i].vb = d3d.streams[i].vb;
            vs_saved_streams[i].stride = d3d.streams[i].stride;
        }
        vs_program = d3d.vertex_shader;
    }
    vs_input_handle = handle;
    vs_input_count = count > 15 ? 15 : count;
    memcpy(vs_input_streams, inputs, vs_input_count * 12);
    /* D3DSTREAM_INPUT: VertexBuffer, Stride, Offset */
    for (UINT_ i = 0; i < count && i < 16; i++) {
        d3d.streams[i].vb = vs_input_shim((D3DResource *)inputs[i * 3], inputs[i * 3 + 2]);
        d3d.streams[i].stride = inputs[i * 3 + 1];
    }
    /* The current program reads the arrays with the handle's layout (with
       a fixed-function shader current, the handle is drawn as before). */
    d3d.vertex_shader = (vs_program & 1) ? vs_with_input(vs_program) : handle;
}

static bool vs_direct_input(void) { return vs_input_handle != 0; }

static void vs_set_aside_stream(UINT_ Stream, D3DResource *vb, ULONG Stride)
{
    vs_saved_streams[Stream].vb = vb;
    vs_saved_streams[Stream].stride = Stride;
}

static void vs_note_slot0(ULONG Handle)
{
    vshader *sh = (vshader *)(Handle & ~1u);
    if (sh->code) vp_slot0_pending = sh;
}

static void vs_set_program_ext(ULONG Handle)
{
    if (Handle & 1) vs_note_slot0(Handle);
    vs_set_program(Handle);
}

static LONG NTAPI D3DDevice_GetVertexShaderInput(ULONG *pHandle, UINT_ *pCount, ULONG *pInputs)
{
    if (pCount) *pCount = vs_input_count;
    if (pHandle) *pHandle = vs_input_handle;
    if (pInputs) memcpy(pInputs, vs_input_streams, vs_input_count * 12);
    return vs_input_handle ? D3D_OK : 1 /* S_FALSE */;
}

/* The ...Direct forms take a D3DVERTEXATTRIBUTEFORMAT (16 D3DVERTEXSHADERINPUTs:
   StreamIndex, Offset, Format, TessType, TessSource) in place of a handle. */
static ULONG direct_layout_handle(const ULONG *vaf)
{
    static vshader **cache;
    static unsigned n;
    vshader tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.tess_normal_in = tmp.tess_normal_out = tmp.tess_uv_out = -1;
    for (int r = 0; r < 16; r++) {
        const ULONG *in = vaf + r * 4;
        tmp.attr[r].stream = -1;
        UCHAR tess_type = in[3] & 0xFF, tess_src = (in[3] >> 8) & 0xFF;
        if (tess_type == 1) { tmp.tess_normal_in = tess_src; tmp.tess_normal_out = r; }
        else if (tess_type == 2) tmp.tess_uv_out = r;
        vattr a;
        memset(&a, 0, sizeof(a));
        if (in[2] == 0x02 || !decode_vsdt(in[2] & 0xFF, &a)) continue;   /* D3DVSDT_NONE */
        a.stream = in[0] & 0xF;
        a.offset = in[1];
        tmp.attr[r] = a;
        tmp.stream_mask |= 1u << a.stream;
    }
    for (unsigned i = 0; i < n; i++)
        if (vs_same_layout(cache[i], &tmp)) return (ULONG)cache[i] | 1;
    vshader *sh = malloc(sizeof(*sh));
    vshader **nc = sh ? realloc(cache, (n + 1) * sizeof(*nc)) : NULL;
    if (!nc) { free(sh); return 0; }
    *sh = tmp;
    cache = nc;
    cache[n++] = sh;
    return (ULONG)sh | 1;
}

static void NTAPI D3DDevice_SetVertexShaderInputDirect(const ULONG *vaf, UINT_ count, const ULONG *inputs)
{
    D3DDevice_SetVertexShaderInput(vaf ? direct_layout_handle(vaf) : 0, count, inputs);
}

static void NTAPI D3DDevice_SelectVertexShaderDirect(const ULONG *vaf, ULONG address)
{
    D3DDevice_SelectVertexShader(vaf ? direct_layout_handle(vaf) : 0, address);
}


/* Vertex shader objects whose program writes back colors (oB0/oB1): only
   those are drawn with two-sided color, as vsh.c leaves the back colors out
   of the rest. */
static unsigned char *two_side_shaders;
static GLuint two_side_cap;

static void note_two_side(GLuint vs, const char *src)
{
    if (!vs) return;
    if (vs >= two_side_cap) {
        GLuint cap = two_side_cap ? two_side_cap : 256;
        while (cap <= vs) cap *= 2;
        unsigned char *n = realloc(two_side_shaders, cap);
        if (!n) return;
        memset(n + two_side_cap, 0, cap - two_side_cap);
        two_side_shaders = n; two_side_cap = cap;
    }
    two_side_shaders[vs] = strstr(src, "gl_BackColor") != NULL;
}

static bool shader_two_side(GLuint vs)
{
    return vs < two_side_cap && two_side_shaders[vs];
}

static GLuint vertex_shader_generic_object(vshader *sh)
{
    if (sh->vs || sh->failed) return sh->vs;
    char *src = vsh_translate(sh->code, sh->ninstr);
    if (!src) {
        xlog("D3D: vertex program (%u instructions) could not be translated: %s", sh->ninstr, vsh_error);
        /* Log the first few programs' microcode so they can be fixed offline. */
        static int dumped;
        if (dumped++ < 4)
            for (unsigned i = 0; i < sh->ninstr; i++)
                xlog("D3D:   vsh %3u: %08x %08x %08x %08x", i, sh->code[i * 4], sh->code[i * 4 + 1],
                     sh->code[i * 4 + 2], sh->code[i * 4 + 3]);
        sh->failed = true;
        return 0;
    }
    if (getenv("XBCOMPAT_VSH_DUMP")) {
        /* Debug aid: the declaration that goes with each dumped program. */
        char line[512]; int n = 0;
        for (int r = 0; r < 16; r++)
            if (sh->attr[r].stream >= 0)
                n += snprintf(line + n, sizeof line - n, " v%d=s%d+%u:%#x", r, sh->attr[r].stream,
                              sh->attr[r].offset, sh->attr[r].type);
        xlog("D3D: vertex program %u instructions, inputs%s", sh->ninstr, line);
    }
    sh->vs = compile_shader(GL_VERTEX_SHADER, src);
    note_two_side(sh->vs, src);
    free(src);
    if (!sh->vs) sh->failed = true;
    return sh->vs;
}

static GLuint vertex_shader_object(vshader *sh)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XBCOMPAT_VSH_Z_VARIANT");
        enabled = e && *e && *e != '0';
    }
    /* XBCOMPAT_VSH_Z_VARIANT: a variant without the w-buffer depth math,
       for draws that don't use D3DZB_USEW; the generic program otherwise. */
    if (enabled && RS(D3DRS_ZENABLE) != 2 /* D3DZB_USEW */) {
        if (!sh->vs_z_attempted) {
            sh->vs_z_attempted = true;
            char *src = vsh_translate_ex(sh->code, sh->ninstr, VSH_VARIANT_Z);
            if (src) {
                sh->vs_z = compile_shader(GL_VERTEX_SHADER, src);
                note_two_side(sh->vs_z, src);
                free(src);
            }
        }
        if (sh->vs_z) return sh->vs_z;
    }
    return vertex_shader_generic_object(sh);
}

static program_entry *program_for(GLuint vs, GLuint fs)
{
    /* Every draw looks its program up; a direct-mapped table in front of the
       list (cleared when programs are deleted) skips the walk. */
    program_entry **slot = &program_lookup[(vs * 31u + fs) & 255];
    if (*slot && (*slot)->vs == vs && (*slot)->fs == fs) return *slot;
    for (program_entry *e = programs; e; e = e->next)
        if (e->vs == vs && e->fs == fs) return *slot = e;
    program_entry *e = calloc(1, sizeof(*e));
    e->vs = vs;
    e->fs = fs;
    e->prog = link_program(vs, fs);
    if (e->prog) {
        for (int r = 0; r < 16; r++) {
            char name[4];
            snprintf(name, sizeof name, "v%d", r);
            e->attr[r] = p_glGetAttribLocation(e->prog, name);
            if (e->attr[r] >= 0) e->attr_mask |= 1u << e->attr[r];
        }
#define U(name) p_glGetUniformLocation(e->prog, name)
        e->loc_c = U("c");
        /* Element i of c at loc_c + i, as Mesa lays arrays out: only then
           can part of it be uploaded on its own. */
        e->c_size = 192;
        if (e->loc_c >= 0 && p_glGetActiveUniform) {
            GLint n = 0;
            p_glGetProgramiv(e->prog, 0x8B86 /* GL_ACTIVE_UNIFORMS */, &n);
            for (GLint i = 0; i < n; i++) {
                char name[16];
                GLint size = 0;
                GLenum type = 0;
                p_glGetActiveUniform(e->prog, i, sizeof name, NULL, &size, &type, name);
                if (!strcmp(name, "c") || !strcmp(name, "c[0]")) { e->c_size = size < 192 ? size : 192; break; }
            }
        }
        char last[16];
        snprintf(last, sizeof last, "c[%d]", e->c_size - 1);
        e->c_partial = e->loc_c >= 0 && U(last) == e->loc_c + e->c_size - 1;
        TRACE("D3D: program %u: c has %d active elements, partial uploads %s", e->prog, e->c_size,
              e->c_partial ? "on" : "off");
        e->loc_flip_y = U("flip_y");
        e->loc_vp_scale = U("vp_scale");
        e->loc_vp_offset = U("vp_offset");
        e->loc_wdepth = U("wdepth");
        static const char *tex[] = { "tex0", "tex1", "tex2", "tex3" }, *cube[] = { "cube0", "cube1", "cube2", "cube3" },
                          *vol[] = { "vol0", "vol1", "vol2", "vol3" };
        for (int i = 0; i < 4; i++) {
            e->loc_tex[i] = U(tex[i]);
            e->loc_cube[i] = U(cube[i]);
            e->loc_vol[i] = U(vol[i]);
        }
        e->loc_tex_scale = U("tex_scale");
        e->loc_c0 = U("c0");
        e->loc_c1 = U("c1");
        e->loc_fc0 = U("fc0");
        e->loc_fc1 = U("fc1");
        e->loc_bump_env = U("bump_env");
        e->loc_bump_lum = U("bump_lum");
        e->loc_ff_lighting = U("ff_lighting");
        e->loc_ff_transform = U("ff_transform");
        e->loc_ff_texcount = U("ff_texcount");
        e->loc_fog_vertex_mode = U("fog_vertex_mode");
        e->loc_zw_scale = U("zw_scale");
        e->loc_shadow_func = U("shadow_func");
        e->loc_shadow_dim = U("shadow_dim");
        e->loc_eye_vector = U("eye_vector");
        e->loc_key_color = U("key_color");
#undef U
    }
    e->next = programs;
    programs = e;
    return *slot = e;
}

/* ---- pixel shaders: fragment programs from the combiner state ---------- */

/* Fragment programs are cached on the render states psh_translate reads,
   minus the combiner constants, which are uniforms. */
typedef struct fshader_entry {
    uint32_t key[64];
    uint32_t hash;
    GLuint fs;
    struct fshader_entry *next;
} fshader_entry;

static fshader_entry *fshaders;
static fshader_entry *fshader_lookup[256];   /* direct-mapped by hash, in front of the list */

static uint32_t fshader_hash(const uint32_t *key)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 64; i++) h = (h ^ key[i]) * 16777619u;
    return h;
}

/* PS_GLOBALFLAGS_TEXMODE_ADJUST (bit 8 of PSFinalCombinerConstants): like
   the real library's LazySetShaderStageProgram, pick each stage's texture
   mode from the texture that is set, so one shader serves 2D, 3D and cube
   textures, and a stage that needs a texture but has none is switched off. */
static ULONG adjusted_texture_modes(void)
{
    ULONG modes = RS(D3DRS_PSTEXTUREMODES);
    const ULONG *def = (const ULONG *)d3d.pixel_shader;
    if (!def || !(def[59] & 0x100)) return modes;
    ULONG out = 0;
    for (int s = 3; s >= 0; s--) {
        ULONG m = (modes >> (s * 5)) & 0x1F;
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        GLenum target = t ? tex_target(t) : 0;
        if (!t && m != 0x04 && m != 0x05 && m != 0x0A && m != 0x11)
            m = 0;
        else if (t && m >= 0x01 && m <= 0x03) {
            /* A depth image (a shadow buffer) becomes PROJECT3D, as a volume
               does, so the compare gets its reference from r/q: the library
               picks PROJECT3D for every depth color format here.  Kept as
               PROJECT2D, the compare used a zero reference and the ATG
               ShadowBuffer sample (a "tex t1" shader) drew no shadow. */
            bool depth = target == GL_TEXTURE_2D && is_depth_format((t->Format >> 8) & 0xFF);
            m = target == GL_TEXTURE_CUBE_MAP ? 0x03 : target == GL_TEXTURE_3D || depth ? 0x02 : 0x01;
        }
        else if (t && (m == 0x0D || m == 0x0E))
            m = target == GL_TEXTURE_CUBE_MAP ? 0x0E : 0x0D;
        out = (out << 5) | m;
    }
    return out;
}

/* Stages whose texture is a depth format: sampled as shadow buffers. */
static ULONG shadow_stages(void)
{
    ULONG mask = 0;
    for (int s = 0; s < 4; s++) {
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        if (t && tex_target(t) == GL_TEXTURE_2D && is_depth_format((t->Format >> 8) & 0xFF)) mask |= 1u << s;
    }
    return mask;
}

static psh_options fragment_shader_options(void)
{
    psh_options o = {0};
    for (int i = 0; i < 4; i++) {
        if (TSS(i, D3DTSS_ALPHAKILL) & 4) o.alpha_kill |= 1u << i;
        o.color_key[i] = TSS(i, D3DTSS_COLORKEYOP) & 3;
        const D3DPixelContainer *t = (const D3DPixelContainer *)d3d.textures[i];
        if (!t) continue;
        ULONG fmt = (t->Format >> 8) & 0xFF;
        if (fmt == 0x03 || fmt == 0x1C || fmt == 0x07 || fmt == 0x1E)
            o.color_key_ignore_alpha |= 1u << i;
        /* X8L8V8U8 aliases X8R8G8B8: preserve ordinary RGB sampling, but
           tell bump stages where U, V and luminance landed in that upload. */
        if (fmt == 0x07 || fmt == 0x1E) o.bump_bgra |= 1u << i;
        if (fmt == 0x28 || fmt == 0x17 || fmt == 0x27 || fmt == 0x37 || fmt == 0x33 || fmt == 0x36)
            o.bump_biased |= 1u << i;
    }
    return o;
}

/* Fixed-function stages as the combiner state the library would load
   (ff_combiner.h): ps[] gets the D3DRS_PS* words, the texture modes are
   returned. */
static ULONG ff_fragment_state(uint32_t *ps)
{
    int kind[4];
    for (int s = 0; s < 4; s++) {
        const D3DPixelContainer *t = (const D3DPixelContainer *)d3d.textures[s];
        GLenum target = t ? tex_target(t) : 0;
        kind[s] = !t ? FFC_TEX_NONE : target == GL_TEXTURE_CUBE_MAP ? FFC_TEX_CUBE
                : target == GL_TEXTURE_3D ? FFC_TEX_3D : FFC_TEX_2D;
    }
    uint32_t rs[D3DRS_MAX];
    ff_combiner_state(rs, d3d.texture_state, kind, RS(D3DRS_POINTSPRITEENABLE) != 0);
    memcpy(ps, rs, D3DRS_PS_MAX * 4);
    return rs[D3DRS_PSTEXTUREMODES];
}

static void fshader_key(uint32_t *key, const psh_options *o, const uint32_t *ps, ULONG modes)
{
    memset(key, 0, 64 * 4);
    memcpy(key, ps, D3DRS_PS_MAX * 4);
    for (int i = D3DRS_PSCONSTANT0_0; i <= D3DRS_PSCONSTANT1_7; i++) key[i] = 0;
    key[D3DRS_PSFINALCOMBINERCONSTANT0] = key[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
    key[57] = modes;
    key[58] = RS(D3DRS_FOGENABLE);
    key[59] = RS(D3DRS_FOGTABLEMODE);
    key[60] = shadow_stages();
    key[61] = RS(D3DRS_SPECULARENABLE) != 0;
    key[62] = o->alpha_kill;
    key[63] = (o->color_key_ignore_alpha << 8) | (o->bump_bgra << 12) | (o->bump_biased << 16);
    for (int i = 0; i < 4; i++) key[63] |= o->color_key[i] << (i * 2);
}

static GLuint fragment_shader_object(void)
{
    uint32_t key[64], ps[D3DRS_PS_MAX];
    ULONG modes;
    /* A fixed-function draw gets here only for stages GL's texture
       environment cannot express (ff_fragment_program). */
    if (d3d.pixel_shader) {
        memcpy(ps, d3d.render_state, sizeof(ps));
        modes = adjusted_texture_modes();
    } else {
        modes = ff_fragment_state(ps);
    }
    psh_options options = fragment_shader_options();
    fshader_key(key, &options, ps, modes);
    uint32_t hash = fshader_hash(key);
    fshader_entry **slot = &fshader_lookup[(hash ^ hash >> 16) & 255];
    if (*slot && (*slot)->hash == hash && gc_same((*slot)->key, key, 64)) return (*slot)->fs;
    for (fshader_entry *e = fshaders; e; e = e->next)
        if (e->hash == hash && gc_same(e->key, key, 64)) return (*slot = e)->fs;
    fshader_entry *e = calloc(1, sizeof(*e));
    memcpy(e->key, key, sizeof(key));
    e->hash = hash;
    *slot = e;
    ULONG rs[D3DRS_MAX];   /* psh.c reads 4400 numbering */
    for (int i = 0; i < D3DRS_MAX; i++) rs[i] = RS(i);
    memcpy(rs, ps, sizeof(ps));
    rs[D3DRS_PSTEXTUREMODES] = modes;
    psh_shadow_stages = key[60];
    char *src = psh_translate_ex(rs, &options);
    if (!src) {
        xlog("D3D: pixel shader %#x could not be translated", d3d.pixel_shader);
    } else {
        /* Debug aid: XBCOMPAT_PSH_DUMP=1 logs every translated pixel shader. */
        if (getenv("XBCOMPAT_PSH_DUMP")) xlog("D3D: pixel shader %#x:\n%s", d3d.pixel_shader, src);
        e->fs = compile_shader(GL_FRAGMENT_SHADER, src);
        free(src);
    }
    e->next = fshaders;
    fshaders = e;
    return e->fs;
}

/* Bind the textures of stages 0..3 to GL units 0..3 for a fragment program
   and point its samplers at them. */
static void apply_shader_textures(program_entry *e)
{
    float scale[4][4], shadow_func[4][4] = { { 0 } }, shadow_dim[4][4] = { { 0 } };
    for (int s = 0; s < 4; s++) {
        scale[s][0] = scale[s][1] = scale[s][2] = scale[s][3] = 1;
        p_glActiveTexture(GL_TEXTURE0 + s);
        /* With GLSL 1.20 the point sprite replacement feeds gl_TexCoord.
           Set every unit here too: a pixel shader draw may not have passed
           through fixed-function texture setup, or may follow a sprite. */
        glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, s == 3 && RS(D3DRS_POINTSPRITEENABLE));
        /* A stage the program never samples keeps whatever is bound: no
           upload, no readback for a texture it would not read. */
        if (e->loc_tex[s] < 0 && e->loc_cube[s] < 0 && e->loc_vol[s] < 0) continue;
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        if (!t) {
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            glBindTexture(GL_TEXTURE_3D, 0);
            continue;
        }
        GLenum target = tex_target(t);
        tex_use_palette(s);
        glBindTexture(target, texture_for(t));
        apply_sampler(target, s);
        if (t->Size) {   /* linear textures are addressed in texels */
            ULONG w, h, pitch;
            container_size(t, &w, &h, &pitch);
            scale[s][0] = 1.0f / w;
            scale[s][1] = 1.0f / h;
        }
        if (is_depth_format((t->Format >> 8) & 0xFF))   /* r/q is in depth buffer steps */
            scale[s][2] = 1.0f / depth_format_max((t->Format >> 8) & 0xFF);
        if (e->loc_shadow_func >= 0 && target == GL_TEXTURE_2D && is_depth_format((t->Format >> 8) & 0xFF)) {
            /* The program compares whole depth steps itself (psh.c
               shadow_lookup): it reads the raw texels, and filters the
               compares rather than the depths. */
            ULONG w, h, pitch, func = RS(D3DRS_SHADOWFUNC) & 7;
            container_size(t, &w, &h, &pitch);
            sampler_i(target, s, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            sampler_i(target, s, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            sampler_i(target, s, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            shadow_func[s][0] = func & 1; shadow_func[s][1] = (func >> 1) & 1; shadow_func[s][2] = (func >> 2) & 1;
            shadow_func[s][3] = depth_format_max((t->Format >> 8) & 0xFF);
            shadow_dim[s][0] = w; shadow_dim[s][1] = h;
            shadow_dim[s][2] = TSS(s, D3DTSS_MAGFILTER) != 1 /* D3DTEXF_POINT */;
        }
        /* The program declares at most one sampler per stage, of the type its
           texture mode wants; point it at unit s whatever is bound there. */
        if (e->loc_tex[s] >= 0) program_uniform1i(e, e->loc_tex[s], s);
        if (e->loc_cube[s] >= 0) program_uniform1i(e, e->loc_cube[s], s);
        if (e->loc_vol[s] >= 0) program_uniform1i(e, e->loc_vol[s], s);
    }
    p_glActiveTexture(GL_TEXTURE0);
    program_uniform4fv(e, e->loc_tex_scale, 4, &scale[0][0]);
    /* DOT_ZW writes depth in steps of the target's depth format. */
    if (e->loc_zw_scale >= 0) program_uniform1f(e, e->loc_zw_scale, d3d.target_zmax);
    if (e->loc_shadow_func >= 0) program_uniform4fv(e, e->loc_shadow_func, 4, &shadow_func[0][0]);
    if (e->loc_shadow_dim >= 0) program_uniform4fv(e, e->loc_shadow_dim, 4, &shadow_dim[0][0]);
    /* shadow_centre reads flip_y too, also under fixed-function vertices. */
    if (e->loc_shadow_func >= 0 && e->loc_flip_y >= 0)
        program_uniform1f(e, e->loc_flip_y, d3d.rt_texture ? -1.0f : 1.0f);
}


/* The per-draw inputs of a fragment program. */
static void upload_ps_uniforms(program_entry *e)
{
    float c[8][4];
    if (e->loc_eye_vector >= 0) program_uniform4fv(e, e->loc_eye_vector, 1, d3d.eye_vector);
    if (e->loc_key_color >= 0) {
        for (int i = 0; i < 4; i++) color4(c[i], TSS(i, D3DTSS_COLORKEYCOLOR));
        program_uniform4fv(e, e->loc_key_color, 4, &c[0][0]);
    }
    if (e->loc_c0 >= 0) {
        for (int i = 0; i < 8; i++) color4(c[i], RS(D3DRS_PSCONSTANT0_0 + i));
        program_uniform4fv(e, e->loc_c0, 8, &c[0][0]);
    }
    /* A fixed-function program (ff_combiner.h) reads TFACTOR as C1, and
       a bump lookup in unit s uses the BUMPENV states of D3D stage s - 1. */
    bool ff = !d3d.pixel_shader;
    if (e->loc_c1 >= 0) {
        for (int i = 0; i < 8; i++) color4(c[i], RS(ff ? D3DRS_TEXTUREFACTOR : D3DRS_PSCONSTANT1_0 + i));
        program_uniform4fv(e, e->loc_c1, 8, &c[0][0]);
    }
    if (e->loc_fc0 >= 0) { color4(c[0], RS(D3DRS_PSFINALCOMBINERCONSTANT0)); program_uniform4fv(e, e->loc_fc0, 1, c[0]); }
    if (e->loc_fc1 >= 0) { color4(c[0], RS(D3DRS_PSFINALCOMBINERCONSTANT1)); program_uniform4fv(e, e->loc_fc1, 1, c[0]); }
    if (e->loc_bump_env >= 0) {
        for (int s = 0; s < 4; s++) {
            int from = ff ? (s ? s - 1 : 0) : s;
            c[s][0] = tss_float(from, D3DTSS_BUMPENVMAT00);
            c[s][1] = tss_float(from, D3DTSS_BUMPENVMAT01);
            c[s][2] = tss_float(from, D3DTSS_BUMPENVMAT10);
            c[s][3] = tss_float(from, D3DTSS_BUMPENVMAT11);
        }
        program_uniform4fv(e, e->loc_bump_env, 4, &c[0][0]);
    }
    if (e->loc_bump_lum >= 0) {
        float lum[4][2];
        for (int s = 0; s < 4; s++) {
            int from = ff ? (s ? s - 1 : 0) : s;
            lum[s][0] = tss_float(from, D3DTSS_BUMPENVLSCALE);
            lum[s][1] = tss_float(from, D3DTSS_BUMPENVLOFFSET);
        }
        program_uniform2fv(e, e->loc_bump_lum, 4, &lum[0][0]);
    }
    DETAIL_TIMED(t_textures, apply_shader_textures(e));
}

/* Fog parameters, read through gl_Fog by translated shaders and by GL's
   own fog stage.  LazySetSpecFogCombiner programs linear fog as
   (end - z) * scale with scale = 1 / (end - start), and with
   MAX_FOG_SCALE (8192) when the endpoints are equal: a hard step at end.
   GL derives gl_Fog.scale from start and end itself, so move start just
   below end to get that steep scale instead of an infinite one. */
static void apply_fog_params(void)
{
    float c[4];
    color4(c, RS(D3DRS_FOGCOLOR));
    glFogfv(GL_FOG_COLOR, c);
    float start = rs_float(D3DRS_FOGSTART), end = rs_float(D3DRS_FOGEND);
    if (start == end) start = end - fmaxf(1.0f / 8192.0f, fabsf(end) * (1.0f / 1048576.0f));
    glFogf(GL_FOG_START, start);
    glFogf(GL_FOG_END, end);
    glFogf(GL_FOG_DENSITY, rs_float(D3DRS_FOGDENSITY));
}

/* Fog for draws GL's fixed fragment stage colors (no fragment program):
   the fog table mode applies to the eye distance, or to oFog of a vertex
   program.  D3DFOG_NONE takes the factor itself from oFog, which linear
   fog from 1 to 0 passes through; without a vertex program it would be
   the specular alpha, which the GL stage cannot read, so that stays off. */
static void apply_fixed_fog(bool programmable_vertex)
{
    ULONG mode = RS(D3DRS_FOGTABLEMODE);
    bool on = RS(D3DRS_FOGENABLE) && mode <= 3 && (mode || programmable_vertex);
    gc_enable(GL_FOG, on);
    if (!on) return;
    static GLint last_mode;
    GLint gl_mode = mode == 1 ? GL_EXP : mode == 2 ? GL_EXP2 : GL_LINEAR;
    if (gl_mode != last_mode) { glFogi(GL_FOG_MODE, gl_mode); last_mode = gl_mode; }
    apply_fog_params();
    if (!mode) { glFogf(GL_FOG_START, 1.0f); glFogf(GL_FOG_END, 0.0f); }
}

/* Fixed-function GL work (draws without shaders, glDrawPixels) needs
   program 0; draws leave their program bound. */
static void program_off(void)
{
    hud_batch_flush();
    if (cur_program) p_glUseProgram(0);
    cur_program = 0;
    glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
    glDisable(GL_VERTEX_PROGRAM_TWO_SIDE);
}


/* Owned FF vertex subset: lighting, weighted transforms, full COLOR2,
   and normal/eye/object texgen. Keep unresolved modes on the existing
   path; see docs/nv2a-ff-lighting-change.md for the tested boundary. */
static bool fixed_lighting_supported(void)
{
    ULONG blend = RS(118 /* VERTEXBLEND */);
    if (blend > 6 || (blend && ff_weight_components < blend / 2 + 1)) return false;
    if (!ff_lighting_candidate || RS(122 /* TWOSIDEDLIGHTING */)
        || RS(D3DRS_FOGENABLE) || RS(D3DRS_POINTSCALEENABLE) || RS(D3DRS_POINTSPRITEENABLE)
        || RS(D3DRS_ZENABLE) == 2)
        return false;
    static const int sources[4] = { 101, 102, 100, 103 }; /* diffuse, ambient, specular, emissive */
    for (int k = 0; k < 4; k++) {
        ULONG source = RS(sources[k]);
        bool active = k != 2 || RS(D3DRS_SPECULARENABLE);
        if (active && RS(D3DRS_COLORVERTEX) && source > 2)
            return false;
        ULONG gen = TSS(k, D3DTSS_TEXCOORDINDEX) >> 16;
        if (gen && gen != 1 && gen != 2 && gen != 4) return false;
    }
    for (int i = 0; i < 8; i++)
        if (d3d.lights[i].enabled && d3d.lights[i].type != 1 && d3d.lights[i].type != 3)
            return false;
    return isfinite(d3d.material_power) && d3d.material_power >= 0;
}

static GLuint fixed_lighting_shader(void)
{
    static GLuint objects[128];
    static bool attempted[128];
    if (!fixed_lighting_supported()) return 0;
    /* One program per set of inputs the draw has (FF_INPUTS): the shader
       then reads no attribute the vertex lacks, and VC4 stays within its
       eight attributes. */
    unsigned key = (ff_texture_units & 15) | (RS(118 /* VERTEXBLEND */) != 0) << 4 | ff_secondary_present << 5
                   | ff_primary_present << 6;
    if (!attempted[key]) {
        attempted[key] = true;
        size_t size = sizeof ff_lighting_source + 64;
        char *source = malloc(size);
        if (!source) return 0;
        snprintf(source, size, "#version 120\n#define FF_INPUTS %u\n%s", key,
                 ff_lighting_source + 13);   /* past its own #version line */
        objects[key] = compile_shader(GL_VERTEX_SHADER, source);
        free(source);
    }
    return objects[key];
}

static void upload_fixed_lighting(program_entry *e)
{
    if (e->loc_ff_lighting < 0) return;
    float state[FF_LIGHTING_VECTORS][4] = { { 0 } };
    ULONG blend = RS(118 /* VERTEXBLEND */);
    state[7][0] = blend ? (blend + 3) / 2 : 0;
    state[7][1] = blend & 1; /* odd modes generate the final weight */
    state[7][2] = ff_pretransformed || !RS(D3DRS_LIGHTING);
    if (e->loc_ff_texcount >= 0) {
        float counts[4];
        for (int i = 0; i < 4; i++) {
            ULONG flags = TSS(i, 21 /* TEXTURETRANSFORMFLAGS */);
            counts[i] = (float)(flags & 0xFF) * ((flags & 0x100) ? 1 : -1);
        }
        program_uniform4fv(e, e->loc_ff_texcount, 1, counts);
    }
    for (int i = 0; i < 4; i++) state[8][i] = TSS(i, D3DTSS_TEXCOORDINDEX) >> 16;
    if (blend && e->loc_ff_transform >= 0) {
        float transform[28][4] = {{0}};
        /* SetVertexBlendModelView's matrices, else world i * view. */
        bool direct = d3d.direct_mv.on && d3d.direct_mv.blend;
        for (unsigned i = 0; i < 4; i++) {
            D3DMATRIX mv;
            if (direct) mv = d3d.direct_mv.mv[i];
            else mat_mul(&mv, &d3d.transforms[6+i], &d3d.transforms[0]);
            memcpy(transform + 7*i, &mv, sizeof mv);
            if (direct && d3d.direct_mv.has_inv && i < d3d.direct_mv.count) {
                /* The title's inverse: normals go through its transpose. */
                for (int row = 0; row < 3; row++)
                    for (int col = 0; col < 3; col++)
                        transform[7*i+4+row][col] = d3d.direct_mv.inv[i].m[col][row];
                continue;
            }
            /* Cofactor(M)/det(M) is the inverse transpose for row-vector
               normal transforms. Translation never participates. */
            float cofactor[3][3];
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++) {
                    int r1=(row+1)%3, r2=(row+2)%3, c1=(col+1)%3, c2=(col+2)%3;
                    cofactor[row][col] = mv.m[r1][c1]*mv.m[r2][c2] - mv.m[r1][c2]*mv.m[r2][c1];
                }
            float det = mv.m[0][0]*cofactor[0][0] + mv.m[0][1]*cofactor[0][1] + mv.m[0][2]*cofactor[0][2];
            if (det != 0)
                for (int row = 0; row < 3; row++)
                    for (int col = 0; col < 3; col++)
                        transform[7*i+4+row][col] = cofactor[row][col]/det;
        }
        program_uniform4fv(e, e->loc_ff_transform, 28, &transform[0][0]);
    }
    state[0][0] = RS(D3DRS_SPECULARENABLE) != 0;
    state[0][1] = RS(D3DRS_NORMALIZENORMALS) != 0;
    state[0][2] = RS(94 /* LOCALVIEWER */) != 0;
    state[0][3] = d3d.material_power;
    static const int sources[4] = { 101, 102, 100, 103 };
    for (int k = 0; k < 4; k++) {
        ULONG source = RS(D3DRS_COLORVERTEX) ? RS(sources[k]) : 0;
        if ((source == 1 && !ff_primary_present) || (source == 2 && !ff_secondary_present)) source = 0;
        state[1][k] = source;
    }
    color4(state[2], RS(D3DRS_AMBIENT));
    memcpy(state[3], d3d.material, sizeof d3d.material);
    for (int i = 0; i < 8; i++) {
        typeof(d3d.lights[0]) *light = &d3d.lights[i];
        if (!light->enabled) continue;
        int base = 9 + 5 * i;
        float world[4], eye[4] = { 0 };
        if (light->type == 3) {
            for (int j = 0; j < 3; j++) world[j] = -light->direction[j];
            world[3] = 0;
        } else {
            memcpy(world, light->position, sizeof world);
        }
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                eye[j] += world[k] * d3d.transforms[0].m[k][j];
        for (int j = 0; j < 3; j++)
            state[base][j] = light->type == 3 ? eye[j] : eye[j] / eye[3];
        state[base][3] = light->type;
        memcpy(state[base + 1], light->ambient, 16);
        memcpy(state[base + 2], light->diffuse, 16);
        memcpy(state[base + 3], light->specular, 16);
        memcpy(state[base + 4], light->attenuation, 12);
        state[base + 4][3] = light->range;
    }
    program_uniform4fv(e, e->loc_ff_lighting, FF_LIGHTING_VECTORS, &state[0][0]);
}

/* Select the program for a draw: `vs` is the vertex shader object (0 for
   fixed function); the fragment side follows the title's pixel shader.
   Returns false when the draw must be skipped (a shader failed), else sets
   *out to the program in use, or NULL when everything is fixed function. */
static bool use_program(GLuint vs, program_entry **out)
{
    *out = NULL;
    GLuint fs = 0;
    if (d3d.pixel_shader || ff_fragment_program()) {
        DETAIL_TIMED(t_fshader, fs = fragment_shader_object());
        if (!fs) return false;
    }
    /* NV2A programmable oPts is selected only with point parameters enabled.
       Fixed-function attenuation remains owned by the fixed-function path.
       Synchronize on every draw, including cached program reuse and VS -> FF. */
    if (vs && RS(122 /* TWOSIDEDLIGHTING */) && shader_two_side(vs)) glEnable(GL_VERTEX_PROGRAM_TWO_SIDE);
    else glDisable(GL_VERTEX_PROGRAM_TWO_SIDE);
    if (vs && RS(D3DRS_POINTSCALEENABLE)) glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
    else glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
    bool programmable_vertex = vs != 0;
    if (fs) apply_fog_params();
    else apply_fixed_fog(programmable_vertex);
    if (fs && (whatif == WHATIF_FLAT_FS || whatif == WHATIF_BOTH)) {
        static GLuint flat;
        if (!flat)
            flat = compile_shader(GL_FRAGMENT_SHADER,
                                  "#version 120\nuniform sampler2D tex0;\nuniform vec4 tex_scale[4];\n"
                                  "void main() { gl_FragColor = gl_Color * texture2D(tex0, gl_TexCoord[0].xy * tex_scale[0].xy); }\n");
        if (flat) fs = flat;
    }
    if (!vs) vs = fixed_lighting_shader();
    if (!vs && !fs) { program_off(); return true; }
    program_entry *e = program_for(vs, fs);
    if (!e->prog) return false;
    /* The program stays bound after the draw: consecutive draws with the
       same program (most of a frame) skip the switch. */
    if (cur_program != e->prog) p_glUseProgram(e->prog);
    cur_program = e->prog;
    if (fs) upload_ps_uniforms(e);
    if (e->loc_fog_vertex_mode >= 0) {
        ULONG mode = RS(D3DRS_FOGTABLEMODE);
        /* Only paired programmable stages share the factor convention.
           Legacy vertex/fragment stages retain their coordinate convention.
           Equal linear endpoints get a finite scale from apply_fog_params. */
        bool factor = programmable_vertex && fs && RS(D3DRS_FOGENABLE)
            && mode >= 1 && mode <= 3;
        program_uniform1f(e, e->loc_fog_vertex_mode, factor ? mode : 0);
    }
    upload_fixed_lighting(e);
    *out = e;
    return true;
}

/* The program stays bound until a draw needs another (use_program binds 0
   for fixed function), so back-to-back draws with one program switch nothing. */
static void end_program(program_entry *e)
{
    (void)e;
}

/* The vertices a draw reads: indices lo to hi - 1 of the streams. Titles
   pack many meshes into one vertex buffer (the dashboard does), so
   converting from vertex 0 would redo every earlier mesh on each draw. */
typedef struct { ULONG lo, hi; } vrange;

/* Conversions and index scans are kept between draws: the dashboard draws
   the same large meshes every frame, and redoing them was most of its frame
   under box86. An entry keeps a copy of the source bytes it was made from.
   Comparing all of it on every draw still cost a fifth of the Pi's CPU (the
   dashboard reads about 3 MB a frame), so a draw compares 33 samples spread
   over the data and the whole copy is compared every CONV_FULL_EVERY frames:
   a rewritten buffer is caught at once, a small edit to one within that
   many frames.  That delay is too long for data the title is known to
   rewrite, so two cases are compared in full on every use: index lists
   (small next to the vertices, and rebuilt in place per view: a batch of
   the same length with a few triangles swapped kept drawing the old ones),
   and vertex data in a range locked for writing since the entry last
   looked (see conv_locked). */
#define CONV_FULL_EVERY 16
enum { CONV_RANGE, CONV_NORMPACKED3, CONV_RGBA, CONV_VBO };
typedef struct {
    const UCHAR *src;
    ULONG stride, kind, count;   /* count: CONV_RANGE's index count */
    ULONG lo, hi;                /* the vertices converted (CONV_RANGE: the result) */
    UCHAR *raw;                  /* the source bytes, from vertex lo */
    size_t raw_len;
    UCHAR *out;                  /* converted vertex lo */
    unsigned used;               /* draw serial of the last use */
    ULONG checked;               /* frame of the last full compare */
    GLuint buf;                  /* CONV_RANGE: the indices; CONV_VBO: the vertices */
    size_t span;                 /* CONV_VBO: bytes from vertex 0 in buf and raw */
    unsigned verified;           /* draw serial of the last check against the source */
    size_t verified_to;          /* CONV_VBO: bytes from vertex 0 that check covered */
    unsigned lock_seen;          /* vb_lock_serial when the entry last looked at the locks */
} conv_entry;

/* Vertex buffer ranges locked for writing, the last VB_LOCKS of them.  A
   lock is the title saying it rewrites those bytes (a dynamic buffer
   refilled each frame, lighting colors or projector coordinates computed on
   the CPU), which the sampled compare can miss for CONV_FULL_EVERY frames.
   The title's thread adds them and the draws (maybe on the worker) read
   them; a lost race only falls back to sampling. */
#define VB_LOCKS 64
static struct { uintptr_t lo, hi; } vb_locks[VB_LOCKS];
static unsigned vb_lock_serial;

static void note_vb_lock(const UCHAR *p, size_t len)
{
    unsigned n = __atomic_load_n(&vb_lock_serial, __ATOMIC_RELAXED);
    vb_locks[n % VB_LOCKS].lo = (uintptr_t)p;
    vb_locks[n % VB_LOCKS].hi = len ? (uintptr_t)p + len : UINTPTR_MAX;
    __atomic_store_n(&vb_lock_serial, n + 1, __ATOMIC_RELEASE);
}

/* Whether bytes lo to hi - 1, which `e` copied, were locked for writing
   since the entry last asked (the caller then compares the whole entry). */
static bool conv_locked(conv_entry *e, uintptr_t lo, uintptr_t hi)
{
    unsigned now = __atomic_load_n(&vb_lock_serial, __ATOMIC_ACQUIRE), seen = e->lock_seen;
    if (now == seen) return false;
    e->lock_seen = now;
    if (now - seen > VB_LOCKS) return true;   /* some fell out of the ring: assume so */
    for (unsigned i = seen; i != now; i++)
        if (vb_locks[i % VB_LOCKS].lo < hi && vb_locks[i % VB_LOCKS].hi > lo) return true;
    return false;
}

#define CONV_SLOTS 1024
static conv_entry conv_cache[CONV_SLOTS];
static unsigned conv_serial;   /* bumped per draw: entries in use by this draw are not evicted */

static conv_entry *conv_find(const UCHAR *src, ULONG stride, ULONG kind, ULONG count)
{
    unsigned h = (uintptr_t)src * 2654435761u ^ stride * 40503u ^ kind ^ count * 97u;
    /* Mix high address bits before selecting the bucket: aligned buffers at
       different addresses otherwise collide in the same eight-slot probe
       window and evict one another on every frame. */
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    h &= CONV_SLOTS - 1;
    conv_entry *victim = NULL;
    for (int i = 0; i < 8; i++) {
        conv_entry *e = &conv_cache[(h + i) & (CONV_SLOTS - 1)];
        if (e->src == src && e->stride == stride && e->kind == kind && e->count == count) {
            if (detail_on) cache_hits++;
            return e;
        }
        if (!e->src) { if (!victim || victim->src) victim = e; continue; }
        if (e->used != conv_serial && (!victim || (victim->src && e->used < victim->used))) victim = e;
    }
    if (!victim) victim = &conv_cache[h];   /* all eight in this draw: share the first */
    if (detail_on) { cache_misses++; if (victim->src) cache_evicts++; }
    free(victim->raw);
    free(victim->out);
    if (victim->buf) { p_glDeleteBuffers(1, &victim->buf); gc_forget_buffer(victim->buf); }
    *victim = (conv_entry){ src, stride, kind, count, 0, 0, NULL, 0, NULL, 0, d3d.frame, 0, 0, 0, 0,
                            __atomic_load_n(&vb_lock_serial, __ATOMIC_ACQUIRE) };
    return victim;
}

/* Whether the entry's whole copy is due for a full compare (see
   CONV_FULL_EVERY). The full compare always covers the whole entry: draws
   that read different parts of one buffer would otherwise keep pushing the
   check off for the parts other draws read. */
static bool conv_full_due(conv_entry *e)
{
    if (d3d.frame - e->checked < CONV_FULL_EVERY) return false;
    e->checked = d3d.frame;
    return true;
}

/* Whether `len` bytes of units `step` apart match `copy`: all of them when
   there are few (a HUD's quads, whose texture coordinates and colors change
   while the positions stay), else 33 samples of `size` bytes spread over
   the data, each at a different place in its unit so that every field of a
   vertex gets looked at, not only the first. */
#define CONV_WHOLE_BYTES 4096
static bool conv_sampled_same(const UCHAR *copy, const UCHAR *cur, size_t len, size_t step, size_t size)
{
    if (len <= CONV_WHOLE_BYTES) return same_bytes(copy, cur, len);
    size_t units = (len - size) / step + 1;
    for (size_t i = 0; i <= 32; i++) {
        size_t off = (i == 32 ? units - 1 : units * i / 32) * step;
        if (step > size) {
            size_t in = (i * size) % (step - size + 1) / size * size;
            if (off + in + size <= len) off += in;
        }
        /* Fixed-size copies compile to single loads (a variable size made
           byte loops). */
        if (size == 2) {
            uint16_t x, y;
            memcpy(&x, copy + off, 2);
            memcpy(&y, cur + off, 2);
            if (x != y) return false;
        } else {
            uint32_t x, y;
            memcpy(&x, copy + off, 4);
            memcpy(&y, cur + off, 4);
            if (x != y) return false;
        }
    }
    return true;
}

/* The cache entry of an index list: its vertex range, and with `upload` a
   GL element buffer holding it. */
static conv_entry *index_entry(const USHORT *ix, ULONG count, bool upload)
{
    conv_entry *e = conv_find((const UCHAR *)ix, 2, CONV_RANGE, count);
    e->used = conv_serial;
    bool same = false;
    /* A draw looks its indices up twice (the range, the buffer); the source
       cannot change in between. */
    if (e->raw && e->verified == conv_serial) same = true;
    else if (e->raw) same = !memcmp(e->raw, ix, count * 2);   /* in full: see CONV_FULL_EVERY */
    if (upload && (!same || !e->buf)) {
        if (!e->buf) p_glGenBuffers(1, &e->buf);
        gc_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, e->buf);
        if (detail_on) { cache_uploads++; cache_upload_bytes += count * 2; }
        DETAIL_TIMED(t_buffer_upload, p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * 2, ix, GL_STATIC_DRAW));
    }
    e->verified = conv_serial;
    if (same) return e;
    ULONG lo = 0xFFFF, hi = 0;
    for (ULONG i = 0; i < count; i++) {
        ULONG v = ix[i];
        if (v < lo) lo = v;
        if (v >= hi) hi = v + 1;
    }
    if (!e->raw) e->raw = malloc(count * 2);
    if (e->raw) memcpy(e->raw, ix, count * 2);
    e->lo = lo;
    e->hi = hi;
    return e;
}

static vrange vertex_range(ULONG first, ULONG count, const USHORT *indices)
{
    if (!indices) return (vrange){ first, first + count };
    if (!count) return (vrange){ 0, 0 };
    conv_entry *e = index_entry(indices + first, count, false);
    return (vrange){ e->lo, e->hi };
}

/* Bring `len` bytes of a cached copy up to date with `src`; true if they
   changed (the caller then redoes what it made from them). */
static bool conv_refresh(UCHAR *copy, const UCHAR *src, size_t len)
{
    if (same_bytes(copy, src, len)) return false;
    memcpy(copy, src, len);
    return true;
}

/* A GL buffer holding a vertex stream from vertex 0 through the last byte
   `need` a draw reads (see CONV_FULL_EVERY for how it is kept current).
   Mesa would otherwise copy client-memory arrays into a new buffer on
   every draw. Returns 0 to draw from client memory instead. */
static GLuint stream_buffer(const UCHAR *base, ULONG stride, vrange r, size_t need)
{
    if (!p_glGenBuffers || r.hi <= r.lo || !stride) return 0;
    conv_entry *e = conv_find(base, stride, CONV_VBO, 0);
    e->used = conv_serial;
    /* Attributes of one stream share the entry: check it once per draw, as
       far as the attribute that reaches furthest (a later attribute of the
       last vertex, its texture coordinates, lies past the position's). */
    if (e->buf && need <= e->span && e->verified == conv_serial && need <= e->verified_to) return e->buf;
    e->verified = conv_serial;
    if (e->buf && need <= e->span) {
        size_t from = (size_t)r.lo * stride, to = need;
        if (conv_locked(e, (uintptr_t)base, (uintptr_t)base + e->span) | conv_full_due(e)) from = 0, to = e->span;
        e->verified_to = to;
        if (to == need && conv_sampled_same(e->raw + from, base + from, to - from, stride, 4)) return e->buf;
        if (!conv_refresh(e->raw + from, base + from, to - from)) return e->buf;
        e->verified_to = e->span;
        /* The bytes outside from..to were only sampled, so bring the whole
           copy current before it becomes the buffer's new store. */
        memcpy(e->raw, base, e->span);
        gc_bind_buffer(GL_ARRAY_BUFFER, e->buf);
        if (detail_on) { cache_uploads++; cache_upload_bytes += e->span; }
        /* Orphan the store instead of BufferSubData: patching a buffer an
           earlier draw may still read makes the driver wait for that draw. */
        DETAIL_TIMED(t_buffer_upload,
                     p_glBufferData(GL_ARRAY_BUFFER, e->span, e->raw, GL_STREAM_DRAW));
        return e->buf;
    }
    if (need < e->span) need = e->span;
    UCHAR *raw = realloc(e->raw, need);
    if (!raw) return 0;
    e->raw = raw;
    memcpy(e->raw, base, need);
    e->span = e->verified_to = need;
    e->checked = d3d.frame;
    if (!e->buf) p_glGenBuffers(1, &e->buf);
    gc_bind_buffer(GL_ARRAY_BUFFER, e->buf);
    if (detail_on) { cache_uploads++; cache_upload_bytes += need; }
    DETAIL_TIMED(t_buffer_upload, p_glBufferData(GL_ARRAY_BUFFER, need, base, GL_STATIC_DRAW));
    return e->buf;
}

/* Convert vertices r.lo to r.hi - 1 of `src` (vertex 0's attribute, `stride`
   apart) with `conv`, `size` bytes out per vertex; the result is indexed
   like the source. With `buf`, also keep it in a GL buffer laid out the
   same way from vertex 0 and return that in *buf (0 if there is none). */
static const void *convert_cached(const UCHAR *src, ULONG stride, vrange r, ULONG kind, ULONG size,
                                  void (*conv)(const UCHAR *, ULONG, ULONG, UCHAR *), GLuint *buf)
{
    static UCHAR empty[16];
    if (buf) *buf = 0;
    if (r.hi <= r.lo) return empty;
    conv_entry *e = conv_find(src, stride, kind, 0);
    e->used = conv_serial;
    if (e->out && r.lo >= e->lo && r.hi <= e->hi) {
        ULONG lo = r.lo, hi = r.hi;
        if (conv_locked(e, (uintptr_t)(src + (size_t)e->lo * stride), (uintptr_t)(src + (size_t)e->hi * stride))
            | conv_full_due(e)) {
            lo = e->lo, hi = e->hi;
        } else {
            size_t len = (size_t)(hi - lo - 1) * stride + 4;   /* each vertex reads 4 bytes */
            if (conv_sampled_same(e->raw + (size_t)(lo - e->lo) * stride, src + (size_t)lo * stride, len, stride, 4))
                goto done;
        }
        size_t len = (size_t)(hi - lo - 1) * stride + 4;
        if (conv_refresh(e->raw + (size_t)(lo - e->lo) * stride, src + (size_t)lo * stride, len)) {
            /* Rewritten (a dynamic buffer): convert the range again. */
            UCHAR *out = e->out + (size_t)(lo - e->lo) * size;
            conv(src + (size_t)lo * stride, stride, hi - lo, out);
            if (e->buf) {
                gc_bind_buffer(GL_ARRAY_BUFFER, e->buf);
                if (detail_on) { cache_uploads++; cache_upload_bytes += (size_t)(e->hi - e->lo) * size; }
                DETAIL_TIMED(t_buffer_upload, p_glBufferData(GL_ARRAY_BUFFER, (size_t)e->hi * size, NULL, GL_STREAM_DRAW));
                DETAIL_TIMED(t_buffer_upload, p_glBufferSubData(GL_ARRAY_BUFFER, (size_t)e->lo * size, (size_t)(e->hi - e->lo) * size, e->out));
            }
        }
        goto done;
    }
    /* Not covered yet: grow the entry to cover both ranges and convert it
       all. */
    if (e->out) {
        if (e->lo < r.lo) r.lo = e->lo;
        if (e->hi > r.hi) r.hi = e->hi;
    }
    const UCHAR *from = src + (size_t)r.lo * stride;
    size_t len = (size_t)(r.hi - r.lo - 1) * stride + 4;
    free(e->raw);
    free(e->out);
    e->raw = malloc(len);
    e->out = malloc((size_t)(r.hi - r.lo) * size);
    if (!e->raw || !e->out) { free(e->raw); free(e->out); e->raw = e->out = NULL; return empty; }
    memcpy(e->raw, from, len);
    e->raw_len = len;
    e->lo = r.lo;
    e->hi = r.hi;
    e->checked = d3d.frame;
    conv(from, stride, r.hi - r.lo, e->out);
    if (e->buf || (buf && p_glGenBuffers)) {
        /* Sized from vertex 0 so offsets match the indices; only lo..hi is filled. */
        if (!e->buf) p_glGenBuffers(1, &e->buf);
        gc_bind_buffer(GL_ARRAY_BUFFER, e->buf);
        DETAIL_TIMED(t_buffer_upload, p_glBufferData(GL_ARRAY_BUFFER, (size_t)r.hi * size, NULL, GL_STATIC_DRAW));
        if (detail_on) { cache_uploads++; cache_upload_bytes += (size_t)(r.hi - r.lo) * size; }
        DETAIL_TIMED(t_buffer_upload, p_glBufferSubData(GL_ARRAY_BUFFER, (size_t)r.lo * size, (size_t)(r.hi - r.lo) * size, e->out));
    }
done:
    if (buf) *buf = e->buf;
    else gc_bind_buffer(GL_ARRAY_BUFFER, 0);   /* the caller passes client pointers */
    return e->out - (size_t)e->lo * size;
}

/* GL has no signed 11:11:10 vertex format, so NORMPACKED3 attributes are
   unpacked to float3. */
static void conv_normpacked3(const UCHAR *src, ULONG stride, ULONG n, UCHAR *out)
{
    float *o = (float *)out;
    for (ULONG i = 0; i < n; i++, src += stride, o += 3) {
        uint32_t v;
        memcpy(&v, src, 4);
        o[0] = ((int32_t)(v << 21) >> 21) / 1023.0f;
        o[1] = ((int32_t)(v << 10) >> 21) / 1023.0f;
        o[2] = ((int32_t)v >> 22) / 511.0f;
    }
}

static const float *unpack_normpacked3(const UCHAR *src, ULONG stride, vrange r, GLuint *buf)
{
    return convert_cached(src, stride, r, CONV_NORMPACKED3, 12, conv_normpacked3, buf);
}

/* D3DCOLOR vertex data (B, G, R, A bytes) copied out as R, G, B, A for GL
   drivers without BGRA vertex fetch (see rgba_vertex_colors), tightly
   packed (stride 4). */
static void conv_rgba(const UCHAR *src, ULONG stride, ULONG n, UCHAR *o)
{
    for (ULONG i = 0; i < n; i++, src += stride, o += 4) {
        o[0] = src[2];
        o[1] = src[1];
        o[2] = src[0];
        o[3] = src[3];
    }
}

static const void *rgba_colors(const UCHAR *src, ULONG stride, vrange r, GLuint *buf)
{
    return convert_cached(src, stride, r, CONV_RGBA, 4, conv_rgba, buf);
}

/* Bind the generic attribute arrays of a declared vertex layout.  `up` is
   the user-pointer data for stream 0 (DrawVerticesUP), else streams come
   from SetStreamSource. Register r goes to its location in `e`'s program. */
/* Set by draw(): the `up` pointer of this draw is stream 0's vertex buffer. */
static bool up_is_vb;

static void bind_attributes(const vshader *sh, const program_entry *e, const UCHAR *up, ULONG up_stride, ULONG first_vertex, ULONG first,
                            ULONG count, const USHORT *indices)
{
    vrange verts = { 0, 0 };
    bool have_range = false;
#define VERTS (have_range ? verts : (have_range = true, verts = vertex_range(first, count, indices)))
    for (int loc = 0; loc < host_vertex_attribs; loc++)
        if (!(e->attr_mask >> loc & 1)) p_glDisableVertexAttribArray(loc);
    for (int r = 0; r < 16; r++) {
        GLint loc = e->attr[r];
        if (loc < 0) continue;
        const vattr *a = &sh->attr[r];
        if (a->stream < 0) {
            p_glDisableVertexAttribArray(loc);
            static const float def[4] = { 0, 0, 0, 1 };
            p_glVertexAttrib4fv(loc, def);
            continue;
        }
        const UCHAR *base;
        ULONG stride;
        if (up && a->stream == 0) {
            base = up;
            stride = up_stride;
        } else {
            D3DResource *vb = d3d.streams[a->stream].vb;
            if (!vb) { p_glDisableVertexAttribArray(loc); continue; }
            base = resource_data(vb);
            stride = d3d.streams[a->stream].stride;
        }
        p_glEnableVertexAttribArray(loc);
        if ((a->type & 0xF) == 6) {
            GLuint buf;
            const void *p = unpack_normpacked3(base + a->offset + first_vertex * stride, stride, VERTS, &buf);
            gc_bind_buffer(GL_ARRAY_BUFFER, buf);
            p_glVertexAttribPointer(loc, 3, GL_FLOAT, GL_FALSE, 12, buf ? NULL : p);
            continue;
        }
        /* D3DCOLOR is stored B, G, R, A and reaches the shader as (R, G, B, A). */
        if (a->type == 0x40 && rgba_vertex_colors) {
            GLuint buf;
            const void *p = rgba_colors(base + a->offset + first_vertex * stride, stride, VERTS, &buf);
            gc_bind_buffer(GL_ARRAY_BUFFER, buf);
            p_glVertexAttribPointer(loc, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, buf ? NULL : p);
            continue;
        }
        const UCHAR *ptr = base + a->offset + first_vertex * stride;
        GLuint buf = 0;
        if ((base != up || up_is_vb) && !first_vertex) {
            vrange v = VERTS;
            if (v.hi > v.lo) buf = stream_buffer(base, stride, v, (size_t)(v.hi - 1) * stride + a->offset + a->bytes);
        }
        gc_bind_buffer(GL_ARRAY_BUFFER, buf);
        if (buf) ptr = (const UCHAR *)(uintptr_t)a->offset;
        p_glVertexAttribPointer(loc, a->type == 0x40 ? GL_BGRA : a->components, a->gl_type, a->normalized, stride, ptr);
    }
}

#undef VERTS

/* Constant row r as the vertex program sees it: the title's, under the
   shader's own (from its program; over marks their rows) and, in the 192
   constant modes, the viewport in c[58] / c[59]. */
static inline const float *constant_row(const vshader *sh, const uint32_t *over, const float (*vp)[4], int r)
{
    if (vp && (r == 58 || r == 59)) return vp[r - 58];
    if (over[r >> 5] >> (r & 31) & 1)
        for (unsigned i = sh->nconsts; i-- > 0;)
            if (sh->const_slots[i] == r) return sh->consts[i];
    return d3d.vs_const[r];
}

static void upload_constants(const vshader *sh, program_entry *e)
{
    uint32_t over[6] = { 0 };
    for (unsigned i = 0; i < sh->nconsts; i++)
        if ((unsigned)sh->const_slots[i] < 192) over[sh->const_slots[i] >> 5] |= 1u << (sh->const_slots[i] & 31);
    /* The program's epilogue takes the viewport from its own uniforms;
       c[58] / c[59] are the title's in the 192 constant modes. */
    float vp[2][4];
    viewport_constants(vp);
    const float (*vp_rows)[4] = (d3d.constant_mode & 0x1F) == 0 ? vp : NULL;
#define CROW(r) constant_row(sh, over, vp_rows, r)
    /* Upload only the registers that changed since this program last drew:
       most draws change a few matrices, and every uniform call costs (all
       192 is 3 KB, through box86 and Mesa on a Raspberry Pi).  The rows are
       compared where they are, without first copying all 192 (that copy, on
       every draw, was much of this function's time on the Pi). */
    if (e->loc_c >= 0 && e->c_partial && e->last_c && e->c_state == d3d_cur && !cuts_off) {
        /* Only rows written since the program last drew can differ, besides
           the shader's own and the viewport's (58 and 59 always: they swap
           between the title's and the viewport with the constant mode).
           Changed rows go up in runs, nearby runs merged into one call. */
        int n = e->c_size, run_lo = -1, run_hi = -1;
        unsigned since = e->c_gen;
        for (int r = 0; r < n; r++) {
            bool maybe = (int)(d3d.vsc_row_gen[r] - since) > 0 || (over[r >> 5] >> (r & 31) & 1) || r == 58 || r == 59;
            if (!maybe) continue;
            const float *row = CROW(r);
            if (gc_same((const uint32_t *)row, e->last_c[r], 4)) continue;
            memcpy(e->last_c[r], row, 16);
            if (run_lo >= 0 && r - run_hi > 4) {
                p_glUniform4fv(e->loc_c + run_lo, run_hi - run_lo + 1, &e->last_c[run_lo][0]);
                run_lo = -1;
            }
            if (run_lo < 0) run_lo = r;
            run_hi = r;
        }
        if (run_lo >= 0) p_glUniform4fv(e->loc_c + run_lo, run_hi - run_lo + 1, &e->last_c[run_lo][0]);
        e->c_gen = d3d.vsc_gen;
        goto uploaded;
    }
    if (e->loc_c >= 0) {
        int n = e->c_size, lo = 0, hi = n - 1;
        e->c_gen = d3d.vsc_gen;
        e->c_state = d3d_cur;
        if (!e->c_partial) {
            float c[192][4];
            for (int r = 0; r < n; r++) memcpy(c[r], CROW(r), 16);
            p_glUniform4fv(e->loc_c, n, &c[0][0]);
            goto uploaded;
        }
        if (e->last_c) {
            /* Word compares, not memcmp: a call per register crosses into
               native code under box86. */
            while (lo < n && gc_same((const uint32_t *)CROW(lo), e->last_c[lo], 4)) lo++;
            if (lo == n) goto uploaded;
            while (gc_same((const uint32_t *)CROW(hi), e->last_c[hi], 4)) hi--;
        } else {
            e->last_c = malloc(192 * sizeof(*e->last_c));
        }
        for (int r = lo; r <= hi; r++) memcpy(e->last_c[r], CROW(r), 16);
        p_glUniform4fv(e->loc_c + lo, hi - lo + 1, &e->last_c[lo][0]);
    }
#undef CROW
uploaded:
    if (e->loc_flip_y >= 0) program_uniform1f(e, e->loc_flip_y, d3d.rt_texture ? -1.0f : 1.0f);
    if (e->loc_vp_scale >= 0) program_uniform4fv(e, e->loc_vp_scale, 1, vp[0]);
    if (e->loc_vp_offset >= 0) {
        /* Hardware screen position h is D3D position h - 0.53125, which we
           draw at the GL pixel centre (+0.5), as the pretransformed path
           does: without it Splinter Cell's glow quad (from the reserved
           constants) missed its target's last column and row, which kept
           the clear colour and bled green up the screen's right and bottom
           edges. */
        float off[4] = { vp[1][0] - 0.5f, vp[1][1] - 0.5f, vp[1][2], vp[1][3] };
        program_uniform4fv(e, e->loc_vp_offset, 1, off);
    }
    if (e->loc_wdepth >= 0) {
        /* D3DZB_USEW: the NV2A depth-tests w and ignores oPos.z, which
           W-buffered titles leave unscaled (Splinter Cell's z/w is 0..1, not
           0..2^24-1, so it all landed on the near plane).  Rebuild the z the
           fixed-function draws get from the projection transform, which the
           library needs set for its W range anyway: z/w = m22/m23 + m32/w. */
        float wd[4] = { 0, 0, 0, 0 };
        const D3DMATRIX *p = &d3d.transforms[1];
        if (RS(D3DRS_ZENABLE) == 2 && p->m[2][3] != 0 && p->m[3][3] == 0) {
            wd[0] = 1;
            wd[1] = p->m[2][2] / p->m[2][3];
            wd[2] = p->m[3][2];
        }
        program_uniform4fv(e, e->loc_wdepth, 1, wd);
    }
}

/* XBCOMPAT_PROFILE_RENDERER: the programs whose draws cost the most GL time,
   with how many draws and indices, and whether the vertex program indexes
   its constants with A0 (Mesa then keeps the whole array in memory the GPU
   reads, instead of the few constants it uses). */
static void program_profile_log(double frames)
{
    program_entry *top[8] = { 0 };
    unsigned n = 0, total = 0;
    for (program_entry *e = programs; e; e = e->next) {
        if (!e->prof_draws) continue;
        total++;
        unsigned i = n < 8 ? n++ : 8;
        while (i > 0 && top[i - 1]->prof_gl_ns < e->prof_gl_ns) { if (i < 8) top[i] = top[i - 1]; i--; }
        if (i < 8) top[i] = e;
    }
    xlog("D3D programs: %u in use; top by GL draw time (ms/frame GL of all, draws, indices per frame):", total);
    for (unsigned i = 0; i < n; i++) {
        program_entry *e = top[i];
        char src[32768];
        GLsizei len = 0;
        src[0] = 0;
        if (e->vs && p_glGetShaderSource) p_glGetShaderSource(e->vs, sizeof src, &len, src);
        xlog("D3D programs:   %u (vs %u fs %u): %.2f of %.2f ms, %.1f draws, %.0f indices%s%s", e->prog, e->vs, e->fs,
             e->prof_gl_ns / 1e6 / frames, e->prof_all_ns / 1e6 / frames, e->prof_draws / frames,
             e->prof_indices / frames, strstr(src, "[ridx(A0") ? ", A0-indexed constants" : "",
             strstr(src, "gl_BackColor") ? ", two-sided" : "");
    }
    for (program_entry *e = programs; e; e = e->next) e->prof_all_ns = e->prof_gl_ns = 0, e->prof_draws = e->prof_indices = 0;
}

/* Draw with a programmable vertex shader: generic attributes + GLSL. */
static void draw_programmable_(vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                               ULONG count, const USHORT *indices);
static void draw_programmable(vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                              ULONG count, const USHORT *indices)
{
    n_draws++;
    conv_serial++;
    TIMED_CPU(t_draw, c_draw, draw_programmable_(sh, PrimitiveType, up, up_stride, first, count, indices); if (prof_sync) glFinish());
}

static void draw_programmable_(vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                               ULONG count, const USHORT *indices)
{
    uint64_t t_begin = detail_on ? now_ns() : 0, t_gl0 = 0, t_gl1 = 0;
    whatif_skip_draw = false;
    GLuint vs = vertex_shader_object(sh);
    if (!vs) return;
    DETAIL_TIMED(t_state, apply_render_states(false, false);
          if (!d3d.pixel_shader) apply_textures());
    program_entry *e;
    bool ready;
    DETAIL_TIMED(t_program, ready = use_program(vs, &e));
    if (!ready) return;
    TRACE_ALL("D3D: draw(program %u) prim %u count %u indices %p vs %u ps %#x z %u/%u/%u blend %u %u/%u atest %u cw %#x stencil %u vp %u,%u %ux%u %g-%g",
          e ? e->prog : 0, PrimitiveType, count, indices ? indices + first : NULL, vs, d3d.pixel_shader,
          RS(D3DRS_ZENABLE), RS(D3DRS_ZFUNC), RS(D3DRS_ZWRITEENABLE), RS(D3DRS_ALPHABLENDENABLE),
          RS(D3DRS_SRCBLEND), RS(D3DRS_DESTBLEND), RS(D3DRS_ALPHATESTENABLE), RS(D3DRS_COLORWRITEENABLE),
          RS(D3DRS_STENCILENABLE), d3d.viewport.X, d3d.viewport.Y, d3d.viewport.Width, d3d.viewport.Height,
          d3d.viewport.MinZ, d3d.viewport.MaxZ);
    if (g_trace == 1 && d3d.pixel_shader)
        xlog("D3D:   ps constants c0 %08x %08x c1 %08x %08x, mapping %08x %08x", RS(D3DRS_PSCONSTANT0_0),
             RS(D3DRS_PSCONSTANT0_0 + 1), RS(D3DRS_PSCONSTANT1_0), RS(D3DRS_PSCONSTANT1_0 + 1),
             ((const ULONG *)d3d.pixel_shader)[57], ((const ULONG *)d3d.pixel_shader)[58]);
    if (g_trace == 1)
        for (int st = 0; st < 4; st++) {
            const D3DPixelContainer *t = (const D3DPixelContainer *)d3d.textures[st];
            if (t) xlog("D3D:   stage %d texture %p data %#x format %#x size %#x", st, (void *)t, t->res.Data,
                        t->Format, t->Size);
        }
    DETAIL_TIMED(t_constants, upload_constants(sh, e));
    /* Indices from a GL buffer too (kept like the vertices, see stream_buffer). */
    conv_entry *ix;
    DETAIL_TIMED(t_indices, ix = indices && count && p_glGenBuffers ? index_entry(indices + first, count, true) : NULL);
    DETAIL_TIMED(t_attributes, bind_attributes(sh, e, up, up_stride, 0, first, count, indices));
    gc_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, ix ? ix->buf : 0);
    shader_diagnostics_mark(d3d.frame, ++shader_diag_draw, d3d.vertex_shader, d3d.pixel_shader, cur_program,
                            "programmable");
    if (whatif_skip_draw || whatif == WHATIF_NO_DRAWS) return;
    if (detail_on) t_gl0 = now_ns();
    for (unsigned pass = 0; scissor_pass(pass); pass++)
        TIMED(t_gl, if (ix) glDrawElements(gl_primitive(PrimitiveType), count, GL_UNSIGNED_SHORT, NULL);
                    else if (indices) glDrawElements(gl_primitive(PrimitiveType), count, GL_UNSIGNED_SHORT, indices + first);
                    else glDrawArrays(gl_primitive(PrimitiveType), first, count));
    frame_draws++;
    if (flush_every && last_frame_draws >= FLUSH_MIN_DRAWS && !d3d.rt_texture && ++flush_count >= flush_every) {
        flush_count = 0;
        glFlush();
    }
    if (detail_on && e) {
        t_gl1 = now_ns();
        e->prof_gl_ns += t_gl1 - t_gl0;
        e->prof_all_ns += t_gl1 - t_begin;
        e->prof_draws++;
        e->prof_indices += count;
    }
    /* The generic arrays stay bound for the next programmable draw; the
       fixed-function paths turn them off (gc_disable_attrib_arrays). */
    end_program(e);
    debug_dump_draw();
}

static const float default_texcoord[4] = { 0, 0, 0, 1 };

/* Unlit specular: the vertex's specular color is added after texturing
   when D3DRS_SPECULARENABLE is on (lighting computes its own). */
static void secondary_color(bool lit, const void *ptr, GLint size, GLenum type, GLsizei stride)
{
    if (!lit && ptr && RS(D3DRS_SPECULARENABLE) && p_glSecondaryColorPointer) {
        glEnable(0x8458 /* GL_COLOR_SUM */);
        glEnableClientState(0x845E /* GL_SECONDARY_COLOR_ARRAY */);
        p_glSecondaryColorPointer(size, type, stride, ptr);
    } else {
        glDisable(0x8458);
        glDisableClientState(0x845E);
    }
}

/* A declaration without a program: fixed function with a custom layout. */
static void draw_declared_(const vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                           ULONG count, const USHORT *indices);
static void draw_declared(const vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                          ULONG count, const USHORT *indices)
{
    n_draws++;
    conv_serial++;
    TIMED_CPU(t_draw, c_draw, draw_declared_(sh, PrimitiveType, up, up_stride, first, count, indices); if (prof_sync) glFinish());
}

static void draw_declared_(const vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                           ULONG count, const USHORT *indices)
{
    const vattr *pos = &sh->attr[0];
    if (pos->stream < 0) return;
#define STREAM(a, out_base, out_stride) do { \
        if (up && (a)->stream == 0) { out_base = up; out_stride = up_stride; } \
        else { D3DResource *vb = d3d.streams[(a)->stream].vb; if (!vb) return; \
               out_base = resource_data(vb); out_stride = d3d.streams[(a)->stream].stride; } \
    } while (0)
    const UCHAR *base; ULONG stride;
    STREAM(pos, base, stride);
    bool pretransformed = pos->components == 4 && pos->gl_type == GL_FLOAT;
    gc_disable_attrib_arrays();
    apply_render_states(pretransformed, sh->attr[2].stream >= 0);
    ff_primary_present = sh->attr[3].stream >= 0;
    ff_secondary_present = sh->attr[4].stream >= 0;
    if (ff_secondary_present) {
        const UCHAR *colors; ULONG color_stride;
        const vattr *a = &sh->attr[4];
        STREAM(a, colors, color_stride);
        gc_bind_buffer(GL_ARRAY_BUFFER, 0);
        p_glEnableVertexAttribArray(6);
        if (a->type == 0x40 && rgba_vertex_colors)
            p_glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4,
                rgba_colors(colors+a->offset, color_stride, vertex_range(first,count,indices), NULL));
        else
            p_glVertexAttribPointer(6, a->type == 0x40 ? GL_BGRA : a->components,
                                   a->gl_type, a->normalized, color_stride, colors+a->offset);
    }
    ff_weight_components = sh->attr[1].stream >= 0 ? sh->attr[1].components : 0;
    if (RS(118) && ff_weight_components) {
        const UCHAR *weights; ULONG weight_stride;
        STREAM(&sh->attr[1], weights, weight_stride);
        gc_bind_buffer(GL_ARRAY_BUFFER, 0);
        p_glEnableVertexAttribArray(1);
        p_glVertexAttribPointer(1, sh->attr[1].components, sh->attr[1].gl_type,
                               sh->attr[1].normalized, weight_stride, weights + sh->attr[1].offset);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(pretransformed ? 3 : pos->components, pos->gl_type, stride, base + pos->offset);
    if (sh->attr[2].stream >= 0) {
        const UCHAR *b; ULONG s;
        STREAM(&sh->attr[2], b, s);
        glEnableClientState(GL_NORMAL_ARRAY);
        if ((sh->attr[2].type & 0xF) == 6)
            glNormalPointer(GL_FLOAT, 12, unpack_normpacked3(b + sh->attr[2].offset, s,
                                                             vertex_range(first, count, indices), NULL));
        else
            glNormalPointer(sh->attr[2].gl_type, s, b + sh->attr[2].offset);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
    }
    bool lit = glIsEnabled(GL_LIGHTING);
    if (sh->attr[3].stream >= 0 && (!lit || RS(D3DRS_COLORVERTEX))) {
        const UCHAR *b; ULONG s;
        STREAM(&sh->attr[3], b, s);
        glEnableClientState(GL_COLOR_ARRAY);
        if (sh->attr[3].type == 0x40 && rgba_vertex_colors)
            glColorPointer(4, GL_UNSIGNED_BYTE, 4,
                           rgba_colors(b + sh->attr[3].offset, s, vertex_range(first, count, indices), NULL));
        else if (sh->attr[3].gl_type == GL_UNSIGNED_BYTE)
            glColorPointer(sh->attr[3].type == 0x40 ? GL_BGRA : 4, GL_UNSIGNED_BYTE, s, b + sh->attr[3].offset);
        else
            glColorPointer(sh->attr[3].components, sh->attr[3].gl_type, s, b + sh->attr[3].offset);
        if (lit) { glEnable(GL_COLOR_MATERIAL); glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE); }
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glDisable(GL_COLOR_MATERIAL);
        glColor4fv(vertex_diffuse());
    }
    if (sh->attr[4].stream >= 0) {
        const UCHAR *b; ULONG s;
        STREAM(&sh->attr[4], b, s);
        const vattr *a = &sh->attr[4];
        if (a->type == 0x40 && rgba_vertex_colors)
            secondary_color(lit, rgba_colors(b + a->offset, s, vertex_range(first, count, indices), NULL), 3,
                            GL_UNSIGNED_BYTE, 4);
        else
            secondary_color(lit, b + a->offset, a->type == 0x40 ? GL_BGRA : 3, a->gl_type, s);
    } else {
        secondary_color(lit, NULL, 0, 0, 0);
    }
    unsigned units = d3d.pixel_shader ? 0xF : apply_textures();
    for (int u = 0; u < 4; u++) {
        ULONG tci = TSS(u, D3DTSS_TEXCOORDINDEX) & 0xFFFF;
        const vattr *tc = &sh->attr[9 + (tci < 4 ? tci : 0)];
        p_glClientActiveTexture(GL_TEXTURE0 + u);
        if (((units >> u) & 1) && tc->stream >= 0) {
            const UCHAR *b; ULONG s;
            STREAM(tc, b, s);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(tc->components, tc->gl_type, s, b + tc->offset);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            p_glMultiTexCoord4fv(GL_TEXTURE0 + u, default_texcoord);
        }
    }
    p_glClientActiveTexture(GL_TEXTURE0);
    {
        int tsize[4];
        for (int u = 0; u < 4; u++) {
            ULONG tci = TSS(u, D3DTSS_TEXCOORDINDEX) & 0xFFFF;
            const vattr *tc = &sh->attr[9 + (tci < 4 ? tci : 0)];
            tsize[u] = tc->stream >= 0 ? tc->components : 2;
        }
        apply_texture_transforms(units, tsize);
    }
#undef STREAM
    program_entry *e;
    if (!use_program(0, &e)) return;
    TRACE_ALL("D3D: draw(declared) prim %u count %u stride %u indices %p shader %#x", PrimitiveType, count, stride,
          indices ? indices + first : NULL, d3d.vertex_shader);
    trace_draw_detail(indices ? NULL : base + first * stride, stride, count, !indices);
    shader_diagnostics_mark(d3d.frame, ++shader_diag_draw, d3d.vertex_shader, d3d.pixel_shader, cur_program,
                            "declared");
    for (unsigned pass = 0; scissor_pass(pass); pass++)
        TIMED(t_gl, if (indices) glDrawElements(gl_primitive(PrimitiveType), count, GL_UNSIGNED_SHORT, indices + first);
                    else glDrawArrays(gl_primitive(PrimitiveType), first, count));
    end_program(e);
}

#include "fvf_plan.h"
#include "fvf_batch.h"
#include "hud_shader.h"
static GLuint hud_vertex_object, hud_fragment_object;
static program_entry *hud_program;
static GLint hud_tex_uniform;

static bool hud_shader_supported(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("XBCOMPAT_HUD_SHADER");
        enabled = e && *e && *e != '0';
    }
    if (!enabled || d3d.vertex_shader != 0x144 || d3d.pixel_shader || d3d.bb_cpu_dirty || RS(D3DRS_SPECULARENABLE)
        || RS(D3DRS_FOGENABLE) || RS(D3DRS_STENCILENABLE)
        || RS(D3DRS_POINTSCALEENABLE) || RS(D3DRS_POINTSPRITEENABLE) || RS(D3DRS_ALPHATESTENABLE)
        || RS(118 /* VERTEXBLEND */) || RS(122 /* TWOSIDEDLIGHTING */) || RS(D3DRS_ZENABLE) == 2 /* USEW */
        || RS(D3DRS_SHADEMODE) == 0x1D00 /* FLAT */ || !d3d.textures[0]
        || tex_target((const D3DPixelContainer *)d3d.textures[0]) != GL_TEXTURE_2D) return false;
    for (int s = 0; s < 4; s++)
        if (TSS(s, 21 /* TEXTURETRANSFORMFLAGS */) || TSS(s, D3DTSS_TEXCOORDINDEX) >> 16
            || (s == 0 ? TSS(s, D3DTSS_TEXCOORDINDEX) != 0
                       : TSS(s, D3DTSS_COLOROP) != 1 /* DISABLE */)) return false;
    return TSS(0, D3DTSS_COLORARG1) == 2 /* TEXTURE */ && TSS(0, D3DTSS_COLORARG2) == 1 /* CURRENT */
        && TSS(0, D3DTSS_ALPHAOP) == 4 /* MODULATE */ && TSS(0, D3DTSS_ALPHAARG1) == 2
        && TSS(0, D3DTSS_ALPHAARG2) == 0 /* DIFFUSE */ && !TSS(0, D3DTSS_ALPHAKILL)
        && TSS(0, D3DTSS_COLOROP) == 4 && !TSS(0, D3DTSS_COLORKEYOP);
}

static bool hud_shader_bind(void)
{
    static bool failed;
    if (failed || !hud_shader_supported()) return false;
    if (!hud_program) {
        hud_vertex_object = compile_shader(GL_VERTEX_SHADER, hud_vs_source);
        hud_fragment_object = compile_shader(GL_FRAGMENT_SHADER, hud_fs_source);
        if (!hud_vertex_object || !hud_fragment_object) { failed = true; return false; }
        hud_program = program_for(hud_vertex_object, hud_fragment_object);
        if (!hud_program->prog) { failed = true; return false; }
        hud_tex_uniform = p_glGetUniformLocation(hud_program->prog, "hud_tex");
    }
    if (cur_program != hud_program->prog) p_glUseProgram(hud_program->prog);
    cur_program = hud_program->prog;
    program_uniform1i(hud_program, hud_tex_uniform, 0);
    return true;
}

static bool hud_prepared_program(ULONG primitive)
{
    return !cur_program || ((primitive == 8 || primitive == 5) && hud_program && cur_program == hud_program->prog
                            && hud_shader_supported());
}

/* `vb`: base is stream 0's vertex buffer, not a ...UP draw's title memory. */
static void draw_fvf(ULONG prim, const UCHAR *base, ULONG stride, ULONG first, ULONG count, const USHORT *indices);
static void draw(ULONG PrimitiveType, const UCHAR *base, ULONG stride, ULONG first, ULONG count,
                 const USHORT *indices, bool vb)
{
    if (d3d.vertex_shader & 1) {
        hud_batch_flush();
        vshader *sh = (vshader *)(d3d.vertex_shader & ~1u);
        up_is_vb = vb;
        if (sh->code) draw_programmable(sh, PrimitiveType, base, stride, first, count, indices);
        else draw_declared(sh, PrimitiveType, base, stride, first, count, indices);
        up_is_vb = false;
        return;
    }
    n_draws++;
    TIMED_CPU(t_draw, c_draw, draw_fvf(PrimitiveType, base, stride, first, count, indices); if (prof_sync) glFinish());
}

static void NTAPI D3DDevice_DrawVertices(ULONG, UINT_, UINT_), D3DDevice_DrawVerticesUP(ULONG, UINT_, const void *, UINT_),
    D3DDevice_SetVertexShaderConstant(LONG, const float *, ULONG);   /* the packets know these by address */
static void FASTCALL SetVertexShaderConstantNotInline(LONG, const float *, ULONG),
    SetVertexShaderConstant1(LONG, const float *), SetVertexShaderConstant4(LONG, const float *);
#include "hud_packet.h"
#include "state_packet.h"
static void draw_fvf(ULONG PrimitiveType, const UCHAR *base, ULONG stride, ULONG first, ULONG count,
                     const USHORT *indices)
{
    fvf_layout l = parse_fvf(d3d.vertex_shader);
    conv_serial++;
    fvf_plan_key key;
    bool eligible = fvf_plan_capture(&key, &l);
    hud_batch_key batch_key;
    bool batchable = hud_batch_capture_key(&batch_key, &key, PrimitiveType, stride, count, indices, eligible);
    if (hud_batch.count && (!batchable
        || memcmp(&batch_key.state, &hud_batch.key.state, sizeof(batch_key.state)) || batch_key.target != hud_batch.key.target
        || batch_key.depth != hud_batch.key.depth || batch_key.stride != hud_batch.key.stride
        /* units follow from the state, and the program is set as the draw joins */
        || memcmp(batch_key.textures, hud_batch.key.textures, sizeof(batch_key.textures))
        || memcmp(batch_key.screen_offset, hud_batch.key.screen_offset, sizeof(batch_key.screen_offset))
        || memcmp(batch_key.backbuffer_scale, hud_batch.key.backbuffer_scale, sizeof(batch_key.backbuffer_scale))))
        hud_batch_flush();
    bool reuse = fvf_plan.valid && fvf_plan.serial == conv_serial - 1 && fvf_plan.epoch == gc_epoch
                 && eligible && hud_prepared_program(PrimitiveType)
                 && !memcmp(&key, &fvf_plan.key, sizeof(key));
    if (reuse && batchable) {
        batch_key.units = fvf_plan.units;
        batch_key.program = cur_program;
        if (hud_batch_append(&batch_key, base, first, count)) {
            fvf_plan_hits++;
            fvf_plan.serial = conv_serial;
            return;
        }
    }
    /* The same state as the last draw, right after it: only the textures
       are bound again (the title may have rewritten one in place). */
    if (reuse) fvf_plan_resources(fvf_plan.units);
    /* Binding them may upload, which moves gc_epoch: then nothing is kept. */
    reuse = reuse && fvf_plan.epoch == gc_epoch && hud_prepared_program(PrimitiveType);

    if (reuse) fvf_plan_hits++; else fvf_plan_misses++;
    fvf_plan.valid = false;
    gc_disable_attrib_arrays();
    if (!reuse) apply_render_states(l.pretransformed, l.normal_off >= 0);
    ff_primary_present = l.diffuse_off >= 0;
    ff_secondary_present = l.specular_off >= 0;
    if (ff_secondary_present) {
        gc_bind_buffer(GL_ARRAY_BUFFER, 0);
        p_glEnableVertexAttribArray(6);
        if (rgba_vertex_colors)
            p_glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4,
                rgba_colors(base+l.specular_off, stride, vertex_range(first,count,indices), NULL));
        else
            p_glVertexAttribPointer(6, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, stride, base+l.specular_off);
    }
    ULONG position_kind = d3d.vertex_shader & D3DFVF_POSITION_MASK;
    ff_weight_components = position_kind >= D3DFVF_XYZB1 ? (position_kind-D3DFVF_XYZB1)/2+1 : 0;
    if (RS(118) && ff_weight_components && ff_weight_components <= 4) {
        gc_bind_buffer(GL_ARRAY_BUFFER, 0);
        p_glEnableVertexAttribArray(1);
        p_glVertexAttribPointer(1, ff_weight_components, GL_FLOAT, GL_FALSE, stride, base+12);
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(l.pos_size, GL_FLOAT, stride, base);
    if (l.normal_off >= 0) {
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, stride, base + l.normal_off);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
    }
    bool lit = glIsEnabled(GL_LIGHTING);
    if (l.diffuse_off >= 0 && (!lit || RS(D3DRS_COLORVERTEX))) {
        glEnableClientState(GL_COLOR_ARRAY);
        if (rgba_vertex_colors)
            glColorPointer(4, GL_UNSIGNED_BYTE, 4,
                           rgba_colors(base + l.diffuse_off, stride, vertex_range(first, count, indices), NULL));
        else
            glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, base + l.diffuse_off);
        if (lit) {
            glEnable(GL_COLOR_MATERIAL);
            glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
        }
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glDisable(GL_COLOR_MATERIAL);
        glColor4fv(vertex_diffuse());
    }
    if (rgba_vertex_colors && l.specular_off >= 0)
        secondary_color(lit, rgba_colors(base + l.specular_off, stride, vertex_range(first, count, indices), NULL), 3,
                        GL_UNSIGNED_BYTE, 4);
    else
        secondary_color(lit, l.specular_off >= 0 ? base + l.specular_off : NULL, GL_BGRA, GL_UNSIGNED_BYTE, stride);
    unsigned units = reuse ? fvf_plan.units : d3d.pixel_shader ? 0xF : apply_textures();
    /* Texture coordinates are per draw (the vertices moved), even reused. */
    for (int u = 0; u < 4; u++) {
        ULONG tci = TSS(u, D3DTSS_TEXCOORDINDEX) & 0xFFFF;
        p_glClientActiveTexture(GL_TEXTURE0 + u);
        if (((units >> u) & 1) && tci < (ULONG)l.ntex) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(l.tex_size[tci], GL_FLOAT, stride, base + l.tex_off[tci]);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            p_glMultiTexCoord4fv(GL_TEXTURE0 + u, default_texcoord);
        }
    }
    p_glClientActiveTexture(GL_TEXTURE0);
    if (!reuse) {
        int tsize[4];
        for (int u = 0; u < 4; u++) {
            ULONG tci = TSS(u, D3DTSS_TEXCOORDINDEX) & 0xFFFF;
            tsize[u] = tci < (ULONG)l.ntex ? l.tex_size[tci] : 2;
        }
        apply_texture_transforms(units, tsize);
    }

    program_entry *e = NULL;
    if (!reuse && !use_program(0, &e)) return;
    if ((PrimitiveType == 8 || PrimitiveType == 5) && hud_shader_supported()) hud_shader_bind();
    TRACE_ALL("D3D: draw prim %u count %u base %p stride %u indices %p fvf %#x", PrimitiveType, count, base, stride,
          indices ? indices + first : NULL, d3d.vertex_shader);
    trace_draw_detail(base + (indices ? 0 : first * stride), stride, count, !indices && (d3d.vertex_shader & 0x0E) == 0x04);
    batch_key.units = units;
    batch_key.program = cur_program;
    bool batched = batchable && !e && hud_batch_append(&batch_key, base, first, count);
    if (!batched) {
        hud_batch_flush();
        shader_diagnostics_mark(d3d.frame, ++shader_diag_draw, d3d.vertex_shader, d3d.pixel_shader, cur_program,
                                "fvf");
        for (unsigned pass = 0; scissor_pass(pass); pass++)
            TIMED(t_gl, if (indices) glDrawElements(gl_primitive(PrimitiveType), count, GL_UNSIGNED_SHORT, indices + first);
                        else glDrawArrays(gl_primitive(PrimitiveType), first, count));
    }
    if (eligible && !e) {
        memcpy(&fvf_plan.key, &key, sizeof(key)); fvf_plan.epoch = gc_epoch;
        fvf_plan.units = units; fvf_plan.valid = true; fvf_plan.serial = conv_serial;
    }
    debug_dump_draw();
    end_program(e);
}

/* ---- higher-order patches ---------------------------------------------- */

/* DrawRectPatch / DrawTriPatch tessellate on the CPU: the control points are
   read from the current streams, every register is evaluated as a float4,
   and the grid is drawn through the declared-layout paths with each register
   in its own 16-byte slot. */

typedef struct { bool used, tri; ULONG handle; ULONG info[7]; } patch_entry;
static patch_entry patches[64];

static patch_entry *patch_slot(ULONG handle, bool create)
{
    patch_entry *free_slot = NULL;
    for (unsigned i = 0; i < 64; i++) {
        if (patches[i].used && patches[i].handle == handle) return &patches[i];
        if (!patches[i].used && !free_slot) free_slot = &patches[i];
    }
    if (!create) return NULL;
    if (!free_slot) free_slot = &patches[handle % 64];
    free_slot->used = true;
    free_slot->handle = handle;
    return free_slot;
}

/* One attribute of one control vertex as a float4, the way the draw paths read it. */
static void read_attr(const vattr *a, const UCHAR *p, float out[4])
{
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    int kind = a->type & 0xF, n = a->components;
    switch (kind) {
    case 0:   /* D3DCOLOR: bytes B, G, R, A */
        out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
        break;
    case 1: for (int i = 0; i < n && i < 4; i++) out[i] = ((const short *)p)[i] / 32767.0f; break;
    case 2: for (int i = 0; i < n && i < 4; i++) memcpy(&out[i], p + 4 * i, 4); break;
    case 4: for (int i = 0; i < n && i < 4; i++) out[i] = p[i] / 255.0f; break;
    case 5: for (int i = 0; i < n && i < 4; i++) out[i] = ((const short *)p)[i]; break;
    case 6: {   /* NORMPACKED3: 11:11:10 signed */
        uint32_t v; memcpy(&v, p, 4);
        out[0] = ((int32_t)(v << 21) >> 21) / 1023.0f;
        out[1] = ((int32_t)(v << 10) >> 21) / 1023.0f;
        out[2] = ((int32_t)v >> 22) / 511.0f;
        break;
    }
    }
}

/* The layout the patch reads: the declaration, or one built from the FVF. */
static bool patch_layout(vshader *out, vshader **real)
{
    *real = NULL;
    if (d3d.vertex_shader & 1) {
        *real = (vshader *)(d3d.vertex_shader & ~1u);
        *out = **real;
        return true;
    }
    memset(out, 0, sizeof(*out));
    for (int r = 0; r < 16; r++) out->attr[r].stream = -1;
    out->tess_normal_in = out->tess_normal_out = out->tess_uv_out = -1;
    fvf_layout l = parse_fvf(d3d.vertex_shader);
    if (l.pretransformed) return false;
    decode_vsdt(0x32, &out->attr[0]); out->attr[0].stream = 0; out->attr[0].offset = 0;
    if (l.normal_off >= 0) { decode_vsdt(0x32, &out->attr[2]); out->attr[2].stream = 0; out->attr[2].offset = l.normal_off; }
    if (l.diffuse_off >= 0) { decode_vsdt(0x40, &out->attr[3]); out->attr[3].stream = 0; out->attr[3].offset = l.diffuse_off; }
    if (l.specular_off >= 0) { decode_vsdt(0x40, &out->attr[4]); out->attr[4].stream = 0; out->attr[4].offset = l.specular_off; }
    for (int i = 0; i < l.ntex && i < 4; i++) {
        decode_vsdt((ULONG)(l.tex_size[i] << 4) | 2, &out->attr[9 + i]);
        out->attr[9 + i].stream = 0;
        out->attr[9 + i].offset = l.tex_off[i];
    }
    d3d.streams[0].stride = d3d.streams[0].stride ? d3d.streams[0].stride : (ULONG)l.stride;
    return true;
}

/* Fetch control vertex `index` (all 16 registers). */
static void fetch_vertex(const vshader *sh, ULONG index, float regs[16][4])
{
    for (int r = 0; r < 16; r++) {
        const vattr *a = &sh->attr[r];
        regs[r][0] = regs[r][1] = regs[r][2] = 0; regs[r][3] = 1;
        if (a->stream < 0) continue;
        D3DResource *vb = d3d.streams[a->stream].vb;
        if (!vb) continue;
        read_attr(a, (const UCHAR *)resource_data(vb) + index * d3d.streams[a->stream].stride + a->offset, regs[r]);
    }
}

/* Weights (and their derivatives) of the control points along one patch
   direction at parameter t in [0,1].  Returns the first control point used;
   *n is how many follow. */
static unsigned basis_weights(ULONG basis, ULONG order, unsigned count, float t, float *w, float *dw, unsigned *n)
{
    if (order < 1) order = 1;
    if (count < 2) { w[0] = 1; dw[0] = 0; *n = 1; return 0; }
    if (basis == 1 && order == 3 && count >= 4) {
        /* Uniform cubic B-spline: count - 3 segments. */
        unsigned segs = count - 3;
        float u = t * segs; unsigned s = u >= segs ? segs - 1 : (unsigned)u; float f = u - s;
        float f2 = f * f, f3 = f2 * f;
        w[0] = (1 - 3 * f + 3 * f2 - f3) / 6; w[1] = (4 - 6 * f2 + 3 * f3) / 6;
        w[2] = (1 + 3 * f + 3 * f2 - 3 * f3) / 6; w[3] = f3 / 6;
        dw[0] = (-3 + 6 * f - 3 * f2) / 6 * segs; dw[1] = (-12 * f + 9 * f2) / 6 * segs;
        dw[2] = (3 + 6 * f - 9 * f2) / 6 * segs; dw[3] = 3 * f2 / 6 * segs;
        *n = 4;
        return s;
    }
    if (basis == 2 && order == 3) {
        /* Interpolating: Catmull-Rom through the points, ends clamped. */
        unsigned segs = count - 1;
        float u = t * segs; unsigned s = u >= segs ? segs - 1 : (unsigned)u; float f = u - s;
        float f2 = f * f, f3 = f2 * f;
        float cw[4] = { (-f3 + 2 * f2 - f) / 2, (3 * f3 - 5 * f2 + 2) / 2, (-3 * f3 + 4 * f2 + f) / 2, (f3 - f2) / 2 };
        float cd[4] = { (-3 * f2 + 4 * f - 1) / 2, (9 * f2 - 10 * f) / 2, (-9 * f2 + 8 * f + 1) / 2, (3 * f2 - 2 * f) / 2 };
        /* Fold the clamped neighbours into the end points: return a window of 4 starting at s-1. */
        int first = (int)s - 1;
        for (int i = 0; i < 4; i++) { w[i] = 0; dw[i] = 0; }
        unsigned lo = first < 0 ? 0 : (unsigned)first;
        unsigned hi = s + 2 > count - 1 ? count - 1 : s + 2;
        for (int i = 0; i < 4; i++) {
            int idx = first + i;
            if (idx < 0) idx = 0;
            if (idx > (int)count - 1) idx = count - 1;
            w[idx - lo] += cw[i];
            dw[idx - lo] += cd[i] * segs;
        }
        *n = hi - lo + 1;
        return lo;
    }
    /* Bezier (and the linear order of any basis): (count - 1) / order segments. */
    if (order > count - 1) order = count - 1;
    unsigned segs = (count - 1) / order;
    if (!segs) segs = 1;
    float u = t * segs; unsigned s = u >= segs ? segs - 1 : (unsigned)u; float f = u - s;
    for (unsigned i = 0; i <= order; i++) {
        /* Bernstein polynomial B(i, order) and its derivative. */
        float c = 1;
        for (unsigned k = 0; k < i; k++) c = c * (order - k) / (k + 1);
        float a = powf(f, (float)i), b = powf(1 - f, (float)(order - i));
        w[i] = c * a * b;
        float da = i ? i * powf(f, (float)(i - 1)) : 0;
        float db = order - i ? (order - i) * powf(1 - f, (float)(order - i - 1)) : 0;
        dw[i] = c * (da * b - a * db) * segs;
    }
    *n = order + 1;
    return s * order;
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Draw `nverts` float4-register vertices with `nidx` triangle-list indices. */
static void draw_patch_mesh(vshader *layout, vshader *real, const float (*verts)[16][4], ULONG nverts,
                            const USHORT *idx, ULONG nidx)
{
    (void)nverts;
    /* The copy below draws with the real shader's GL objects: compile them
       there first, so they are kept with the shader rather than the copy. */
    if (real && layout->code) vertex_shader_object(real);
    vshader tmp = *layout;
    if (real) {
        tmp.vs = real->vs; tmp.vs_z = real->vs_z;
        tmp.failed = real->failed; tmp.vs_z_attempted = real->vs_z_attempted;
    }
    for (int r = 0; r < 16; r++) {
        bool generated = r == layout->tess_normal_out || r == layout->tess_uv_out;
        if (layout->attr[r].stream < 0 && !generated) { tmp.attr[r].stream = -1; continue; }
        int comps = layout->attr[r].stream >= 0 && (layout->attr[r].type & 0xF) == 2 ? layout->attr[r].components : 4;
        if (r == layout->tess_normal_out) comps = 3;
        if (r == layout->tess_uv_out) comps = 2;
        tmp.attr[r] = (vattr){ 0, (ULONG)r * 16, (ULONG)(comps << 4) | 2, comps, GL_FLOAT, false, comps * 4 };
    }
    if (tmp.code) {
        draw_programmable(&tmp, 5, (const UCHAR *)verts, 256, 0, nidx, idx);
    } else {
        draw_declared(&tmp, 5, (const UCHAR *)verts, 256, 0, nidx, idx);
    }
}

static LONG NTAPI D3DDevice_DrawRectPatch(UINT_ Handle, const float *pNumSegs, const ULONG *info)
{
    gl_require();
    pusher_drain();
    patch_entry *pe = NULL;
    if (Handle) {
        pe = patch_slot(Handle, info != NULL);
        if (!pe) return D3DERR_INVALIDCALL;
        if (info) { memcpy(pe->info, info, 7 * 4); pe->tri = false; }
        info = pe->info;
    }
    if (!info) return D3DERR_INVALIDCALL;
    vshader layout, *real;
    if (!patch_layout(&layout, &real)) return D3DERR_INVALIDCALL;
    ULONG x0 = info[0], y0 = info[1], width = info[2], height = info[3], stride = info[4];
    ULONG basis = info[5], order = info[6];
    float su = 1, sv = 1;
    if (pNumSegs) {
        su = fmaxf(pNumSegs[0], pNumSegs[2]);
        sv = fmaxf(pNumSegs[1], pNumSegs[3]);
    }
    unsigned nu = su < 1 ? 1 : su > 64 ? 64 : (unsigned)(su + 0.5f);
    unsigned nv = sv < 1 ? 1 : sv > 64 ? 64 : (unsigned)(sv + 0.5f);
    if (width < 2 || height < 2 || !stride) return D3DERR_INVALIDCALL;

    float (*cp)[16][4] = malloc(width * height * sizeof(*cp));
    for (ULONG j = 0; j < height; j++)
        for (ULONG i = 0; i < width; i++)
            fetch_vertex(&layout, (y0 + j) * stride + x0 + i, cp[j * width + i]);

    ULONG nverts = (nu + 1) * (nv + 1);
    float (*out)[16][4] = calloc(nverts, sizeof(*out));
    int nin = layout.tess_normal_in;
    for (unsigned b = 0; b <= nv; b++) {
        for (unsigned a = 0; a <= nu; a++) {
            float wu[8], dwu[8], wv[8], dwv[8];
            unsigned cu, cv;
            unsigned iu = basis_weights(basis, order, width, (float)a / nu, wu, dwu, &cu);
            unsigned iv = basis_weights(basis, order, height, (float)b / nv, wv, dwv, &cv);
            float (*o)[4] = out[b * (nu + 1) + a];
            float du[3] = { 0, 0, 0 }, dv[3] = { 0, 0, 0 };
            for (unsigned j = 0; j < cv; j++) {
                for (unsigned i = 0; i < cu; i++) {
                    const float (*c)[4] = cp[(iv + j) * width + iu + i];
                    float wgt = wu[i] * wv[j];
                    for (int r = 0; r < 16; r++)
                        for (int k = 0; k < 4; k++) o[r][k] += wgt * c[r][k];
                    if (nin >= 0)
                        for (int k = 0; k < 3; k++) {
                            du[k] += dwu[i] * wv[j] * c[nin][k];
                            dv[k] += wu[i] * dwv[j] * c[nin][k];
                        }
                }
            }
            if (layout.tess_normal_out >= 0 && nin >= 0) {
                float nrm[3];
                cross3(du, dv, nrm);
                float len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
                if (len > 1e-12f) for (int k = 0; k < 3; k++) nrm[k] /= len;
                memcpy(o[layout.tess_normal_out], nrm, 12);
                o[layout.tess_normal_out][3] = 1;
            }
            if (layout.tess_uv_out >= 0) {
                o[layout.tess_uv_out][0] = (float)a / nu;
                o[layout.tess_uv_out][1] = (float)b / nv;
                o[layout.tess_uv_out][2] = 0;
                o[layout.tess_uv_out][3] = 1;
            }
        }
    }
    /* Degenerate edges (a pole, like the teapot's lid) give zero normals:
       borrow the nearest non-zero one along v. */
    if (layout.tess_normal_out >= 0 && nin >= 0) {
        int r = layout.tess_normal_out;
        for (unsigned a = 0; a <= nu; a++)
            for (unsigned b = 0; b <= nv; b++) {
                float *n = out[b * (nu + 1) + a][r];
                if (n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 1e-12f) continue;
                for (unsigned d = 1; d <= nv; d++) {
                    float *m = NULL;
                    if (b + d <= nv) m = out[(b + d) * (nu + 1) + a][r];
                    if ((!m || m[0] * m[0] + m[1] * m[1] + m[2] * m[2] <= 1e-12f) && b >= d) m = out[(b - d) * (nu + 1) + a][r];
                    if (m && m[0] * m[0] + m[1] * m[1] + m[2] * m[2] > 1e-12f) { memcpy(n, m, 12); break; }
                }
            }
    }
    ULONG nidx = nu * nv * 6;
    USHORT *idx = malloc(nidx * sizeof(USHORT));
    ULONG k = 0;
    for (unsigned b = 0; b < nv; b++)
        for (unsigned a = 0; a < nu; a++) {
            USHORT v00 = b * (nu + 1) + a, v10 = v00 + 1, v01 = v00 + nu + 1, v11 = v01 + 1;
            idx[k++] = v00; idx[k++] = v10; idx[k++] = v11;
            idx[k++] = v00; idx[k++] = v11; idx[k++] = v01;
        }
    draw_patch_mesh(&layout, real, (const float (*)[16][4])out, nverts, idx, nidx);
    free(idx);
    free(out);
    free(cp);
    return D3D_OK;
}

/* Triangle patches: the corners are tessellated as a flat (linear) patch;
   higher orders take their first three control points as the corners. */
static LONG NTAPI D3DDevice_DrawTriPatch(UINT_ Handle, const float *pNumSegs, const ULONG *info)
{
    gl_require();
    pusher_drain();
    patch_entry *pe = NULL;
    if (Handle) {
        pe = patch_slot(Handle, info != NULL);
        if (!pe) return D3DERR_INVALIDCALL;
        if (info) { memcpy(pe->info, info, 4 * 4); pe->tri = true; }
        info = pe->info;
    }
    if (!info) return D3DERR_INVALIDCALL;
    if (info[3] != 1) {
        static bool warned;
        if (!warned) { xlog("D3D: tri patches of order %u are drawn flat through their corners", info[3]); warned = true; }
    }
    vshader layout, *real;
    if (!patch_layout(&layout, &real)) return D3DERR_INVALIDCALL;
    float s = pNumSegs ? fmaxf(pNumSegs[0], fmaxf(pNumSegs[1], pNumSegs[2])) : 1;
    unsigned n = s < 1 ? 1 : s > 64 ? 64 : (unsigned)(s + 0.5f);
    float c[3][16][4];
    for (int i = 0; i < 3; i++) fetch_vertex(&layout, info[0] + i, c[i]);
    ULONG nverts = (n + 1) * (n + 2) / 2;
    float (*out)[16][4] = calloc(nverts, sizeof(*out));
    float nrm[3] = { 0, 0, 1 };
    if (layout.tess_normal_in >= 0) {
        int r = layout.tess_normal_in;
        float e1[3], e2[3];
        for (int k = 0; k < 3; k++) { e1[k] = c[1][r][k] - c[0][r][k]; e2[k] = c[2][r][k] - c[0][r][k]; }
        cross3(e2, e1, nrm);
        float len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
        if (len > 1e-12f) for (int k = 0; k < 3; k++) nrm[k] /= len;
    }
    ULONG v = 0;
    for (unsigned row = 0; row <= n; row++)
        for (unsigned col = 0; col <= n - row; col++, v++) {
            float b1 = (float)col / n, b2 = (float)row / n, b0 = 1 - b1 - b2;
            for (int r = 0; r < 16; r++)
                for (int k = 0; k < 4; k++) out[v][r][k] = b0 * c[0][r][k] + b1 * c[1][r][k] + b2 * c[2][r][k];
            if (layout.tess_normal_out >= 0) { memcpy(out[v][layout.tess_normal_out], nrm, 12); out[v][layout.tess_normal_out][3] = 1; }
            if (layout.tess_uv_out >= 0) { out[v][layout.tess_uv_out][0] = b1; out[v][layout.tess_uv_out][1] = b2; }
        }
    ULONG nidx = n * n * 3, k = 0;
    USHORT *idx = malloc(nidx * sizeof(USHORT));
    unsigned start = 0;
    for (unsigned row = 0; row < n; row++) {
        unsigned len = n - row + 1, next = start + len;
        for (unsigned col = 0; col < len - 1; col++) {
            idx[k++] = start + col; idx[k++] = start + col + 1; idx[k++] = next + col;
            if (col < len - 2) { idx[k++] = start + col + 1; idx[k++] = next + col + 1; idx[k++] = next + col; }
        }
        start = next;
    }
    draw_patch_mesh(&layout, real, (const float (*)[16][4])out, nverts, idx, k);
    free(idx);
    free(out);
    return D3D_OK;
}

static void NTAPI D3DDevice_DeletePatch(UINT_ Handle)
{
    patch_entry *pe = patch_slot(Handle, false);
    if (pe) pe->used = false;
}

static void NTAPI D3DDevice_DrawVertices(ULONG PrimitiveType, UINT_ StartVertex, UINT_ VertexCount)
{
    gl_require();
    pusher_drain();
    if (async_front()) { async_draw(PrimitiveType, StartVertex, VertexCount); return; }
    if (d3d.recording) { ULONG a[3] = { PrimitiveType, StartVertex, VertexCount }; pb_record(OP_DRAW, a, sizeof(a)); return; }
    D3DResource *vb = d3d.streams[0].vb;
    if (!vb) return;
    draw(PrimitiveType, resource_data(vb), d3d.streams[0].stride, StartVertex, VertexCount, NULL, true);
}

static void NTAPI D3DDevice_DrawVerticesUP(ULONG PrimitiveType, UINT_ VertexCount, const void *pData,
                                           UINT_ Stride)
{
    gl_require();
    pusher_drain();
    if (async_front()) { async_draw_up(PrimitiveType, VertexCount, pData, Stride); return; }
    if (d3d.recording) {
        ULONG h[3] = { PrimitiveType, VertexCount, Stride };
        pb_record2(OP_DRAW_UP, h, sizeof(h), pData, VertexCount * Stride);
        return;
    }
    draw(PrimitiveType, pData, Stride, 0, VertexCount, NULL, false);
}

static void NTAPI D3DDevice_DrawIndexedVertices(ULONG PrimitiveType, UINT_ VertexCount, const USHORT *pIndexData)
{
    gl_require();
    pusher_drain();
    if (async_front()) { async_draw_indexed(PrimitiveType, VertexCount, pIndexData); return; }
    if (d3d.recording) {
        ULONG h[2] = { PrimitiveType, VertexCount };
        pb_record2(OP_DRAW_INDEXED, h, sizeof(h), pIndexData, VertexCount * 2);
        return;
    }
    D3DResource *vb = d3d.streams[0].vb;
    if (!vb) return;
    const UCHAR *base = (const UCHAR *)resource_data(vb) + d3d.base_vertex_index * d3d.streams[0].stride;
    draw(PrimitiveType, base, d3d.streams[0].stride, 0, VertexCount, pIndexData, true);
}

static void NTAPI D3DDevice_DrawIndexedVerticesUP(ULONG PrimitiveType, UINT_ VertexCount, const USHORT *pIndexData,
                                                  const void *pVertexData, UINT_ Stride)
{
    gl_require();
    pusher_drain();
    if (async_front()) { async_draw_indexed_up(PrimitiveType, VertexCount, pIndexData, pVertexData, Stride); return; }
    if (d3d.recording) {
        ULONG nverts = 0;
        for (UINT_ i = 0; i < VertexCount; i++) if (pIndexData[i] + 1u > nverts) nverts = pIndexData[i] + 1u;
        ULONG ib = (VertexCount * 2 + 3) & ~3u;
        ULONG *h = malloc(16 + ib + nverts * Stride);
        h[0] = PrimitiveType; h[1] = VertexCount; h[2] = Stride; h[3] = nverts;
        memcpy(h + 4, pIndexData, VertexCount * 2);
        memcpy((char *)(h + 4) + ib, pVertexData, nverts * Stride);
        pb_record(OP_DRAW_INDEXED_UP, h, 16 + ib + nverts * Stride);
        free(h);
        return;
    }
    draw(PrimitiveType, pVertexData, Stride, 0, VertexCount, pIndexData, false);
}

static void NTAPI D3DDevice_SetIndices(D3DResource *ib, UINT_ BaseVertexIndex)
{
    if (d3d.recording) { ULONG a[2] = { (ULONG)ib, BaseVertexIndex }; pb_record(OP_INDICES, a, sizeof(a)); }
    d3d.indices = ib;
    d3d.base_vertex_index = BaseVertexIndex;
    /* The DrawIndexedPrimitive inline in d3d8.h reads D3D__IndexData + StartIndex. */
    if (d3d.index_data) *d3d.index_data = ib ? (USHORT *)ib->Data : NULL;
}

/* ---- formats and surfaces -------------------------------------------- */


#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

/* Framebuffer objects are GL 3.0 / ARB_framebuffer_object: fetch them. */
static void (APIENTRY *p_glGenFramebuffers)(GLsizei, GLuint *);
static void (APIENTRY *p_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (APIENTRY *p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static void (APIENTRY *p_glGenRenderbuffers)(GLsizei, GLuint *);
static void (APIENTRY *p_glBindRenderbuffer)(GLenum, GLuint);
static void (APIENTRY *p_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static GLenum (APIENTRY *p_glCheckFramebufferStatus)(GLenum);
static void (APIENTRY *p_glDeleteFramebuffers)(GLsizei, const GLuint *);
static void (APIENTRY *p_glDeleteRenderbuffers)(GLsizei, const GLuint *);
static void (APIENTRY *p_glGetRenderbufferParameteriv)(GLenum, GLenum, GLint *);

static void load_fbo_functions(void)
{
#define LOAD(n) p_##n = SDL_GL_GetProcAddress(#n)
    LOAD(glGenFramebuffers); LOAD(glBindFramebuffer); LOAD(glFramebufferTexture2D);
    LOAD(glFramebufferRenderbuffer); LOAD(glGenRenderbuffers); LOAD(glBindRenderbuffer);
    LOAD(glRenderbufferStorage); LOAD(glCheckFramebufferStatus);
    LOAD(glDeleteFramebuffers); LOAD(glDeleteRenderbuffers);
    LOAD(glGetRenderbufferParameteriv);
#undef LOAD
}

/* The back buffer is a framebuffer object of the size the title asked for,
   standing in for framebuffer 0 everywhere, and Present draws it over the
   whole window the way the Xbox's video encoder put the frame on the TV:
   KMSDRM always makes the window the display mode, so a 640x480 back buffer
   meets a 720x480 NTSC screen and is stretched across it.

   The back buffer is multisampled when the title asked for antialiasing
   (D3DPRESENT_PARAMETERS.MultiSampleType: the dashboard asks for 4 samples
   with a gaussian filter), and Present approximates the downsample filter
   and the encoder's flicker filter (SetFlickerFilter, 5 unless the title
   changes it) with a small blur. XBCOMPAT_MSAA=n forces n samples (0 or 1
   turns antialiasing and its blur off); XBCOMPAT_FLICKER=0..5 forces the
   flicker filter level. */
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#define GL_RGB565 0x8D62
static struct {
    GLuint fbo;                  /* the back buffer (multisampled when samples > 1) */
    GLuint resolve;              /* single-sample copy of a multisampled back buffer */
    GLuint tex;                  /* the finished frame: resolve's (or fbo's) color */
    GLuint color_rb, depth_rb, resolve_depth_rb;
    GLenum color_storage, depth_storage;   /* the GL formats standing in for the title's */
    int w, h, samples;
    float soft;                  /* weight of each neighbour in the downsample filter */
    GLuint prog;
    GLint u_tex, u_step, u_weight, u_lut, u_gamma;
    GLuint lut;                  /* the gamma ramp as a 256x1 texture */
    void (APIENTRY *bind)(GLenum, GLuint);
    void (APIENTRY *blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
} window_fb;

/* The reverse of readback_surface for the device depth buffer: the CPU
   wrote its memory, so the GPU's depth becomes those bytes, as on the NV2A
   where the memory is the depth buffer.  A later readback then keeps them
   wherever the GPU did not draw since. */
static void upload_depth_memory(void)
{
    if (!window_fb.fbo || window_fb.depth_storage != 0x88F0 /* GL_DEPTH24_STENCIL8 */) return;
    if (!p_glWindowPos2i) p_glWindowPos2i = SDL_GL_GetProcAddress("glWindowPos2i");
    if (!p_glWindowPos2i) return;
    hud_batch_flush();
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)d3d.depth, &w, &h, &pitch);
    if (w > (ULONG)d3d.width) w = d3d.width;
    if (h > (ULONG)d3d.height) h = d3d.height;
    const uint8_t *px = (const uint8_t *)(d3d.depth->Data | CONTIG_BASE);
    uint8_t *tmp = malloc(w * h * 4);
    if (!tmp) return;
    ULONG row = pitch < w * 4 ? pitch : w * 4;
    for (ULONG y = 0; y < h; y++) memcpy(tmp + (h - 1 - y) * w * 4, px + y * pitch, row);
    /* Called while a draw binds its textures: its program comes back after. */
    GLuint program = cur_program;
    p_glUseProgram(0);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    for (int u = 3; u >= 0; u--) {
        p_glActiveTexture(GL_TEXTURE0 + u);
        glDisable(GL_TEXTURE_2D); glDisable(GL_TEXTURE_3D); glDisable(GL_TEXTURE_CUBE_MAP);
    }
    glDisable(GL_SCISSOR_TEST); glDisable(GL_ALPHA_TEST); glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);   /* glDrawPixels writes depth only with the test on */
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glWindowPos2i(0, 0);
    glDrawPixels(w, h, 0x84F9 /* GL_DEPTH_STENCIL */, 0x84FA /* GL_UNSIGNED_INT_24_8 */, tmp);
    if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, d3d.rt_texture ? d3d.fbo : 0);
    glPopAttrib();
    gc_reset();   /* the cached GL state no longer matches */
    if (program) p_glUseProgram(program);
    free(tmp);
    TRACE_ALL("D3D: depth memory written by the CPU loaded into the depth buffer");
}

/* The back buffer keeps the title's formats where GL has them: R5G6B5
   stays 16-bit (its banding and its blending are the title's), X8R8G8B8
   has no alpha for destination-alpha blending to find, D16 keeps its
   precision.  Anything else is approximated and says so, once, at
   device creation.  XBCOMPAT_BACKBUFFER_FORMAT=rgba8 asks for RGBA8
   color always, the storage before formats were followed.  0 depth is
   no depth buffer. */
static GLenum backbuffer_color_storage(ULONG fmt)
{
    const char *override = getenv("XBCOMPAT_BACKBUFFER_FORMAT");
    if (override && !strcmp(override, "rgba8")) return GL_RGBA8;
    if (fmt == 0x05 || fmt == 0x11) return GL_RGB565;   /* R5G6B5, LIN_R5G6B5 */
    /* X8R8G8B8, LIN_X8R8G8B8: the alpha a blend reads back is 1 */
    if (fmt == 0x07 || fmt == 0x1E) return GL_RGB8;
    if (fmt == 0x06 || fmt == 0x12 || (fmt >= 0x3A && fmt <= 0x3C) || (fmt >= 0x3F && fmt <= 0x41))
        return GL_RGBA8;   /* A8R8G8B8 and its other byte orders */
    /* X1R5G5B5, A1R5G5B5, A4R4G4B4 and the rest */
    xlog("D3D: unsupported back-buffer format %#x, explicit legacy RGBA8 approximation", fmt);
    return GL_RGBA8;
}

/* F16 and F24S8 have no integer format that orders the same way. */
static GLenum backbuffer_depth_storage(ULONG fmt)
{
    switch (fmt) {
    case 0: return 0;
    case 0x2C: case 0x30: return 0x81A5;   /* D16, LIN_D16: GL_DEPTH_COMPONENT16 */
    case 0x2A: case 0x2E: return GL_DEPTH24_STENCIL8;   /* D24S8, LIN_D24S8 */
    case 0x2D: case 0x31:   /* F16, LIN_F16 */
        xlog("D3D: floating depth %#x uses legacy integer D16 approximation", fmt);
        return 0x81A5;
    default:
        xlog("D3D: depth %#x uses legacy integer D24S8 approximation", fmt);
        return GL_DEPTH24_STENCIL8;
    }
}

static void destroy_window_framebuffer(void)
{
    if (window_fb.bind) p_glBindFramebuffer = window_fb.bind;
    if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (p_glDeleteFramebuffers) {
        if (window_fb.resolve) p_glDeleteFramebuffers(1, &window_fb.resolve);
        if (window_fb.fbo) p_glDeleteFramebuffers(1, &window_fb.fbo);
    }
    if (window_fb.tex) glDeleteTextures(1, &window_fb.tex);
    if (p_glDeleteRenderbuffers) {
        if (window_fb.color_rb) p_glDeleteRenderbuffers(1, &window_fb.color_rb);
        if (window_fb.depth_rb) p_glDeleteRenderbuffers(1, &window_fb.depth_rb);
        if (window_fb.resolve_depth_rb) p_glDeleteRenderbuffers(1, &window_fb.resolve_depth_rb);
    }
    window_fb.fbo = window_fb.resolve = window_fb.tex = 0;
    window_fb.color_rb = window_fb.depth_rb = window_fb.resolve_depth_rb = 0;
    window_fb.bind = NULL;
}

static void APIENTRY bind_framebuffer(GLenum target, GLuint fb)
{
    window_fb.bind(target, fb ? fb : window_fb.fbo);
}

static int env_int(const char *name, int fallback)
{
    const char *e = getenv(name);
    return e && *e ? atoi(e) : fallback;
}

/* Samples and downsample softness for an Xbox D3DMULTISAMPLE_TYPE: the low
   two nibbles are the sample grid (0x22 = 2x2), the next one the filter
   (0 linear, 1 quincunx, 2 gaussian). */
static int multisample_samples(ULONG type, float *soft)
{
    int n = (type & 0xF) * ((type >> 4) & 0xF);
    *soft = n > 1 && ((type >> 8) & 0xF) ? 0.125f : 0.0f;
    return n > 1 ? n : 1;
}

static GLuint make_renderbuffer(int samples, GLenum format, int w, int h)
{
    static void (APIENTRY *storage_ms)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    if (!storage_ms) storage_ms = SDL_GL_GetProcAddress("glRenderbufferStorageMultisample");
    GLuint rb = 0;
    p_glGenRenderbuffers(1, &rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
    if (samples > 1 && storage_ms) storage_ms(GL_RENDERBUFFER, samples, format, w, h);
    else p_glRenderbufferStorage(GL_RENDERBUFFER, format, w, h);
    /* (the host may give fewer samples: create_window_framebuffer asks) */
    return rb;
}

static GLuint make_texture_framebuffer(GLuint *tex)
{
    GLuint fb;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, window_fb.color_storage, d3d.width, d3d.height, 0,
                 window_fb.color_storage == GL_RGB565 ? GL_RGB : GL_BGRA,
                 window_fb.color_storage == GL_RGB565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, NULL);
    if (window_fb.color_storage == GL_RGB565) {
        GLint r = 0, g = 0, b = 0, a = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &r);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_GREEN_SIZE, &g);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_BLUE_SIZE, &b);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &a);
        if (r != 5 || g != 6 || b != 5 || a) {
            xlog("D3D: host cannot allocate exact RGB565 texture (%d/%d/%d/%d bits)", r, g, b, a);
            glDeleteTextures(1, tex); *tex = 0; return 0;
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glGenFramebuffers(1, &fb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (window_fb.depth_storage) {
        window_fb.resolve_depth_rb = make_renderbuffer(1, window_fb.depth_storage, d3d.width, d3d.height);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, window_fb.depth_storage == GL_DEPTH24_STENCIL8
                                    ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                    window_fb.resolve_depth_rb);
    }
    return fb;
}

static bool create_window_framebuffer(void)
{
    destroy_window_framebuffer();
    SDL_GL_GetDrawableSize(d3d.window, &window_fb.w, &window_fb.h);
    window_fb.blit = SDL_GL_GetProcAddress("glBlitFramebuffer");
    window_fb.color_storage = backbuffer_color_storage(d3d.color_format);
    window_fb.depth_storage = backbuffer_depth_storage(d3d.depth_format);
    if (!p_glGenFramebuffers || !window_fb.blit) {
        xlog("D3D: host lacks framebuffer support for title storage"); return false;
    }
    window_fb.samples = multisample_samples(d3d.multisample_type, &window_fb.soft);
    const char *msaa = getenv("XBCOMPAT_MSAA");
    if (msaa && *msaa && strcmp(msaa, "auto") && strcmp(msaa, "title")) {
        char *end;
        long n = strtol(msaa, &end, 10);
        if (!*end && n >= 0 && n <= 64) window_fb.samples = n < 1 ? 1 : n;
        else xlog("D3D: invalid XBCOMPAT_MSAA=%s, honoring title", msaa);
    }
    if (window_fb.samples <= 1) window_fb.samples = 1, window_fb.soft = 0;
    GLint max = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max);
    if (window_fb.samples > max) {
        xlog("D3D: requested %d samples, host maximum %d", window_fb.samples, max);
        window_fb.samples = max > 1 ? max : 1;
    }
    if (window_fb.samples > 1 && !SDL_GL_GetProcAddress("glRenderbufferStorageMultisample")) {
        xlog("D3D: host lacks multisample storage; falling back to one sample");
        window_fb.samples = 1;
    }
    if (window_fb.samples == 1) window_fb.soft = 0;

    window_fb.resolve = make_texture_framebuffer(&window_fb.tex);
    if (!window_fb.resolve || p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        xlog("D3D: cannot allocate requested back-buffer storage");
        destroy_window_framebuffer(); return false;
    }
    if (window_fb.samples > 1) {
        p_glGenFramebuffers(1, &window_fb.fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, window_fb.fbo);
        window_fb.color_rb = make_renderbuffer(window_fb.samples, window_fb.color_storage, d3d.width, d3d.height);
        GLint actual_samples = 0;
        if (p_glGetRenderbufferParameteriv)
            p_glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &actual_samples);
        bool exact_color = actual_samples > 1;
        if (actual_samples != window_fb.samples)
            xlog("D3D: multisample storage requested %d samples, host allocated %d", window_fb.samples, actual_samples);
        if (actual_samples > 1) window_fb.samples = actual_samples;
        if (window_fb.color_storage == GL_RGB565) {
            GLint r = 0, g = 0, b = 0, a = 0;
            if (p_glGetRenderbufferParameteriv) {
                p_glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_RED_SIZE, &r);
                p_glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_GREEN_SIZE, &g);
                p_glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_BLUE_SIZE, &b);
                p_glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_ALPHA_SIZE, &a);
            }
            bool exact_bits = r == 5 && g == 6 && b == 5 && !a;
            exact_color = exact_color && exact_bits;
            if (!exact_bits) xlog("D3D: host multisample storage is not RGB565 (%d/%d/%d/%d bits)", r, g, b, a);
        }
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, window_fb.color_rb);
        if (window_fb.depth_storage) {
            window_fb.depth_rb = make_renderbuffer(window_fb.samples, window_fb.depth_storage, d3d.width, d3d.height);
            p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, window_fb.depth_storage == GL_DEPTH24_STENCIL8
                                        ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                        window_fb.depth_rb);
        }
        if (!exact_color || p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            xlog("D3D: no %d-sample back buffer, drawing without antialiasing", window_fb.samples);
            window_fb.samples = 1;
            window_fb.soft = 0;
            p_glDeleteFramebuffers(1, &window_fb.fbo); window_fb.fbo = 0;
            p_glDeleteRenderbuffers(1, &window_fb.color_rb); window_fb.color_rb = 0;
            if (window_fb.depth_rb) { p_glDeleteRenderbuffers(1, &window_fb.depth_rb); window_fb.depth_rb = 0; }
        }
    }
    if (window_fb.samples <= 1) {
        window_fb.fbo = window_fb.resolve;
        window_fb.resolve = 0;
        p_glBindFramebuffer(GL_FRAMEBUFFER, window_fb.fbo);
    }
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        xlog("D3D: no back buffer framebuffer, drawing %dx%d into the corner of a %dx%d window", d3d.width,
             d3d.height, window_fb.w, window_fb.h);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        destroy_window_framebuffer(); return false;
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    window_fb.bind = p_glBindFramebuffer;
    p_glBindFramebuffer = bind_framebuffer;
    xlog("D3D: %dx%d back buffer title color %#x depth %#x multisample %#x; host color %#x depth %#x %d samples (%s), window %dx%d",
         d3d.width, d3d.height, d3d.color_format, d3d.depth_format, d3d.multisample_type, window_fb.color_storage,
         window_fb.depth_storage, window_fb.samples, msaa && *msaa ? msaa : "title", window_fb.w, window_fb.h);
    return true;
}

/* Bring a multisampled back buffer's samples down into the resolve framebuffer. */
static void resolve_window_framebuffer(GLbitfield mask)
{
    if (!window_fb.resolve) return;
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    window_fb.bind(GL_READ_FRAMEBUFFER, window_fb.fbo);
    window_fb.bind(GL_DRAW_FRAMEBUFFER, window_fb.resolve);
    window_fb.blit(0, 0, d3d.width, d3d.height, 0, 0, d3d.width, d3d.height, mask, GL_NEAREST);
    window_fb.bind(GL_DRAW_FRAMEBUFFER, d3d.rt_texture ? d3d.fbo : window_fb.fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

/* The back buffer paired with a depth texture (SetRenderTarget(NULL, z) for
   a depth peel or shadow pass): GL can't attach the window framebuffer's
   color to the FBO holding the texture, and the texture's rows run top-down
   while the window's run bottom-up, so the pass draws into a texture
   standing in for the back buffer, filled from the window when the pair is
   set and copied back, flipped, when it is left or the frame is shown.
   Rendering into the window's own depth instead put every pass of the
   ATG NoSortAlphaBlend sample into the main depth buffer and left its
   peel textures empty. */
static void backbuffer_proxy_blit(bool to_window)
{
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    int w = (int)d3d.width, h = (int)d3d.height;
    if (to_window) {
        window_fb.bind(GL_READ_FRAMEBUFFER, d3d.fbo);
        window_fb.bind(GL_DRAW_FRAMEBUFFER, window_fb.fbo);
    } else {
        resolve_window_framebuffer(GL_COLOR_BUFFER_BIT);
        window_fb.bind(GL_READ_FRAMEBUFFER, window_fb.resolve ? window_fb.resolve : window_fb.fbo);
        window_fb.bind(GL_DRAW_FRAMEBUFFER, d3d.fbo);
    }
    window_fb.blit(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    window_fb.bind(GL_FRAMEBUFFER, d3d.rt_texture ? d3d.fbo : window_fb.fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

/* Bring the window up to date with what the pass drew.  A multisampled
   window can't be the target of a flipping blit: its color from such a pass
   is lost (depth-only passes, the usual case, don't mind). */
static void backbuffer_proxy_sync(void)
{
    if (!d3d.bb_proxy) return;
    if (window_fb.resolve) {
        static bool warned;
        if (!warned) xlog("D3D: color drawn with a depth texture over a multisampled back buffer is dropped");
        warned = true;
        return;
    }
    backbuffer_proxy_blit(true);
}

static void backbuffer_proxy_end(void)
{
    if (!d3d.bb_proxy) return;
    backbuffer_proxy_sync();
    d3d.bb_proxy = false;
}

/* The stand-in texture, in the window's color format, attached by the
   caller to d3d.fbo before backbuffer_proxy_blit(false) fills it. */
static GLuint backbuffer_proxy_texture(void)
{
    if (!d3d.bb_proxy_tex) {
        GLuint id;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, window_fb.color_storage, d3d.width, d3d.height, 0,
                     window_fb.color_storage == GL_RGB565 ? GL_RGB : GL_BGRA,
                     window_fb.color_storage == GL_RGB565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE, NULL);
        d3d.bb_proxy_tex = id;
    }
    return d3d.bb_proxy_tex;
}

/* Point GL_READ_FRAMEBUFFER at the back buffer's pixels for glReadPixels
   (GL can't read a multisampled buffer directly). */
static void backbuffer_read_begin(GLbitfield mask)
{
    backbuffer_proxy_sync();
    if (!window_fb.fbo) {
        if (p_glBindFramebuffer) p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        return;
    }
    resolve_window_framebuffer(mask);
    window_fb.bind(GL_READ_FRAMEBUFFER, window_fb.resolve ? window_fb.resolve : window_fb.fbo);
}

static void backbuffer_read_end(void)
{
    if (p_glBindFramebuffer) p_glBindFramebuffer(GL_READ_FRAMEBUFFER, d3d.rt_texture ? d3d.fbo : 0);
}

/* A GL texture holding the back buffer's current pixels, top row first like
   an upload of its memory would, for a linear 32-bit texture header over
   it.  0 when the window has no framebuffer object to blit from. */
/* Copy the back buffer's top-left w x h into a 2D texture, top row first. */
static void backbuffer_blit_to_texture(GLuint tex, ULONG w, ULONG h)
{
    static GLuint fb;
    if (!fb) p_glGenFramebuffers(1, &fb);
    GLint draw = 0;
    glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &draw);
    backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
    window_fb.bind(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, fb);
    p_glFramebufferTexture2D(0x8CA9, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    window_fb.blit(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    window_fb.bind(0x8CA9, draw);
    backbuffer_read_end();
    stats.readbacks++;
}

static GLuint backbuffer_copy_texture(ULONG data, ULONG w, ULONG h)
{
    static struct { ULONG data, w, h; GLuint tex; } slot[8];
    if (!window_fb.fbo || !window_fb.blit || w > (ULONG)d3d.width || h > (ULONG)d3d.height) return 0;
    unsigned i = 0;
    while (i < 8 && slot[i].tex && !(slot[i].data == data && slot[i].w == w && slot[i].h == h)) i++;
    if (i == 8) i = data % 8;
    /* Phantom Dust binds one view to two stages of a draw: the second asks
       for the same pixels again.  While nothing has drawn, cleared or been
       written by the CPU since, the copy made then still holds them.  On the
       Pi each copy is a GPU job of its own, and Mesa's VC4 driver makes the
       CPU wait for the GPU once more than five are in flight. */
    static int last_slot = -1;
    static unsigned last_draw, last_clear;
    static ULONG last_frame;
    if ((int)i == last_slot && slot[i].tex && slot[i].data == data && slot[i].w == w && slot[i].h == h
        && last_draw == conv_serial && last_clear == clear_serial && last_frame == d3d.frame && !d3d.bb_cpu_dirty) {
        TRACE_ALL("D3D: back buffer copy for a view at %#x reused (slot %u)", data, i);
        return slot[i].tex;
    }
    last_slot = i; last_draw = conv_serial; last_clear = clear_serial; last_frame = d3d.frame;
    if (!slot[i].tex || slot[i].data != data || slot[i].w != w || slot[i].h != h) {
        if (!slot[i].tex) glGenTextures(1, &slot[i].tex);
        glBindTexture(GL_TEXTURE_2D, slot[i].tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        /* One level: a stage with a mip filter (FocusBlur's blur passes)
           would otherwise find the texture incomplete and sample black. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        slot[i].data = data; slot[i].w = w; slot[i].h = h;
    }
    backbuffer_blit_to_texture(slot[i].tex, w, h);
    TRACE_ALL("D3D: back buffer copied for a view at %#x, %ux%u (slot %u)", data, w, h, i);
    return slot[i].tex;
}

/* ---- the front buffer and the persisted frame ----

   GetBackBuffer(-1) is the frame on screen.  The host draws into one
   frame, so the front buffer gets memory of its own, and once a title has
   asked for it each Swap copies the frame it shows into a texture: a view
   of it samples that, a lock or a copy reads it into the memory.  Splinter
   Cell cross-fades from it; handing out the back buffer instead faded from
   whatever was drawn since (white at boot, the new frame into gameplay). */

/* Traced frames: the average colour of the frame going out, so a trace shows
   which frame first turned a colour. */
static void trace_presented_colour(void)
{
    if (g_trace != 1 || !d3d.backbuffer) return;
    ULONG w = d3d.width, h = d3d.height;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    while (glGetError() != GL_NO_ERROR) {}
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    GLenum err = glGetError();
    GLint rfb = 0;
    glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &rfb);
    backbuffer_read_end();
    if (err) xlog("D3D: reading frame %lu back failed (GL error %#x, read framebuffer %d)", (unsigned long)d3d.frame, err, rfb);
    unsigned long long sum[3] = { 0, 0, 0 };
    for (size_t i = 0; i < (size_t)w * h; i++)
        for (int c = 0; c < 3; c++) sum[c] += px[i * 4 + c];
    size_t n = (size_t)w * h;
    xlog("D3D: frame %lu goes out with average colour %02llx%02llx%02llx", (unsigned long)d3d.frame,
         sum[2] / n, sum[1] / n, sum[0] / n);
    free(px);
}

static void front_buffer_update(void)
{
    trace_presented_colour();
    if (!front.used || !front.s || !window_fb.fbo || !window_fb.blit) return;
    /* Nothing drawn: the frame on screen stays what it was.  Splinter Cell's
       title picture sat through 38 such frames that read back black here, and
       its cross-fade into the menu then faded from that. */
    if (frame_idle && front.tex) return;
    ULONG w = d3d.width, h = d3d.height;
    if (!front.tex) {
        glGenTextures(1, &front.tex);
        glBindTexture(GL_TEXTURE_2D, front.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    backbuffer_blit_to_texture(front.tex, w, h);
    front.memory_stale = true;
}

/* front.snap: the front buffer's memory as xbcompat last left it. */
static void front_snap_take(void)
{
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)front.s, &w, &h, &pitch);
    uint8_t *snap = realloc(front.snap, (size_t)pitch * h);
    if (!snap) return;
    memcpy(snap, (const void *)(front.s->Data | CONTIG_BASE), (size_t)pitch * h);
    front.snap = snap;
}

/* The CPU wrote the front buffer, through a lock (TrueTypeFont's XFONT
   text) or a texture header over its memory: the hardware shows that on the
   frame on screen.  Here the frame on screen is already out, so put the
   pixels the CPU changed over the one being presented. */
static void front_buffer_overlay(void)
{
    if (!front.s || !front.snap) return;
    ULONG fmt = (front.s->Format >> 8) & 0xFF, w, h, pitch;
    container_size((D3DPixelContainer *)front.s, &w, &h, &pitch);
    if (!memcmp(front.snap, (const void *)(front.s->Data | CONTIG_BASE), (size_t)pitch * h)) return;
    if (fmt != 0x12 && fmt != 0x1E) { front_snap_take(); return; }
    if (w > (ULONG)d3d.width) w = d3d.width;
    if (h > (ULONG)d3d.height) h = d3d.height;
    const uint32_t *now = (const uint32_t *)(front.s->Data | CONTIG_BASE), *was = (const uint32_t *)front.snap;
    uint32_t *px = calloc((size_t)d3d.width * d3d.height, 4);
    if (!px) return;
    unsigned changed = 0;
    for (ULONG y = 0; y < h; y++)
        for (ULONG x = 0; x < w; x++) {
            uint32_t v = now[y * pitch / 4 + x];
            if (v != was[y * pitch / 4 + x]) px[(h - 1 - y) * d3d.width + x] = v | 0xFF000000u, changed++;
        }
    if (changed) draw_window_pixels_masked((const uint8_t *)px, GL_BGRA, GL_UNSIGNED_BYTE, true);
    TRACE_ALL("D3D: %u front buffer pixels written by the CPU put over the frame", changed);
    free(px);
    front_snap_take();
}

/* Bring the front buffer's memory up to date for the CPU or a copy. */
static void front_buffer_memory(void)
{
    if (!front.memory_stale || !front.tex) return;
    gl_require();
    front.memory_stale = false;
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)front.s, &w, &h, &pitch);
    if (w > (ULONG)d3d.width) w = d3d.width;
    if (h > (ULONG)d3d.height) h = d3d.height;
    GLenum format, type;
    unsigned bytes;
    if (!surface_color_transfer((front.s->Format >> 8) & 0xFF, &format, &type, &bytes)) return;
    uint8_t *tmp = malloc((size_t)d3d.width * d3d.height * bytes);
    if (!tmp) return;
    glBindTexture(GL_TEXTURE_2D, front.tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, format, type, tmp);
    uint8_t *px = (uint8_t *)(front.s->Data | CONTIG_BASE);
    for (ULONG y = 0; y < h; y++) memcpy(px + y * pitch, tmp + (size_t)y * d3d.width * bytes, w * bytes);
    free(tmp);
    tex_invalidate(front.s->Data);
    front_snap_take();
}

/* PersistDisplay's frame for the next title, which is its own process:
   <hdd>/.persisted holds its size and BGRA rows, top first.  Within a title
   the library keeps its copy at AvGetSavedDataAddress too, until the next
   PersistDisplay replaces it or the first Present after CreateDevice or
   Reset frees it (present.cpp SwapFirstFlip).  Splinter Cell persists each
   loading picture and fades the next one in from that copy. */
static struct { D3DSurface *s, *own; bool loaded, presented, first_flip; } persisted;
void NTAPI AvSetSavedDataAddress(PVOID p);

static D3DSurface *new_linear_surface(ULONG fmt, ULONG w, ULONG h);
static void persisted_path(char *path, size_t n) { snprintf(path, n, "%s/.persisted", fs_hdd_root()); }

static void persisted_save(void)
{
    ULONG w = d3d.width, h = d3d.height;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    backbuffer_read_end();
    char path[600];
    persisted_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t hdr[2] = { w, h };
        fwrite(hdr, sizeof hdr, 1, f);
        for (ULONG y = h; y-- > 0;) fwrite(px + (size_t)y * w * 4, w * 4, 1, f);
        fclose(f);
    }
    /* The library's own copy, which replaces the last one. */
    ULONG ow = 0, oh = 0, pitch = 0;
    if (persisted.own) container_size((D3DPixelContainer *)persisted.own, &ow, &oh, &pitch);
    if (!persisted.own || ow != w || oh != h) persisted.own = new_linear_surface(0x1E /* LIN_X8R8G8B8 */, w, h);
    if (persisted.own) {
        container_size((D3DPixelContainer *)persisted.own, &ow, &oh, &pitch);
        uint8_t *dst = (uint8_t *)(persisted.own->Data | CONTIG_BASE);
        for (ULONG y = 0; y < h; y++) memcpy(dst + y * pitch, px + (size_t)(h - 1 - y) * w * 4, w * 4);
        tex_invalidate(persisted.own->Data);
        AvSetSavedDataAddress(persisted.own);
    }
    free(px);
}

static void persisted_load(void)
{
    persisted.loaded = true;
    char path[600];
    persisted_path(path, sizeof path);
    struct stat st;
    /* Left by a title that ran a while ago (a crash, a hand-run title): not ours. */
    if (stat(path, &st) || time(NULL) - st.st_mtime > 120) return;
    FILE *f = fopen(path, "rb");
    uint32_t hdr[2];
    if (!f || fread(hdr, sizeof hdr, 1, f) != 1 || !hdr[0] || !hdr[1] || hdr[0] > 4096 || hdr[1] > 4096) {
        if (f) fclose(f);
        return;
    }
    persisted.s = new_linear_surface(0x1E /* LIN_X8R8G8B8 */, hdr[0], hdr[1]);
    if (persisted.s) {
        ULONG w, h, pitch;
        container_size((D3DPixelContainer *)persisted.s, &w, &h, &pitch);
        uint8_t *px = (uint8_t *)(persisted.s->Data | CONTIG_BASE);
        for (ULONG y = 0; y < hdr[1]; y++)
            if (fread(px + y * pitch, hdr[0] * 4, 1, f) != 1) break;
        xlog("D3D: the last title's persisted %ux%u frame is the persisted surface", hdr[0], hdr[1]);
    }
    fclose(f);
}

PVOID NTAPI AvGetSavedDataAddress(void);
static void persisted_first_present(void)
{
    if (persisted.first_flip) {
        persisted.first_flip = false;
        if (AvGetSavedDataAddress()) {
            AvSetSavedDataAddress(NULL);   /* the library frees it here */
            TRACE_ALL("D3D: the first Present since the device was set up lets the persisted frame go");
        }
    }
    if (persisted.presented) return;
    persisted.presented = true;
    if (!persisted.loaded) {
        persisted.loaded = true;
        char path[600];
        persisted_path(path, sizeof path);
        unlink(path);
    }
}

static const char present_vs[] =
    "#version 120\n"
    "varying vec2 uv;\n"
    "void main() { uv = gl_Vertex.xy * 0.5 + 0.5; gl_Position = gl_Vertex; }\n";
#include "present_shader.h"
#include "present_target.h"

static bool present_program(unsigned variant)
{
    static GLuint programs[8];   /* by variant: bit 0 FILTER_X, 1 FILTER_Y, 2 FILTER_GAMMA */
    static unsigned failed;
    GLuint prog = programs[variant];
    if (prog && prog == window_fb.prog) return true;
    if (failed >> variant & 1 || !p_glCreateProgram) return false;
    if (prog) goto ready;
    char source[sizeof present_fs + 128];
    snprintf(source, sizeof source, "#version 120\n#define FILTER_X %u\n#define FILTER_Y %u\n#define FILTER_GAMMA %u\n%s",
             variant & 1, variant >> 1 & 1, variant >> 2 & 1, present_fs);
    GLuint vs = compile_shader(GL_VERTEX_SHADER, present_vs), fs = compile_shader(GL_FRAGMENT_SHADER, source);
    prog = vs && fs ? p_glCreateProgram() : 0;
    GLint ok = 0;
    if (prog) {
        p_glAttachShader(prog, vs);
        p_glAttachShader(prog, fs);
        p_glLinkProgram(prog);
        shader_diagnostics_link(prog, vs, fs);
        p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    }
    if (vs) p_glDeleteShader(vs);
    if (fs) p_glDeleteShader(fs);
    if (!ok) {
        xlog("D3D: no present filter program, the frame goes out unfiltered");
        if (prog) p_glDeleteProgram(prog);
        failed |= 1u << variant;
        return false;
    }
    programs[variant] = prog;
ready:
    window_fb.prog = prog;
    window_fb.u_tex = p_glGetUniformLocation(prog, "tex");
    window_fb.u_step = p_glGetUniformLocation(prog, "texel");
    window_fb.u_weight = p_glGetUniformLocation(prog, "weight");
    window_fb.u_lut = p_glGetUniformLocation(prog, "lut");
    window_fb.u_gamma = p_glGetUniformLocation(prog, "gamma");
    return true;
}

static void present_window_framebuffer(void)
{
    if (!window_fb.fbo) return;
    backbuffer_proxy_sync();
    DETAIL_TIMED(t_resolve, resolve_window_framebuffer(GL_COLOR_BUFFER_BIT));
    uint64_t filter_start = detail_on ? now_ns() : 0;
    static int flicker_override = -2;
    if (flicker_override == -2) flicker_override = env_int("XBCOMPAT_FLICKER", -1);
    int flicker = flicker_override >= 0 ? flicker_override : (int)d3d.flicker_filter;
    if (flicker > 5) flicker = 5;
    /* Flicker filter 5 blends each line with the ones above and below it
       1:2:1; the soft display (luma) filter softens along the line. */
    float wx = window_fb.soft + (d3d.soft_display ? 0.125f : 0.0f);
    float wy = window_fb.soft + flicker * 0.05f;
    if (wy > 0.3f) wy = 0.3f;

    static int filter_enabled = -1;
    if (filter_enabled < 0) filter_enabled = env_int("XBCOMPAT_PRESENT_FILTER", 1);
    if (!filter_enabled) wx = wy = 0;
    if ((wx <= 0 && wy <= 0 && !d3d.gamma_set) || !present_program((wx > 0) | (wy > 0) << 1 | d3d.gamma_set << 2)) {
        shader_diagnostics_mark(d3d.frame, ++shader_diag_draw, 0, 0, 0, "present-blit");
        GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        window_fb.bind(GL_READ_FRAMEBUFFER, window_fb.resolve ? window_fb.resolve : window_fb.fbo);
        window_fb.bind(GL_DRAW_FRAMEBUFFER, 0);
        window_fb.blit(0, 0, d3d.width, d3d.height, 0, 0, window_fb.w, window_fb.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (filter_start) t_filter += now_ns() - filter_start;
        return;
    }

    GLint prev_prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    window_fb.bind(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, window_fb.w, window_fb.h);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_POLYGON_STIPPLE);
    for (int i = 0; i < 6; i++) glDisable(GL_CLIP_PLANE0 + i);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (present_target_clear_enabled()) present_target_clear_color();
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, window_fb.tex);
    GLuint saved_sampler[2] = { bound_sampler[0], bound_sampler[1] };
    if (separate_samplers) { bind_stage_sampler(0, 0); bind_stage_sampler(1, 0); }
    p_glUseProgram(window_fb.prog);
    p_glUniform1i(window_fb.u_tex, 0);
    float step[2] = { 1.0f / d3d.width, 1.0f / d3d.height }, weight[2] = { wx, wy };
    p_glUniform2fv(window_fb.u_step, 1, step);
    p_glUniform2fv(window_fb.u_weight, 1, weight);
    p_glUniform1f(window_fb.u_gamma, d3d.gamma_set ? 1.0f : 0.0f);
    if (d3d.gamma_set) {
        p_glActiveTexture(GL_TEXTURE1);
        if (!window_fb.lut) {
            glGenTextures(1, &window_fb.lut);
            glBindTexture(GL_TEXTURE_2D, window_fb.lut);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            d3d.gamma_dirty = true;
        }
        glBindTexture(GL_TEXTURE_2D, window_fb.lut);
        if (d3d.gamma_dirty) {
            unsigned char rgb[256][3];
            for (int i = 0; i < 256; i++)
                rgb[i][0] = d3d.gamma[0][i], rgb[i][1] = d3d.gamma[1][i], rgb[i][2] = d3d.gamma[2][i];
            GLint align;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
            glPixelStorei(GL_UNPACK_ALIGNMENT, align);
            d3d.gamma_dirty = false;
        }
        p_glUniform1i(window_fb.u_lut, 1);
        p_glActiveTexture(GL_TEXTURE0);
    }
    shader_diagnostics_mark(d3d.frame, ++shader_diag_draw, 0, 0, window_fb.prog, "present-filter");
    glBegin(GL_TRIANGLE_STRIP);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(-1, 1); glVertex2f(1, 1);
    glEnd();
    if (separate_samplers) {
        bind_stage_sampler(0, saved_sampler[0]); bind_stage_sampler(1, saved_sampler[1]);
    }
    p_glUseProgram(prev_prog);
    glPopAttrib();
    if (filter_start) t_filter += now_ns() - filter_start;
}

/* The Guide (src/guide) draws over the finished frame, in window pixels,
   just before the swap.  Weak: builds without it (the native renderer
   library) skip it. */
__attribute__((weak)) bool guide_frame(int w, int h);
__attribute__((weak)) void guide_draw(int w, int h);

static void guide_overlay(void)
{
    if (!guide_frame || !guide_frame(window_fb.w, window_fb.h)) return;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    window_fb.bind(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, window_fb.w, window_fb.h);
    p_glActiveTexture(GL_TEXTURE0);
    GLuint samplers[2] = { bound_sampler[0], bound_sampler[1] };
    if (separate_samplers) bind_stage_sampler(0, 0);
    p_glUseProgram(0);
    guide_draw(window_fb.w, window_fb.h);
    if (separate_samplers) bind_stage_sampler(0, samplers[0]), bind_stage_sampler(1, samplers[1]);
    glPopAttrib();
    p_glUseProgram(cur_program);
}

static void restore_window_framebuffer(void)
{
    if (window_fb.fbo) window_fb.bind(GL_FRAMEBUFFER, d3d.rt_texture ? d3d.fbo : window_fb.fbo);
}

/* Bits per texel of an Xbox D3DFORMAT and whether it is stored linearly
   (otherwise swizzled).  DXT formats report bits per texel too. */
static int format_bits(ULONG fmt, bool *linear)
{
    *linear = false;
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x19: return 8;
    case 0x13: case 0x1B: case 0x1F: *linear = true; return 8;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: case 0x27: case 0x28: case 0x29:
    case 0x2C: case 0x2D: case 0x32: case 0x38: case 0x39: return 16;
    case 0x10: case 0x11: case 0x16: case 0x17: case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25:
    case 0x30: case 0x31: case 0x35: case 0x37: case 0x3D: case 0x3E: *linear = true; return 16;
    case 0x06: case 0x07: case 0x2A: case 0x2B: case 0x33: case 0x3A: case 0x3B: case 0x3C: return 32;
    case 0x12: case 0x1E: case 0x2E: case 0x2F: case 0x36: case 0x3F: case 0x40: case 0x41:
        *linear = true; return 32;
    case 0x0C: return 4;
    case 0x0E: case 0x0F: return 8;
    default: return 32;
    }
}

static ULONG log2u(ULONG v)
{
    ULONG l = 0;
    while ((1u << l) < v) l++;
    return l;
}

/* Bytes of one mip level of a swizzled or compressed texture. */
static ULONG level_bytes(ULONG fmt, ULONG w, ULONG h, ULONG d)
{
    bool linear;
    int bits = format_bits(fmt, &linear);
    if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F)
        return ((w + 3) / 4) * ((h + 3) / 4) * (fmt == 0x0C ? 8 : 16) * d;
    return w * h * d * bits / 8;
}

static void container_size(const D3DPixelContainer *t, ULONG *w, ULONG *h, ULONG *pitch)
{
    bool linear;
    int bits = format_bits((t->Format >> 8) & 0xFF, &linear);
    if (t->Size) {
        *w = (t->Size & 0xFFF) + 1;
        *h = ((t->Size >> 12) & 0xFFF) + 1;
        *pitch = ((t->Size >> 24) + 1) * 64;
    } else {
        *w = 1u << ((t->Format >> 20) & 0xF);
        *h = 1u << ((t->Format >> 24) & 0xF);
        *pitch = *w * bits / 8;
    }
}

/* Offset of mip level `level` from the start of the texture data. */
static ULONG level_offset(const D3DPixelContainer *t, ULONG level)
{
    ULONG fmt = (t->Format >> 8) & 0xFF, w, h, pitch, off = 0;
    container_size(t, &w, &h, &pitch);
    ULONG d = 1u << ((t->Format >> 28) & 0xF);
    for (ULONG i = 0; i < level; i++) {
        off += level_bytes(fmt, w, h, d);
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
        if (d > 1) d >>= 1;
    }
    return off;
}

static ULONG level_count(const D3DPixelContainer *t)
{
    ULONG n = (t->Format >> 16) & 0xF;
    return n ? n : 1;
}

static D3DSurface *alloc_surface(ULONG format, ULONG size, ULONG data, D3DPixelContainer *parent)
{
    D3DSurface *s = pool_alloc(sizeof(*s));
    memset(s, 0, sizeof(*s));
    s->Common = 1 | D3DCOMMON_TYPE_SURFACE | D3DCOMMON_D3DCREATED;
    s->Format = format;
    s->Size = size;
    s->Data = data;
    s->Parent = parent;
    if (parent) parent->res.Common++;
    return s;
}

/* Format/Size words for a standalone linear surface of w x h. */
static void linear_words(ULONG fmt, ULONG w, ULONG h, ULONG *format, ULONG *size, ULONG *bytes)
{
    bool linear;
    int bits = format_bits(fmt, &linear);
    ULONG pitch = (w * bits / 8 + 63) & ~63u;
    *format = 1 | (2 << 4) | (fmt << 8) | (1 << 16);
    *size = (w - 1) | ((h - 1) << 12) | ((pitch / 64 - 1) << 24);
    *bytes = pitch * h;
}

/* The lib describes the back buffer with the linear variant of the format
   the title asked for. */
static ULONG linear_variant(ULONG fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;  /* L8 */
    case 0x01: return 0x1B;  /* AL8 */
    case 0x04: return 0x1D;  /* A4R4G4B4 */
    case 0x19: return 0x1F;  /* A8 */
    case 0x1A: return 0x20;  /* A8L8 */
    case 0x27: return 0x37;  /* R6G5B5 / L6V5U5 */
    case 0x28: return 0x17;  /* G8B8 / V8U8 */
    case 0x29: return 0x16;  /* R8B8 */
    case 0x32: return 0x35;  /* L16 */
    case 0x33: return 0x36;  /* V16U16 */
    case 0x38: return 0x3D;  /* R5G5B5A1 */
    case 0x39: return 0x3E;  /* R4G4B4A4 */
    case 0x3A: return 0x3F;  /* A8B8G8R8 */
    case 0x3B: return 0x40;  /* B8G8R8A8 */
    case 0x3C: return 0x41;  /* R8G8B8A8 */
    case 0x06: return 0x12;  /* A8R8G8B8 */
    case 0x07: return 0x1E;  /* X8R8G8B8 */
    case 0x05: return 0x11;  /* R5G6B5 */
    case 0x03: return 0x1C;  /* X1R5G5B5 */
    case 0x02: return 0x10;  /* A1R5G5B5 */
    case 0x2A: return 0x2E;  /* D24S8 */
    case 0x2B: return 0x2F;  /* F24S8 */
    case 0x2C: return 0x30;  /* D16 */
    case 0x2D: return 0x31;  /* F16 */
    default: return fmt;
    }
}

static D3DSurface *new_linear_surface(ULONG fmt, ULONG w, ULONG h)
{
    ULONG format, size, bytes;
    linear_words(linear_variant(fmt), w, h, &format, &size, &bytes);
    PVOID mem = MmAllocateContiguousMemoryEx(bytes, 0, 0x7FFFFFFF, 64, 4);
    if (!mem) return NULL;
    memset(mem, 0, bytes);
    return alloc_surface(format, size, (ULONG)mem & 0x7FFFFFFF, NULL);
}

static void create_device_surfaces(ULONG format, ULONG depth_format)
{
    d3d.backbuffer = new_linear_surface(format, d3d.width, d3d.height);
    d3d.depth = new_linear_surface(depth_format ? depth_format : 0x2A, d3d.width, d3d.height);
    if (!front.s && (front.s = new_linear_surface(format, d3d.width, d3d.height))) front_snap_take();
#ifndef XBC_TRANSLATED
    /* Copy every presented frame from the start, so the first ask for it
       finds the frame on screen rather than whatever is drawn by then.  Not
       on the Pi, where each copy is a GPU job of its own. */
    front.used = true;
#endif
    if (!persisted.loaded && !persisted.presented) {
        persisted_load();
        if (persisted.s) AvSetSavedDataAddress(persisted.s);
    }
    persisted.first_flip = true;
    d3d.zscale = depth_format == 0x2C || depth_format == 0x2D || depth_format == 0x30 || depth_format == 0x31
                     ? 65535.0f : 16777215.0f;
    zsync.hash = depth_memory_hash();
    zsync.writes = depth_gpu_writes;
    backbuffer_synced();
    d3d.target = d3d.backbuffer;
    d3d.target_depth = d3d.depth;
    internal_addref_surface(d3d.target);
    internal_addref_surface(d3d.target_depth);
    d3d.rt_width = d3d.width;
    d3d.rt_height = d3d.height;
    d3d.rt_texture = false;
    d3d.target_zmax = d3d.zscale;
    d3d.host_zmax = window_fb.depth_storage == 0x81A5 /* GL_DEPTH_COMPONENT16 */ ? 65535.0f : 16777215.0f;
}

static void NTAPI D3DDevice_GetBackBuffer(LONG BackBuffer, ULONG Type, D3DSurface **pp)
{
    (void)Type;
    TRACE_ALL("D3D: GetBackBuffer(%d)", (int)BackBuffer);
    /* -1 is the frame on screen (see front_buffer_update). */
    D3DSurface *s = BackBuffer == -1 && front.s && window_fb.fbo ? front.s : d3d.backbuffer;
    if (s == front.s) {
        front.used = true;
        /* Its memory holds the frame on screen, as the hardware's does:
           Splinter Cell builds texture headers over it (XGSetTextureHeader)
           and copies from it with D3DX on the CPU.  The lock-free state call
           of queued drawing has no GL to read it with; a lock still does. */
        if (!async_front()) front_buffer_memory();
    }
    s->Common++;
    *pp = s;
}

static LONG NTAPI D3DDevice_GetDepthStencilSurface(D3DSurface **pp)
{
    if (!(*pp = d3d.target_depth)) return D3DERR_NOTFOUND;
    d3d.target_depth->Common++;
    /* The current depth surface, which SetRenderTarget may have changed or removed. */
    return D3D_OK;
}

static void NTAPI D3DDevice_GetRenderTarget(D3DSurface **pp)
{
    d3d.target->Common++;
    *pp = d3d.target;
}

static GLuint texture_for(D3DPixelContainer *t);
static ULONG cube_face_bytes(D3DPixelContainer *t);


/* Packed color transfer descriptors. These describe byte layout, not a
   colorspace conversion. Only directly representable color formats belong
   here; palette, bump, video and floating depth need separate semantics. */
static bool surface_color_transfer(ULONG fmt, GLenum *format, GLenum *type, unsigned *bytes)
{
    *bytes = 4;
    switch (fmt) {
    case 0x06: case 0x07: case 0x12: case 0x1E:
        *format = GL_BGRA; *type = GL_UNSIGNED_BYTE; return true;
    case 0x3A: case 0x3F:
        *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; return true;
    case 0x3B: case 0x40:
        *format = GL_BGRA; *type = GL_UNSIGNED_INT_8_8_8_8; return true;
    case 0x3C: case 0x41:
        *format = GL_RGBA; *type = GL_UNSIGNED_INT_8_8_8_8; return true;
    }
    *bytes = 2;
    switch (fmt) {
    case 0x05: case 0x11:
        *format = GL_RGB; *type = GL_UNSIGNED_SHORT_5_6_5; return true;
    case 0x02: case 0x03: case 0x10: case 0x1C:
        *format = GL_BGRA; *type = GL_UNSIGNED_SHORT_1_5_5_5_REV; return true;
    case 0x04: case 0x1D:
        *format = GL_BGRA; *type = GL_UNSIGNED_SHORT_4_4_4_4_REV; return true;
    case 0x38: case 0x3D:
        *format = GL_RGBA; *type = GL_UNSIGNED_SHORT_5_5_5_1; return true;
    case 0x39: case 0x3E:
        *format = GL_RGBA; *type = GL_UNSIGNED_SHORT_4_4_4_4; return true;
    default: return false;
    }
}

/* Fixed integer depth has an exact GL transfer representation too. Floating
   encodings remain excluded until their numeric conversion is specified. */
static bool surface_transfer(ULONG fmt, GLenum *format, GLenum *type, unsigned *bytes)
{
    if (fmt == 0x2A || fmt == 0x2E) {
        *format = GL_DEPTH_STENCIL; *type = GL_UNSIGNED_INT_24_8; *bytes = 4; return true;
    }
    if (fmt == 0x2C || fmt == 0x30) {
        *format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_SHORT; *bytes = 2; return true;
    }
    return surface_color_transfer(fmt, format, type, bytes);
}

/* The GL texture behind a standalone render target surface, kept in the
   texture cache under the surface's own memory so destroy_resource frees it. */
static GLuint surface_rt_texture(D3DSurface *s, ULONG w, ULONG h)
{
    for (tex_entry *e = *tex_bucket(s->Data); e; e = e->next)
        if (e->data == s->Data && e->format == s->Format && e->size == s->Size) return e->id;
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    ULONG fmt = (s->Format >> 8) & 0xFF;
    GLenum transfer_format, transfer_type;
    unsigned bytes;
    if (surface_transfer(fmt, &transfer_format, &transfer_type, &bytes)) {
        ULONG sw, sh, pitch;
        container_size((D3DPixelContainer *)s, &sw, &sh, &pitch);
        upload_image3(GL_TEXTURE_2D, 0, fmt, w, h, 1, pitch,
                      (const uint8_t *)(s->Data | CONTIG_BASE));
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    }
    tex_entry *e = malloc(sizeof(*e));
    *e = (tex_entry){ s->Data, s->Format, s->Size, 0, id, GL_TEXTURE_2D, *tex_bucket(s->Data) };
    *tex_bucket(s->Data) = e;
    tex_cache_version++;
    return id;
}

/* Resolve a surface view without replacing its parent texture. */
static tex_entry *surface_image(D3DSurface *s, GLenum *face, ULONG *level)
{
    D3DPixelContainer *t = s->Parent ? s->Parent : (D3DPixelContainer *)s;
    for (tex_entry *e = *tex_bucket(t->res.Data); e; e = e->next) {
        if (e->data != t->res.Data || e->format != t->Format || e->size != t->Size) continue;
        if (e->target != GL_TEXTURE_2D && e->target != GL_TEXTURE_CUBE_MAP) return NULL;
        ULONG offset = s->Data - t->res.Data;
        *face = e->target;
        *level = 0;
        if (e->target == GL_TEXTURE_CUBE_MAP) {
            ULONG stride = cube_face_bytes(t), f = offset / stride;
            if (f >= 6) return NULL;
            *face = GL_TEXTURE_CUBE_MAP_POSITIVE_X + f;
            offset %= stride;
        }
        if (s->Parent) {
            while (*level < level_count(t) && level_offset(t, *level) != offset) (*level)++;
            if (*level == level_count(t)) return NULL;
        }
        return e;
    }
    return NULL;
}

/* Texture-backed surfaces retain GPU writes until explicitly read back.
   GL row zero is the Xbox top row for these render targets. */
static bool readback_rt_surface(D3DSurface *s)
{
    hud_batch_flush();
    if (s == d3d.backbuffer) return false;
    /* A colour header over the depth buffer's memory (Splinter Cell locks
       one to decode its loading pictures there): the GL texture under that
       address is only a copy made from memory when it was last sampled, not
       a render target, and reading it back put the previous picture (black
       at first) over what the CPU had written since through its pointer. */
    if (d3d.depth && (s->Data | CONTIG_BASE) == (d3d.depth->Data | CONTIG_BASE)) return false;
    ULONG fmt = (s->Format >> 8) & 0xFF, level;
    GLenum transfer_format, transfer_type;
    unsigned bytes;
    if (!surface_transfer(fmt, &transfer_format, &transfer_type, &bytes)) return false;
    GLenum face;
    tex_entry *e = surface_image(s, &face, &level);
    if (!e) return false;
    if (gl_lost()) {   /* the holder may drop the copy meanwhile: look again once ours */
        if (!gl_require() || !(e = surface_image(s, &face, &level))) return false;
    }
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)s, &w, &h, &pitch);
    if (!w || !h || w > SIZE_MAX / h / bytes) return false;
    uint8_t *tmp = malloc((size_t)w * h * bytes);
    if (!tmp) return false;
    uint8_t *px = (uint8_t *)(s->Data | CONTIG_BASE);
    glBindTexture(e->target, e->id);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(face, level, transfer_format, transfer_type, tmp);
    if (s->Size) {
        for (ULONG y = 0; y < h; y++) memcpy(px + y * pitch, tmp + y * w * bytes, w * bytes);
    } else {
        swizzle(tmp, w * bytes, px, w, h, bytes);
    }
    free(tmp);
    stats.readbacks++;
    TRACE_ALL("D3D: surface %p at %#x read back from GL texture %u", (void *)s, (unsigned)s->Data, e->id);
    return true;
}

/* --trace aid: while /data/xbox/xbcompat-dump-targets exists, a traced
   frame writes each texture render target it leaves to
   /tmp/xbc-dump/F-N-DATA-WxH.ppm (top row first) for looking at offline. */
static void trace_dump_target(D3DSurface *s)
{
    static unsigned seq;
    static unsigned long seq_frame;
    if (g_trace != 1 || !s || s == d3d.backbuffer || !d3d.rt_texture) return;
    if (access("/data/xbox/xbcompat-dump-targets", F_OK) && access("/tmp/xbcompat-dump-targets", F_OK)) return;
    GLenum face;
    ULONG level;
    tex_entry *e = s->Parent ? surface_image(s, &face, &level) : NULL;
    if (!s->Parent) {
        for (e = *tex_bucket(s->Data); e; e = e->next)
            if (e->data == s->Data && e->format == s->Format && e->size == s->Size) break;
        face = GL_TEXTURE_2D; level = 0;
    }
    if (!e) return;
    hud_batch_flush();
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)s, &w, &h, &pitch);
    if (!w || !h || w > 2048 || h > 2048) return;
    uint8_t *px = malloc(w * h * 4);
    if (!px) return;
    glBindTexture(e->target, e->id);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(face, level, GL_RGBA, GL_UNSIGNED_BYTE, px);
    mkdir("/tmp/xbc-dump", 0755);
    if (seq_frame != d3d.frame) { seq_frame = d3d.frame; seq = 0; }
    char path[128];
    snprintf(path, sizeof path, "/tmp/xbc-dump/%lu-%02u-%08x-%ux%u.ppm", (unsigned long)d3d.frame, seq++,
             (unsigned)s->Data, (unsigned)w, (unsigned)h);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6 %u %u 255\n", (unsigned)w, (unsigned)h);
        for (ULONG i = 0; i < w * h; i++) fwrite(px + i * 4, 1, 3, f);
        fclose(f);
        xlog("D3D:   dumped %s", path);
    }
    free(px);
    gc_reset();   /* the texture binding changed behind the cache */
}

static void NTAPI D3DDevice_SetRenderTarget(D3DSurface *target, D3DSurface *z)
{
    gl_require();
    trace_dump_target(d3d.target);
    pusher_drain();
    if (async_front()) { async_target(target, z); return; }
    TRACE_ALL("D3D: SetRenderTarget %#x, depth %#x", target ? (unsigned)target->Data : 0, z ? (unsigned)z->Data : 0);
    if (async_replaying) {
        /* The front took the references and told the title's device. */
        d3d.target = target;
        d3d.target_depth = z;
    } else {
    if (d3d.recording) { ULONG a[2] = { (ULONG)target, (ULONG)z }; pb_record(OP_RENDER_TARGET, a, sizeof(a)); }
    if (!target) target = d3d.target;
    /* The device holds its targets with internal references, and resets the
       viewport to the whole new target. */
    internal_addref_surface(target);
    if (z) internal_addref_surface(z);
    if (d3d.target) internal_release_surface(d3d.target);
    if (d3d.target_depth) internal_release_surface(d3d.target_depth);
    d3d.target = target;
    d3d.target_depth = z;
    d3d.scissors.count = 0;   /* via the library's SetViewport */
    sync_device_surfaces();
    }
    TRACE_ALL("D3D: render target %p (parent %p) depth %p", (void *)target, (void *)target->Parent, (void *)z);
    backbuffer_proxy_end();
    /* Depth surfaces with an image of their own: a depth texture's level, or
       a standalone integer depth surface (kept like one). */
    bool z_image = z && ((z->Parent && tex_target(z->Parent) == GL_TEXTURE_2D)
                         || (!z->Parent && (((z->Format >> 8) & 0xFF) == 0x2E
                                          || ((z->Format >> 8) & 0xFF) == 0x30)));
    bool bb_proxy = target == d3d.backbuffer && z_image && z != d3d.depth && window_fb.fbo && window_fb.blit
                    && p_glGenFramebuffers;
    if (!bb_proxy && (target == d3d.backbuffer || (!target->Parent && !p_glGenFramebuffers))) {
        if (target != d3d.backbuffer)
            xlog("D3D: no framebuffer objects, rendering a standalone surface into the back buffer");
        if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (target == d3d.backbuffer && window_fb.fbo && (!z || z == d3d.depth)) {
            /* The window framebuffer's depth follows the title's: the device's
               own depth surface attaches it, none detaches it. */
            GLenum attachment = window_fb.depth_storage == GL_DEPTH24_STENCIL8
                                ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
            p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            if (z) p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER,
                                              window_fb.depth_rb ? window_fb.depth_rb : window_fb.resolve_depth_rb);
            if (window_fb.resolve) {
                window_fb.bind(GL_FRAMEBUFFER, window_fb.resolve);
                p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
                if (z) p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, window_fb.resolve_depth_rb);
                window_fb.bind(GL_FRAMEBUFFER, window_fb.fbo);
            }
        }
        d3d.rt_width = d3d.width;
        d3d.rt_height = d3d.height;
        d3d.rt_texture = false;
        d3d.target_zmax = z ? depth_format_max((z->Format >> 8) & 0xFF) : d3d.zscale;
        d3d.host_zmax = window_fb.depth_storage == 0x81A5 /* GL_DEPTH_COMPONENT16 */ ? 65535.0f : 16777215.0f;
        target_viewport_reset(z, d3d.rt_width, d3d.rt_height);
        return;
    }
    if (!p_glGenFramebuffers) {
        xlog("D3D: no framebuffer objects, cannot render to texture");
        return;
    }
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)target, &w, &h, &pitch);
    if (!d3d.fbo) p_glGenFramebuffers(1, (GLuint *)&d3d.fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, d3d.fbo);
    /* A standalone surface (CreateRenderTarget) renders into a GL texture of
       its own; CopyRects reads it back. */
    GLuint tex = bb_proxy ? backbuffer_proxy_texture()
                 : target->Parent ? texture_for(target->Parent) : surface_rt_texture(target, w, h);
    GLenum face_target = GL_TEXTURE_2D;
    ULONG target_level = 0;
    /* Surface dimensions and offsets identify the actual mip, including a
       mip within a cube face. The parent's level zero is not this view. */
    if (!bb_proxy && target->Parent && !surface_image(target, &face_target, &target_level)) {
        xlog("D3D: cannot resolve render target subresource");
        return;
    }
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, face_target, tex, target_level);
    d3d.target_zmax = z ? depth_format_max((z->Format >> 8) & 0xFF) : d3d.zscale;
    if (z_image) {
        /* Give standalone integer depth surfaces their own persistent image,
           just like texture views. A shared scratch renderbuffer loses data
           when targets alternate and cannot be copied or locked back. */
        ULONG zw, zh, zp;
        container_size((D3DPixelContainer *)z, &zw, &zh, &zp);
        GLuint dt = z->Parent ? texture_for(z->Parent) : surface_rt_texture(z, zw, zh);
        GLenum depth_face;
        ULONG depth_level;
        if (!surface_image(z, &depth_face, &depth_level)) {
            xlog("D3D: cannot resolve depth target subresource");
            return;
        }
        bool d24 = depth_format_max((z->Format >> 8) & 0xFF) > 65535.0f;
        /* upload_image3 stores D16 images as GL_DEPTH_COMPONENT24 too, so
           GL's polygon offset unit is 2^-24 for both: with 65535 here a D16
           shadow buffer got 1/256 of its D3DRS_POLYGONOFFSETZOFFSET. */
        d3d.host_zmax = 16777215.0f;
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, d24 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                 depth_face, dt, depth_level);
        GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) xlog("D3D: depth texture framebuffer incomplete (%#x)", st);
        d3d.rt_width = w;
        d3d.rt_height = h;
        d3d.rt_texture = true;
        if (bb_proxy && st == GL_FRAMEBUFFER_COMPLETE) {
            backbuffer_proxy_blit(false);
            d3d.bb_proxy = true;
        }
        target_viewport_reset(z, w, h);
        return;
    }
    /* Any other depth surface gets a scratch renderbuffer in its own format;
       with none, the target has no depth at all, as on the Xbox. */
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    static GLuint rb; static ULONG rb_w, rb_h;
    static GLenum rb_format;
    GLenum depth_storage = z ? backbuffer_depth_storage((z->Format >> 8) & 0xFF) : 0;
    d3d.host_zmax = depth_storage == 0x81A5 /* GL_DEPTH_COMPONENT16 */ ? 65535.0f : 16777215.0f;
    if (z && (!rb || rb_w != w || rb_h != h || rb_format != depth_storage)) {
        if (!rb) p_glGenRenderbuffers(1, &rb);
        p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
        p_glRenderbufferStorage(GL_RENDERBUFFER, depth_storage, w, h);
        rb_w = w; rb_h = h;
        rb_format = depth_storage;
    }
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    if (z) p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, depth_storage == GL_DEPTH24_STENCIL8
                                       ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) xlog("D3D: render target framebuffer incomplete (%#x)", st);
    d3d.rt_width = w;
    d3d.rt_height = h;
    d3d.rt_texture = true;
    target_viewport_reset(z, w, h);
}

static void NTAPI D3DSurface_GetDesc(D3DSurface *s, ULONG *desc)
{
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)s, &w, &h, &pitch);
    desc[0] = (s->Format >> 8) & 0xFF;       /* Format */
    desc[1] = 1;                             /* D3DRTYPE_SURFACE */
    desc[2] = 0;                             /* Usage */
    desc[3] = pitch * h;                     /* Size */
    desc[4] = 0;                             /* MultiSampleType */
    desc[5] = w;
    desc[6] = h;
}

static void tex_invalidate(ULONG data);

static void NTAPI D3DSurface_LockRect(D3DSurface *s, ULONG *locked, const LONG *rect, ULONG flags)
{
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)s, &w, &h, &pitch);
    TRACE_ALL("D3D: LockRect surface %p data %#x %ux%u flags %#x%s%s", (void *)s, (unsigned)s->Data, (unsigned)w,
              (unsigned)h, (unsigned)flags, s == d3d.backbuffer ? " backbuffer" : "", s == d3d.depth ? " depth" : "");
    if (s == d3d.backbuffer && !d3d.bb_cpu_dirty && gl_require()) {
        /* Hand out the real pixels: read the frame back.  Not while memory
           holds writes from an earlier lock: GL hasn't seen them yet (a draw
           flushes them first), so memory is already the newest frame, and
           reading GL back would erase them.  XFONT locks once per TextOut,
           so every string but a frame's last went missing. */
        uint8_t *px = (uint8_t *)(s->Data | CONTIG_BASE);
        GLenum format, type; unsigned bytes;
        if (!surface_color_transfer((s->Format >> 8) & 0xFF, &format, &type, &bytes)) {
            xlog("D3D: unsupported back-buffer lock transfer format"); return;
        }
        uint8_t *tmp = malloc((size_t)w * h * bytes);
        if (!tmp) return;
        backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, format, type, tmp);
        backbuffer_read_end();
        for (ULONG y = 0; y < h; y++) memcpy(px + y * pitch, tmp + (h - 1 - y) * w * bytes, w * bytes);
        free(tmp);
        backbuffer_synced();
    }
    if (s == front.s) front_buffer_memory();
    else if (s != d3d.backbuffer) readback_rt_surface(s);
    if (s == d3d.backbuffer && !(flags & 0x80 /* D3DLOCK_READONLY */)) d3d.bb_cpu_dirty = true;
    if (s->Parent && !(flags & 0x80)) tex_invalidate(s->Parent->res.Data);
    ULONG fmt = (s->Format >> 8) & 0xFF;
    bool linear;
    ULONG offset = rect ? rect[1] * pitch + rect[0] * (format_bits(fmt, &linear) / 8) : 0;
    if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) {   /* rows of 4x4 blocks, as in lock_level */
        ULONG block = fmt == 0x0C ? 8 : 16;
        pitch = ((w + 3) / 4) * block;
        offset = rect ? (rect[1] / 4) * pitch + (rect[0] / 4) * block : 0;
    }
    locked[0] = pitch;
    locked[1] = (s->Data | CONTIG_BASE) + offset;
}

static LONG NTAPI D3DDevice_CreateImageSurface(UINT_ w, UINT_ h, ULONG fmt, D3DSurface **pp)
{
    *pp = new_linear_surface(fmt, w, h);
    return *pp ? D3D_OK : 0x8007000E;
}

static LONG NTAPI D3DDevice_CreateRenderTarget(UINT_ w, UINT_ h, ULONG fmt, ULONG ms, BOOLEAN lockable,
                                               D3DSurface **pp)
{
    (void)ms; (void)lockable;
    return D3DDevice_CreateImageSurface(w, h, fmt, pp);
}

static LONG NTAPI D3DDevice_CreateDepthStencilSurface(UINT_ w, UINT_ h, ULONG fmt, ULONG ms, D3DSurface **pp)
{
    (void)ms;
    return D3DDevice_CreateImageSurface(w, h, fmt, pp);
}


/* Compressed 2D/cube copies preserve encoded blocks. They do not render or
   convert formats. Small mip edges may end inside the final complete block. */
static void copy_compressed_surface(D3DSurface *src, const LONG *rects, UINT_ n,
                                    D3DSurface *dst, const LONG *points)
{
    ULONG fmt = (src->Format >> 8) & 0xFF;
    if (fmt != ((dst->Format >> 8) & 0xFF) || (fmt != 0x0C && fmt != 0x0E && fmt != 0x0F)) {
        xlog("D3D: compressed CopyRects requires identical DXT formats");
        return;
    }
    ULONG sw,sh,sp,dw,dh,dp;
    container_size((D3DPixelContainer *)src,&sw,&sh,&sp);
    container_size((D3DPixelContainer *)dst,&dw,&dh,&dp);
    unsigned block = fmt == 0x0C ? 8 : 16;
    size_t source_pitch = ((size_t)sw+3)/4*block, dest_pitch = ((size_t)dw+3)/4*block;
    size_t source_size = source_pitch*((sh+3)/4), dest_size = dest_pitch*((dh+3)/4);
    /* Resource dimensions have already been limited by their encoded fields. */
    if (!source_size || !dest_size) return;
    for (UINT_ i = 0; i < (n ? n : 1); i++) {
        LONG x0=n?rects[4*i]:0, y0=n?rects[4*i+1]:0;
        LONG x1=n?rects[4*i+2]:(LONG)sw, y1=n?rects[4*i+3]:(LONG)sh;
        LONG dx=n&&points?points[2*i]:0, dy=n&&points?points[2*i+1]:0;
        if (x0<0 || y0<0 || dx<0 || dy<0 || x1<=x0 || y1<=y0
            || x1>(LONG)sw || y1>(LONG)sh || dx>(LONG)dw-(x1-x0) || dy>(LONG)dh-(y1-y0)
            || ((x0|y0|dx|dy)&3)
            || ((x1&3) && x1!=(LONG)sw) || ((y1&3) && y1!=(LONG)sh)
            || (((dx+x1-x0)&3) && dx+x1-x0!=(LONG)dw)
            || (((dy+y1-y0)&3) && dy+y1-y0!=(LONG)dh)) {
            xlog("D3D: compressed CopyRects needs in-bounds block-aligned rectangles");
            return;
        }
    }
    uint8_t *source = malloc(source_size);
    if (!source) return;
    memcpy(source,(const void *)(src->Data|CONTIG_BASE),source_size);
    uint8_t *dest = (void *)(dst->Data|CONTIG_BASE);
    for (UINT_ i = 0; i < (n ? n : 1); i++) {
        LONG x0=n?rects[4*i]:0, y0=n?rects[4*i+1]:0;
        LONG x1=n?rects[4*i+2]:(LONG)sw, y1=n?rects[4*i+3]:(LONG)sh;
        LONG dx=n&&points?points[2*i]:0, dy=n&&points?points[2*i+1]:0;
        size_t rows=((size_t)(y1-y0)+3)/4, bytes=((size_t)(x1-x0)+3)/4*block;
        for (size_t row=0; row<rows; row++)
            memcpy(dest+(dy/4+row)*dest_pitch+(dx/4)*block,
                   source+(y0/4+row)*source_pitch+(x0/4)*block,bytes);
    }
    free(source);
    GLenum face; ULONG level;
    tex_entry *e = surface_image(dst,&face,&level);
    if (e) {
        GLenum internal = fmt == 0x0C ? 0x83F1 : fmt == 0x0E ? 0x83F2 : 0x83F3;
        glBindTexture(e->target,e->id);
        glCompressedTexSubImage2D(face,level,0,0,dw,dh,internal,dest_size,dest);
    }
}

static void NTAPI D3DDevice_CopyRects(D3DSurface *src, const LONG *rects, UINT_ n, D3DSurface *dst,
                                      const LONG *points)
{
    gl_require();
    pusher_drain();
    ULONG sw, sh, sp, dw, dh, dp;
    container_size((D3DPixelContainer *)src, &sw, &sh, &sp);
    container_size((D3DPixelContainer *)dst, &dw, &dh, &dp);
    TRACE_ALL("D3D: CopyRects %p data %#x %ux%u -> %p data %#x %ux%u, %u rects", (void *)src, (unsigned)src->Data,
              (unsigned)sw, (unsigned)sh, (void *)dst, (unsigned)dst->Data, (unsigned)dw, (unsigned)dh, (unsigned)n);
    bool lin;
    ULONG src_fmt = (src->Format >> 8) & 0xFF, dst_fmt = (dst->Format >> 8) & 0xFF;
    if (src_fmt == 0x0C || src_fmt == 0x0E || src_fmt == 0x0F ||
        dst_fmt == 0x0C || dst_fmt == 0x0E || dst_fmt == 0x0F) {
        copy_compressed_surface(src, rects, n, dst, points);
        return;
    }
    /* CopyRects copies equal format layouts; it is not a conversion API.
       A linear/swizzled pair of the same format differs only in addressing. */
    if (linear_variant(src_fmt) != linear_variant(dst_fmt)) {
        xlog("D3D: CopyRects requires matching source and destination formats");
        return;
    }
    int bytes = format_bits(src_fmt, &lin) / 8;
    if (src == d3d.backbuffer) {
        ULONG locked[2];
        D3DSurface_LockRect(src, locked, NULL, 0x80);   /* refresh its pixels */
    } else if (src == front.s) {
        front_buffer_memory();
    } else {
        readback_rt_surface(src);
    }
    bool dst_lin;
    int dst_bytes = format_bits((dst->Format >> 8) & 0xFF, &dst_lin) / 8;
    if (bytes <= 0 || dst_bytes != bytes) {
        xlog("D3D: CopyRects needs matching uncompressed pixel sizes");
        return;
    }
    /* Work in linear rows even when either surface is swizzled. Separate
       buffers also make overlapping self-copies independent of write order. */
    if (dst != src) {
        if (dst == d3d.backbuffer && !d3d.bb_cpu_dirty) {
            ULONG locked[2];
            D3DSurface_LockRect(dst, locked, NULL, 0x80);
        } else {
            readback_rt_surface(dst);
        }
    }
    if (!sw || !sh || !dw || !dh || sw > SIZE_MAX / sh / bytes || dw > SIZE_MAX / dh / bytes) {
        xlog("D3D: CopyRects surface is too large");
        return;
    }
    const uint8_t *source = (const uint8_t *)(src->Data | CONTIG_BASE);
    uint8_t *dest = (uint8_t *)(dst->Data | CONTIG_BASE);
    uint8_t *source_rows = malloc(sw * sh * bytes);
    uint8_t *dest_rows = malloc(dw * dh * bytes);
    if (!source_rows || !dest_rows) {
        free(source_rows);
        free(dest_rows);
        xlog("D3D: CopyRects could not allocate row buffers");
        return;
    }
    if (lin) {
        for (ULONG y = 0; y < sh; y++) memcpy(source_rows + y * sw * bytes, source + y * sp, sw * bytes);
    } else {
        unswizzle(source, source_rows, sw, sh, bytes);
    }
    if (dst_lin) {
        for (ULONG y = 0; y < dh; y++) memcpy(dest_rows + y * dw * bytes, dest + y * dp, dw * bytes);
    } else {
        unswizzle(dest, dest_rows, dw, dh, bytes);
    }
    for (UINT_ i = 0; i < (n ? n : 1); i++) {
        LONG x0 = n ? rects[i * 4] : 0, y0 = n ? rects[i * 4 + 1] : 0;
        LONG x1 = n ? rects[i * 4 + 2] : (LONG)sw, y1 = n ? rects[i * 4 + 3] : (LONG)sh;
        LONG dx = n && points ? points[i * 2] : 0, dy = n && points ? points[i * 2 + 1] : 0;
        if (x0 < 0) { dx -= x0; x0 = 0; }
        if (y0 < 0) { dy -= y0; y0 = 0; }
        if (dx < 0) { x0 -= dx; dx = 0; }
        if (dy < 0) { y0 -= dy; dy = 0; }
        if (x1 > (LONG)sw) x1 = sw;
        if (y1 > (LONG)sh) y1 = sh;
        if (x1 - x0 > (LONG)dw - dx) x1 = x0 + (LONG)dw - dx;
        if (y1 - y0 > (LONG)dh - dy) y1 = y0 + (LONG)dh - dy;
        if (x1 <= x0 || y1 <= y0) continue;
        for (LONG y = y0; y < y1; y++)
            memcpy(dest_rows + ((dy + y - y0) * dw + dx) * bytes,
                   source_rows + (y * sw + x0) * bytes, (x1 - x0) * bytes);
    }
    if (dst_lin) {
        for (ULONG y = 0; y < dh; y++) memcpy(dest + y * dp, dest_rows + y * dw * bytes, dw * bytes);
    } else {
        swizzle(dest_rows, dw * bytes, dest, dw, dh, bytes);
    }
    /* Updating the existing image preserves sibling mip/face GPU contents
       and any live framebuffer attachment to this texture. */
    GLenum face;
    ULONG level;
    tex_entry *e = dst == d3d.backbuffer ? NULL : surface_image(dst, &face, &level);
    GLenum transfer_format, transfer_type;
    unsigned transfer_bytes;
    if (e && surface_transfer(dst_fmt, &transfer_format, &transfer_type, &transfer_bytes)) {
        glBindTexture(e->target, e->id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(face, level, 0, 0, dw, dh, transfer_format, transfer_type, dest_rows);
    } else if (dst->Parent) {
        tex_invalidate(dst->Parent->res.Data);
    }
    free(source_rows);
    free(dest_rows);
    if (dst == d3d.backbuffer) d3d.bb_cpu_dirty = true;
}

/* ---- texture objects --------------------------------------------------- */

static D3DPixelContainer *alloc_texture(ULONG w, ULONG h, ULONG depth, ULONG levels, ULONG fmt, bool cube)
{
    bool linear;
    format_bits(fmt, &linear);
    ULONG max_levels = 1;
    if (!linear) {
        ULONG m = w > h ? w : h;
        if (depth > m) m = depth;
        max_levels = log2u(m) + 1;
    }
    if (!levels || levels > max_levels) levels = max_levels;

    D3DPixelContainer *t = pool_alloc(sizeof(*t));
    memset(t, 0, sizeof(*t));
    t->res.Common = 1 | D3DCOMMON_TYPE_TEXTURE | D3DCOMMON_D3DCREATED;
    ULONG bytes;
    if (linear) {
        ULONG size;
        linear_words(fmt, w, h, &t->Format, &size, &bytes);
        t->Size = size;
    } else {
        t->Format = 1 | ((depth > 1 ? 3 : 2) << 4) | (fmt << 8) | (levels << 16) | (log2u(w) << 20) |
                    (log2u(h) << 24) | (log2u(depth) << 28);
        bytes = level_offset(t, levels);
    }
    if (cube) {
        t->Format |= 0x4;
        bytes = ((bytes + 127) & ~127u) * 6;
    }
    PVOID mem = MmAllocateContiguousMemoryEx(bytes, 0, 0x7FFFFFFF, 128, 4);
    if (!mem) { pool_free(t); return NULL; }
    memset(mem, 0, bytes);
    t->res.Data = (ULONG)mem & 0x7FFFFFFF;
    TRACE_ALL("D3D: texture %ux%ux%u fmt %#x levels %u%s = %p (%u bytes)", w, h, depth, fmt, levels,
          cube ? " cube" : "", (void *)t, bytes);
    return t;
}

static LONG NTAPI D3DDevice_CreateTexture(UINT_ w, UINT_ h, UINT_ levels, ULONG usage, ULONG fmt, ULONG pool,
                                          D3DPixelContainer **pp)
{
    (void)usage; (void)pool;
    *pp = alloc_texture(w, h, 1, levels, fmt, false);
    return *pp ? D3D_OK : 0x8007000E;
}

static LONG NTAPI D3DDevice_CreateCubeTexture(UINT_ edge, UINT_ levels, ULONG usage, ULONG fmt, ULONG pool,
                                              D3DPixelContainer **pp)
{
    (void)usage; (void)pool;
    *pp = alloc_texture(edge, edge, 1, levels, fmt, true);
    return *pp ? D3D_OK : 0x8007000E;
}

static LONG NTAPI D3DDevice_CreateVolumeTexture(UINT_ w, UINT_ h, UINT_ depth, UINT_ levels, ULONG usage,
                                                ULONG fmt, ULONG pool, D3DPixelContainer **pp)
{
    (void)usage; (void)pool;
    *pp = alloc_texture(w, h, depth, levels, fmt, false);
    return *pp ? D3D_OK : 0x8007000E;
}

static ULONG NTAPI D3DBaseTexture_GetLevelCount(D3DPixelContainer *t)
{
    return level_count(t);
}

static void NTAPI D3DVolumeTexture_GetLevelDesc(D3DPixelContainer *t, UINT_ level, ULONG *desc);

static void NTAPI D3DTexture_GetLevelDesc(D3DPixelContainer *t, UINT_ level, ULONG *desc)
{
    if (tex_target(t) == GL_TEXTURE_3D) { D3DVolumeTexture_GetLevelDesc(t, level, desc); return; }
    ULONG w, h, pitch;
    container_size(t, &w, &h, &pitch);
    for (UINT_ i = 0; i < level; i++) { if (w > 1) w >>= 1; if (h > 1) h >>= 1; }
    ULONG fmt = (t->Format >> 8) & 0xFF;
    desc[0] = fmt;
    desc[1] = 1;   /* D3DRTYPE_SURFACE: a level is described as a surface */
    desc[2] = 0;
    desc[3] = level_bytes(fmt, w, h, 1);
    desc[4] = 0;
    desc[5] = w;
    desc[6] = h;
}

static ULONG cube_face_bytes(D3DPixelContainer *t)
{
    return (level_offset(t, level_count(t)) + 127) & ~127u;
}

static D3DSurface *surface_of_level(D3DPixelContainer *t, ULONG face, ULONG level)
{
    ULONG w, h, pitch;
    container_size(t, &w, &h, &pitch);
    ULONG format = t->Format, size = t->Size;
    if (!size) {
        ULONG lw = log2u(w) > level ? log2u(w) - level : 0, lh = log2u(h) > level ? log2u(h) - level : 0;
        format = (format & ~0x0FFF0000u) | (1 << 16) | (lw << 20) | (lh << 24);
    }
    ULONG data = t->res.Data + level_offset(t, level) + face * cube_face_bytes(t);
    return alloc_surface(format & ~0x4u, size, data, t);
}

static LONG NTAPI D3DTexture_GetSurfaceLevel(D3DPixelContainer *t, UINT_ level, D3DSurface **pp)
{
    *pp = surface_of_level(t, 0, level);
    return D3D_OK;
}

static LONG NTAPI D3DCubeTexture_GetCubeMapSurface(D3DPixelContainer *t, ULONG face, UINT_ level, D3DSurface **pp)
{
    *pp = surface_of_level(t, face, level);
    return D3D_OK;
}

static void lock_level(D3DPixelContainer *t, ULONG face, ULONG level, ULONG *locked, const LONG *rect, ULONG flags)
{
    ULONG w, h, pitch;
    container_size(t, &w, &h, &pitch);
    bool linear;
    int bits = format_bits((t->Format >> 8) & 0xFF, &linear);
    for (ULONG i = 0; i < level; i++) { if (w > 1) w >>= 1; if (h > 1) h >>= 1; }
    ULONG fmt = (t->Format >> 8) & 0xFF, offset = rect ? rect[1] * pitch + rect[0] * bits / 8 : 0;
    if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) {
        /* Compressed levels are rows of 4x4 blocks; the pitch is one such row
           (PixelJar: Width * 2 for DXT1, Width * 4 otherwise). */
        ULONG block = fmt == 0x0C ? 8 : 16;
        pitch = ((w + 3) / 4) * block;
        offset = rect ? (rect[1] / 4) * pitch + (rect[0] / 4) * block : 0;
    } else if (!linear) {
        pitch = w * bits / 8;
        offset = rect ? rect[1] * pitch + rect[0] * bits / 8 : 0;
    }
    if (!(flags & 0x80)) tex_invalidate(t->res.Data);
    locked[0] = pitch;
    locked[1] = (t->res.Data | CONTIG_BASE) + level_offset(t, level) + face * cube_face_bytes(t) + offset;
}

static void NTAPI D3DTexture_LockRect(D3DPixelContainer *t, UINT_ level, ULONG *locked, const LONG *rect, ULONG flags)
{
    TRACE_ALL("D3D: LockRect texture %p data %#x level %u flags %#x", (void *)t, (unsigned)t->res.Data, (unsigned)level,
              (unsigned)flags);
    lock_level(t, 0, level, locked, rect, flags);
}

static void NTAPI D3DCubeTexture_LockRect(D3DPixelContainer *t, ULONG face, UINT_ level, ULONG *locked,
                                          const LONG *rect, ULONG flags)
{
    lock_level(t, face, level, locked, rect, flags);
}

/* Width, height and depth of mip level `level` of a volume texture. */
static void volume_dims(const D3DPixelContainer *t, ULONG level, ULONG *w, ULONG *h, ULONG *d)
{
    ULONG pitch;
    container_size(t, w, h, &pitch);
    *d = 1u << ((t->Format >> 28) & 0xF);
    for (ULONG i = 0; i < level; i++) {
        if (*w > 1) *w >>= 1;
        if (*h > 1) *h >>= 1;
        if (*d > 1) *d >>= 1;
    }
}

static void volume_desc(ULONG format, ULONG w, ULONG h, ULONG d, ULONG *desc)
{
    ULONG fmt = (format >> 8) & 0xFF;
    desc[0] = fmt;
    desc[1] = 2;   /* D3DRTYPE_VOLUME */
    desc[2] = 0;
    desc[3] = level_bytes(fmt, w, h, d);
    desc[4] = w;
    desc[5] = h;
    desc[6] = d;
}

/* The debug library folds Texture, CubeTexture and VolumeTexture GetLevelDesc
   into one function, so both table entries can land on the same address:
   each one answers by the container's real type. */
static void NTAPI D3DVolumeTexture_GetLevelDesc(D3DPixelContainer *t, UINT_ level, ULONG *desc)
{
    if (tex_target(t) != GL_TEXTURE_3D) { D3DTexture_GetLevelDesc(t, level, desc); return; }
    ULONG w, h, d;
    volume_dims(t, level, &w, &h, &d);
    volume_desc(t->Format, w, h, d, desc);
}

/* A volume is a one-level volume container pointing into its texture's data. */
static LONG NTAPI D3DVolumeTexture_GetVolumeLevel(D3DPixelContainer *t, UINT_ level, D3DSurface **pp)
{
    ULONG w, h, d;
    volume_dims(t, level, &w, &h, &d);
    ULONG format = (t->Format & ~0xFFFF0000u) | (1 << 16) | (log2u(w) << 20) | (log2u(h) << 24) | (log2u(d) << 28);
    *pp = alloc_surface(format, 0, t->res.Data + level_offset(t, level), t);
    return D3D_OK;
}

static void NTAPI D3DVolume_GetDesc(D3DSurface *v, ULONG *desc)
{
    volume_desc(v->Format, 1u << ((v->Format >> 20) & 0xF), 1u << ((v->Format >> 24) & 0xF),
                1u << ((v->Format >> 28) & 0xF), desc);
}

static void NTAPI D3DVolume_GetContainer(D3DSurface *v, D3DPixelContainer **pp)
{
    *pp = v->Parent;
    if (v->Parent) v->Parent->res.Common++;
}

/* D3DLOCKED_BOX {RowPitch, SlicePitch, pBits}; D3DBOX {Left, Top, Right, Bottom, Front, Back}.
   Swizzled volumes are addressed as if linear, like the real library. */
static void lock_box(ULONG data, ULONG fmt, ULONG w, ULONG h, ULONG *locked, const LONG *box, ULONG flags)
{
    bool linear;
    int bits = format_bits(fmt, &linear);
    ULONG pitch = w * bits / 8, slice = pitch * h;
    if (fmt == 0x0C) { pitch = ((w + 3) / 4) * 8; slice = pitch * ((h + 3) / 4); }
    else if (fmt == 0x0E || fmt == 0x0F) { pitch = ((w + 3) / 4) * 16; slice = pitch * ((h + 3) / 4); }
    if (!(flags & 0x80)) tex_invalidate(data);
    locked[0] = pitch;
    locked[1] = slice;
    locked[2] = (data | CONTIG_BASE) + (box ? box[4] * slice + box[1] * pitch + box[0] * bits / 8 : 0);
}

static void NTAPI D3DVolumeTexture_LockBox(D3DPixelContainer *t, UINT_ level, ULONG *locked, const LONG *box,
                                           ULONG flags)
{
    ULONG w, h, d;
    volume_dims(t, level, &w, &h, &d);
    lock_box(t->res.Data + level_offset(t, level), (t->Format >> 8) & 0xFF, w, h, locked, box, flags);
    if (!(flags & 0x80)) tex_invalidate(t->res.Data);
}

static void NTAPI D3DVolume_LockBox(D3DSurface *v, ULONG *locked, const LONG *box, ULONG flags)
{
    lock_box(v->Data, (v->Format >> 8) & 0xFF, 1u << ((v->Format >> 20) & 0xF), 1u << ((v->Format >> 24) & 0xF),
             locked, box, flags);
    if (v->Parent && !(flags & 0x80)) tex_invalidate(v->Parent->res.Data);
}

/* ---- index buffers, palettes, memory --------------------------------- */

static LONG NTAPI D3DDevice_CreateIndexBuffer(UINT_ Length, ULONG Usage, ULONG Format, ULONG Pool, D3DResource **pp)
{
    (void)Usage; (void)Format; (void)Pool;
    D3DResource *ib = pool_alloc(sizeof(*ib));
    PVOID mem = MmAllocateContiguousMemoryEx(Length, 0, 0x7FFFFFFF, 0, 4);
    if (!ib || !mem) return 0x8007000E;
    ib->Common = 1 | D3DCOMMON_TYPE_INDEXBUFFER | D3DCOMMON_D3DCREATED;
    ib->Data = (ULONG)mem;   /* virtual, like the real library: D3D__IndexData points into it */
    ib->Lock = 0;
    *pp = ib;
    return D3D_OK;
}

static void NTAPI D3DIndexBuffer_Lock(D3DResource *ib, UINT_ Offset, UINT_ Size, PVOID *ppbData, ULONG Flags)
{
    (void)Size; (void)Flags;
    *ppbData = (UCHAR *)ib->Data + Offset;
}

static LONG NTAPI D3DDevice_CreatePalette(ULONG Size, D3DPalette **pp)
{
    D3DPalette *p = pool_alloc(sizeof(*p));
    ULONG bytes = 1024u >> (Size & 3);
    PVOID mem = MmAllocateContiguousMemoryEx(bytes, 0, 0x7FFFFFFF, 0, 4);
    if (!p || !mem) return 0x8007000E;
    p->Common = 1 | D3DCOMMON_TYPE_PALETTE | D3DCOMMON_D3DCREATED | (Size << 30);
    p->Data = (ULONG)mem & 0x7FFFFFFF;
    p->Lock = 0;
    *pp = p;
    return D3D_OK;
}

static void NTAPI D3DPalette_Lock(D3DPalette *p, PVOID *ppColors, ULONG Flags)
{
    (void)Flags;
    *ppColors = (PVOID)(p->Data | CONTIG_BASE);
}

static void NTAPI D3DDevice_SetPalette(ULONG Stage, D3DPalette *p)
{
    if (d3d.recording) { ULONG a[2] = { Stage, (ULONG)p }; pb_record(OP_PALETTE, a, sizeof(a)); }
    if (Stage < 4) d3d.palettes[Stage] = p;
}

static PVOID NTAPI D3D_AllocNoncontiguousMemory(ULONG Size)
{
    return Size <= 65536 ? pool_alloc(Size) : arena_alloc(Size, 0);
}

static void NTAPI D3D_FreeNoncontiguousMemory(PVOID p)
{
    if ((ULONG)p < ARENA_BASE || (ULONG)p >= ARENA_END) return;
    /* Pool blocks are freed; big arena blocks stay mapped. */
    pool_free(p);
}

/* ---- getters and small state ---------------------------------------- */

static void NTAPI D3DDevice_GetTransform(ULONG State, D3DMATRIX *m)
{
    if (State < 10) *m = d3d.transforms[State]; else identity(m);
}

static void NTAPI D3DDevice_GetViewport(D3DVIEWPORT8 *v) { *v = d3d.viewport; }

static void NTAPI D3DDevice_GetRenderState(ULONG State, ULONG *v)
{
    *v = State < d3d.rs_count ? d3d.render_state[State] : 0;
}
static void NTAPI D3DDevice_GetTextureStageState(ULONG Stage, ULONG Type, ULONG *v)
{
    *v = Stage < 4 && Type < 32 ? TSS(Stage, Type) : 0;
}

static void NTAPI D3DDevice_GetTexture(ULONG Stage, D3DResource **pp)
{
    *pp = Stage < 4 ? d3d.textures[Stage] : NULL;
    if (*pp) (*pp)->Common++;
}

static void NTAPI D3DDevice_GetStreamSource(UINT_ Stream, D3DResource **pp, UINT_ *stride)
{
    *pp = Stream < 16 ? d3d.streams[Stream].vb : NULL;
    *stride = Stream < 16 ? d3d.streams[Stream].stride : 0;
    if (*pp) (*pp)->Common++;
}

static void NTAPI D3DDevice_GetIndices(D3DResource **pp, UINT_ *base)
{
    *pp = d3d.indices;
    *base = d3d.base_vertex_index;
    if (*pp) (*pp)->Common++;
}

static void NTAPI D3DDevice_GetMaterial(float *m)
{
    memcpy(m, d3d.material, 64);
    m[16] = d3d.material_power;
}

static void NTAPI D3DDevice_GetLight(ULONG Index, D3DLIGHT8 *l)
{
    memset(l, 0, sizeof(*l));
    if (Index >= 8) return;
    typeof(d3d.lights[0]) *L = &d3d.lights[Index];
    l->Type = L->type;
    memcpy(l->Diffuse, L->diffuse, 16);
    memcpy(l->Ambient, L->ambient, 16);
    memcpy(l->Specular, L->specular, 16);
    memcpy(l->Position, L->position, 12);
    memcpy(l->Direction, L->direction, 12);
    l->Range = L->range;
    l->Attenuation0 = L->attenuation[0];
    l->Attenuation1 = L->attenuation[1];
    l->Attenuation2 = L->attenuation[2];
}

static void NTAPI D3DDevice_GetLightEnable(ULONG Index, ULONG *on) { *on = Index < 8 && d3d.lights[Index].enabled; }

static void NTAPI D3DDevice_GetVertexShader(ULONG *h)
{
    /* A layout or program combined for SelectVertexShader/SetVertexShaderInput
       answers with the handle the title set. */
    ULONG v = d3d.vertex_shader;
    *h = (v & 1) && ((vshader *)(v & ~1u))->user_handle ? ((vshader *)(v & ~1u))->user_handle : v;
}

/* Constants are numbered -96..95 by the title (D3DSCM_96CONSTANTS puts 0..95
   at hardware slots 96..191; D3DSCM_192CONSTANTS exposes all of them). */
static int constant_slot(LONG reg)
{
    int slot = (d3d.constant_mode & 0xF) == 0 ? reg + 96 : reg + 96;
    return slot < 0 ? 0 : slot > 191 ? 191 : slot;
}

static void NTAPI D3DDevice_SetVertexShaderConstant(LONG Register, const float *data, ULONG count)
{
    if (d3d.recording) { ULONG h[2] = { (ULONG)Register, count }; pb_record2(OP_VS_CONST, h, 8, data, count * 16); }
    TRACE_ALL("D3D: vs constant %d x%u = %g %g %g %g", Register, count, data[0], data[1], data[2], data[3]);
    for (unsigned r = 1; r < count && r < 8; r++)
        TRACE_ALL("D3D:   +%u = %g %g %g %g", r, data[r * 4], data[r * 4 + 1], data[r * 4 + 2], data[r * 4 + 3]);
    bool front = async_front();
    for (ULONG i = 0; i < count; i++) {
        int slot = constant_slot(Register + i);
        memcpy(d3d.vs_const[slot], data + i * 4, 16);
        vsc_touch(slot, 1);
        if (front) async_vs_touch(slot);
    }
}

static void NTAPI D3DDevice_GetVertexShaderConstant(LONG Register, float *data, ULONG count)
{
    for (ULONG i = 0; i < count; i++) memcpy(data + i * 4, d3d.vs_const[constant_slot(Register + i)], 16);
}

static void NTAPI D3DDevice_SetShaderConstantMode(ULONG mode)
{
    /* Phantom Dust switches modes several times a frame: log the first few. */
    static unsigned logged;
    if (mode != d3d.constant_mode && logged < 8) { logged++; xlog("D3D: shader constant mode %#x", mode); }
    d3d.constant_mode = mode;
}
static void NTAPI D3DDevice_GetShaderConstantMode(ULONG *mode) { *mode = d3d.constant_mode; }

static void NTAPI D3DDevice_GetProjectionViewportMatrix(D3DMATRIX *out)
{
    /* Not viewport_transform(): with D3DZB_USEW its zscale / wfar scaling
       made Splinter Cell (4928) build model matrices that put every model
       out of view.  Titles combine this with their own vertex programs, and
       xbcompat's program epilogue expects the plain viewport matrix. */
    D3DVIEWPORT8 *v = &d3d.viewport;
    D3DMATRIX vp;
    identity(&vp);
    vp.m[0][0] = v->Width / 2.0f;
    vp.m[1][1] = -(float)v->Height / 2.0f;
    vp.m[2][2] = v->MaxZ - v->MinZ;
    vp.m[3][0] = v->X + v->Width / 2.0f;
    vp.m[3][1] = v->Y + v->Height / 2.0f;
    vp.m[3][2] = v->MinZ;
    mat_mul(out, &d3d.transforms[1], &vp);
}

static void NTAPI D3DDevice_GetDisplayMode(ULONG *mode)
{
    mode[0] = d3d.width;
    mode[1] = d3d.height;
    mode[2] = 60;
    mode[3] = 0;
    mode[4] = (d3d.backbuffer->Format >> 8) & 0xFF;
}

/* The Xbox's D3DCAPS8 (the library's g_DeviceCaps, from se/d3dbase.cpp and
   the KELVIN_* masks in se/caps.hpp). */
static void NTAPI D3DDevice_GetDeviceCaps(ULONG *caps)
{
    static const ULONG xbox_caps[53] = {
        1, 0, 0x20000, 0, 0, 0x80000003,               /* HAL, Caps READ_SCANLINE, intervals ONE|TWO|IMMEDIATE */
        0, 0x7bbef0, 0xcf2, 0x377101, 0xff, 0x1fff,    /* Cursor, Dev, PrimitiveMisc, Raster, ZCmp, SrcBlend */
        0x1fff, 0xff, 0x84208, 0x7ec87, 0x1f030700,    /* DestBlend, AlphaCmp, Shade, Texture, TextureFilter */
        0x1f030700, 0, 0x17, 0, 0x1f,                  /* CubeFilter, VolumeFilter, Address, VolumeAddress, Line */
        0x1000, 0x1000, 0, 8192, 0, 2,                 /* MaxTexture W/H, VolumeExtent, Repeat, AspectRatio, Aniso */
        0x501502f9,                                    /* MaxVertexW 1e10 */
        0xccbebc20, 0xccbebc20, 0x4cbebc20, 0x4cbebc20, /* guard band -1e8, -1e8, 1e8, 1e8 */
        0, 0xff, 0x80004, 0xffffff, 4, 4, 0xbb,        /* ExtentsAdjust, Stencil, FVF, TextureOps, stages, VtxP */
        8, 0, 4, 0, 0x42800000,                        /* lights, clip planes, blend matrices, index, MaxPointSize 64 */
        0xffff, 0xffff, 16, 0xff,                      /* MaxPrimitiveCount, MaxVertexIndex, streams, stride */
        0xffff0101, 96, 0xffff0101, 0x3f800000,        /* VS 1.1, 96 constants, PS 1.1, MaxPixelShaderValue 1 */
    };
    memcpy(caps, xbox_caps, sizeof(xbox_caps));
}

static LONG NTAPI Direct3D_GetDeviceCaps(UINT_ Adapter, ULONG DeviceType, ULONG *caps)
{
    (void)Adapter; (void)DeviceType;
    D3DDevice_GetDeviceCaps(caps);
    return D3D_OK;
}

static void NTAPI D3DDevice_GetCreationParameters(ULONG *p) { p[0] = 0; p[1] = 1; p[2] = 0; p[3] = 0; }
static LONG NTAPI Direct3D_CheckDeviceFormat(UINT_ a, ULONG b, ULONG c, ULONG d, ULONG e, ULONG f) { return D3D_OK; }
static LONG NTAPI Direct3D_CheckDeviceType(UINT_ a, ULONG b, ULONG c, ULONG d, BOOLEAN e) { return D3D_OK; }
static LONG NTAPI Direct3D_CheckDeviceMultiSampleType(UINT_ a, ULONG b, ULONG c, BOOLEAN d, ULONG e) { return D3D_OK; }
static LONG NTAPI Direct3D_CheckDepthStencilMatch(UINT_ a, ULONG b, ULONG c, ULONG d, ULONG e) { return D3D_OK; }
static UINT_ NTAPI Direct3D_GetAdapterModeCount(UINT_ a) { return 1; }
static LONG NTAPI Direct3D_GetAdapterDisplayMode(UINT_ a, ULONG *mode)
{
    mode[0] = 640; mode[1] = 480; mode[2] = 60; mode[3] = 0; mode[4] = 0x1E;
    return D3D_OK;
}
static LONG NTAPI Direct3D_EnumAdapterModes(UINT_ a, UINT_ m, ULONG *mode) { return Direct3D_GetAdapterDisplayMode(a, mode); }
static LONG NTAPI Direct3D_GetAdapterIdentifier(UINT_ a, ULONG flags, char *id)
{
    memset(id, 0, 1024);
    strcpy(id, "xbcompat");
    strcpy(id + 512, "NV2A");
    return D3D_OK;
}

/* The library keeps no rectangles as one inclusive rectangle over the
   viewport (and SetViewport sets that); here a count of 0 stands for it,
   so draws need no scissor, and GetScissors reports it as the library does. */
static void NTAPI D3DDevice_SetScissors(ULONG Count, BOOLEAN Exclusive, const D3DRECT *rects)
{
    if (Count > 8) Count = 8;
    if (!rects) Count = 0;
    if (!Count) Exclusive = 0;
    if (d3d.recording) { ULONG h[2] = { Count, Exclusive }; pb_record2(OP_SCISSORS, h, 8, rects, Count * sizeof(D3DRECT)); }
    d3d.scissors.count = Count;
    d3d.scissors.exclusive = Exclusive;
    if (Count) memcpy(d3d.scissors.rects, rects, Count * sizeof(D3DRECT));
    TRACE_ALL("D3D: SetScissors(%u, %u) first %d,%d-%d,%d", Count, Exclusive, Count ? (int)rects[0].x1 : 0,
              Count ? (int)rects[0].y1 : 0, Count ? (int)rects[0].x2 : 0, Count ? (int)rects[0].y2 : 0);
}

static void NTAPI D3DDevice_GetScissors(ULONG *Count, ULONG *Exclusive, D3DRECT *rects)
{
    const D3DVIEWPORT8 *v = &d3d.viewport;
    D3DRECT whole = { (LONG)v->X, (LONG)v->Y, (LONG)(v->X + v->Width), (LONG)(v->Y + v->Height) };
    ULONG n = d3d.scissors.count;
    if (Count) *Count = n ? n : 1;
    if (Exclusive) *Exclusive = n ? d3d.scissors.exclusive : 0;
    if (rects) {
        if (n) memcpy(rects, d3d.scissors.rects, n * sizeof(D3DRECT));
        else rects[0] = whole;
    }
}

/* Callbacks run "when the GPU gets there": we run them at the next sync point. */
static void run_callbacks(void)
{
    hud_batch_flush();
    gl_no_yield++;
    for (unsigned i = 0; i < d3d.ncallbacks; i++) {
#ifdef XBC_TRANSLATED
        CPU_CALL(d3d.callbacks[i].fn, CONV_CDECL, (uint32_t)d3d.callbacks[i].ctx);
#else
        void (CDECLAPI *fn)(ULONG) = d3d.callbacks[i].fn;
        fn(d3d.callbacks[i].ctx);
#endif
    }
    gl_no_yield--;
    d3d.ncallbacks = 0;
}

static void NTAPI D3DDevice_InsertCallback(ULONG Type, PVOID fn, ULONG Context)
{
    (void)Type;
    if (d3d.ncallbacks == 64) run_callbacks();
    d3d.callbacks[d3d.ncallbacks].fn = fn;
    d3d.callbacks[d3d.ncallbacks].ctx = Context;
    d3d.ncallbacks++;
}

static void NTAPI D3DDevice_SetVerticalBlankCallback(PVOID fn) { d3dv.vblank_callback = fn; }
static void NTAPI D3DDevice_SetSwapCallback(PVOID fn) { d3dv.swap_callback = fn; }
static void NTAPI D3DDevice_GetDisplayFieldStatus(ULONG *st) { st[0] = 3; st[1] = d3d.frame; }
static void NTAPI D3DDevice_SetScreenSpaceOffset(float x, float y) { d3d.screen_offset[0] = x; d3d.screen_offset[1] = y; }
static void NTAPI D3DDevice_SetBackBufferScale(float x, float y) { d3d.backbuffer_scale[0] = x; d3d.backbuffer_scale[1] = y; }
static void NTAPI D3DDevice_GetBackBufferScale(float *x, float *y) { *x = d3d.backbuffer_scale[0]; *y = d3d.backbuffer_scale[1]; }
/* The marker is only recorded (the library keeps it for its debug dumps):
   SetDebugMarker returns the previous one. */
static ULONG debug_marker;
static ULONG NTAPI D3DDevice_SetDebugMarker(ULONG v) { ULONG old = debug_marker; debug_marker = v; return old; }
static ULONG NTAPI D3DDevice_GetDebugMarker(void) { return debug_marker; }
/* Tiles (D3DTILE: Flags, pMemory, Size, Pitch, ZStartTag, ZOffset) only set
   how the NV2A lays out and compresses memory; GetTile returns what SetTile
   set, as the library does. */
static ULONG d3d_tiles[8][6];
static void NTAPI D3DDevice_SetTile(ULONG i, const void *t)
{
    if (i >= 8) return;
    if (t) memcpy(d3d_tiles[i], t, sizeof d3d_tiles[i]);
    else memset(d3d_tiles[i], 0, sizeof d3d_tiles[i]);
}
static void NTAPI D3DDevice_GetTile(ULONG i, ULONG *t) { if (i < 8) memcpy(t, d3d_tiles[i], sizeof d3d_tiles[i]); else memset(t, 0, 24); }
/* No Z compression: no tag is ever set. */
static void NTAPI D3DDevice_SetTileCompressionTagBits(ULONG part, ULONG addr, const ULONG *data, ULONG n)
{
    (void)part; (void)addr; (void)data; (void)n;
}
static void NTAPI D3DDevice_GetTileCompressionTagBits(ULONG part, ULONG addr, ULONG *data, ULONG n)
{
    (void)part; (void)addr;
    if (data) memset(data, 0, n * 4);
}
static ULONG NTAPI D3DDevice_GetTileCompressionTags(ULONG start, ULONG end) { (void)start; (void)end; return 0; }

/* CopyRects' blit engine state (D3DCOPYRECTSTATE, 8 dwords, and
   D3DCOPYRECTROPSTATE, 8).  CopyRects copies as D3DCOPYRECT_SRCCOPY with
   the default color format; anything else is kept for GetCopyRectsState
   and noted once. */
static ULONG copyrect_state[8] = { 0, 3 /* D3DCOPYRECT_SRCCOPY */ }, copyrect_rop[8];
static void NTAPI D3DDevice_SetCopyRectsState(const ULONG *state, const ULONG *rop)
{
    if (state) memcpy(copyrect_state, state, sizeof copyrect_state);
    if (rop) memcpy(copyrect_rop, rop, sizeof copyrect_rop);
    static bool noted;
    if (state && (state[1] != 3 || state[0] || state[2] || state[7]) && !noted) {
        noted = true;
        xlog("D3D: SetCopyRectsState(format %u, operation %u, color key %u, clip %#x) is not emulated; "
             "CopyRects copies", state[0], state[1], state[2], state[7]);
    }
}
static void NTAPI D3DDevice_GetCopyRectsState(ULONG *state, ULONG *rop)
{
    if (state) memcpy(state, copyrect_state, sizeof copyrect_state);
    if (rop) { memcpy(rop, copyrect_rop, sizeof copyrect_rop); rop[7] = 0; /* ColorPattern: not kept */ }
}

/* The video encoder's border color (no border is drawn here). */
static ULONG overscan_color;
static void NTAPI D3DDevice_SetOverscanColor(ULONG c) { overscan_color = c; }
static ULONG NTAPI D3DDevice_GetOverscanColor(void) { return overscan_color; }

/* Closed captions go to the TV encoder; there is none.  GetCCStatus
   reports both fields free for more data. */
static void NTAPI D3DDevice_EnableCC(ULONG on) { (void)on; }
static void NTAPI D3DDevice_SendCC(ULONG field, ULONG cc1, ULONG cc2) { (void)field; (void)cc1; (void)cc2; }
static void NTAPI D3DDevice_GetCCStatus(ULONG *f1, ULONG *f2) { if (f1) *f1 = 1; if (f2) *f2 = 1; }

/* Resume(Reset) re-sends all device state after the title drove the push
   buffer itself; the state here never left the device. */
static void NTAPI D3DDevice_Resume(ULONG reset) { (void)reset; pusher_drain(); }
static void NTAPI D3DDevice_KickPushBuffer(void) { pusher_drain(); }
/* A fence: drawing that waits has passed it at once, queued drawing when
   the worker gets there (calls that wait for one drain the queue first). */
static ULONG NTAPI D3D_SetFence(ULONG Flags);
static ULONG NTAPI D3DDevice_InsertFence(void) { return D3D_SetFence(0); }
static BOOLEAN NTAPI D3DDevice_IsFencePending(ULONG f) { (void)f; return 0; }
static void NTAPI D3DDevice_BlockOnFence(ULONG f) { (void)f; }
/* The ramp the video DAC applied on the Xbox (the dashboard's screen saver
   dims the screen with it); Present applies it. */
static void gamma_identity(void)
{
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) d3d.gamma[c][i] = (unsigned char)i;
    d3d.gamma_set = false;
    d3d.gamma_dirty = true;
}

static void NTAPI D3DDevice_SetGammaRamp(ULONG flags, const unsigned char *ramp)
{
    (void)flags;
    memcpy(d3d.gamma, ramp, sizeof d3d.gamma);
    d3d.gamma_set = false;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++)
            if (d3d.gamma[c][i] != i) d3d.gamma_set = true;
    d3d.gamma_dirty = true;
}

static void NTAPI D3DDevice_GetGammaRamp(unsigned char *ramp) { memcpy(ramp, d3d.gamma, sizeof d3d.gamma); }
/* The video overlay: the NV2A scales a YUY2 surface onto the screen as it
   is scanned out, over the frame buffer or only where the frame buffer holds
   the color key.  It never touches the frame buffer, so present_overlay()
   composites into the window just for the swap and restore_overlay() puts
   the back buffer back afterwards. */
typedef struct { LONG left, top, right, bottom; } XRECT;
static struct {
    bool enabled;
    D3DSurface *surface;
    XRECT src, dst;
    bool use_key;
    ULONG key;
    uint8_t *saved;   /* the frame buffer under the overlay, bottom row first */
} overlay;

static void NTAPI D3DDevice_EnableOverlay(BOOLEAN on)
{
    overlay.enabled = on;
    if (!on) overlay.surface = NULL;
}

static void NTAPI D3DDevice_UpdateOverlay(D3DSurface *s, const XRECT *src, const XRECT *dst, BOOLEAN use_key, ULONG key)
{
    overlay.surface = s;
    if (!s) return;
    ULONG w, h, pitch;
    container_size((D3DPixelContainer *)s, &w, &h, &pitch);
    overlay.src = src ? *src : (XRECT){ 0, 0, (LONG)w, (LONG)h };
    overlay.dst = dst ? *dst : (XRECT){ 0, 0, d3d.width, d3d.height };
    overlay.use_key = use_key;
    overlay.key = key & 0xFFFFFF;
}

static BOOLEAN NTAPI D3DDevice_GetOverlayUpdateStatus(void) { return 1; }

static void present_overlay(void)
{
    if (!overlay.enabled || !overlay.surface) return;
    D3DSurface *s = overlay.surface;
    ULONG fmt = (s->Format >> 8) & 0xFF, sw, sh, pitch;
    if (fmt != 0x24 && fmt != 0x25) return;
    container_size((D3DPixelContainer *)s, &sw, &sh, &pitch);
    const uint8_t *yuv = (const uint8_t *)(s->Data | CONTIG_BASE);
    int w = d3d.width, h = d3d.height;
    XRECT r = overlay.src, d = overlay.dst;
    if (r.right <= r.left || r.bottom <= r.top || d.right <= d.left || d.bottom <= d.top) return;
    if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    overlay.saved = malloc(w * h * 4);
    backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, overlay.saved);
    backbuffer_read_end();
    uint8_t *px = malloc(w * h * 4);
    memcpy(px, overlay.saved, w * h * 4);
    bool uyvy = fmt == 0x25;
    for (int y = d.top < 0 ? 0 : d.top; y < d.bottom && y < h; y++) {
        ULONG sy = r.top + (ULONG)(y - d.top) * (r.bottom - r.top) / (d.bottom - d.top);
        if (sy >= sh) break;
        const uint8_t *row = yuv + sy * pitch;
        uint8_t *o = px + (h - 1 - y) * w * 4;
        for (int x = d.left < 0 ? 0 : d.left; x < d.right && x < w; x++) {
            uint8_t *p = o + x * 4;
            if (overlay.use_key && (ULONG)(p[0] | p[1] << 8 | p[2] << 16) != overlay.key) continue;
            ULONG sx = r.left + (ULONG)(x - d.left) * (r.right - r.left) / (d.right - d.left);
            if (sx >= sw) break;
            const uint8_t *q = row + (sx & ~1u) * 2;
            int Y = uyvy ? q[1 + (sx & 1) * 2] : q[(sx & 1) * 2];
            int U = (uyvy ? q[0] : q[1]) - 128, V = (uyvy ? q[2] : q[3]) - 128;
            int c = (Y - 16) * 298 + 128;
            int rgb[3] = { (c + 516 * U) >> 8, (c - 100 * U - 208 * V) >> 8, (c + 409 * V) >> 8 };   /* b, g, r */
            for (int j = 0; j < 3; j++) p[j] = rgb[j] < 0 ? 0 : rgb[j] > 255 ? 255 : rgb[j];
            p[3] = 0xFF;
        }
    }
    draw_window_pixels(px);
    free(px);
}

static void restore_overlay(void)
{
    if (!overlay.saved) return;
    draw_window_pixels(overlay.saved);
    free(overlay.saved);
    overlay.saved = NULL;
}
static void NTAPI D3DDevice_GetRasterStatus(ULONG *st) { st[0] = 1; st[1] = 0; }
static ULONG NTAPI D3DDevice_GetPushDistance(ULONG a) { (void)a; return 0; }
static ULONG NTAPI D3DPERF_Zero(void) { return 0; }
static ULONG NTAPI D3DPERF_Zero4(ULONG a) { (void)a; return 0; }
static ULONG NTAPI D3DPERF_Zero8(ULONG a, ULONG b) { return 0; }
static ULONG NTAPI D3DPERF_Zero12(ULONG a, ULONG b, ULONG c) { return 0; }
static ULONG NTAPI D3DPERF_Zero16(ULONG a, ULONG b, ULONG c, ULONG d) { return 0; }
static BOOLEAN NTAPI D3DResource_IsBusy(D3DResource *r) { (void)r; return 0; }
static void NTAPI D3DResource_BlockUntilNotBusy(D3DResource *r) { (void)r; }
static void NTAPI D3DResource_GetDevice(D3DResource *r, PVOID *dev) { (void)r; *dev = d3d.device; }
static void NTAPI D3DResource_MoveResourceMemory(D3DResource *r, ULONG where) {}
static LONG NTAPI D3DResource_SetPrivateData(D3DResource *r, const void *g, const void *d, ULONG s, ULONG f) { return D3D_OK; }
static LONG NTAPI D3DResource_GetPrivateData(D3DResource *r, const void *g, void *d, ULONG *s) { return D3DERR_INVALIDCALL; }
static void NTAPI D3DResource_FreePrivateData(D3DResource *r, const void *g) {}

/* ---- pixel shader objects --------------------------------------------- */

/* A handle is the address of a copy of the D3DPIXELSHADERDEF (60 dwords).
   Like the real library, SetPixelShader loads the first 57 into the title's
   render states (D3DRS_PSALPHAINPUTS0 ..) plus D3DRS_PSTEXTUREMODES; the
   last three dwords map D3D constant registers to combiner constants. */
#define PSDEF_DWORDS 60

static void NTAPI D3DDevice_CreatePixelShader(const ULONG *def, ULONG *handle)
{
    ULONG *copy = pool_alloc(PSDEF_DWORDS * 4);
    memcpy(copy, def, PSDEF_DWORDS * 4);
    *handle = (ULONG)copy;
    if (getenv("XBCOMPAT_PSH_CATALOG")) {
        /* Debug aid: log every definition a title creates (tests/psh_catalog.py). */
        char line[PSDEF_DWORDS * 9 + 1], *o = line;
        for (int i = 0; i < PSDEF_DWORDS; i++) o += sprintf(o, "%08lx ", (unsigned long)def[i]);
        xlog("D3D: psdef %#lx %s", (unsigned long)*handle, line);
    }
}

/* Load a D3DPIXELSHADERDEF into the render states.  `obj` is the title's
   { RefCount, D3DOwned, pPSDef } shader object for it, if it passed one.
   The 5849 CDevice points m_pPixelShader (device+0x784) at the current
   object, and LTCG titles inline SetPixelShaderConstant, which finds the
   constant mapping through it: without it Phantom Dust's menu frames,
   dialog boxes and button glyphs got no color constant and drew nothing. */
static void set_pixel_shader_def(const ULONG *def, ULONG *obj)
{
    TRACE_ALL("D3D: pixel shader %p", (void *)def);
    if (d3d.recording) { ULONG h = (ULONG)def; pb_record(OP_PIXEL_SHADER, &h, 4); }
    d3d.pixel_shader = (ULONG)def;
    if (device_layout_5849 && d3d.device) {
        static ULONG *user;   /* the device's m_UserPixelShader */
        if (def && !obj) {
            if (!user) user = pool_alloc(16);
            user[0] = 1; user[1] = 0; user[2] = (ULONG)def;
            obj = user;
        }
        d3d.device[0x784 / 4] = def ? (ULONG)obj : 0;
    }
    if (!def) return;
    if (getenv("XBCOMPAT_PSH_CATALOG")) {
        static ULONG seen[512]; static int nseen;
        int k = 0;
        while (k < nseen && seen[k] != (ULONG)def) k++;
        if (k == nseen && nseen < 512) {
            seen[nseen++] = (ULONG)def;
            char line[PSDEF_DWORDS * 9 + 1], *o = line;
            for (int i = 0; i < PSDEF_DWORDS; i++) o += sprintf(o, "%08lx ", (unsigned long)def[i]);
            xlog("D3D: psdef-used %#lx %s", (unsigned long)def, line);
        }
    }
    memcpy(d3d.render_state, def, D3DRS_PS_MAX * 4);
    RS(D3DRS_PSTEXTUREMODES) = def[54];
}

static void NTAPI D3DDevice_SetPixelShader(ULONG handle)
{
    const ULONG *def = (const ULONG *)handle;
    /* A title that builds its own shader objects (LTCG, no CreatePixelShader
       to replace) passes the library's { RefCount, D3DOwned, pPSDef }. */
    ULONG *obj = NULL;
    if (def && !pool_owns(def) && def[0] < 0x10000 && def[1] <= 1 && def[2] >= 0x10000) {
        obj = (ULONG *)def;
        def = (const ULONG *)def[2];
    }
    set_pixel_shader_def(def, obj);
}

/* The library points its own shader object at the title's definition; the
   definition is used in place, and NULL turns pixel shaders off.  It is a
   definition, never a shader object: one whose first combiner dwords look
   like { RefCount, D3DOwned, pointer } (Phantom Dust's attack effects) must
   not be taken for one. */
static void NTAPI D3DDevice_SetPixelShaderProgram(const ULONG *def)
{
    set_pixel_shader_def(def, NULL);
}

static void NTAPI D3DDevice_GetPixelShader(ULONG *handle) { *handle = d3d.pixel_shader; }
static void NTAPI D3DDevice_DeletePixelShader(ULONG handle)
{
    if (!handle) return;
    if (handle == d3d.pixel_shader) d3d.pixel_shader = 0;
    pool_free((void *)handle);
}
static void NTAPI D3DDevice_GetPixelShaderFunction(ULONG handle, ULONG *def)
{
    if (handle) memcpy(def, (const void *)handle, PSDEF_DWORDS * 4);
}

static ULONG float_color(const float *f)
{
    ULONG c = 0;
    for (int i = 0; i < 4; i++) {
        float v = f[i] < 0 ? 0 : f[i] > 1 ? 1 : f[i];
        c |= (ULONG)(v * 255.0f + 0.5f) << (i == 3 ? 24 : 16 - 8 * i);
    }
    return c;
}

/* A D3D constant register feeds every combiner constant whose mapping
   nibble (PSC0Mapping / PSC1Mapping / PSFinalCombinerConstants) names it. */
static void NTAPI D3DDevice_SetPixelShaderConstant(ULONG Register, const float *data, ULONG count)
{
    if (d3d.recording) { ULONG h[2] = { Register, count }; pb_record2(OP_PS_CONST, h, 8, data, count * 16); }
    const ULONG *def = (const ULONG *)d3d.pixel_shader;
    TRACE_ALL("D3D: SetPixelShaderConstant(%u, %g %g %g %g, %u) with shader %p", Register, data[0], data[1], data[2],
          data[3], count, (void *)def);
    for (ULONG i = 0; i < count && Register < 16; i++, Register++, data += 4) {
        memcpy(d3d.ps_const[Register], data, 16);
        if (!def) continue;
        ULONG c = float_color(data);
        for (int r = 0; r < 8; r++) {
            if (((def[57] >> (4 * r)) & 0xF) == Register) RS(D3DRS_PSCONSTANT0_0 + r) = c;
            if (((def[58] >> (4 * r)) & 0xF) == Register) RS(D3DRS_PSCONSTANT1_0 + r) = c;
        }
        for (int r = 0; r < 2; r++)
            if (((def[59] >> (4 * r)) & 0xF) == Register) RS(D3DRS_PSFINALCOMBINERCONSTANT0 + r) = c;
    }
}

static void NTAPI D3DDevice_GetPixelShaderConstant(ULONG Register, float *data, ULONG count)
{
    for (ULONG i = 0; i < count && Register + i < 16; i++) memcpy(data + i * 4, d3d.ps_const[Register + i], 16);
}


/* ---- immediate mode (Begin / SetVertexData / End) --------------------- */

/* The title sets vertex registers one at a time; writing the position
   register emits a vertex.  Register values persist between vertices. */
static struct {
    bool active;
    ULONG prim;
    float reg[16][4];
    bool used[16];
    float (*verts)[16][4];
    unsigned n, cap;
} imm;

/* The vertex registers persist across Begin/End; they start out as the
   hardware defaults. */
/* SetVertexData outside Begin/End sets the diffuse register for draws
   whose vertices carry none (the library's SetVertexShader resets it to
   white for such a shader, so it holds until the shader changes).
   Splinter Cell fades its loading and menu pictures with it: a
   BLENDDIFFUSEALPHA stage between the new picture and the old one, the
   alpha going from 1 to 0.  Always white, every fade stayed on the old
   picture (black for the first load, the menu over a level's briefing). */
static struct { bool on; ULONG vs; } persistent_diffuse;
static const float *vertex_diffuse(void)
{
    static const float white[4] = { 1, 1, 1, 1 };
    if (persistent_diffuse.on && d3d.vertex_shader != persistent_diffuse.vs) persistent_diffuse.on = false;
    return persistent_diffuse.on ? imm.reg[3] : white;
}

static void imm_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int r = 0; r < 16; r++) { imm.reg[r][0] = imm.reg[r][1] = imm.reg[r][2] = 0; imm.reg[r][3] = 1; }
    imm.reg[3][0] = imm.reg[3][1] = imm.reg[3][2] = 1;   /* diffuse defaults to white */
}

static void NTAPI D3DDevice_Begin(ULONG PrimitiveType)
{
    pusher_drain();
    imm_init();
    imm.active = true;
    imm.prim = PrimitiveType;
    imm.n = 0;
}

static void imm_set(LONG reg, float a, float b, float c, float d)
{
    imm_init();
    if (reg == -1) reg = 0;
    if (reg < 0 || reg > 15) return;
    imm.reg[reg][0] = a; imm.reg[reg][1] = b; imm.reg[reg][2] = c; imm.reg[reg][3] = d;
    imm.used[reg] = true;
    if (reg == 3 && !imm.active) {
        persistent_diffuse.on = true;
        persistent_diffuse.vs = d3d.vertex_shader;
        TRACE_ALL("D3D: persistent diffuse %g %g %g %g", a, b, c, d);
    }
    if (reg == 0 && imm.active) {
        if (imm.n == imm.cap) {
            imm.cap = imm.cap ? imm.cap * 2 : 256;
            imm.verts = realloc(imm.verts, imm.cap * sizeof(*imm.verts));
        }
        memcpy(imm.verts[imm.n++], imm.reg, sizeof(imm.reg));
    }
}

static void NTAPI D3DDevice_SetVertexData2f(LONG r, float a, float b) { imm_set(r, a, b, 0, 1); }
static void NTAPI D3DDevice_SetVertexData4f(LONG r, float a, float b, float c, float d) { imm_set(r, a, b, c, d); }
static void NTAPI D3DDevice_SetVertexData2s(LONG r, SHORT a, SHORT b) { imm_set(r, a, b, 0, 1); }
static void NTAPI D3DDevice_SetVertexData4s(LONG r, SHORT a, SHORT b, SHORT c, SHORT d) { imm_set(r, a, b, c, d); }
static void NTAPI D3DDevice_SetVertexData4ub(LONG r, ULONG a, ULONG b, ULONG c, ULONG d)
{
    imm_set(r, (a & 255) / 255.0f, (b & 255) / 255.0f, (c & 255) / 255.0f, (d & 255) / 255.0f);
}
static void NTAPI D3DDevice_SetVertexDataColor(LONG r, ULONG color)
{
    float c[4];
    color4(c, color);
    imm_set(r, c[0], c[1], c[2], c[3]);
}

/* Draw the vertices collected in `imm` as primitive `prim`. */
static void imm_flush(ULONG prim)
{
    if (!imm.n) return;
    gl_require();
    hud_batch_flush();   /* batched draws made before this one land first */
    vshader *sh = (d3d.vertex_shader & 1) ? (vshader *)(d3d.vertex_shader & ~1u) : NULL;
    if (sh && sh->code) {
        /* Feed the vertex registers to the program as 16 float4 attributes.
           Compile on the real shader first so the copy reuses its object. */
        vertex_shader_object(sh);
        vshader tmp = *sh;
        for (int r = 0; r < 16; r++) {
            if (r == 0 || imm.used[r]) tmp.attr[r] = (vattr){ 0, r * 16, 0x42, 4, GL_FLOAT, false, 16 };
            else tmp.attr[r].stream = -1;
        }
        draw_programmable(&tmp, prim, (const UCHAR *)imm.verts, sizeof(*imm.verts), 0, imm.n, NULL);
        return;
    }
    bool pretransformed = sh ? sh->attr[0].components == 4 && sh->attr[0].gl_type == GL_FLOAT
                             : (d3d.vertex_shader & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    gc_disable_attrib_arrays();
    apply_render_states(pretransformed, imm.used[2]);
    ff_primary_present = imm.used[3];
    ff_secondary_present = imm.used[4];
    bool lit = glIsEnabled(GL_LIGHTING);
    if (lit && imm.used[3] && RS(D3DRS_COLORVERTEX)) {
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
    } else {
        glDisable(GL_COLOR_MATERIAL);
    }
    unsigned units = d3d.pixel_shader ? 0xF : apply_textures();
    if (!sh) { static const int two[4] = { 2, 2, 2, 2 }; apply_texture_transforms(units, two); }
    program_entry *e;
    if (!use_program(0, &e)) return;
    TRACE_ALL("D3D: immediate draw prim %u, %u vertices, vs %#x%s, colour %s, first at %g %g %g %g colour %g %g %g %g",
              prim, imm.n, d3d.vertex_shader, pretransformed ? " (screen space)" : "",
              imm.used[3] ? "per vertex" : "none", imm.verts[0][0][0], imm.verts[0][0][1], imm.verts[0][0][2],
              imm.verts[0][0][3], imm.verts[0][3][0], imm.verts[0][3][1], imm.verts[0][3][2], imm.verts[0][3][3]);
    shader_diagnostics_mark(d3d.frame, ++shader_diag_draw, d3d.vertex_shader, d3d.pixel_shader, cur_program, "immediate");
    for (unsigned pass = 0; scissor_pass(pass); pass++) {
        glBegin(gl_primitive(prim));
        for (unsigned i = 0; i < imm.n; i++) {
            float (*v)[4] = imm.verts[i];
            for (int u = 0; u < 4; u++) {
                if (!((units >> u) & 1)) continue;
                ULONG tci = TSS(u, D3DTSS_TEXCOORDINDEX) & 0xFFFF;
                p_glMultiTexCoord4fv(GL_TEXTURE0 + u, v[9 + (tci < 4 ? tci : 0)]);
            }
            if (imm.used[3]) glColor4fv(v[3]);
            if (imm.used[2]) glNormal3fv(v[2]);
            if (pretransformed) glVertex3f(v[0][0], v[0][1], v[0][2]);
            else glVertex4fv(v[0]);
        }
        glEnd();
    }
    /* A draw like any other (Splinter Cell's loading bar is one of these;
       uncounted, its frames read as frames with nothing drawn). */
    debug_dump_draw();
    end_program(e);
}

static void NTAPI D3DDevice_End(void)
{
    pusher_drain();
    imm.active = false;
    if (!imm.n) return;
    if (d3d.recording) {
        ULONG h[3] = { imm.prim, imm.n, 0 };
        for (int r = 0; r < 16; r++) if (imm.used[r]) h[2] |= 1u << r;
        pb_record2(OP_BEGIN_END, h, sizeof(h), imm.verts, imm.n * sizeof(*imm.verts));
        return;
    }
    imm_flush(imm.prim);
}


/* ---- push buffers ------------------------------------------------------ */

/* A recorded push buffer holds a stream of our own ops: the state changes
   and draws made between BeginPushBuffer and EndPushBuffer, each as
   [op | dwords << 8, payload...].  Render and texture states the title
   writes inline are picked up as differences against a snapshot whenever
   something else is recorded.  Offsets (from GetPushBufferOffset, used by
   the fixup functions) are byte offsets into that stream, so a fixup can
   replace the payload of the op recorded at that offset.  Running a push
   buffer replays the ops through the ordinary entry points. */

typedef struct D3DPushBuffer {
    DWORD Common, Data, Lock, Size, AllocationSize;
} D3DPushBuffer;

/* The recorded stream of a push buffer, kept beside it: an LTCG build's
   inlined CreatePushBuffer makes headers of the Xbox size itself. */
typedef struct pb_stream {
    const D3DPushBuffer *pb;
    ULONG *ops;              /* the recorded ops */
    unsigned n, cap;         /* dwords used / allocated */
    struct pb_stream *next;
} pb_stream;
static pb_stream *pb_streams;

static pb_stream *pb_stream_of(const D3DPushBuffer *pb, bool create)
{
    for (pb_stream *s = pb_streams; s; s = s->next)
        if (s->pb == pb) return s;
    if (!create || !pb) return NULL;
    pb_stream *s = calloc(1, sizeof(*s));
    s->pb = pb;
    s->next = pb_streams;
    pb_streams = s;
    return s;
}

typedef struct D3DFixup {
    DWORD Common, Data, Lock, Run, Next, Size;
} D3DFixup;

#define D3DERR_BUFFERTOOSMALL ((LONG)0x88760829)

/* Recording changes only our shadow state; this is put back at the end. */
static struct {
    ULONG rs[D3DRS_MAX], tss[4 * 32];
    D3DResource *textures[4];
    ULONG vertex_shader, pixel_shader;
    typeof(d3d.streams) streams;
    D3DResource *indices;
    ULONG base_vertex_index;
    typeof(d3d.transforms) transforms;
    D3DVIEWPORT8 viewport;
    typeof(d3d.lights) lights;
    float material[4][4], material_power;
    float vs_const[192][4], ps_const[16][4], eye_vector[4];
    D3DPalette *palettes[4];
    D3DSurface *target, *target_depth;
    typeof(d3d.scissors) scissors;
    typeof(d3d.direct_mv) direct_mv;
} pb_saved;
static ULONG pb_rs[D3DRS_MAX], pb_tss[4 * 32];   /* the state last written to the stream */

static void pushbuffer_free(D3DPushBuffer *pb)
{
    if (d3d.recording == pb) d3d.recording = NULL;
    for (pb_stream **l = &pb_streams; *l; l = &(*l)->next) {
        if ((*l)->pb != pb) continue;
        pb_stream *s = *l;
        *l = s->next;
        free(s->ops);
        free(s);
        break;
    }
    if (pb->Data) MmFreeContiguousMemory((PVOID)pb->Data);
    if (pool_owns(pb)) pool_free(pb);   /* a header the title allocated stays its own */
}

static ULONG *pb_emit(unsigned op, unsigned payload_dwords)
{
    pb_stream *pb = pb_stream_of(d3d.recording, true);
    unsigned need = pb->n + 1 + payload_dwords;
    if (need > pb->cap) {
        while (pb->cap < need) pb->cap = pb->cap ? pb->cap * 2 : 1024;
        pb->ops = realloc(pb->ops, pb->cap * 4);
    }
    ULONG *p = pb->ops + pb->n;
    p[0] = op | ((1 + payload_dwords) << 8);
    pb->n = need;
    return p + 1;
}

static void pb_flush_state(void)
{
    for (unsigned i = 0; i < D3DRS_MAX; i++)
        if (RS(i) != pb_rs[i]) {
            ULONG *p = pb_emit(OP_RS, 2);
            p[0] = i;
            p[1] = pb_rs[i] = RS(i);
        }
    for (unsigned i = 0; i < 4 * 32; i++)
        if (d3d.texture_state[i] != pb_tss[i]) {
            ULONG *p = pb_emit(OP_TSS, 2);
            p[0] = i;
            p[1] = pb_tss[i] = d3d.texture_state[i];
        }
}

static void pb_record2(unsigned op, const void *head, unsigned head_bytes, const void *data, unsigned data_bytes)
{
    pb_flush_state();
    ULONG *p = pb_emit(op, (head_bytes + data_bytes + 3) / 4);
    memcpy(p, head, head_bytes);
    if (data_bytes) memcpy((char *)p + head_bytes, data, data_bytes);
}

static void pb_record(unsigned op, const void *payload, unsigned bytes)
{
    pb_record2(op, payload, bytes, NULL, 0);
}

static LONG NTAPI D3DDevice_CreatePushBuffer(UINT_ Size, ULONG RunUsingCpuCopy, D3DPushBuffer **pp)
{
    (void)RunUsingCpuCopy;
    D3DPushBuffer *pb = pool_alloc(sizeof(*pb));
    memset(pb, 0, sizeof(*pb));
    pb->Common = 1 | D3DCOMMON_TYPE_PUSHBUFFER | D3DCOMMON_D3DCREATED;
    pb->Data = (ULONG)MmAllocateContiguousMemoryEx(Size ? Size : 4, 0, 0x7FFFFFFF, 4, 4);
    pb->AllocationSize = Size;
    *pp = pb;
    return D3D_OK;
}

static void NTAPI D3DDevice_BeginPushBuffer(D3DPushBuffer *pb)
{
    pusher_drain();
    if (d3d.recording) xlog("D3D: BeginPushBuffer while already recording");
    d3d.recording = pb;
    pb_stream_of(pb, true)->n = 0;
    for (int i = 0; i < D3DRS_MAX; i++) pb_rs[i] = RS(i);
    memcpy(pb_tss, d3d.texture_state, sizeof(pb_tss));
    for (int i = 0; i < D3DRS_MAX; i++) pb_saved.rs[i] = RS(i);
    memcpy(pb_saved.tss, d3d.texture_state, sizeof(pb_saved.tss));
    memcpy(pb_saved.textures, d3d.textures, sizeof(pb_saved.textures));
    pb_saved.vertex_shader = d3d.vertex_shader;
    pb_saved.pixel_shader = d3d.pixel_shader;
    memcpy(&pb_saved.streams, &d3d.streams, sizeof(pb_saved.streams));
    pb_saved.indices = d3d.indices;
    pb_saved.base_vertex_index = d3d.base_vertex_index;
    memcpy(&pb_saved.transforms, &d3d.transforms, sizeof(pb_saved.transforms));
    pb_saved.viewport = d3d.viewport;
    memcpy(&pb_saved.lights, &d3d.lights, sizeof(pb_saved.lights));
    memcpy(pb_saved.material, d3d.material, sizeof(pb_saved.material));
    pb_saved.material_power = d3d.material_power;
    memcpy(pb_saved.vs_const, d3d.vs_const, sizeof(pb_saved.vs_const));
    memcpy(pb_saved.ps_const, d3d.ps_const, sizeof(pb_saved.ps_const));
    memcpy(pb_saved.eye_vector, d3d.eye_vector, sizeof(pb_saved.eye_vector));
    memcpy(pb_saved.palettes, d3d.palettes, sizeof(pb_saved.palettes));
    pb_saved.target = d3d.target;
    pb_saved.target_depth = d3d.target_depth;
    pb_saved.scissors = d3d.scissors;
    pb_saved.direct_mv = d3d.direct_mv;
}

static LONG NTAPI D3DDevice_EndPushBuffer(void)
{
    gl_require();
    D3DPushBuffer *pb = d3d.recording;
    if (!pb) return D3DERR_INVALIDCALL;
    pb_flush_state();
    pb->Size = pb_stream_of(pb, true)->n * 4 + 4;
    d3d.recording = NULL;
    for (int i = 0; i < D3DRS_MAX; i++) RS(i) = pb_saved.rs[i];
    memcpy(d3d.texture_state, pb_saved.tss, sizeof(pb_saved.tss));
    memcpy(d3d.textures, pb_saved.textures, sizeof(pb_saved.textures));
    d3d.vertex_shader = pb_saved.vertex_shader;
    d3d.pixel_shader = pb_saved.pixel_shader;
    memcpy(&d3d.streams, &pb_saved.streams, sizeof(pb_saved.streams));
    d3d.indices = pb_saved.indices;
    d3d.base_vertex_index = pb_saved.base_vertex_index;
    memcpy(&d3d.transforms, &pb_saved.transforms, sizeof(pb_saved.transforms));
    d3d.viewport = pb_saved.viewport;
    memcpy(&d3d.lights, &pb_saved.lights, sizeof(pb_saved.lights));
    memcpy(d3d.material, pb_saved.material, sizeof(pb_saved.material));
    d3d.material_power = pb_saved.material_power;
    memcpy(d3d.vs_const, pb_saved.vs_const, sizeof(pb_saved.vs_const));
    vsc_touch(0, 192);
    memcpy(d3d.ps_const, pb_saved.ps_const, sizeof(pb_saved.ps_const));
    memcpy(d3d.eye_vector, pb_saved.eye_vector, sizeof(pb_saved.eye_vector));
    memcpy(d3d.palettes, pb_saved.palettes, sizeof(pb_saved.palettes));
    if (d3d.target != pb_saved.target || d3d.target_depth != pb_saved.target_depth)
        D3DDevice_SetRenderTarget(pb_saved.target, pb_saved.target_depth);
    d3d.scissors = pb_saved.scissors;   /* after SetRenderTarget, which resets them */
    d3d.direct_mv = pb_saved.direct_mv;
    TRACE_ALL("D3D: recorded push buffer %p: %u dwords", (void *)pb, pb_stream_of(pb, true)->n);
    return D3D_OK;
}

static void NTAPI D3DDevice_GetPushBufferOffset(ULONG *off)
{
    if (!d3d.recording) { *off = 0; return; }
    pb_flush_state();
    *off = pb_stream_of(d3d.recording, true)->n * 4;
}

/* ---- fixups ---- */

static D3DFixup *g_fixup;              /* BeginFixup/EndFixup bracket target */
static D3DPushBuffer *g_fixup_pb;

static LONG NTAPI D3DDevice_CreateFixup(UINT_ Size, D3DFixup **pp)
{
    D3DFixup *fx = pool_alloc(sizeof(*fx) + Size);
    memset(fx, 0, sizeof(*fx));
    fx->Common = 1 | D3DCOMMON_TYPE_FIXUP | D3DCOMMON_D3DCREATED;
    fx->Data = (ULONG)(fx + 1);
    fx->Size = Size;
    *pp = fx;
    return D3D_OK;
}

static void NTAPI D3DFixup_Reset(D3DFixup *fx) { fx->Next = fx->Run = 0; }
static void NTAPI D3DFixup_GetSize(D3DFixup *fx, ULONG *size) { *size = fx->Next - fx->Run; }
static void NTAPI D3DFixup_GetSpace(D3DFixup *fx, ULONG *space) { *space = fx->Size > fx->Next ? fx->Size - fx->Next : 0; }

static void NTAPI D3DPushBuffer_BeginFixup(D3DPushBuffer *pb, D3DFixup *fx, ULONG NoWait)
{
    (void)NoWait;
    g_fixup = fx;
    g_fixup_pb = pb;
    if (fx) fx->Run = fx->Next;
}

static LONG NTAPI D3DPushBuffer_EndFixup(D3DPushBuffer *pb)
{
    (void)pb;
    D3DFixup *fx = g_fixup;
    g_fixup = NULL;
    if (!fx) return D3D_OK;
    ULONG at = fx->Next;
    fx->Next += 4;
    if (fx->Next > fx->Size) return D3DERR_BUFFERTOOSMALL;
    *(ULONG *)(fx->Data + at) = 0xFFFFFFFF;
    return D3D_OK;
}

/* Render and texture stage states the title set between GetPushBufferOffset
   and the call it means to fix up are recorded ahead of that call: the
   offset of the op of kind `op` at or just past those, as recorded. */
static ULONG pb_op_offset(D3DPushBuffer *b, ULONG offset, unsigned op)
{
    pb_stream *pb = pb_stream_of(b, false);
    unsigned i = offset / 4;
    while (pb && pb->ops && i < pb->n && (pb->ops[i] & 0xFF) != op
           && ((pb->ops[i] & 0xFF) == OP_RS || (pb->ops[i] & 0xFF) == OP_TSS))
        i += pb->ops[i] >> 8;
    return i * 4;
}

/* The op recorded at `offset`, or NULL (with one complaint) when there is
   no op of that kind there. */
static ULONG *pb_op_at(D3DPushBuffer *b, ULONG offset, unsigned op)
{
    pb_stream *pb = pb_stream_of(b, false);
    offset = pb_op_offset(b, offset, op);
    if (pb && pb->ops && offset / 4 < pb->n && (pb->ops[offset / 4] & 0xFF) == op) return pb->ops + offset / 4 + 1;
    static bool warned;
    if (!warned) {
        xlog("D3D: push buffer fixup at offset %u does not match the recorded op (kind %u)", offset, op);
        warned = true;
    }
    return NULL;
}

/* Room for a fixup payload: in the fixup object of the current bracket
   (as [size, offset, kind, payload...], terminated by EndFixup), or in the
   op itself when BeginFixup was given no fixup object. */
static ULONG *fixup_payload(D3DPushBuffer *pb, ULONG offset, unsigned op, unsigned bytes)
{
    pb_op_at(pb, offset, op);
    if (g_fixup) {
        D3DFixup *fx = g_fixup;
        ULONG size = 4 + bytes, at = fx->Next;
        fx->Next += 8 + size;
        if (fx->Next > fx->Size) return NULL;
        ULONG *e = (ULONG *)(fx->Data + at);
        e[0] = size;
        e[1] = pb_op_offset(pb, offset, op);
        e[2] = op;
        return e + 3;
    }
    ULONG *p = pb_op_at(pb, offset, op);
    if (p && ((p[-1] >> 8) - 1) * 4 < bytes) return NULL;
    return p;
}

static const ULONG *fixup_find(const D3DFixup *fx, ULONG offset, unsigned op)
{
    if (!fx) return NULL;
    ULONG end = fx->Next < fx->Size ? fx->Next : fx->Size;
    const ULONG *e = (const ULONG *)(fx->Data + fx->Run), *stop = (const ULONG *)(fx->Data + end);
    while (e + 3 <= stop && e[0] != 0xFFFFFFFF) {
        if (e[1] == offset && e[2] == op) return e + 3;
        e += 2 + e[0] / 4;
    }
    return NULL;
}

static void NTAPI D3DPushBuffer_SetVertexShaderConstant(D3DPushBuffer *pb, ULONG Offset, LONG Register,
                                                        const float *data, ULONG count)
{
    const ULONG *rec = pb_op_at(pb, Offset, OP_VS_CONST);
    ULONG *p = fixup_payload(pb, Offset, OP_VS_CONST, 8 + count * 16);
    if (!p) return;
    p[0] = rec ? rec[0] : (ULONG)Register;
    p[1] = count;
    memcpy(p + 2, data, count * 16);
}

static void NTAPI D3DPushBuffer_SetTexture(D3DPushBuffer *pb, ULONG Offset, ULONG Stage, D3DResource *t)
{
    ULONG *p = fixup_payload(pb, Offset, OP_TEXTURE, 8);
    if (p) { p[0] = Stage; p[1] = (ULONG)t; }
}

static void NTAPI D3DPushBuffer_SetPalette(D3DPushBuffer *pb, ULONG Offset, ULONG Stage, D3DPalette *pal)
{
    ULONG *p = fixup_payload(pb, Offset, OP_PALETTE, 8);
    if (p) { p[0] = Stage; p[1] = (ULONG)pal; }
}

static void NTAPI D3DPushBuffer_SetRenderTarget(D3DPushBuffer *pb, ULONG Offset, D3DSurface *rt, D3DSurface *z)
{
    ULONG *p = fixup_payload(pb, Offset, OP_RENDER_TARGET, 8);
    if (p) { p[0] = (ULONG)rt; p[1] = (ULONG)z; }
}

static void NTAPI D3DPushBuffer_SetVertexShaderInput(D3DPushBuffer *pb, ULONG Offset, ULONG Handle, UINT_ count,
                                                     const ULONG *inputs)
{
    ULONG *p = fixup_payload(pb, Offset, OP_VS_INPUT, 8 + count * 12);
    if (p) { p[0] = Handle; p[1] = count; memcpy(p + 2, inputs, count * 12); }
}

static void NTAPI D3DPushBuffer_SetVertexShaderInputDirect(D3DPushBuffer *pb, ULONG Offset, const ULONG *vaf,
                                                           UINT_ count, const ULONG *inputs)
{
    D3DPushBuffer_SetVertexShaderInput(pb, Offset, vaf ? direct_layout_handle(vaf) : 0, count, inputs);
}

static void NTAPI D3DPushBuffer_RunPushBuffer(D3DPushBuffer *pb, ULONG Offset, D3DPushBuffer *dest, D3DFixup *fx)
{
    ULONG *p = fixup_payload(pb, Offset, OP_RUN, 8);
    if (p) { p[0] = (ULONG)dest; p[1] = (ULONG)fx; }
}

/* Offset holds a recorded Nop (or an earlier jump): running the buffer then
   goes on at DestinationOffset of the same buffer, or past the Nop when the
   two offsets are equal. */
static void NTAPI D3DPushBuffer_Jump(D3DPushBuffer *pb, ULONG Offset, ULONG DestinationOffset)
{
    ULONG *p = fixup_payload(pb, Offset, OP_NOP, 8);
    if (p) { p[0] = Offset != DestinationOffset; p[1] = DestinationOffset; }
}

static void NTAPI D3DPushBuffer_SetModelView(D3DPushBuffer *pb, ULONG Offset, const D3DMATRIX *mv,
                                             const D3DMATRIX *inv, const D3DMATRIX *composite)
{
    ULONG *p = fixup_payload(pb, Offset, OP_MODELVIEW, 4 + 3 * sizeof(D3DMATRIX));
    if (!p) return;
    p[0] = (mv && composite) | (inv != NULL) << 1;
    if (mv) memcpy(p + 1, mv, sizeof(D3DMATRIX));
    if (inv) memcpy(p + 17, inv, sizeof(D3DMATRIX));
    if (composite) memcpy(p + 33, composite, sizeof(D3DMATRIX));
}

static void NTAPI D3DPushBuffer_SetVertexBlendModelView(D3DPushBuffer *pb, ULONG Offset, UINT_ count,
                                                        const D3DMATRIX *mv, const D3DMATRIX *inv,
                                                        const D3DMATRIX *proj_vp)
{
    ULONG *p = fixup_payload(pb, Offset, OP_BLEND_MODELVIEW, 8 + 9 * sizeof(D3DMATRIX));
    if (!p) return;
    if (count > 4) count = 4;
    p[0] = mv ? count : 0;
    p[1] = (mv != NULL) | (inv != NULL) << 1 | (proj_vp != NULL) << 2;
    if (mv) memcpy(p + 2, mv, count * sizeof(D3DMATRIX));
    if (inv) memcpy(p + 2 + 64, inv, count * sizeof(D3DMATRIX));
    if (proj_vp) memcpy(p + 2 + 128, proj_vp, sizeof(D3DMATRIX));
}

static LONG NTAPI D3DPushBuffer_EndVisibilityTest(D3DPushBuffer *pb, ULONG Offset, ULONG Index)
{
    ULONG *p = fixup_payload(pb, Offset, OP_VIS_END, 4);
    if (p) p[0] = Index;
    return D3D_OK;
}

static void NTAPI D3DPushBuffer_Verify(D3DPushBuffer *pb, ULONG StampResources) { (void)pb; (void)StampResources; }

/* ---- running ---- */

static void pb_interpret(const ULONG *p, unsigned n);
static void pb_run(D3DPushBuffer *b, D3DFixup *fx, int depth);

static void NTAPI D3DDevice_BeginVisibilityTest(void);
static LONG NTAPI D3DDevice_EndVisibilityTest(ULONG index);
static void NTAPI D3DDevice_SetModelView(const D3DMATRIX *mv, const D3DMATRIX *inv, const D3DMATRIX *composite);
static void NTAPI D3DDevice_SetVertexBlendModelView(UINT_ count, const D3DMATRIX *mv, const D3DMATRIX *inv,
                                                    const D3DMATRIX *proj_vp);
static void NTAPI D3DDevice_SetScissors(ULONG Count, BOOLEAN Exclusive, const D3DRECT *rects);

/* Whether `i` (in dwords) starts a recorded op: a jump may only go there. */
static bool pb_op_boundary(const pb_stream *pb, unsigned i)
{
    unsigned k = 0;
    while (k < i && k < pb->n) k += pb->ops[k] >> 8;
    return k == i;
}

static void pb_run_body(D3DPushBuffer *b, D3DFixup *fx, int depth)
{
    pb_stream *pb = pb_stream_of(b, false);
    if (!pb || !pb->ops || depth > 8) return;
    unsigned jumps = 0;
    for (unsigned i = 0; i < pb->n;) {
        const ULONG *p = pb->ops + i;
        unsigned op = p[0] & 0xFF, len = p[0] >> 8;
        const ULONG *a = p + 1;
        const ULONG *f = fixup_find(fx, i * 4, op);
        if (f) a = f;
        switch (op) {
        case OP_RS: if (a[0] < D3DRS_MAX) RS(a[0]) = a[1]; break;
        case OP_TSS: if (a[0] < 4 * 32) d3d.texture_state[a[0]] = a[1]; break;
        case OP_VS_CONST: D3DDevice_SetVertexShaderConstant((LONG)a[0], (const float *)(a + 2), a[1]); break;
        case OP_VERTEX_SHADER: D3DDevice_SetVertexShader(a[0]); break;
        case OP_TEXTURE: D3DDevice_SetTexture(a[0], (D3DResource *)a[1]); break;
        case OP_STREAM: D3DDevice_SetStreamSource(a[0], (D3DResource *)a[1], a[2]); break;
        case OP_INDICES: D3DDevice_SetIndices((D3DResource *)a[0], a[1]); break;
        case OP_TRANSFORM: D3DDevice_SetTransform(a[0], (const D3DMATRIX *)(a + 1)); break;
        case OP_VIEWPORT: D3DDevice_SetViewport(a[0] ? (const D3DVIEWPORT8 *)(a + 1) : NULL); break;
        case OP_DRAW: D3DDevice_DrawVertices(a[0], a[1], a[2]); break;
        case OP_DRAW_INDEXED: D3DDevice_DrawIndexedVertices(a[0], a[1], (const USHORT *)(a + 2)); break;
        case OP_DRAW_UP: D3DDevice_DrawVerticesUP(a[0], a[1], a + 3, a[2]); break;
        case OP_DRAW_INDEXED_UP: {
            ULONG count = a[1], stride = a[2], ib = (count * 2 + 3) & ~3u;
            D3DDevice_DrawIndexedVerticesUP(a[0], count, (const USHORT *)(a + 4), (const char *)(a + 4) + ib, stride);
            break;
        }
        case OP_BEGIN_END: {
            unsigned nv = a[1];
            if (nv > imm.cap) {
                imm.cap = nv;
                imm.verts = realloc(imm.verts, imm.cap * sizeof(*imm.verts));
            }
            memcpy(imm.verts, a + 3, nv * sizeof(*imm.verts));
            imm.n = nv;
            for (int r = 0; r < 16; r++) imm.used[r] = (a[2] >> r) & 1;
            imm_flush(a[0]);
            break;
        }
        case OP_PIXEL_SHADER: set_pixel_shader_def((const ULONG *)a[0], NULL); break;
        case OP_PS_CONST: D3DDevice_SetPixelShaderConstant(a[0], (const float *)(a + 2), a[1]); break;
        case OP_VS_INPUT: D3DDevice_SetVertexShaderInput(a[0], a[1], a + 2); break;
        case OP_RENDER_TARGET: D3DDevice_SetRenderTarget((D3DSurface *)a[0], (D3DSurface *)a[1]); break;
        case OP_PALETTE: D3DDevice_SetPalette(a[0], (D3DPalette *)a[1]); break;
        case OP_RUN: pb_run((D3DPushBuffer *)a[0], (D3DFixup *)a[1], depth + 1); break;
        case OP_CLEAR: {
            float z;
            memcpy(&z, &a[3], 4);
            D3DDevice_Clear(a[0], a[0] ? (const D3DRECT *)(a + 5) : NULL, a[1], a[2], z, a[4]);
            break;
        }
        case OP_LIGHT: D3DDevice_SetLight(a[0], (const D3DLIGHT8 *)(a + 1)); break;
        case OP_LIGHT_ENABLE: D3DDevice_LightEnable(a[0], a[1]); break;
        case OP_MATERIAL: D3DDevice_SetMaterial((const float *)a); break;
        case OP_RAW: pb_interpret(a + 1, a[0]); break;
        case OP_NOP:
            if (!a[0]) break;
            /* A jump: a title loops only as long as some other agent (a DPC,
               a fixup between runs) changes it; give up on one that won't end. */
            if (a[1] / 4 > pb->n || !pb_op_boundary(pb, a[1] / 4) || ++jumps > 1u << 16) {
                xlog("D3D: push buffer jump from %u to %u abandoned", i * 4, a[1]);
                return;
            }
            i = a[1] / 4;
            continue;
        case OP_VIS_BEGIN: D3DDevice_BeginVisibilityTest(); break;
        case OP_VIS_END: D3DDevice_EndVisibilityTest(a[0]); break;
        case OP_MODELVIEW:
            D3DDevice_SetModelView(a[0] & 1 ? (const D3DMATRIX *)(a + 1) : NULL,
                                   a[0] & 2 ? (const D3DMATRIX *)(a + 17) : NULL,
                                   a[0] & 1 ? (const D3DMATRIX *)(a + 33) : NULL);
            break;
        case OP_BLEND_MODELVIEW:
            D3DDevice_SetVertexBlendModelView(a[0], a[1] & 1 ? (const D3DMATRIX *)(a + 2) : NULL,
                                              a[1] & 2 ? (const D3DMATRIX *)(a + 2 + 64) : NULL,
                                              a[1] & 4 ? (const D3DMATRIX *)(a + 2 + 128) : NULL);
            break;
        case OP_SCISSORS: D3DDevice_SetScissors(a[0], (BOOLEAN)a[1], a[0] ? (const D3DRECT *)(a + 2) : NULL); break;
        default: xlog("D3D: bad push buffer op %u", op); return;
        }
        i += len;
    }
}

static void pb_run(D3DPushBuffer *b, D3DFixup *fx, int depth)
{
    hud_batch_flush();
    bool previous = hud_raw_active;
    hud_raw_active = true;
    pb_run_body(b, fx, depth);
    hud_raw_active = previous;
}

static void NTAPI D3DDevice_RunPushBuffer(D3DPushBuffer *pb, D3DFixup *fx)
{
    gl_require();
    pusher_drain();
    if (d3d.recording) {
        ULONG a[2] = { (ULONG)pb, (ULONG)fx };
        pb_record(OP_RUN, a, sizeof(a));
        return;
    }
    pb_run(pb, fx, 0);
}

/* ---- BeginPush / EndPush: raw NV2A methods ---- */

/* The title writes [D3DPUSH_ENCODE(method, count), data...] runs into a
   scratch buffer; EndPush hands it to a small interpreter for the methods
   titles use this way (inline vertex data, draws, constants). */
static ULONG *push_scratch;
static unsigned push_scratch_cap;

static void NTAPI D3DDevice_BeginPush(ULONG Count, ULONG **pp)
{
    pusher_drain();
    if (Count + 1 > push_scratch_cap) {
        push_scratch_cap = Count + 1;
        push_scratch = realloc(push_scratch, push_scratch_cap * 4);
    }
    *pp = push_scratch;
}

/* Stride and layout of inline-array vertices for the current vertex
   shader: the enabled attributes packed in register order. */
static vshader inline_layout(const vshader *sh, ULONG *stride)
{
    vshader tmp = *sh;
    ULONG off = 0;
    for (int r = 0; r < 16; r++) {
        if (tmp.attr[r].stream < 0) continue;
        tmp.attr[r].stream = 0;
        tmp.attr[r].offset = off;
        off += tmp.attr[r].bytes;
    }
    *stride = off;
    return tmp;
}

static void pb_interpret_body(const ULONG *p, unsigned n)
{
    static uint8_t *inline_buf;
    static unsigned inline_n, inline_cap;
    static USHORT *elems;
    static unsigned elems_n, elems_cap;
    static float partial[16][4];
    static ULONG const_load;
    static ULONG unknown[32];
    static unsigned nunknown;
    ULONG prim = 0;
    inline_n = elems_n = 0;

    for (unsigned i = 0; i < n;) {
        ULONG hdr = p[i++];
        if ((hdr & 3) != 0 || hdr == 0x20000 || (hdr & 0xE0000000) == 0x20000000) {   /* jump / call / return */
            xlog("D3D: push buffer control word %#x is not supported", hdr);
            return;
        }
        unsigned method = hdr & 0x1FFC, count = (hdr >> 18) & 0x7FF;
        bool noinc = hdr & 0x40000000;
        if (i + count > n) return;
        const ULONG *v = p + i;
        i += count;

        if (method == 0x1818) {   /* NV097_INLINE_ARRAY */
            if (inline_n + count * 4 > inline_cap) {
                inline_cap = (inline_n + count * 4) * 2;
                inline_buf = realloc(inline_buf, inline_cap);
            }
            memcpy(inline_buf + inline_n, v, count * 4);
            inline_n += count * 4;
            continue;
        }
        for (unsigned k = 0; k < count; k++) {
            unsigned m = noinc ? method : method + 4 * k;
            ULONG d = v[k];
            float f;
            memcpy(&f, &d, 4);
            if (m == 0x17FC) {   /* NV097_SET_BEGIN_END */
                if (d) {
                    prim = d;
                    imm_init();
                    imm.active = true;
                    imm.prim = d;
                    imm.n = 0;
                    inline_n = elems_n = 0;
                } else {
                    imm.active = false;
                    if (inline_n) {
                        ULONG stride;
                        if (d3d.vertex_shader & 1) {
                            vshader *sh = (vshader *)(d3d.vertex_shader & ~1u);
                            if (sh->code) vertex_shader_object(sh);   /* so the copy reuses it */
                            vshader tmp = inline_layout(sh, &stride);
                            if (stride) {
                                if (sh->code) draw_programmable(&tmp, prim, inline_buf, stride, 0, inline_n / stride, NULL);
                                else draw_declared(&tmp, prim, inline_buf, stride, 0, inline_n / stride, NULL);
                            }
                        } else {
                            stride = parse_fvf(d3d.vertex_shader).stride;
                            if (stride) draw(prim, inline_buf, stride, 0, inline_n / stride, NULL, false);
                        }
                    } else if (elems_n) {
                        D3DResource *vb = d3d.streams[0].vb;
                        if (vb) draw(prim, resource_data(vb), d3d.streams[0].stride, 0, elems_n, elems, true);
                    } else {
                        imm_flush(prim);
                    }
                    inline_n = elems_n = imm.n = 0;
                }
            } else if (m == 0x1810) {   /* NV097_DRAW_ARRAYS: count-1 << 24 | start */
                D3DResource *vb = d3d.streams[0].vb;
                if (vb) draw(prim, resource_data(vb), d3d.streams[0].stride, d & 0xFFFFFF, (d >> 24) + 1, NULL, true);
            } else if (m == 0x1800 || m == 0x1808) {   /* NV097_ARRAY_ELEMENT16 / 32 */
                if (elems_n + 2 > elems_cap) {
                    elems_cap = (elems_n + 2) * 2;
                    elems = realloc(elems, elems_cap * sizeof(*elems));
                }
                if (m == 0x1800) {
                    elems[elems_n++] = d & 0xFFFF;
                    elems[elems_n++] = d >> 16;
                } else {
                    elems[elems_n++] = d;
                }
            } else if (m >= 0x181C && m <= 0x1824) {   /* NV097_SET_EYE_VECTOR */
                d3d.eye_vector[(m - 0x181C) / 4] = f;
            } else if (m == 0x1EA4) {   /* NV097_SET_TRANSFORM_CONSTANT_LOAD */
                const_load = d;
            } else if (m >= 0xB80 && m < 0xC00) {   /* NV097_SET_TRANSFORM_CONSTANT */
                /* Consecutive dwords fill vec4s from the load index, which
                   moves on by the number of vec4s written. */
                unsigned pos = noinc ? k : (m - 0xB80) / 4;
                unsigned slot = const_load + pos / 4;
                if (slot < 192) { d3d.vs_const[slot][pos % 4] = f; vsc_touch(slot, 1); }
                if (k == count - 1) const_load += (pos + 1) / 4;
            } else if (m >= 0x1880 && m < 0x1900) {   /* NV097_SET_VERTEX_DATA2F_M */
                unsigned slot = (m - 0x1880) / 8, c = ((m - 0x1880) % 8) / 4;
                partial[slot][c] = f;
                if (c == 1) imm_set(slot, partial[slot][0], f, 0, 1);
            } else if (m >= 0x1900 && m < 0x1940) {   /* NV097_SET_VERTEX_DATA2S */
                unsigned slot = (m - 0x1900) / 4;
                imm_set(slot, (SHORT)(d & 0xFFFF), (SHORT)(d >> 16), 0, 1);
            } else if (m >= 0x1940 && m < 0x1980) {   /* NV097_SET_VERTEX_DATA4UB */
                unsigned slot = (m - 0x1940) / 4;
                imm_set(slot, (d & 255) / 255.0f, ((d >> 8) & 255) / 255.0f, ((d >> 16) & 255) / 255.0f,
                        (d >> 24) / 255.0f);
            } else if (m >= 0x1980 && m < 0x1A00) {   /* NV097_SET_VERTEX_DATA4S_M */
                unsigned slot = (m - 0x1980) / 8, c = ((m - 0x1980) % 8) / 4;
                partial[slot][c * 2] = (SHORT)(d & 0xFFFF);
                partial[slot][c * 2 + 1] = (SHORT)(d >> 16);
                if (c == 1) imm_set(slot, partial[slot][0], partial[slot][1], partial[slot][2], partial[slot][3]);
            } else if (m >= 0x1A00 && m < 0x1B00) {   /* NV097_SET_VERTEX_DATA4F_M */
                unsigned slot = (m - 0x1A00) / 16, c = ((m - 0x1A00) % 16) / 4;
                partial[slot][c] = f;
                if (c == 3) imm_set(slot, partial[slot][0], partial[slot][1], partial[slot][2], partial[slot][3]);
            } else if (m == 0x100) {   /* NV097_NO_OPERATION */
            } else if (pb_render_state(m, d)) {
            } else {
                unsigned u = 0;
                while (u < nunknown && unknown[u] != m) u++;
                if (u == nunknown && nunknown < 32) {
                    unknown[nunknown++] = m;
                    xlog("D3D: push buffer method %#x is not supported", m);
                }
            }
        }
    }
}

/* Whether a run of methods only sets state: no drawing, vertex data or
   control words (pb_interpret_body then touches nothing but the device
   state, so the title's thread can run it on its copy). */
static bool pb_state_only(const ULONG *p, unsigned n)
{
    for (unsigned i = 0; i < n;) {
        ULONG hdr = p[i++];
        if ((hdr & 3) != 0 || hdr == 0x20000 || (hdr & 0xE0000000) == 0x20000000) return false;
        unsigned method = hdr & 0x1FFC, count = (hdr >> 18) & 0x7FF;
        bool noinc = hdr & 0x40000000;
        if (i + count > n) return false;
        i += count;
        unsigned last = noinc || !count ? method : method + 4 * (count - 1);
        /* BEGIN_END, DRAW_ARRAYS, ARRAY_ELEMENT16/32, INLINE_ARRAY and the
           immediate vertex data all sit in 0x1800..0x1AFF, with the eye
           vector between them. */
        if (last >= 0x17FC && method < 0x1B00 && !(method >= 0x181C && last <= 0x1824)) return false;
    }
    return true;
}

static void pb_interpret(const ULONG *p, unsigned n)
{
    /* Raw methods and recorded push buffers draw outside the API calls the
       HUD batch and the packet captures watch: flush what is queued, and
       keep both off until the run is over. */
    hud_batch_flush();
    bool previous = hud_raw_active;
    hud_raw_active = true;
    pb_interpret_body(p, n);
    hud_raw_active = previous;
}

static void NTAPI D3DDevice_EndPush(ULONG *end)
{
    gl_require();
    unsigned n = end - push_scratch;
    if (!push_scratch || n > push_scratch_cap) return;
    if (d3d.recording) {
        ULONG h = n;
        pb_record2(OP_RAW, &h, 4, push_scratch, n * 4);
        return;
    }
    pb_interpret(push_scratch, n);
}

/* The device's own push buffer.  LTCG builds inline StartPush/EndPush: the
   title writes methods at g_pDevice->m_Pusher.m_pPut (the device's first
   dword) and calls MakeRequestedSpace when it passes m_pThreshold (the
   second).  Give it a buffer there and run what it wrote through the same
   interpreter as BeginPush before anything that depends on it. */
#define PUSHER_BYTES (1u << 20)
/* pusher_buf itself is in hud_packet.h: the packet captures read it too. */

static void pusher_init(ULONG *dev)
{
    if (!pusher_buf) pusher_buf = pool_alloc(PUSHER_BYTES);
    dev[0] = (ULONG)pusher_buf;
    dev[1] = (ULONG)pusher_buf + PUSHER_BYTES - 0x10000;
}

static void pusher_drain(void)
{
    static bool busy;
    ULONG *dev = d3d.device;
    /* Queued drawing: the front hands the methods over, the worker runs
       them in their place in the queue (async_draws.h). */
    if (!pusher_buf || !dev || busy || (async_on && async_worker_thread)) return;
    ULONG *put = (ULONG *)dev[0];
    if (put < pusher_buf || put > pusher_buf + PUSHER_BYTES / 4) {
        xlog("D3D: push buffer pointer %p is outside the device's buffer", (void *)put);
        dev[0] = (ULONG)pusher_buf;
        return;
    }
    unsigned n = put - pusher_buf;
    if (!n) return;
    busy = true;
    if (async_front()) {
        dev[0] = (ULONG)pusher_buf;
        /* Methods that only set state set it on the front, and the next
           queued draw carries it; anything else goes to the worker. */
        if (pb_state_only(put - n, n)) { pb_interpret_body(put - n, n); async_touch(~0u); }
        else async_raw_front(put - n, n);
        busy = false;
        return;
    }
    if (d3d.recording) {
        ULONG h = n;
        pb_record2(OP_RAW, &h, 4, pusher_buf, n * 4);
    } else {
        if (!pb_state_only(pusher_buf, n)) gl_require();
        Uint64 t0 = prof_draw_begin();
        pb_interpret(pusher_buf, n);
        if (t0) prof.drain += SDL_GetPerformanceCounter() - t0;
    }
    dev[0] = (ULONG)pusher_buf;
    busy = false;
}

static ULONG *NTAPI D3D_MakeRequestedSpace(ULONG Minimum, ULONG Requested)
{
    (void)Minimum; (void)Requested;
    pusher_drain();
    return pusher_buf;
}

static ULONG *NTAPI D3DDevice_MakeSpace(void) { return D3D_MakeRequestedSpace(0, 0); }

/* The LTCG BeginPush (count in esi): the title writes at the returned
   pointer and its inlined EndPush stores the end in m_pPut. */
static ULONG *NTAPI D3DDevice_BeginPushLTCG(ULONG Count)
{
    pusher_drain();
    if (Count * 4 + 0x10000 > PUSHER_BYTES) xlog("D3D: BeginPush of %u dwords", Count);
    return pusher_buf;
}

/* Device internals LTCG titles call directly.  The host keeps the state, so
   flushing it into the push buffer is only a drain. */
/* Lazy state lives in xbcompat's own state, not the CDevice: nothing to flush. */
static void NTAPI D3D_LazySetState(void)
{
    pusher_drain();
}

static void NTAPI D3D_DacProgramGammaRamp(PVOID miniport) { (void)miniport; }
static void NTAPI CDevice_SetStateVB(PVOID self, ULONG x) { (void)self; (void)x; pusher_drain(); }
static void NTAPI CDevice_SetStateUP(PVOID self) { (void)self; pusher_drain(); }
static void NTAPI CDevice_KickOff(PVOID self) { (void)self; pusher_drain(); }

/* Recorded, a Nop holds the place of a later D3DPushBuffer_Jump. */
static void NTAPI D3DDevice_Nop(void)
{
    if (d3d.recording) { ULONG a[2] = { 0, 0 }; pb_record(OP_NOP, a, sizeof(a)); }
}

/* ---- odds and ends ----------------------------------------------------- */

static void NTAPI Direct3D_SetPushBufferSize(ULONG size, ULONG kickoff) { (void)size; (void)kickoff; }
static ULONG NTAPI D3DDevice_AddRef(void) { return ++d3d.device_refs; }
static ULONG NTAPI D3DDevice_Release(void) { return d3d.device_refs > 1 ? --d3d.device_refs : 1; }
static void NTAPI D3DDevice_GetDirect3D(PVOID *pp) { *pp = direct3d_object; }
/* Leave the frame on screen for the next title (see kernel/av.c): what
   Present would put on screen now, including anything drawn since the last
   Present (ani2 draws the Microsoft logo into the front buffer and then
   persists it). */
static LONG NTAPI D3DDevice_PersistDisplay(void)
{
    gl_require();
    pusher_drain();
    if (!d3d.window) return D3DERR_INVALIDCALL;
    flush_cpu_backbuffer();
    persisted_save();
    int w = window_fb.fbo ? window_fb.w : (int)d3d.width, h = window_fb.fbo ? window_fb.h : (int)d3d.height;
    uint8_t *px = malloc((size_t)w * h * 4), *flipped = malloc((size_t)w * h * 4);
    if (!px || !flipped) { free(px); free(flipped); return D3DERR_INVALIDCALL; }
    if (window_fb.fbo) {
        present_window_framebuffer();
        window_fb.bind(GL_READ_FRAMEBUFFER, 0);
    } else {
        backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    if (window_fb.fbo) restore_window_framebuffer();
    else backbuffer_read_end();
    for (int y = 0; y < h; y++) memcpy(flipped + (size_t)y * w * 4, px + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
    av_persist(flipped, w, h);
    unsigned long long sum[3] = { 0, 0, 0 };
    for (size_t i = 0; i < (size_t)w * h; i++)
        for (int c = 0; c < 3; c++) sum[c] += flipped[i * 4 + c];
    unsigned long long n = (unsigned long long)w * h ? (unsigned long long)w * h : 1;
    free(px);
    free(flipped);
    xlog("D3D: PersistDisplay kept a %dx%d frame, average colour %02llx%02llx%02llx", w, h,
         sum[2] / n, sum[1] / n, sum[0] / n);
    return D3D_OK;
}

/* The last title's PersistDisplay frame until the first Present, else NULL
   (the library's own answer; it doesn't count references either). */
static LONG NTAPI D3DDevice_GetPersistedSurface(D3DSurface **pp)
{
    if (!persisted.loaded && !persisted.presented) {
        persisted_load();
        if (persisted.s) AvSetSavedDataAddress(persisted.s);
    }
    *pp = (D3DSurface *)AvGetSavedDataAddress();
    TRACE_ALL("D3D: GetPersistedSurface: %s", !*pp ? "none" : *pp == persisted.own ? "this title's" : "the last title's frame");
    return *pp ? D3D_OK : D3DERR_INVALIDCALL;
}

static void NTAPI D3DDevice_SetBackMaterial(const float *m)
{
    memcpy(d3d.back_material, m, 64);
    d3d.back_material_power = m[16];
}

static void NTAPI D3DDevice_GetBackMaterial(float *m)
{
    memcpy(m, d3d.back_material, 64);
    m[16] = d3d.back_material_power;
}

/* Visibility tests are GL occlusion queries, one per index the title uses.
   Indices run to D3DVISIBILITY_TEST_MAX (4096) and each keeps its own
   report, as the library's do.  With 256 slots, index i and i + 256 shared
   one: the later EndVisibilityTest dropped the earlier query, whose result
   then read as "nothing drawn" (D3DERR_INVALIDCALL, 0 pixels), so whatever
   the title culls on that result vanished depending on which tests the
   view happened to issue. */
#define VIS_SLOTS 4096
static struct { ULONG index; GLuint query; bool used; } vis[VIS_SLOTS];
static GLuint vis_active;

static void NTAPI D3DDevice_BeginVisibilityTest(void)
{
    gl_require();
    pusher_drain();
    if (d3d.recording) { ULONG a = 0; pb_record(OP_VIS_BEGIN, &a, 4); return; }
    if (async_front()) { async_vis_begin(); return; }
    hud_batch_flush();
    if (!p_glGenQueries || vis_active) return;
    p_glGenQueries(1, &vis_active);
    p_glBeginQuery(0x8914 /* GL_SAMPLES_PASSED */, vis_active);
    hud_query_active = true;
}

static LONG NTAPI D3DDevice_EndVisibilityTest(ULONG index)
{
    gl_require();
    pusher_drain();
    if (d3d.recording) { pb_record(OP_VIS_END, &index, 4); return D3D_OK; }
    if (async_front()) { async_vis_end(index); return D3D_OK; }
    hud_batch_flush();
    if (!vis_active) return D3DERR_INVALIDCALL;
    p_glEndQuery(0x8914);
    unsigned slot = index % VIS_SLOTS;
    if (vis[slot].used && vis[slot].query) {
        static void (APIENTRY *del)(GLsizei, const GLuint *);
        if (!del) del = SDL_GL_GetProcAddress("glDeleteQueries");
        if (del) del(1, &vis[slot].query);
    }
    vis[slot].index = index;
    vis[slot].query = vis_active;
    vis[slot].used = true;
    vis_active = 0;
    hud_query_active = false;
    return D3D_OK;
}

static LONG NTAPI D3DDevice_GetVisibilityTestResult(ULONG index, UINT_ *result, ULONGLONG *stamp)
{
    gl_require();
    unsigned slot = index % VIS_SLOTS;
    if (stamp) *stamp = d3d.frame;
    if (async_front()) return async_vis_result(index, result);
    if (!vis[slot].used || vis[slot].index != index) { if (result) *result = 0; return D3DERR_INVALIDCALL; }
    GLuint avail = 0, n = 0;
    p_glGetQueryObjectuiv(vis[slot].query, 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &avail);
    if (!avail) return 0x88760828;   /* D3DERR_TESTINCOMPLETE */
    p_glGetQueryObjectuiv(vis[slot].query, 0x8866 /* GL_QUERY_RESULT */, &n);
    if (result) *result = n;
    return D3D_OK;
}

/* ---- state blocks ----------------------------------------------------- */

/* A state block is a snapshot of everything a title can set.  Every block
   type captures the whole device (a superset of what PIXELSTATE or
   VERTEXSTATE ask for), and Begin/EndStateBlock capture the state as it
   stands at EndStateBlock. */
typedef struct state_block {
    ULONG rs[D3DRS_MAX];
    ULONG tss[4 * 32];
    D3DMATRIX transforms[10];
    D3DVIEWPORT8 viewport;
    typeof(d3d.streams) streams;
    D3DResource *textures[4];
    ULONG vertex_shader, pixel_shader, base_vertex_index;
    D3DResource *indices;
    typeof(d3d.lights) lights;
    float material[4][4], material_power, back_material[4][4], back_material_power;
    float vs_const[192][4];
    float ps_const[16][4];
    float eye_vector[4];
} state_block;

static void state_capture(state_block *b)
{
    pusher_drain();
    for (int i = 0; i < D3DRS_MAX; i++) b->rs[i] = RS(i);
    memcpy(b->tss, d3d.texture_state, sizeof(b->tss));
    memcpy(b->transforms, d3d.transforms, sizeof(b->transforms));
    b->viewport = d3d.viewport;
    memcpy(b->streams, d3d.streams, sizeof(b->streams));
    memcpy(b->textures, d3d.textures, sizeof(b->textures));
    b->vertex_shader = d3d.vertex_shader;
    b->pixel_shader = d3d.pixel_shader;
    b->indices = d3d.indices;
    b->base_vertex_index = d3d.base_vertex_index;
    memcpy(b->lights, d3d.lights, sizeof(b->lights));
    memcpy(b->material, d3d.material, sizeof(b->material));
    b->material_power = d3d.material_power;
    memcpy(b->back_material, d3d.back_material, sizeof(b->back_material));
    b->back_material_power = d3d.back_material_power;
    memcpy(b->vs_const, d3d.vs_const, sizeof(b->vs_const));
    memcpy(b->ps_const, d3d.ps_const, sizeof(b->ps_const));
    memcpy(b->eye_vector, d3d.eye_vector, sizeof(b->eye_vector));
}

static void state_apply(const state_block *b)
{
    for (int i = 0; i < D3DRS_MAX; i++) RS(i) = b->rs[i];
    memcpy(d3d.texture_state, b->tss, sizeof(b->tss));
    memcpy(d3d.transforms, b->transforms, sizeof(b->transforms));
    D3DDevice_SetViewport(&b->viewport);
    memcpy(d3d.streams, b->streams, sizeof(b->streams));
    memcpy(d3d.textures, b->textures, sizeof(b->textures));
    d3d.vertex_shader = b->vertex_shader;
    d3d.pixel_shader = b->pixel_shader;
    D3DDevice_SetIndices(b->indices, b->base_vertex_index);
    memcpy(d3d.lights, b->lights, sizeof(b->lights));
    memcpy(d3d.material, b->material, sizeof(b->material));
    d3d.material_power = b->material_power;
    memcpy(d3d.back_material, b->back_material, sizeof(b->back_material));
    d3d.back_material_power = b->back_material_power;
    memcpy(d3d.vs_const, b->vs_const, sizeof(b->vs_const));
    vsc_touch(0, 192);
    memcpy(d3d.ps_const, b->ps_const, sizeof(b->ps_const));
    memcpy(d3d.eye_vector, b->eye_vector, sizeof(b->eye_vector));
}

static LONG NTAPI D3DDevice_CreateStateBlock(ULONG Type, ULONG *pToken)
{
    (void)Type;
    state_block *b = malloc(sizeof(*b));
    if (!b) return 0x8007000E;   /* E_OUTOFMEMORY */
    state_capture(b);
    *pToken = (ULONG)b;
    return D3D_OK;
}

static LONG NTAPI D3DDevice_ApplyStateBlock(ULONG Token)
{
    if (Token && Token != (ULONG)-1) state_apply((state_block *)Token);
    return D3D_OK;
}

static LONG NTAPI D3DDevice_CaptureStateBlock(ULONG Token)
{
    if (Token && Token != (ULONG)-1) state_capture((state_block *)Token);
    return D3D_OK;
}

static LONG NTAPI D3DDevice_DeleteStateBlock(ULONG Token)
{
    if (Token && Token != (ULONG)-1) free((state_block *)Token);
    return D3D_OK;
}

static LONG NTAPI D3DDevice_BeginStateBlock(void)
{
    static bool warned;
    if (!warned) { xlog("D3D: BeginStateBlock records the whole device state at EndStateBlock"); warned = true; }
    return D3D_OK;
}

static LONG NTAPI D3DDevice_EndStateBlock(ULONG *pToken)
{
    return D3DDevice_CreateStateBlock(1, pToken);
}

/* ---- table ------------------------------------------------------------- */

#define F(dec, fn) { dec, (void *)fn }
/* ---- XDK 5xxx variants -------------------------------------------------
   Later libraries return objects instead of filling an out parameter, and
   pass some arguments in registers. */

static D3DPixelContainer *NTAPI D3DDevice_CreateTexture2(UINT_ w, UINT_ h, UINT_ depth, UINT_ levels, ULONG usage,
                                                         ULONG fmt, ULONG type)
{
    D3DPixelContainer *t = NULL;
    TRACE_ALL("D3D: CreateTexture2(%u, %u, %u, levels %u, usage %#x, fmt %#x, type %u)", w, h, depth, levels, usage, fmt,
          type);
    switch (type) {
    case 4: D3DDevice_CreateVolumeTexture(w, h, depth, levels, usage, fmt, 0, &t); break;   /* D3DRTYPE_VOLUMETEXTURE */
    case 5: D3DDevice_CreateCubeTexture(w, levels, usage, fmt, 0, &t); break;              /* D3DRTYPE_CUBETEXTURE */
    default: D3DDevice_CreateTexture(w, h, levels, usage, fmt, 0, &t); break;
    }
    return t;
}

static D3DSurface *NTAPI D3DDevice_GetBackBuffer2(LONG BackBuffer)
{
    D3DSurface *s;
    D3DDevice_GetBackBuffer(BackBuffer, 0, &s);
    return s;
}

static D3DSurface *NTAPI D3DDevice_GetDepthStencilSurface2(void)
{
    if (!d3d.depth) return NULL;
    d3d.depth->Common++;
    return d3d.depth;
}

static D3DSurface *NTAPI D3DDevice_GetRenderTarget2(void)
{
    D3DSurface *s;
    D3DDevice_GetRenderTarget(&s);
    return s;
}

static D3DResource *NTAPI D3DDevice_GetTexture2(ULONG Stage)
{
    D3DResource *r;
    D3DDevice_GetTexture(Stage, &r);
    return r;
}

static D3DSurface *NTAPI D3DTexture_GetSurfaceLevel2(D3DPixelContainer *t, UINT_ level)
{
    return surface_of_level(t, 0, level);
}

static PVOID NTAPI D3DPalette_Lock2(D3DPalette *p, ULONG Flags)
{
    PVOID colors;
    D3DPalette_Lock(p, &colors, Flags);
    return colors;
}

/* PixelJar::Get2DSurfaceDesc, which 5xxx headers call from the inline
   D3DSurface_GetDesc and D3DTexture_GetLevelDesc. */
static void NTAPI Get2DSurfaceDesc(D3DPixelContainer *p, UINT_ level, ULONG *desc)
{
    D3DTexture_GetLevelDesc(p, level, desc);
    desc[1] = ((p->res.Common >> 16) & 7) == 5 ? 1 /* D3DRTYPE_SURFACE */ : 3 /* D3DRTYPE_TEXTURE */;
    if ((D3DSurface *)p == d3d.backbuffer || (D3DSurface *)p == d3d.target) desc[2] = 1;   /* D3DUSAGE_RENDERTARGET */
    else if ((D3DSurface *)p == d3d.depth) desc[2] = 2;                                   /* D3DUSAGE_DEPTHSTENCIL */
}

/* The inline SetVertexShaderConstant adds 96 before calling these, so
   their register is a 0..191 slot rather than D3D's -96..95, and it passes
   the NotInline forms a count of dwords, not vectors. */
static void FASTCALL SetVertexShaderConstantNotInline(LONG Register, const float *data, ULONG dwords)
{
    D3DDevice_SetVertexShaderConstant(Register - 96, data, dwords / 4);
}

static void FASTCALL SetVertexShaderConstant1(LONG Register, const float *data)
{
    D3DDevice_SetVertexShaderConstant(Register - 96, data, 1);
}

static void FASTCALL SetVertexShaderConstant4(LONG Register, const float *data)
{
    D3DDevice_SetVertexShaderConstant(Register - 96, data, 4);
}

static D3DResource *NTAPI D3DDevice_CreateVertexBuffer2(UINT_ Length)
{
    D3DResource *vb = NULL;
    D3DDevice_CreateVertexBuffer(Length, 0, 0, 0, &vb);
    return vb;
}

static D3DResource *NTAPI D3DDevice_CreateIndexBuffer2(UINT_ Length)
{
    D3DResource *ib = NULL;
    D3DDevice_CreateIndexBuffer(Length, 0, 0x65 /* D3DFMT_INDEX16 */, 0, &ib);
    return ib;
}

static D3DPalette *NTAPI D3DDevice_CreatePalette2(ULONG Size)
{
    D3DPalette *p = NULL;
    D3DDevice_CreatePalette(Size, &p);
    return p;
}

static D3DSurface *NTAPI D3DCubeTexture_GetCubeMapSurface2(D3DPixelContainer *t, ULONG face, UINT_ level)
{
    D3DSurface *s = NULL;
    D3DCubeTexture_GetCubeMapSurface(t, face, level, &s);
    return s;
}

static D3DResource *NTAPI D3DDevice_GetStreamSource2(UINT_ Stream, UINT_ *stride)
{
    D3DResource *vb = NULL;
    D3DDevice_GetStreamSource(Stream, &vb, stride);
    return vb;
}

static PVOID NTAPI D3DVertexBuffer_Lock2(D3DResource *vb, ULONG Flags)
{
    PVOID p;
    D3DVertexBuffer_Lock(vb, 0, 0, &p, Flags);
    return p;
}

static ULONG *NTAPI D3DDevice_BeginPush2(ULONG Count)
{
    pusher_drain();
    ULONG *p;
    D3DDevice_BeginPush(Count, &p);
    return p;
}

SIZE_T NTAPI MmQueryAllocationSize(PVOID BaseAddress);

static void NTAPI D3DVertexBuffer_GetDesc(D3DResource *vb, ULONG *desc)
{
    /* D3DVERTEXBUFFER_DESC: Format, Type, Usage, Pool, Size, FVF.  The
       resource does not record its length; report the allocation's. */
    desc[0] = 100;   /* D3DFMT_VERTEXDATA */
    desc[1] = 6;     /* D3DRTYPE_VERTEXBUFFER */
    desc[2] = 0;
    desc[3] = 0;
    desc[4] = MmQueryAllocationSize(resource_data(vb));
    desc[5] = 0;
}

/* D3DINDEXBUFFER_DESC: Format, Type (the library fills only these). */
static void NTAPI D3DIndexBuffer_GetDesc(D3DResource *ib, ULONG *desc)
{
    (void)ib;
    desc[0] = 101;   /* D3DFMT_INDEX16 */
    desc[1] = 7;     /* D3DRTYPE_INDEXBUFFER */
}

/* D3DPALETTESIZE, as CreatePalette put it in Common's top bits. */
static ULONG NTAPI D3DPalette_GetSize(D3DPalette *p) { return p->Common >> 30; }

static void NTAPI D3DDevice_GetPalette(ULONG Stage, D3DPalette **pp)
{
    D3DPalette *p = Stage < 4 ? d3d.palettes[Stage] : NULL;
    if (p) D3DResource_AddRef((D3DResource *)p);
    *pp = p;
}

/* The texture a surface level belongs to, with a reference. */
static LONG NTAPI D3DSurface_GetContainer(D3DSurface *s, D3DPixelContainer **pp)
{
    *pp = s->Parent;
    if (!s->Parent) return (LONG)0x80004005;   /* E_FAIL */
    D3DResource_AddRef(&s->Parent->res);
    return D3D_OK;
}

static void NTAPI D3DCubeTexture_GetLevelDesc(D3DPixelContainer *t, UINT_ level, ULONG *desc)
{
    D3DTexture_GetLevelDesc(t, level, desc);
}

static void NTAPI D3DSurface_UnlockRect(D3DSurface *s) { (void)s; }

static void NTAPI D3DDevice_GetScreenSpaceOffset(float *x, float *y)
{
    *x = d3d.screen_offset[0];
    *y = d3d.screen_offset[1];
}

/* SetVertexBlendModelView gives the skinning matrices and the projection
   (with the viewport) directly, in place of the ones the fixed-function
   pipeline derives from the world, view and projection transforms, until
   it is called with NULL (apply_direct_modelview draws with them). */
static void NTAPI D3DDevice_SetVertexBlendModelView(UINT_ count, const D3DMATRIX *mv, const D3DMATRIX *inv,
                                                    const D3DMATRIX *proj_vp)
{
    if (d3d.recording) {
        static ULONG rec[2 + 9 * 16];
        memset(rec, 0, sizeof(rec));
        ULONG n = count > 4 ? 4 : count;
        rec[0] = mv ? n : 0;
        rec[1] = (mv != NULL) | (inv != NULL) << 1 | (proj_vp != NULL) << 2;
        if (mv) memcpy(rec + 2, mv, n * sizeof(D3DMATRIX));
        if (inv) memcpy(rec + 2 + 64, inv, n * sizeof(D3DMATRIX));
        if (proj_vp) memcpy(rec + 2 + 128, proj_vp, sizeof(D3DMATRIX));
        pb_record(OP_BLEND_MODELVIEW, rec, sizeof(rec));
    }
    d3d.direct_mv.on = mv != NULL;
    if (!mv) return;
    d3d.direct_mv.blend = true;
    d3d.direct_mv.count = count > 4 ? 4 : count;
    d3d.direct_mv.has_inv = inv != NULL;
    memcpy(d3d.direct_mv.mv, mv, d3d.direct_mv.count * sizeof(D3DMATRIX));
    if (inv) memcpy(d3d.direct_mv.inv, inv, d3d.direct_mv.count * sizeof(D3DMATRIX));
    if (proj_vp) d3d.direct_mv.composite = d3d.direct_mv.proj_vp = *proj_vp;
}
static LONG NTAPI D3DDevice_GetVertexBlendModelView(UINT_ count, D3DMATRIX *mv, D3DMATRIX *proj_vp)
{
    if (mv) memcpy(mv, d3d.direct_mv.mv, (count > 4 ? 4 : count) * sizeof(D3DMATRIX));
    if (proj_vp) *proj_vp = d3d.direct_mv.proj_vp;
    return d3d.direct_mv.on ? D3D_OK : 1 /* S_FALSE */;
}

/* The inline Release in 5xxx headers drops the count itself and calls this
   on the last reference, after releasing a surface's parent itself. */
static void NTAPI D3D_DestroyResource(D3DResource *r)
{
    destroy_resource(r);
}

static void NTAPI D3DDevice_MultiplyTransform(ULONG State, const D3DMATRIX *m)
{
    if (State >= 10) return;
    D3DMATRIX r;
    mat_mul(&r, m, &d3d.transforms[State]);
    D3DDevice_SetTransform(State, &r);
}

/* The modelview SetModelView or SetVertexBlendModelView last gave, and
   whether one is in effect (S_OK) or not (S_FALSE), as the library has it. */
static LONG NTAPI D3DDevice_GetModelView(D3DMATRIX *m)
{
    if (m) *m = d3d.direct_mv.mv[0];
    return d3d.direct_mv.on ? D3D_OK : 1 /* S_FALSE */;
}

/* SetModelView gives the modelview, its inverse (optional) and the
   composite matrix (modelview * projection * viewport) directly, in place
   of world * view and the projection transform, until it is called with
   NULL (apply_direct_modelview draws with them). */
static void NTAPI D3DDevice_SetModelView(const D3DMATRIX *mv, const D3DMATRIX *inv, const D3DMATRIX *composite)
{
    if (d3d.recording) {
        static ULONG rec[1 + 3 * 16];
        memset(rec, 0, sizeof(rec));
        rec[0] = (mv && composite) | (inv != NULL) << 1;
        if (mv) memcpy(rec + 1, mv, sizeof(D3DMATRIX));
        if (inv) memcpy(rec + 17, inv, sizeof(D3DMATRIX));
        if (composite) memcpy(rec + 33, composite, sizeof(D3DMATRIX));
        pb_record(OP_MODELVIEW, rec, sizeof(rec));
    }
    d3d.direct_mv.on = mv && composite;
    if (!d3d.direct_mv.on) return;
    d3d.direct_mv.blend = false;
    d3d.direct_mv.count = 1;
    d3d.direct_mv.has_inv = inv != NULL;
    d3d.direct_mv.mv[0] = *mv;
    if (inv) d3d.direct_mv.inv[0] = *inv;
    d3d.direct_mv.composite = *composite;
}

static void NTAPI D3DDevice_GetViewportOffsetAndScale(float *offset, float *scale)
{
    /* The screen-space transform the hardware applies after projection
       (CDevice::GetViewportOffsetAndScale): pixel centres at +0.53125 and
       z scaled to the depth buffer's range. */
    float zscale = d3d.zscale;   /* the library's m_ZScale, from the last depth buffer set */
    const D3DVIEWPORT8 *v = &d3d.viewport;
    scale[0] = v->Width * 0.5f;
    scale[1] = -(v->Height * 0.5f);
    scale[2] = (v->MaxZ - v->MinZ) * zscale;
    scale[3] = 1;
    offset[0] = v->X + v->Width * 0.5f + 0.53125f;
    offset[1] = v->Y + v->Height * 0.5f + 0.53125f;
    offset[2] = v->MinZ * zscale;
    offset[3] = 0;
}

static void NTAPI D3DDevice_SetRenderTargetFast(D3DSurface *target, D3DSurface *z, ULONG Flags)
{
    gl_require();
    pusher_drain();
    (void)Flags;
    D3DDevice_SetRenderTarget(target, z);
}

/* SetTexture's fast path when only the image moves (same size and format):
   the method is NV097_SET_TEXTURE_OFFSET(stage).  The stage keeps a copy of
   its texture header pointing at the new data. */
static void FASTCALL D3DDevice_SwitchTexture(ULONG Method, ULONG Data, ULONG Format)
{
    static D3DPixelContainer shadow[4];
    ULONG stage = ((Method - 0x1B00) >> 6) & 3;
    D3DPixelContainer *cur = (D3DPixelContainer *)d3d.textures[stage];
    if (!cur) { xlog("D3D: SwitchTexture on empty stage %u", stage); return; }
    if (cur != &shadow[stage]) shadow[stage] = *cur;
    shadow[stage].res.Data = Data;
    shadow[stage].Format = Format;
    D3DDevice_SetTexture(stage, &shadow[stage].res);
}

static ULONG fence;
static bool async_front(void);
static void async_fence(ULONG f);
static ULONG NTAPI D3D_SetFence(ULONG Flags)
{
    (void)Flags;
    pusher_drain();
    ++fence;
    if (d3d.cpu_time) *d3d.cpu_time = fence;
    if (async_front()) async_fence(fence);
    else __atomic_store_n(&async_gpu_time, fence, __ATOMIC_RELEASE);
    return fence;
}
static void NTAPI D3D_BlockOnTime(ULONG Time, ULONG Flags) { (void)Time; (void)Flags; pusher_drain(); }
static void NTAPI D3D_BlockOnResource(D3DResource *r) { (void)r; }
/* ---- LTCG builds: entry points the whole-program optimizer cut down ---- */

/* CreateDevice with the adapter, device type and window it always ignores dropped. */
static LONG NTAPI Direct3D_CreateDevice_LTCG(ULONG Flags, D3DPRESENT_PARAMETERS *pp, PVOID *ppDevice)
{
    return Direct3D_CreateDevice(0, 1, NULL, Flags, pp, ppDevice);
}

/* PixelJar::Lock2DSurface and Lock3DSurface, which LTCG builds call in place
   of the inlined LockRect and LockBox of every resource type. */
static void NTAPI Lock2DSurface(D3DPixelContainer *p, ULONG face, UINT_ level, ULONG *locked, const LONG *rect,
                                ULONG flags)
{
    if (((p->res.Common >> 16) & 7) == 5) D3DSurface_LockRect((D3DSurface *)p, locked, rect, flags);
    else D3DCubeTexture_LockRect(p, face, level, locked, rect, flags);
}

static void NTAPI Lock3DSurface(D3DPixelContainer *p, UINT_ level, ULONG *locked, const LONG *box, ULONG flags)
{
    if (((p->res.Common >> 16) & 7) == 5) D3DVolume_LockBox((D3DSurface *)p, locked, box, flags);
    else D3DVolumeTexture_LockBox(p, level, locked, box, flags);
}

static void NTAPI D3D_Nop0(void) {}
static void NTAPI D3D_Nop4(ULONG a) { (void)a; }
static void NTAPI D3D_Nop8(ULONG a, ULONG b) { (void)a; (void)b; }
static void NTAPI D3D_Nop12(ULONG a, ULONG b, ULONG c) { (void)a; (void)b; (void)c; }
static void FASTCALL D3D_FastNop(ULONG a, ULONG b) { (void)a; (void)b; }
static LONG NTAPI D3DDevice_Reset(PVOID pp)
{
    if (pp) set_present_interval(((D3DPRESENT_PARAMETERS *)pp)->FullScreen_PresentationInterval);
    persisted.first_flip = true;   /* the mode is set again: the next Present frees the persisted frame */
    return D3D_OK;
}
static void NTAPI SetRenderState_SampleAlpha(ULONG Value) { (void)Value; }

static ULONG NTAPI D3D_GetAdapterModeCount2(UINT_ Adapter, ULONG Format)
{
    (void)Format;
    return Direct3D_GetAdapterModeCount(Adapter);
}

/* ---- more 5xxx entry points: internals LTCG titles call, renamed APIs -- */

/* The library's texture constructor, which the Create*Texture APIs wrap
   (d3dbase.cpp); LTCG titles call it themselves, isVolume a byte in dl. */
static LONG NTAPI D3D_CreateTexture(UINT_ w, UINT_ h, UINT_ depth, UINT_ levels, ULONG usage, ULONG fmt,
                                    ULONG cube, ULONG volume, D3DPixelContainer **pp)
{
    (void)usage;
    bool is_cube = (cube & 0xFF) != 0, is_volume = (volume & 0xFF) != 0;
    *pp = alloc_texture(w, is_cube ? w : h, is_volume ? depth : 1, levels, fmt, is_cube);
    return *pp ? D3D_OK : 0x8007000E;
}

static D3DSurface *NTAPI D3DDevice_GetPersistedSurface2(void)
{
    D3DSurface *s = NULL;
    D3DDevice_GetPersistedSurface(&s);
    return s;
}

/* The headers make Present a Swap(0); some libraries keep a body for it. */
static LONG NTAPI D3DDevice_Present(const void *src, const void *dst, PVOID unused1, PVOID unused2)
{
    gl_require();
    (void)src; (void)dst; (void)unused1; (void)unused2;
    D3DDevice_Swap(0);
    return D3D_OK;
}

/* SetVertexShaderConstantNotInline with every argument in a register
   (slot in ebx, data in edx, dwords in eax): the same slot and dword count. */
static void NTAPI SetVertexShaderConstantNotInline_LTCG(LONG Register, const float *data, ULONG dwords)
{
    SetVertexShaderConstantNotInline(Register, data, dwords);
}

static void FASTCALL D3D_BlockOnResourceFast(D3DResource *r) { D3D_BlockOnResource(r); }

/* EnableOverlay with its argument folded in: which way is not known, and
   UpdateOverlay decides what is shown. */
static void NTAPI D3DDevice_EnableOverlay_LTCG(void)
{
    static bool said;
    if (!said) xlog("D3D: EnableOverlay (LTCG, argument folded in) ignored");
    said = true;
}

#include "async_draws.h"

const struct hle_func d3d8_funcs[] = {
    F("_Direct3DCreate8@4", Direct3DCreate8),
    F("_Direct3D_CreateDevice@24", Direct3D_CreateDevice),
    F("_D3DDevice_Swap@4", D3DDevice_Swap),
    F("_D3DDevice_Clear@24", D3DDevice_Clear),
    F("_D3DDevice_CreateVertexBuffer@20", D3DDevice_CreateVertexBuffer),
    F("_D3DVertexBuffer_Lock@20", D3DVertexBuffer_Lock),
    F("_D3DResource_AddRef@4", D3DResource_AddRef),
    F("_D3DResource_Release@4", D3DResource_Release),
    F("_D3DResource_GetType@4", D3DResource_GetType),
    F("_D3DDevice_SetStreamSource@12", D3DDevice_SetStreamSource),
    F("_D3D_AllocContiguousMemory@8", D3D_AllocContiguousMemory),
    F("_D3D_FreeContiguousMemory@4", D3D_FreeContiguousMemory),
    F("_D3DResource_Register@8", D3DResource_Register),
    F("_D3DDevice_SetTexture@8", D3DDevice_SetTexture),
    F("_D3DDevice_SetTextureStageStateNotInline@12", D3DDevice_SetTextureStageStateNotInline),
    F("_D3DDevice_SetTextureState_TexCoordIndex@8", SetTextureState_TexCoordIndex),
    F("_D3DDevice_SetTextureState_BorderColor@8", SetTextureState_BorderColor),
    F("_D3DDevice_SetTextureState_ColorKeyColor@8", SetTextureState_ColorKeyColor),
    F("_D3DDevice_SetTextureState_BumpEnv@12", SetTextureState_BumpEnv),
    F("_D3DDevice_SetTextureState_ParameterCheck@12", SetTextureState_ParameterCheck),
    F("_D3DDevice_SetVertexShader@4", D3DDevice_SetVertexShader),
    F("_D3DDevice_SetTransform@8", D3DDevice_SetTransform),
    F("_D3DDevice_SetViewport@4", D3DDevice_SetViewport),
    F("_D3DDevice_SetLight@8", D3DDevice_SetLight),
    F("_D3DDevice_LightEnable@8", D3DDevice_LightEnable),
    F("_D3DDevice_SetMaterial@4", D3DDevice_SetMaterial),
    F("_D3DDevice_SetBackMaterial@4", D3DDevice_SetBackMaterial),
    F("_D3DDevice_GetBackMaterial@4", D3DDevice_GetBackMaterial),
    F("_Direct3D_SetPushBufferSize@8", Direct3D_SetPushBufferSize),
    F("_D3DDevice_AddRef@0", D3DDevice_AddRef),
    F("_D3DDevice_Release@0", D3DDevice_Release),
    F("_D3DDevice_GetDirect3D@4", D3DDevice_GetDirect3D),
    F("_D3DDevice_GetPersistedSurface@4", D3DDevice_GetPersistedSurface),
    F("_D3DDevice_DrawRectPatch@12", D3DDevice_DrawRectPatch),
    F("_D3DDevice_DrawTriPatch@12", D3DDevice_DrawTriPatch),
    F("_D3DDevice_DeletePatch@4", D3DDevice_DeletePatch),
    F("_D3DDevice_CreateStateBlock@8", D3DDevice_CreateStateBlock),
    F("_D3DDevice_ApplyStateBlock@4", D3DDevice_ApplyStateBlock),
    F("_D3DDevice_CaptureStateBlock@4", D3DDevice_CaptureStateBlock),
    F("_D3DDevice_DeleteStateBlock@4", D3DDevice_DeleteStateBlock),
    F("_D3DDevice_BeginStateBlock@0", D3DDevice_BeginStateBlock),
    F("_D3DDevice_EndStateBlock@4", D3DDevice_EndStateBlock),
    F("_D3DDevice_BeginVisibilityTest@0", D3DDevice_BeginVisibilityTest),
    F("_D3DDevice_EndVisibilityTest@4", D3DDevice_EndVisibilityTest),
    F("_D3DDevice_GetVisibilityTestResult@12", D3DDevice_GetVisibilityTestResult),
    F("_D3DDevice_SetIndices@8", D3DDevice_SetIndices),
    F("_D3DDevice_DrawVertices@12", D3DDevice_DrawVertices),
    F("_D3DDevice_DrawVerticesUP@16", D3DDevice_DrawVerticesUP),
    F("_D3DDevice_DrawIndexedVertices@12", D3DDevice_DrawIndexedVertices),
    F("_D3DDevice_DrawIndexedVerticesUP@20", D3DDevice_DrawIndexedVerticesUP),
    F("_D3DDevice_BlockUntilIdle@0", D3DDevice_BlockUntilIdle),
    F("_D3DDevice_BlockUntilVerticalBlank@0", D3DDevice_BlockUntilVerticalBlank),
    F("_D3DDevice_IsBusy@0", D3DDevice_IsBusy),
    F("_D3DDevice_SetFlickerFilter@4", D3DDevice_SetFlickerFilter),
    F("_D3DDevice_SetSoftDisplayFilter@4", D3DDevice_SetSoftDisplayFilter),
    F("@D3DDevice_SetRenderState_Simple@8", SetRenderState_Simple),
    F("@D3DDevice_SetRenderState_Simple_Fast@8", SetRenderState_Simple),
    F("_D3DDevice_SetRenderState_ParameterCheck@8", SetRenderState_ParameterCheck),
    F("_D3DDevice_SetRenderStateNotInline@8", SetRenderStateNotInline),
    F("_D3DDevice_SetRenderState_ZEnable@4", SetRenderState_ZEnable),
    F("_D3DDevice_SetRenderState_CullMode@4", SetRenderState_CullMode),
    F("_D3DDevice_SetRenderState_FillMode@4", SetRenderState_FillMode),
    F("_D3DDevice_SetRenderState_FrontFace@4", SetRenderState_FrontFace),
    F("_D3DDevice_SetRenderState_NormalizeNormals@4", SetRenderState_NormalizeNormals),
    F("_D3DDevice_SetRenderState_StencilEnable@4", SetRenderState_StencilEnable),
    F("_D3DDevice_SetRenderState_TextureFactor@4", SetRenderState_TextureFactor),
    F("_D3DDevice_SetRenderState_BackFillMode@4", SetRenderState_BackFillMode),
    F("_D3DDevice_SetRenderState_TwoSidedLighting@4", SetRenderState_TwoSidedLighting),
    F("_D3DDevice_SetRenderState_StencilFail@4", SetRenderState_StencilFail),
    F("_D3DDevice_SetRenderState_ZBias@4", SetRenderState_ZBias),
    F("_D3DDevice_SetRenderState_LogicOp@4", SetRenderState_LogicOp),
    F("_D3DDevice_SetRenderState_EdgeAntiAlias@4", SetRenderState_EdgeAntiAlias),
    F("_D3DDevice_SetRenderState_MultiSampleAntiAlias@4", SetRenderState_MultiSampleAntiAlias),
    F("_D3DDevice_SetRenderState_MultiSampleMask@4", SetRenderState_MultiSampleMask),
    F("_D3DDevice_SetRenderState_MultiSampleMode@4", SetRenderState_MultiSampleMode),
    F("_D3DDevice_SetRenderState_MultiSampleRenderTargetMode@4", SetRenderState_MultiSampleRenderTargetMode),
    F("_D3DDevice_SetRenderState_ShadowFunc@4", SetRenderState_ShadowFunc),
    F("_D3DDevice_SetRenderState_LineWidth@4", SetRenderState_LineWidth),
    F("_D3DDevice_SetRenderState_Dxt1NoiseEnable@4", SetRenderState_Dxt1NoiseEnable),
    F("_D3DDevice_SetRenderState_YuvEnable@4", SetRenderState_YuvEnable),
    F("_D3DDevice_SetRenderState_OcclusionCullEnable@4", SetRenderState_OcclusionCullEnable),
    F("_D3DDevice_SetRenderState_StencilCullEnable@4", SetRenderState_StencilCullEnable),
    F("_D3DDevice_SetRenderState_RopZCmpAlwaysRead@4", SetRenderState_RopZCmpAlwaysRead),
    F("_D3DDevice_SetRenderState_RopZRead@4", SetRenderState_RopZRead),
    F("_D3DDevice_SetRenderState_DoNotCullUncompressed@4", SetRenderState_DoNotCullUncompressed),
    F("_D3DDevice_SetRenderState_VertexBlend@4", SetRenderState_VertexBlend),
    F("_D3DDevice_SetRenderState_FogColor@4", SetRenderState_FogColor),
    F("_D3DDevice_SetRenderState_PSTextureModes@4", SetRenderState_PSTextureModes),
    F("_D3DDevice_GetBackBuffer@12", D3DDevice_GetBackBuffer),
    F("_D3DDevice_GetDepthStencilSurface@4", D3DDevice_GetDepthStencilSurface),
    F("_D3DDevice_GetRenderTarget@4", D3DDevice_GetRenderTarget),
    F("_D3DDevice_SetRenderTarget@8", D3DDevice_SetRenderTarget),
    F("_D3DSurface_GetDesc@8", D3DSurface_GetDesc),
    F("_D3DSurface_LockRect@16", D3DSurface_LockRect),
    F("_D3DDevice_CreateImageSurface@16", D3DDevice_CreateImageSurface),
    F("_D3DDevice_CreateRenderTarget@24", D3DDevice_CreateRenderTarget),
    F("_D3DDevice_CreateDepthStencilSurface@20", D3DDevice_CreateDepthStencilSurface),
    F("_D3DDevice_CopyRects@20", D3DDevice_CopyRects),
    F("_D3DDevice_CreateTexture@28", D3DDevice_CreateTexture),
    F("_D3DDevice_CreateCubeTexture@24", D3DDevice_CreateCubeTexture),
    F("_D3DDevice_CreateVolumeTexture@32", D3DDevice_CreateVolumeTexture),
    F("_D3DBaseTexture_GetLevelCount@4", D3DBaseTexture_GetLevelCount),
    F("_D3DTexture_GetLevelDesc@12", D3DTexture_GetLevelDesc),
    F("_D3DTexture_GetSurfaceLevel@12", D3DTexture_GetSurfaceLevel),
    F("_D3DTexture_LockRect@20", D3DTexture_LockRect),
    F("_D3DCubeTexture_GetCubeMapSurface@16", D3DCubeTexture_GetCubeMapSurface),
    F("_D3DCubeTexture_LockRect@24", D3DCubeTexture_LockRect),
    F("_D3DVolumeTexture_LockBox@20", D3DVolumeTexture_LockBox),
    F("_D3DVolumeTexture_GetLevelDesc@12", D3DVolumeTexture_GetLevelDesc),
    F("_D3DVolumeTexture_GetVolumeLevel@12", D3DVolumeTexture_GetVolumeLevel),
    F("_D3DVolume_GetDesc@8", D3DVolume_GetDesc),
    F("_D3DVolume_GetContainer@8", D3DVolume_GetContainer),
    F("_D3DVolume_LockBox@16", D3DVolume_LockBox),
    F("_D3DDevice_CreateIndexBuffer@20", D3DDevice_CreateIndexBuffer),
    F("_D3DIndexBuffer_Lock@20", D3DIndexBuffer_Lock),
    F("_D3DDevice_CreatePalette@8", D3DDevice_CreatePalette),
    F("_D3DPalette_Lock@12", D3DPalette_Lock),
    F("_D3DDevice_SetPalette@8", D3DDevice_SetPalette),
    F("_D3D_AllocNoncontiguousMemory@4", D3D_AllocNoncontiguousMemory),
    F("_D3D_FreeNoncontiguousMemory@4", D3D_FreeNoncontiguousMemory),
    F("_D3DDevice_GetTransform@8", D3DDevice_GetTransform),
    F("_D3DDevice_GetViewport@4", D3DDevice_GetViewport),
    F("_D3DDevice_GetRenderState@8", D3DDevice_GetRenderState),
    F("_D3DDevice_GetTextureStageState@12", D3DDevice_GetTextureStageState),
    F("_D3DDevice_GetTexture@8", D3DDevice_GetTexture),
    F("_D3DDevice_GetStreamSource@12", D3DDevice_GetStreamSource),
    F("_D3DDevice_GetIndices@8", D3DDevice_GetIndices),
    F("_D3DDevice_GetMaterial@4", D3DDevice_GetMaterial),
    F("_D3DDevice_GetLight@8", D3DDevice_GetLight),
    F("_D3DDevice_GetLightEnable@8", D3DDevice_GetLightEnable),
    F("_D3DDevice_GetVertexShader@4", D3DDevice_GetVertexShader),
    F("_D3DDevice_SetVertexShaderConstant@12", D3DDevice_SetVertexShaderConstant),
    F("_D3DDevice_CreateTexture2@28", D3DDevice_CreateTexture2),
    F("_D3DDevice_GetBackBuffer2@4", D3DDevice_GetBackBuffer2),
    F("_D3DDevice_GetDepthStencilSurface2@0", D3DDevice_GetDepthStencilSurface2),
    F("_D3DDevice_GetRenderTarget2@0", D3DDevice_GetRenderTarget2),
    F("_D3DDevice_GetTexture2@4", D3DDevice_GetTexture2),
    F("_D3DTexture_GetSurfaceLevel2@8", D3DTexture_GetSurfaceLevel2),
    F("_D3DPalette_Lock2@8", D3DPalette_Lock2),
    F("_Get2DSurfaceDesc@12", Get2DSurfaceDesc),
    F("_Lock2DSurface@24", Lock2DSurface),
    /* the same in the 4xxx library, which keeps their C++ names */
    F("?Get2DSurfaceDesc@PixelJar@D3D@@YGXPAUD3DPixelContainer@@IPAU_D3DSURFACE_DESC@@@Z", Get2DSurfaceDesc),
    F("?Lock2DSurface@PixelJar@D3D@@YGXPAUD3DPixelContainer@@W4_D3DCUBEMAP_FACES@@IPAU_D3DLOCKED_RECT@@PBUtagRECT@@K@Z",
      Lock2DSurface),
    F("_Lock3DSurface@20", Lock3DSurface),
    F("_Direct3D_CreateDevice_LTCG@12", Direct3D_CreateDevice_LTCG),
    F("_D3D_CommonSetMultiSampleModeAndScale@8", D3D_Nop8),
    F("@D3D_CommonSetMultiSampleModeAndScale@8", D3D_FastNop),
    F("@D3D_BlockOnResource@4", D3D_BlockOnResourceFast),
    F("_D3D_CreateTexture@36", D3D_CreateTexture),
    /* _D3D_CreateStandAloneSurface@16 and _D3D_CommonSetRenderTarget@12 stay
       guest code: the library's own internal callers (Splinter Cell, 4928)
       pass surfaces the host can't resolve, and every model vanished; their
       push buffer output is interpreted fine. */
    F("_D3DDevice_GetPersistedSurface2@0", D3DDevice_GetPersistedSurface2),
    F("_D3DDevice_Present@16", D3DDevice_Present),
    F("_D3DDevice_BeginStateBig@4", D3DDevice_BeginPushLTCG),
    F("_D3DDevice_SetTextureStageStateNotInline2@12", D3DDevice_SetTextureStageStateNotInline),
    F("_D3D_CDevice_SetTextureStageStateNotInline@12", D3DDevice_SetTextureStageStateNotInline),
    F("_D3DDevice_SetVertexShaderConstantNotInline_LTCG@12", SetVertexShaderConstantNotInline_LTCG),
    F("_D3DDevice_EnableOverlay_LTCG@0", D3DDevice_EnableOverlay_LTCG),
    F("_D3DDevice_SetSoftDisplayFilter_LTCG@0", D3D_Nop0),
    F("_IDirect3DVertexBuffer8_Lock@20", D3DVertexBuffer_Lock),
    F("_D3D_KickOffAndWaitForIdle2@8", D3D_Nop8),
    F("@D3DDevice_SetVertexShaderConstantNotInline@12", SetVertexShaderConstantNotInline),
    F("@D3DDevice_SetVertexShaderConstant1@8", SetVertexShaderConstant1),
    F("@D3DDevice_SetVertexShaderConstant1Fast@8", SetVertexShaderConstant1),
    F("@D3DDevice_SetVertexShaderConstant4@8", SetVertexShaderConstant4),
    F("_D3DDevice_CreateVertexBuffer2@4", D3DDevice_CreateVertexBuffer2),
    F("_D3DDevice_CreateIndexBuffer2@4", D3DDevice_CreateIndexBuffer2),
    F("_D3DDevice_CreatePalette2@4", D3DDevice_CreatePalette2),
    F("_D3DCubeTexture_GetCubeMapSurface2@12", D3DCubeTexture_GetCubeMapSurface2),
    F("_D3DDevice_GetStreamSource2@8", D3DDevice_GetStreamSource2),
    F("_D3DVertexBuffer_Lock2@8", D3DVertexBuffer_Lock2),
    F("_D3DVertexBuffer_GetDesc@8", D3DVertexBuffer_GetDesc),
    F("_D3DDevice_BeginPush_4@4", D3DDevice_BeginPush2),
    F("_D3D_DestroyResource@4", D3D_DestroyResource),
    F("_D3DDevice_MultiplyTransform@8", D3DDevice_MultiplyTransform),
    F("_D3DDevice_GetModelView@4", D3DDevice_GetModelView),
    F("_D3DDevice_SetModelView@12", D3DDevice_SetModelView),
    F("_D3DDevice_GetViewportOffsetAndScale@8", D3DDevice_GetViewportOffsetAndScale),
    F("_D3DDevice_SetRenderTargetFast@12", D3DDevice_SetRenderTargetFast),
    F("@D3DDevice_SwitchTexture@12", D3DDevice_SwitchTexture),
    F("_D3DDevice_SetRenderState@8", SetRenderStateNotInline),
    F("_D3DDevice_SetRenderState2@8", SetRenderStateNotInline),
    F("_D3DDevice_SetRenderState_SampleAlpha@4", SetRenderState_SampleAlpha),
    F("_D3DDevice_Reset@4", D3DDevice_Reset),
    F("_D3DDevice_Suspend@0", D3D_Nop0),
    F("_D3DDevice_FlushVertexCache@0", D3D_Nop0),
    F("_D3DDevice_PrimeVertexCache@8", D3D_Nop8),
    F("_D3DDevice_SetStipple@4", D3DDevice_SetStipple),
    F("_D3DDevice_SetDepthClipPlanes@12", D3D_Nop12),
    F("_D3D_SetFence@4", D3D_SetFence),
    F("_D3D_BlockOnTime@8", D3D_BlockOnTime),
    F("_D3D_BlockOnResource@4", D3D_BlockOnResource),
    F("_D3D_KickOffAndWaitForIdle@0", D3D_Nop0),
    F("_D3D_CommonSetDebugRegisters@0", D3D_Nop0),
    F("_D3D_ClearStateBlockFlags@0", D3D_Nop0),
    F("_D3D_RecordStateBlock@4", D3D_Nop4),
    F("_D3D_UpdateProjectionViewportTransform@0", D3D_Nop0),
    F("_D3D_LazySetPointParams@4", D3D_Nop4),
    F("_D3D_SetTileNoWait@8", D3DDevice_SetTile),
    F("@D3D_CommonSetMultiSampleModeAndScale@8", D3D_FastNop),
    F("_D3D_SetPushBufferSize@8", Direct3D_SetPushBufferSize),
    F("_D3D_GetDeviceCaps@12", Direct3D_GetDeviceCaps),
    F("_D3D_CheckDeviceFormat@24", Direct3D_CheckDeviceFormat),
    F("_D3D_GetAdapterModeCount@8", D3D_GetAdapterModeCount2),
    F("_D3D_GetAdapterModeCount@4", Direct3D_GetAdapterModeCount),   /* 4928: (Adapter) only */
    F("_D3D_GetAdapterDisplayMode@8", Direct3D_GetAdapterDisplayMode),
    F("_D3D_EnumAdapterModes@12", Direct3D_EnumAdapterModes),
    F("_D3D_GetAdapterIdentifier@12", Direct3D_GetAdapterIdentifier),
    F("@D3DDevice_SetVertexShaderConstantNotInlineFast@12", SetVertexShaderConstantNotInline),
    F("_D3DDevice_GetVertexShaderConstant@12", D3DDevice_GetVertexShaderConstant),
    F("_D3DDevice_SetShaderConstantMode@4", D3DDevice_SetShaderConstantMode),
    F("_D3DDevice_GetShaderConstantMode@4", D3DDevice_GetShaderConstantMode),
    F("_D3DDevice_GetProjectionViewportMatrix@4", D3DDevice_GetProjectionViewportMatrix),
    F("_D3DDevice_GetDisplayMode@4", D3DDevice_GetDisplayMode),
    F("_D3DDevice_GetDeviceCaps@4", D3DDevice_GetDeviceCaps),
    F("_D3DDevice_GetCreationParameters@4", D3DDevice_GetCreationParameters),
    F("_Direct3D_GetDeviceCaps@12", Direct3D_GetDeviceCaps),
    F("_Direct3D_CheckDeviceFormat@24", Direct3D_CheckDeviceFormat),
    F("_Direct3D_CheckDeviceType@20", Direct3D_CheckDeviceType),
    F("_Direct3D_CheckDeviceMultiSampleType@20", Direct3D_CheckDeviceMultiSampleType),
    F("_Direct3D_CheckDepthStencilMatch@20", Direct3D_CheckDepthStencilMatch),
    F("_Direct3D_GetAdapterModeCount@4", Direct3D_GetAdapterModeCount),
    F("_Direct3D_GetAdapterDisplayMode@8", Direct3D_GetAdapterDisplayMode),
    F("_Direct3D_EnumAdapterModes@12", Direct3D_EnumAdapterModes),
    F("_Direct3D_GetAdapterIdentifier@12", Direct3D_GetAdapterIdentifier),
    F("_D3DDevice_SetScissors@12", D3DDevice_SetScissors),
    F("_D3DDevice_GetScissors@12", D3DDevice_GetScissors),
    F("_D3DDevice_InsertCallback@12", D3DDevice_InsertCallback),
    F("_D3DDevice_SetVerticalBlankCallback@4", D3DDevice_SetVerticalBlankCallback),
    F("_D3DDevice_SetSwapCallback@4", D3DDevice_SetSwapCallback),
    F("_D3DDevice_GetDisplayFieldStatus@4", D3DDevice_GetDisplayFieldStatus),
    F("_D3DDevice_PersistDisplay@0", D3DDevice_PersistDisplay),
    F("_D3DDevice_SetScreenSpaceOffset@8", D3DDevice_SetScreenSpaceOffset),
    F("_D3DDevice_SetBackBufferScale@8", D3DDevice_SetBackBufferScale),
    F("_D3DDevice_GetBackBufferScale@8", D3DDevice_GetBackBufferScale),
    F("_D3DDevice_SetDebugMarker@4", D3DDevice_SetDebugMarker),
    F("_D3DDevice_SetTile@8", D3DDevice_SetTile),
    F("_D3DDevice_GetTile@8", D3DDevice_GetTile),
    F("_D3DDevice_KickPushBuffer@0", D3DDevice_KickPushBuffer),
    F("_D3DDevice_CreatePushBuffer@12", D3DDevice_CreatePushBuffer),
    F("_D3DDevice_BeginPushBuffer@4", D3DDevice_BeginPushBuffer),
    F("_D3DDevice_EndPushBuffer@0", D3DDevice_EndPushBuffer),
    F("_D3DDevice_RunPushBuffer@8", D3DDevice_RunPushBuffer),
    F("_D3DDevice_GetPushBufferOffset@4", D3DDevice_GetPushBufferOffset),
    F("_D3DDevice_CreateFixup@8", D3DDevice_CreateFixup),
    F("_D3DDevice_BeginPush@8", D3DDevice_BeginPush),
    F("_D3DDevice_EndPush@4", D3DDevice_EndPush),
    F("_D3DDevice_Nop@0", D3DDevice_Nop),
    F("_D3DFixup_Reset@4", D3DFixup_Reset),
    F("_D3DFixup_GetSize@8", D3DFixup_GetSize),
    F("_D3DFixup_GetSpace@8", D3DFixup_GetSpace),
    F("_D3DPushBuffer_BeginFixup@12", D3DPushBuffer_BeginFixup),
    F("_D3DPushBuffer_EndFixup@4", D3DPushBuffer_EndFixup),
    F("_D3DPushBuffer_SetVertexShaderConstant@20", D3DPushBuffer_SetVertexShaderConstant),
    F("_D3DPushBuffer_SetTexture@16", D3DPushBuffer_SetTexture),
    F("_D3DPushBuffer_SetPalette@16", D3DPushBuffer_SetPalette),
    F("_D3DPushBuffer_SetRenderTarget@16", D3DPushBuffer_SetRenderTarget),
    F("_D3DPushBuffer_SetVertexShaderInput@20", D3DPushBuffer_SetVertexShaderInput),
    F("_D3DPushBuffer_RunPushBuffer@16", D3DPushBuffer_RunPushBuffer),
    F("_D3DPushBuffer_Verify@8", D3DPushBuffer_Verify),
    F("_D3DDevice_InsertFence@0", D3DDevice_InsertFence),
    F("_D3DDevice_IsFencePending@4", D3DDevice_IsFencePending),
    F("_D3DDevice_BlockOnFence@4", D3DDevice_BlockOnFence),
    F("_D3DDevice_SetGammaRamp@8", D3DDevice_SetGammaRamp),
    F("_D3DDevice_GetGammaRamp@4", D3DDevice_GetGammaRamp),
    F("_D3DDevice_EnableOverlay@4", D3DDevice_EnableOverlay),
    F("_D3DDevice_UpdateOverlay@20", D3DDevice_UpdateOverlay),
    F("_D3DDevice_GetOverlayUpdateStatus@0", D3DDevice_GetOverlayUpdateStatus),
    F("_D3DDevice_GetRasterStatus@4", D3DDevice_GetRasterStatus),
    F("_D3DDevice_GetPushDistance@4", D3DDevice_GetPushDistance),
    F("_D3DPERF_DumpCounterCycleInfo@12", D3DPERF_Zero12),
    F("_D3DPERF_DumpFrameRateInfo@0", D3DPERF_Zero),
    F("_D3DPERF_DumpPerfEvents@0", D3DPERF_Zero),
    F("_D3DPERF_DumpPerfProfCounts@0", D3DPERF_Zero),
    F("_D3DPERF_GetPushBufferBytesWritten@0", D3DPERF_Zero),
    F("_D3DPERF_HandlePresent@0", D3DPERF_Zero),
    F("_D3DPERF_PerfEventEnd@8", D3DPERF_Zero8),
    F("_D3DPERF_PerfEventStart@8", D3DPERF_Zero8),
    F("_D3DPERF_Reset@0", D3DPERF_Zero),
    F("_D3DPERF_StartCountingPerfEvent@4", D3DPERF_Zero4),
    F("_D3DPERF_StartPerfProfile@0", D3DPERF_Zero),
    F("_D3DPERF_SetShowFrameRateInterval@4", D3DPERF_Zero4),
    F("_D3DRDI_GetRamData@16", D3DPERF_Zero16),
    F("_PerfGetPushBufferDistance@8", D3DPERF_Zero8),
    F("_D3DResource_IsBusy@4", D3DResource_IsBusy),
    F("_D3DResource_BlockUntilNotBusy@4", D3DResource_BlockUntilNotBusy),
    F("_D3DResource_GetDevice@8", D3DResource_GetDevice),
    F("_D3DResource_MoveResourceMemory@8", D3DResource_MoveResourceMemory),
    F("_D3DResource_SetPrivateData@20", D3DResource_SetPrivateData),
    F("_D3DResource_GetPrivateData@16", D3DResource_GetPrivateData),
    F("_D3DResource_FreePrivateData@8", D3DResource_FreePrivateData),
    F("_D3DDevice_CreateVertexShader@16", D3DDevice_CreateVertexShader),
    F("_D3DDevice_DeleteVertexShader@4", D3DDevice_DeleteVertexShader),
    F("_D3DDevice_GetVertexShaderSize@8", D3DDevice_GetVertexShaderSize),
    F("_D3DDevice_GetVertexShaderType@8", D3DDevice_GetVertexShaderType),
    F("_D3DDevice_GetVertexShaderDeclaration@12", D3DDevice_GetVertexShaderDeclaration),
    F("_D3DDevice_GetVertexShaderFunction@12", D3DDevice_GetVertexShaderFunction),
    F("_D3DDevice_LoadVertexShader@8", D3DDevice_LoadVertexShader),
    F("_D3D_MakeRequestedSpace@8", D3D_MakeRequestedSpace),
    F("_D3DDevice_BeginPushLTCG@4", D3DDevice_BeginPushLTCG),
    F("_CDevice_SetStateVB@8", CDevice_SetStateVB),
    F("_D3D_LazySetState@0", D3D_LazySetState),
    F("_D3D_DacProgramGammaRamp@4", D3D_DacProgramGammaRamp),
    F("_CDevice_SetStateUP@4", CDevice_SetStateUP),
    F("_CDevice_KickOff@4", CDevice_KickOff),
    F("_D3DDevice_MakeSpace@0", D3DDevice_MakeSpace),
    F("_D3DDevice_LoadVertexShaderProgram@8", D3DDevice_LoadVertexShaderProgram),
    F("_D3DDevice_SelectVertexShader@8", D3DDevice_SelectVertexShader),
    F("_D3DDevice_RunVertexStateShader@8", D3DDevice_RunVertexStateShader),
    F("_D3DDevice_SetVertexShaderInput@12", D3DDevice_SetVertexShaderInput),
    F("_D3DDevice_SetVertexShaderInputDirect@12", D3DDevice_SetVertexShaderInputDirect),
    F("_D3DDevice_GetVertexShaderInput@12", D3DDevice_GetVertexShaderInput),
    F("_D3DDevice_SelectVertexShaderDirect@8", D3DDevice_SelectVertexShaderDirect),
    F("_D3DPushBuffer_SetVertexShaderInputDirect@20", D3DPushBuffer_SetVertexShaderInputDirect),
    F("_D3DPushBuffer_Jump@12", D3DPushBuffer_Jump),
    F("_D3DPushBuffer_SetModelView@20", D3DPushBuffer_SetModelView),
    F("_D3DPushBuffer_SetVertexBlendModelView@24", D3DPushBuffer_SetVertexBlendModelView),
    F("_D3DPushBuffer_EndVisibilityTest@12", D3DPushBuffer_EndVisibilityTest),
    F("_D3DDevice_GetDebugMarker@0", D3DDevice_GetDebugMarker),
    F("_D3DDevice_SetTileCompressionTagBits@16", D3DDevice_SetTileCompressionTagBits),
    F("_D3DDevice_GetTileCompressionTagBits@16", D3DDevice_GetTileCompressionTagBits),
    F("_D3DDevice_GetTileCompressionTags@8", D3DDevice_GetTileCompressionTags),
    F("_D3DDevice_SetCopyRectsState@8", D3DDevice_SetCopyRectsState),
    F("_D3DDevice_GetCopyRectsState@8", D3DDevice_GetCopyRectsState),
    F("_D3DDevice_SetOverscanColor@4", D3DDevice_SetOverscanColor),
    F("_D3DDevice_GetOverscanColor@0", D3DDevice_GetOverscanColor),
    F("_D3DDevice_EnableCC@4", D3DDevice_EnableCC),
    F("_D3DDevice_SendCC@12", D3DDevice_SendCC),
    F("_D3DDevice_GetCCStatus@8", D3DDevice_GetCCStatus),
    F("_D3DDevice_Resume@4", D3DDevice_Resume),
    F("_D3DIndexBuffer_GetDesc@8", D3DIndexBuffer_GetDesc),
    F("_D3DPalette_GetSize@4", D3DPalette_GetSize),
    F("_D3DDevice_GetPalette@8", D3DDevice_GetPalette),
    F("_D3DSurface_GetContainer@8", D3DSurface_GetContainer),
    F("_D3DCubeTexture_GetLevelDesc@12", D3DCubeTexture_GetLevelDesc),
    F("_D3DSurface_UnlockRect@4", D3DSurface_UnlockRect),
    F("_D3DDevice_GetScreenSpaceOffset@8", D3DDevice_GetScreenSpaceOffset),
    F("_D3DDevice_SetVertexBlendModelView@16", D3DDevice_SetVertexBlendModelView),
    F("_D3DDevice_GetVertexBlendModelView@12", D3DDevice_GetVertexBlendModelView),
    F("_D3DDevice_SetTextureStageState@12", D3DDevice_SetTextureStageStateNotInline),
    F("@D3DDevice_SwitchTexture_Fast@12", D3DDevice_SwitchTexture),
    F("_D3DDevice_Begin@4", D3DDevice_Begin),
    F("_D3DDevice_End@0", D3DDevice_End),
    F("_D3DDevice_SetVertexData2f@12", D3DDevice_SetVertexData2f),
    F("_D3DDevice_SetVertexData4f@20", D3DDevice_SetVertexData4f),
    F("_D3DDevice_SetVertexData2s@12", D3DDevice_SetVertexData2s),
    F("_D3DDevice_SetVertexData4s@20", D3DDevice_SetVertexData4s),
    F("_D3DDevice_SetVertexData4ub@20", D3DDevice_SetVertexData4ub),
    F("_D3DDevice_SetVertexDataColor@8", D3DDevice_SetVertexDataColor),
    F("_D3DDevice_CreatePixelShader@8", D3DDevice_CreatePixelShader),
    F("_D3DDevice_SetPixelShader@4", D3DDevice_SetPixelShader),
    F("_D3DDevice_SetPixelShaderProgram@4", D3DDevice_SetPixelShaderProgram),
    F("_D3DDevice_GetPixelShader@4", D3DDevice_GetPixelShader),
    F("_D3DDevice_DeletePixelShader@4", D3DDevice_DeletePixelShader),
    F("_D3DDevice_GetPixelShaderFunction@8", D3DDevice_GetPixelShaderFunction),
    F("_D3DDevice_SetPixelShaderConstant@12", D3DDevice_SetPixelShaderConstant),
    F("_D3DDevice_GetPixelShaderConstant@12", D3DDevice_GetPixelShaderConstant),
    { NULL, NULL },
};
