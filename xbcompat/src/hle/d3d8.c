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
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../xbcompat.h"
#include "hle.h"
#include "nv2a_shaders.h"

typedef ULONG UINT_;

#define D3D_OK 0
#define D3DERR_INVALIDCALL ((LONG)0x8876086C)

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
    D3DRS_ALPHAREF = 61, D3DRS_SRCBLEND = 62, D3DRS_DESTBLEND = 63, D3DRS_ZWRITEENABLE = 64,
    D3DRS_SHADEMODE = 66, D3DRS_COLORWRITEENABLE = 67, D3DRS_BLENDOP = 74,
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

static struct {
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
    ULONG fbo;                   /* framebuffer object for texture targets */
    struct { ULONG count; ULONG exclusive; D3DRECT rects[8]; } scissors;
    float screen_offset[2];
    float backbuffer_scale[2];
    ULONG multisample_type;      /* D3DPRESENT_PARAMETERS.MultiSampleType */
    ULONG flicker_filter;        /* SetFlickerFilter level, 0 (off) to 5 */
    bool soft_display;           /* SetSoftDisplayFilter (the encoder's luma filter) */
    ULONG constant_mode;
    float vs_const[192][4];      /* vertex shader constants, hardware numbering */
    float ps_const[16][4];
    ULONG pixel_shader;
    float zscale;                /* depth range of the target: 2^24-1 or 2^16-1 */
    PVOID vblank_callback, swap_callback;
    ULONG *miniport_swap_cb;     /* the device's m_pSwapCallback when the title inlines its setters */
    volatile ULONG vblank_count, vblank_at_swap;
    volatile int swapped;        /* a Swap since the last vertical blank */
    ULONG *cpu_time;             /* the device's m_CpuTime, when its layout is known */
    struct { PVOID fn; ULONG ctx; } callbacks[64];
    unsigned ncallbacks;
    struct D3DPalette *palettes[4];
    struct D3DPushBuffer *recording;   /* push buffer being recorded, if any */
    bool bb_cpu_dirty;           /* the title locked the back buffer and may have written it */
} d3d;

/* Push buffer recording (see the push buffer section). */
static void pb_record(unsigned op, const void *payload, unsigned bytes);
static void pb_record2(unsigned op, const void *head, unsigned head_bytes, const void *data, unsigned data_bytes);
enum {
    OP_RS = 1, OP_TSS, OP_VS_CONST, OP_VERTEX_SHADER, OP_TEXTURE, OP_STREAM, OP_INDICES, OP_TRANSFORM,
    OP_VIEWPORT, OP_DRAW, OP_DRAW_INDEXED, OP_DRAW_UP, OP_DRAW_INDEXED_UP, OP_BEGIN_END, OP_PIXEL_SHADER,
    OP_PS_CONST, OP_VS_INPUT, OP_RENDER_TARGET, OP_PALETTE, OP_RUN, OP_CLEAR, OP_LIGHT, OP_LIGHT_ENABLE,
    OP_MATERIAL, OP_RAW,
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

enum {
    D3DTSS_ADDRESSU = 0, D3DTSS_ADDRESSV = 1, D3DTSS_ADDRESSW = 2, D3DTSS_MAGFILTER = 3, D3DTSS_MINFILTER = 4,
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
static void create_window_framebuffer(void);
static void present_window_framebuffer(void);
static void restore_window_framebuffer(void);
static void backbuffer_read_begin(GLbitfield mask);
static void backbuffer_read_end(void);
static GLuint cur_program;   /* the GL program bound for draws (see use_program) */
static void program_off(void);
static GLuint backbuffer_copy_texture(ULONG data, ULONG w, ULONG h);
static void run_callbacks(void);
static void pusher_drain(void);
static void pusher_init(ULONG *dev);
static void sync_device_surfaces(void);
static bool device_layout_5849;   /* d3d.device is laid out like the 5849 CDevice */

static ULONG direct3d_object[4];

static PVOID NTAPI Direct3DCreate8(UINT_ SDKVersion)
{
    TRACE("Direct3DCreate8(%#x)", SDKVersion);
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

/* Frames presented so far (XBCOMPAT_INPUT_SCRIPT counts in these). */
ULONG d3d_frame_count(void) { return d3d.frame; }

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
    xlog("D3D: render states at %p, device at %p", (void *)d3d.render_state, (void *)d3d.device);
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
    d3d.miniport_swap_cb = (ULONG *)((uint8_t *)dev + off);
    KEVENT *ev = (KEVENT *)(d3d.miniport_swap_cb + 2);
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

/* A 60 Hz vertical blank, from the DPC thread (the GPU interrupt's DPC on an
   Xbox): sets the device's blank event and runs the title's callback. */
static void d3d_vblank(void)
{
    extern LONG NTAPI KeSetEvent(KEVENT *, LONG, BOOLEAN);
    ULONG n = ++d3d.vblank_count;
    ULONG flags = __atomic_exchange_n(&d3d.swapped, 0, __ATOMIC_ACQ_REL) ? 1 /* D3DVBLANK_SWAPDONE */ : 0;
    PVOID cb = d3d.vblank_callback;
    if (d3d.miniport_swap_cb) {
        KeSetEvent((KEVENT *)(d3d.miniport_swap_cb + 2), 1, 0);
        if (d3d.miniport_swap_cb[1]) cb = (PVOID)d3d.miniport_swap_cb[1];
    }
    if (cb) {
        ULONG data[3] = { n, d3d.frame, flags };
        ((void (CDECLAPI *)(ULONG *))cb)(data);
    }
}

static LONG NTAPI Direct3D_CreateDevice(UINT_ Adapter, ULONG DeviceType, PVOID pUnused, ULONG Flags,
                                       D3DPRESENT_PARAMETERS *pp, PVOID *ppDevice)
{
    d3d.width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    d3d.height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    d3d.multisample_type = pp->MultiSampleType;
    d3d.flicker_filter = 5;   /* what the Xbox's device init sets */
    d3d.soft_display = false;
    xlog("D3D: CreateDevice %ux%u, format %#x, depth %s, multisample %#x", d3d.width, d3d.height,
         pp->BackBufferFormat, pp->EnableAutoDepthStencil ? "yes" : "no", pp->MultiSampleType);

    av_title_starting();
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
    SDL_GL_SetSwapInterval(1);
    xlog("D3D: OpenGL %s on %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));

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
    create_window_framebuffer();
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
    g_vblank_hook = d3d_vblank;
    if (d3d.device_ptr) *d3d.device_ptr = (ULONG)dev;
    *ppDevice = dev;
    return D3D_OK;
}

/* ---- present and clears ---------------------------------------------- */

static void save_screenshot(const char *path)
{
    int w = d3d.width, h = d3d.height;
    uint8_t *px = malloc(w * h * 4);
    backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    backbuffer_read_end();
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    for (int y = 0; y < h; y++)
        memcpy((uint8_t *)s->pixels + y * s->pitch, px + (h - 1 - y) * w * 4, w * 4);
    if (SDL_SaveBMP(s, path) != 0) xlog("screenshot failed: %s", SDL_GetError());
    else xlog("D3D: saved frame %u to %s", d3d.frame, path);
    SDL_FreeSurface(s);
    free(px);
}

/* Debug aid: XBCOMPAT_DUMP_DRAWS=dir saves the current target after every
   draw of one frame (XBCOMPAT_DUMP_FRAME, default the first) as
   dir/drawNNN.bmp. */
/* Work counted for XBCOMPAT_LOG_FPS: what a slow frame spends its time on. */
static struct { unsigned draws, uploads, readbacks, shaders; } stats;
/* Host time per frame, in performance-counter ticks: draws (with
   XBCOMPAT_PROFILE_SYNC=1 each draw also waits for the GPU), push buffer
   replay (draws included), present + buffer swap, and pacing sleep. */
static struct { Uint64 draw, drain, swap, pace; } prof;
static int prof_sync = -1;

static void prof_draw_end(Uint64 t0)
{
    if (prof_sync < 0) { const char *e = getenv("XBCOMPAT_PROFILE_SYNC"); prof_sync = e && *e && *e != '0'; }
    if (prof_sync) glFinish();
    prof.draw += SDL_GetPerformanceCounter() - t0;
}

static void debug_dump_draw(void)
{
    stats.draws++;
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
static void draw_window_pixels(const uint8_t *tmp);
static void present_overlay(void);
static void restore_overlay(void);
static LONG NTAPI D3DDevice_PersistDisplay(void);

static void flush_cpu_backbuffer(void)
{
    if (!d3d.bb_cpu_dirty || !d3d.backbuffer) return;
    d3d.bb_cpu_dirty = false;
    ULONG w = d3d.width, h = d3d.height;
    ULONG pitch = d3d.backbuffer->Size ? ((d3d.backbuffer->Size >> 24) + 1) * 64 : w * 4;
    const uint8_t *px = (const uint8_t *)(d3d.backbuffer->Data | CONTIG_BASE);
    uint8_t *tmp = malloc(w * h * 4);
    for (ULONG y = 0; y < h; y++) memcpy(tmp + (h - 1 - y) * w * 4, px + y * pitch, w * 4);
    draw_window_pixels(tmp);
    free(tmp);
}

/* Replace the window's color buffer with BGRA pixels, bottom row first. */
static void draw_window_pixels(const uint8_t *tmp)
{
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
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    p_glWindowPos2i(0, 0);
    glDrawPixels(w, h, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
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
    while ((now = SDL_GetPerformanceCounter()) < t)
        SDL_Delay((Uint32)(((t - now) * 1000 + freq - 1) / freq));
}

static void wait_vblank(void)
{
    static Uint64 base;
    if (unpaced()) return;
    Uint64 now = SDL_GetPerformanceCounter(), period = SDL_GetPerformanceFrequency() / 60;
    if (!base) base = now;
    sleep_until(base + ((now - base) / period + 1) * period);
}

/* XBCOMPAT_LOG_FPS: log the frame rate every 5 seconds. */
static void log_fps(void)
{
    static int on = -1;
    static Uint64 since;
    static unsigned frames;
    if (on < 0) { const char *e = getenv("XBCOMPAT_LOG_FPS"); on = e && *e && *e != '0'; }
    if (!on) return;
    Uint64 now = SDL_GetTicks64();
    if (!since) since = now;
    frames++;
    if (now - since >= 5000) {
        static unsigned traps;
        xlog("D3D: %.1f fps; per frame %u draws, %u texture uploads, %u readbacks, %u shader compiles, %u guest traps",
             frames * 1000.0 / (double)(now - since), stats.draws / frames, stats.uploads / frames,
             stats.readbacks / frames, stats.shaders / frames, (g_guest_traps - traps) / frames);
        traps = g_guest_traps;
        double ms = 1000.0 / (double)SDL_GetPerformanceFrequency() / frames;
        double total = (double)(now - since) / frames;
        xlog("D3D: per frame %.1f ms: %.1f draws, %.1f push buffer (draws included), %.1f present+swap, "
             "%.1f pacing, %.1f rest (game code)%s", total, prof.draw * ms, prof.drain * ms, prof.swap * ms,
             prof.pace * ms, total - (prof.draw + prof.swap + prof.pace) * ms - (prof.drain > prof.draw ? (prof.drain - prof.draw) * ms : 0),
             prof_sync > 0 ? " [GPU synced per draw]" : "");
        memset(&prof, 0, sizeof(prof));
        memset(&stats, 0, sizeof(stats));
        since = now;
        frames = 0;
    }
}

static void pace_present(void)
{
    static Uint64 next;
    log_fps();
    if (unpaced()) return;
    Uint64 now = SDL_GetPerformanceCounter(), period = SDL_GetPerformanceFrequency() / 60;
    if (!next || now > next + period) next = now;   /* first frame, or fell behind: resync */
    sleep_until(next);
    prof.pace += SDL_GetPerformanceCounter() - now;
    next += period;
}

static ULONG NTAPI D3DDevice_Swap(ULONG Flags)
{
    pusher_drain();
    (void)Flags;
    flush_cpu_backbuffer();
    d3d.frame++;
    ke_frame_presented();
    run_callbacks();
    present_overlay();
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
    Uint64 t0 = SDL_GetPerformanceCounter();
    present_window_framebuffer();
    SDL_GL_SwapWindow(d3d.window);
    restore_window_framebuffer();
    prof.swap += SDL_GetPerformanceCounter() - t0;
    restore_overlay();
    pace_present();
    d3d.swapped = 1;
    PVOID swap_cb = d3d.miniport_swap_cb && d3d.miniport_swap_cb[0] ? (PVOID)d3d.miniport_swap_cb[0]
                                                                     : d3d.swap_callback;
    if (swap_cb) {
        /* D3DSWAPDATA: Swap, SwapVBlank, MissedVBlanks, TimeUntilSwapVBlank, TimeBetweenSwapVBlanks */
        ULONG vb = d3d.vblank_count, missed = vb - d3d.vblank_at_swap > 1 ? vb - d3d.vblank_at_swap - 1 : 0;
        ULONG data[5] = { d3d.frame, vb, missed, 0, 16667 };
        d3d.vblank_at_swap = vb;
        ((void (CDECLAPI *)(ULONG *))swap_cb)(data);
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            xlog("window closed");
            exit(0);
        }
    }
    /* A keyboard plugged in mid-game makes SDL take the fault signals back. */
    install_fault_handlers();
    if (g_exit_after_frames && (int)d3d.frame >= g_exit_after_frames) {
        xlog("D3D: %u frames presented, exiting", d3d.frame);
        /* Leave the way a title does: XBCOMPAT_PERSIST=1 persists the frame first. */
        if (getenv("XBCOMPAT_PERSIST")) D3DDevice_PersistDisplay();
        av_hand_over();
        exit(0);
    }
    return d3d.frame;
}

static void apply_viewport(void)
{
    D3DVIEWPORT8 *v = &d3d.viewport;
    /* GL rows run bottom-up: flip the window viewport; a texture target is
       drawn upside down instead (see apply_render_states). */
    if (d3d.rt_texture) glViewport(v->X, v->Y, v->Width, v->Height);
    else glViewport(v->X, d3d.rt_height - (v->Y + v->Height), v->Width, v->Height);
    glDepthRange(v->MinZ, v->MaxZ);
    if (d3d.scissors.count && !d3d.scissors.exclusive) {
        const D3DRECT *r = &d3d.scissors.rects[0];
        glEnable(GL_SCISSOR_TEST);
        if (d3d.rt_texture) glScissor(r->x1, r->y1, r->x2 - r->x1, r->y2 - r->y1);
        else glScissor(r->x1, d3d.rt_height - r->y2, r->x2 - r->x1, r->y2 - r->y1);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

static void NTAPI D3DDevice_Clear(ULONG Count, const D3DRECT *pRects, ULONG Flags, ULONG Color, float Z,
                                  ULONG Stencil)
{
    pusher_drain();
    if (d3d.recording) {
        ULONG h[5] = { Count, Flags, Color, 0, Stencil };
        memcpy(&h[3], &Z, 4);
        pb_record2(OP_CLEAR, h, sizeof(h), pRects, Count && pRects ? Count * sizeof(D3DRECT) : 0);
        return;
    }
    TRACE("D3D: Clear(%u rects, flags %#x, color %#x, z %g)", Count, Flags, Color, Z);
    flush_cpu_backbuffer();   /* earlier CPU writes go under the clear, not over it at Present */
    GLbitfield mask = 0;
    if (Flags & 0xF0) {
        glClearColor(((Color >> 16) & 255) / 255.0f, ((Color >> 8) & 255) / 255.0f, (Color & 255) / 255.0f,
                     (Color >> 24) / 255.0f);
        glColorMask(!!(Flags & 0x10), !!(Flags & 0x20), !!(Flags & 0x40), !!(Flags & 0x80));
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (Flags & 1) { glClearDepth(Z); glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    if (Flags & 2) { glClearStencil(Stencil); glStencilMask(0xFF); mask |= GL_STENCIL_BUFFER_BIT; }
    if (Count && pRects) {
        glEnable(GL_SCISSOR_TEST);
        for (ULONG i = 0; i < Count; i++) {
            const D3DRECT *r = &pRects[i];
            if (d3d.rt_texture) glScissor(r->x1, r->y1, r->x2 - r->x1, r->y2 - r->y1);
            else glScissor(r->x1, d3d.rt_height - r->y2, r->x2 - r->x1, r->y2 - r->y1);
            glClear(mask);
        }
        glDisable(GL_SCISSOR_TEST);
    } else {
        glClear(mask);
    }
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
    TRACE("CreateVertexBuffer(%u) = %p", Length, (void *)vb);
    return D3D_OK;
}

static void NTAPI D3DVertexBuffer_Lock(D3DResource *vb, UINT_ Offset, UINT_ Size, PVOID *ppbData, ULONG Flags)
{
    (void)Size; (void)Flags;
    *ppbData = (UCHAR *)resource_data(vb) + Offset;
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
    TRACE("D3D: Release(%p) type %#x refs %u from %p", (void *)r, r->Common & D3DCOMMON_TYPE_MASK,
          (r->Common & D3DCOMMON_REFCOUNT_MASK) - 1, __builtin_return_address(0));
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

static void NTAPI D3DResource_Register(D3DResource *r, PVOID base)
{
    ULONG mem = (ULONG)base + r->Data, type = r->Common & D3DCOMMON_TYPE_MASK;
    /* Push and index buffers keep a virtual address (the CPU reads them);
       everything else a GPU (physical) one. */
    r->Data = type == D3DCOMMON_TYPE_PUSHBUFFER || type == D3DCOMMON_TYPE_INDEXBUFFER ? mem : (mem & 0x7FFFFFFF);
}

/* ---- textures ---------------------------------------------------------- */


typedef struct tex_entry {
    ULONG data, format, size;
    GLuint id;
    GLenum target;
    struct tex_entry *next;
} tex_entry;

static tex_entry *tex_cache;

/* Forget the GL copy of a texture the title is about to modify. */
static void tex_invalidate(ULONG data)
{
    tex_entry **pp = &tex_cache;
    while (*pp) {
        tex_entry *e = *pp;
        if (e->data == data) {
            glDeleteTextures(1, &e->id);
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
static void (APIENTRY *p_glBeginQuery)(GLenum, GLuint);
static void (APIENTRY *p_glEndQuery)(GLenum);
static void (APIENTRY *p_glGetQueryObjectuiv)(GLuint, GLenum, GLuint *);
static void (APIENTRY *p_glPointParameterf)(GLenum, GLfloat);
static void (APIENTRY *p_glUniform2fv)(GLint, GLsizei, const GLfloat *);

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

static void upload_image3(GLenum target, ULONG fmt, ULONG w, ULONG h, ULONG d, ULONG pitch, const uint8_t *src)
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
        if (d24) glTexImage2D(target, 0, 0x88F0 /* GL_DEPTH24_STENCIL8 */, w, h, 0, 0x84F9 /* GL_DEPTH_STENCIL */,
                              0x84FA /* GL_UNSIGNED_INT_24_8 */, px);
        else glTexImage2D(target, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, px);
        free(px);
        return;
    }
    /* conv: 1 = V8U8 (bump), 2 = L6V5U5 (bump), both expanded to RGBA8 with
       du/dv as the signed bytes' bit patterns in r/g and luminance in b;
       3 = YUY2 and 4 = UYVY (video frames), converted to RGB with BT.601;
       5 = A8, which the NV2A samples as white with that alpha. */
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
        { 0x06, 4, true,  GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, false, 0 },   /* A8R8G8B8 */
        { 0x07, 4, true,  GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, true, 0 },    /* X8R8G8B8 */
        { 0x12, 4, false, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, false, 0 },   /* LIN_A8R8G8B8 */
        { 0x1E, 4, false, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, true, 0 },    /* LIN_X8R8G8B8 */
        { 0x05, 2, true,  GL_RGB,  GL_UNSIGNED_SHORT_5_6_5, false, 0 },       /* R5G6B5 */
        { 0x11, 2, false, GL_RGB,  GL_UNSIGNED_SHORT_5_6_5, false, 0 },       /* LIN_R5G6B5 */
        { 0x02, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, false, 0 }, /* A1R5G5B5 */
        { 0x03, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, true, 0 },  /* X1R5G5B5 */
        { 0x10, 2, false, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, false, 0 }, /* LIN_A1R5G5B5 */
        { 0x04, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, false, 0 }, /* A4R4G4B4 */
        { 0x1D, 2, false, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, false, 0 }, /* LIN_A4R4G4B4 */
        { 0x00, 1, true,  GL_LUMINANCE, GL_UNSIGNED_BYTE, false, 0 },         /* L8 */
        { 0x13, 1, false, GL_LUMINANCE, GL_UNSIGNED_BYTE, false, 0 },         /* LIN_L8 */
        { 0x19, 1, true,  GL_RGBA, GL_UNSIGNED_BYTE, false, 5 },              /* A8 */
        { 0x1F, 1, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 5 },              /* LIN_A8 */
        { 0x1A, 2, true,  GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, false, 0 },   /* A8L8 */
        { 0x20, 2, false, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, false, 0 },   /* LIN_A8L8 */
        { 0x24, 2, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 3 },            /* YUY2 */
        { 0x25, 2, false, GL_RGBA, GL_UNSIGNED_BYTE, false, 4 },            /* UYVY */
    };

    if ((fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) && target != GL_TEXTURE_3D) {
        /* DXT1/3/5 are stored linearly in 4x4 blocks, like on the PC. */
        static const GLenum dxt[] = { 0x83F1, 0x83F2, 0x83F3 };  /* GL_COMPRESSED_RGBA_S3TC_DXT{1,3,5}_EXT */
        GLenum internal = dxt[fmt == 0x0C ? 0 : fmt == 0x0E ? 1 : 2];
        ULONG block = fmt == 0x0C ? 8 : 16;
        ULONG bytes = ((w + 3) / 4) * ((h + 3) / 4) * block;
        glCompressedTexImage2D(target, 0, internal, w, h, 0, bytes, src);
        return;
    }
    unsigned i = 0;
    while (i < sizeof(table) / sizeof(table[0]) && table[i].fmt != fmt) i++;
    if (i == sizeof(table) / sizeof(table[0])) {
        xlog("D3D: texture format %#x is not supported yet%s", fmt, target == GL_TEXTURE_3D ? " for volumes" : "");
        uint32_t magenta = 0xFFFF00FF;
        if (target == GL_TEXTURE_3D) glTexImage3D(target, 0, GL_RGBA, 1, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &magenta);
        else glTexImage2D(target, 0, GL_RGBA, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &magenta);
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
    if (table[i].conv == 5) {
        uint8_t *rgba = malloc(w * h * d * 4);
        for (ULONG k = 0; k < w * h * d; k++) {
            rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = 0xFF;
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
                o[0] = v & 0xFF; o[1] = v >> 8; o[2] = 0xFF;
            } else {
                int du = (int)(v << 27) >> 27, dv = (int)((v >> 5) << 27) >> 27;   /* signed 5-bit */
                o[0] = (uint8_t)(int8_t)(du * 127 / 15 < -128 ? -128 : du * 127 / 15);
                o[1] = (uint8_t)(int8_t)(dv * 127 / 15 < -128 ? -128 : dv * 127 / 15);
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
        glTexImage3D(target, 0, table[i].force_alpha ? GL_RGB : GL_RGBA, w, h, d, 0, table[i].gl_fmt, table[i].gl_type, px);
    else
        glTexImage2D(target, 0, table[i].force_alpha ? GL_RGB : GL_RGBA, w, h, 0, table[i].gl_fmt, table[i].gl_type, px);
    free(px);
}

static void upload_image(GLenum target, ULONG fmt, ULONG w, ULONG h, ULONG pitch, const uint8_t *src)
{
    upload_image3(target, fmt, w, h, 1, pitch, src);
}

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
static void (APIENTRY *p_glBindFramebuffer)(GLenum, GLuint);   /* loaded with the other FBO functions */

/* Copy the window's color or depth/stencil buffer into a device surface's
   memory (top row first), the way the NV2A keeps them in RAM. */
static void readback_surface(D3DSurface *s, bool depth)
{
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

static GLuint texture_for(D3DPixelContainer *t)
{
    /* A texture header over the back buffer or the depth buffer (titles
       build these with XGSetTextureHeader to filter the frame): read the
       current pixels back and upload them fresh. */
    ULONG va = t->res.Data | CONTIG_BASE;
    bool on_color = d3d.backbuffer && va == (d3d.backbuffer->Data | CONTIG_BASE) && t != (D3DPixelContainer *)d3d.backbuffer;
    bool on_depth = d3d.depth && va == (d3d.depth->Data | CONTIG_BASE) && t != (D3DPixelContainer *)d3d.depth;
    if (on_color) {
        /* A 32-bit view of the back buffer (Phantom Dust filters the frame
           through three every frame): copy it on the GPU, no readback. */
        ULONG fmt = (t->Format >> 8) & 0xFF, w, h, pitch;
        container_size(t, &w, &h, &pitch);
        GLuint id = fmt == 0x12 || fmt == 0x1E ? backbuffer_copy_texture(t->res.Data, w, h) : 0;
        if (id) return id;
    }
    if (on_color || on_depth) {
        readback_surface(on_color ? d3d.backbuffer : d3d.depth, on_depth);
        tex_invalidate(t->res.Data);
    }
    static int nocache = -1;
    if (nocache < 0) nocache = getenv("XBCOMPAT_NO_TEXCACHE") != NULL;
    if (nocache) tex_invalidate(t->res.Data);
    for (tex_entry *e = tex_cache; e; e = e->next)
        if (e->data == t->res.Data && e->format == t->Format && e->size == t->Size) return e->id;

    ULONG fmt = (t->Format >> 8) & 0xFF, w, h, pitch;
    container_size(t, &w, &h, &pitch);
    if (!t->Size) pitch = 0;
    const uint8_t *src = (const uint8_t *)(t->res.Data | CONTIG_BASE);
    GLenum target = tex_target(t);

    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(target, id);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (target == GL_TEXTURE_CUBE_MAP) {
        /* Six faces, each a whole mip chain, 128-byte aligned. */
        ULONG face = (level_offset(t, level_count(t)) + 127) & ~127u;
        for (int f = 0; f < 6; f++)
            upload_image(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, fmt, w, h, pitch, src + f * face);
    } else if (target == GL_TEXTURE_3D) {
        upload_image3(GL_TEXTURE_3D, fmt, w, h, 1u << ((t->Format >> 28) & 0xF), pitch, src);
    } else {
        upload_image(GL_TEXTURE_2D, fmt, w, h, pitch, src);
    }
    stats.uploads++;
    TRACE("D3D: uploaded %ux%u texture format %#x%s", w, h, fmt, target == GL_TEXTURE_CUBE_MAP ? " (cube)" : "");
    debug_dump_texture(target, t->res.Data, fmt, w, h);

    tex_entry *e = malloc(sizeof(*e));
    *e = (tex_entry){ t->res.Data, t->Format, t->Size, id, target, tex_cache };
    tex_cache = e;
    return id;
}

static void NTAPI D3DDevice_SetTexture(ULONG Stage, D3DResource *t)
{
    if (d3d.recording) { ULONG a[2] = { Stage, (ULONG)t }; pb_record(OP_TEXTURE, a, sizeof(a)); }
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
static bool have_combine3(void)
{
    static int have = -1;
    if (have < 0) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        have = ext && (strstr(ext, "GL_ATI_texture_env_combine3") || strstr(ext, "GL_NV_texture_env_combine4"));
        if (!have) xlog("D3D: no GL_ATI_texture_env_combine3; triadic texture ops fall back to MODULATE");
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

/* Map one D3D texture op onto GL_COMBINE for either RGB or alpha. */
static void combine(bool alpha, ULONG op, ULONG arg0, ULONG arg1, ULONG arg2)
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
    return f == 1 ? GL_NEAREST : GL_LINEAR;   /* D3DTEXF_POINT; no mip levels are uploaded */
}

/* Wrap and filter modes of stage `s`, applied to the texture bound to `target`. */
static void apply_sampler(GLenum target, int s)
{
    glTexParameteri(target, GL_TEXTURE_WRAP_S, gl_wrap(TSS(s, D3DTSS_ADDRESSU)));
    glTexParameteri(target, GL_TEXTURE_WRAP_T, gl_wrap(TSS(s, D3DTSS_ADDRESSV)));
    if (target != GL_TEXTURE_2D) glTexParameteri(target, GL_TEXTURE_WRAP_R, gl_wrap(TSS(s, D3DTSS_ADDRESSW)));
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, gl_filter(TSS(s, D3DTSS_MAGFILTER)));
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, gl_filter(TSS(s, D3DTSS_MINFILTER)));
    float border[4];
    color4(border, TSS(s, D3DTSS_BORDERCOLOR));
    glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, border);
    D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
    if (target == GL_TEXTURE_2D && t && is_depth_format((t->Format >> 8) & 0xFF)) {
        /* The NV2A compares the stage's r/q with the stored depth as
           "stored SHADOWFUNC r"; GL compares "r FUNC stored", so mirror it.
           The D3DCMP values are the GL enums. */
        static const GLenum mirror[8] = { GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL,
                                          GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS };
        glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, mirror[RS(D3DRS_SHADOWFUNC) & 7]);
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
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_CUBE_MAP);
        glDisable(GL_TEXTURE_3D);
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        bool sprite = s == 3 && RS(D3DRS_POINTSPRITEENABLE);   /* point sprites always use stage 3 */
        glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, sprite);
        TRACE("D3D: stage %d texture %p colorop %u(%u,%u) alphaop %u(%u,%u) tfactor %#x", s, (void *)t,
              TSS(s, D3DTSS_COLOROP), TSS(s, D3DTSS_COLORARG1), TSS(s, D3DTSS_COLORARG2), TSS(s, D3DTSS_ALPHAOP),
              TSS(s, D3DTSS_ALPHAARG1), TSS(s, D3DTSS_ALPHAARG2), RS(D3DRS_TEXTUREFACTOR));
        if (TSS(s, D3DTSS_COLOROP) == 1 || (ended && !sprite)) { ended = true; continue; }
        GLenum target = t ? tex_target(t) : GL_TEXTURE_2D;
        glEnable(target);
        if (t) {
            glBindTexture(target, texture_for(t));
            apply_sampler(target, s);
        } else {
            /* A stage with no texture still combines DIFFUSE, CURRENT and
               TFACTOR on the Xbox; run it with a white texture. */
            glBindTexture(GL_TEXTURE_2D, white_texture());
        }
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        float tf[4];
        color4(tf, RS(D3DRS_TEXTUREFACTOR));
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tf);
        combine(false, TSS(s, D3DTSS_COLOROP), TSS(s, D3DTSS_COLORARG0), TSS(s, D3DTSS_COLORARG1), TSS(s, D3DTSS_COLORARG2));
        combine(true, TSS(s, D3DTSS_ALPHAOP), TSS(s, D3DTSS_ALPHAARG0), TSS(s, D3DTSS_ALPHAARG1), TSS(s, D3DTSS_ALPHAARG2));
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
    static const float sx[4] = { 1, 0, 0, 0 }, sy[4] = { 0, 1, 0, 0 }, sz[4] = { 0, 0, 1, 0 };
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();   /* eye-linear planes are given in eye space */
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
        if (on && t && t->Size) {
            /* Linear textures are addressed in texels; GL wants [0,1]. */
            ULONG w, h, pitch;
            container_size(t, &w, &h, &pitch);
            for (int i = 0; i < 4; i++) { m[i][0] /= w; m[i][1] /= h; }
        }
        glMatrixMode(GL_TEXTURE);
        glLoadMatrixf(&m[0][0]);
        glMatrixMode(GL_MODELVIEW);
    }
    glPopMatrix();
    p_glActiveTexture(GL_TEXTURE0);
}

/* ---- state setters ----------------------------------------------------- */

static void NTAPI D3DDevice_SetStreamSource(UINT_ Stream, D3DResource *vb, UINT_ Stride)
{
    if (d3d.recording) { ULONG a[3] = { Stream, (ULONG)vb, Stride }; pb_record(OP_STREAM, a, sizeof(a)); }
    if (Stream >= 16) return;
    d3d.streams[Stream].vb = vb;
    d3d.streams[Stream].stride = Stride;
}

static void NTAPI D3DDevice_SetVertexShader(ULONG Handle)
{
    if (d3d.recording) pb_record(OP_VERTEX_SHADER, &Handle, 4);
    d3d.vertex_shader = Handle;
}

static void NTAPI D3DDevice_SetTransform(ULONG State, const D3DMATRIX *m)
{
    if (d3d.recording) pb_record2(OP_TRANSFORM, &State, 4, m, sizeof(*m));
    if (State < 10) d3d.transforms[State] = *m;
}

static void NTAPI D3DDevice_SetViewport(const D3DVIEWPORT8 *v)
{
    if (d3d.recording) { ULONG has = v != NULL; pb_record2(OP_VIEWPORT, &has, 4, v, v ? sizeof(*v) : 0); }
    if (v) d3d.viewport = *v;
    else d3d.viewport = (D3DVIEWPORT8){ 0, 0, d3d.width, d3d.height, 0, 1 };
    d3d.scissors.count = 0;   /* the library's SetViewport ends with SetScissors(0, 0, NULL) */
}

/* The NV097 method each simple state from D3DRS_ZFUNC to
   D3DRS_SOLIDOFFSETENABLE writes; their values go to the GPU unchanged. */
static const USHORT simple_state_method[] = {
    0x354, 0x33C, 0x304, 0x300, 0x340, 0x344, 0x348, 0x35C, 0x310, 0x37C, 0x358, 0x370, 0x374,
    0x364, 0x368, 0x36C, 0x360, 0x350, 0x34C, 0x9F8, 0x384, 0x388, 0x318, 0x31C, 0x320,
};

static void FASTCALL SetRenderState_Simple(ULONG Method, ULONG Value)
{
    /* Inline header code usually stores the value in D3D__RenderState
       itself, but LTCG builds can drop or defer that store; record it from
       the method too. */
    Method &= 0x1FFC;
    for (unsigned i = 0; i < sizeof(simple_state_method) / sizeof(simple_state_method[0]); i++)
        if (simple_state_method[i] == Method) { RS(D3DRS_ZFUNC + i) = Value; return; }
}

/* A render state method LTCG code wrote into the push buffer itself. */
static bool pb_render_state(ULONG m, ULONG d)
{
    for (unsigned i = 0; i < sizeof(simple_state_method) / sizeof(simple_state_method[0]); i++)
        if (simple_state_method[i] == m) { RS(D3DRS_ZFUNC + i) = d; return true; }
    if (m == 0x30C) {   /* NV097_SET_DEPTH_TEST_ENABLE */
        RS(D3DRS_ZENABLE) = d ? (RS(D3DRS_ZENABLE) ? RS(D3DRS_ZENABLE) : 1) : 0;
        return true;
    }
    if (m == 0x32C) { RS(D3DRS_STENCILENABLE) = d; return true; }   /* NV097_SET_STENCIL_TEST_ENABLE */
    return false;
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

static void apply_render_states(bool pretransformed, bool has_normal)
{
    flush_cpu_backbuffer();
    apply_viewport();

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
        /* The offset counts steps of the Xbox depth format; GL's of a 24-bit buffer. */
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(rs_float(D3DRS_POLYGONOFFSETZSLOPESCALE),
                        rs_float(D3DRS_POLYGONOFFSETZOFFSET) * 16777215.0f / d3d.target_zmax);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (RS(D3DRS_ALPHABLENDENABLE)) {
        glEnable(GL_BLEND);
        glBlendFunc(RS(D3DRS_SRCBLEND), RS(D3DRS_DESTBLEND));
    } else {
        glDisable(GL_BLEND);
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
    TRACE("D3D: points size %g scale %u sprite %u min %g max %g", psize, RS(D3DRS_POINTSCALEENABLE),
          RS(D3DRS_POINTSPRITEENABLE), pmin, pmax);

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
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        if (d3d.rt_texture) glOrtho(0, d3d.viewport.Width, 0, d3d.viewport.Height, 0, -1);
        else glOrtho(0, d3d.viewport.Width, d3d.viewport.Height, 0, 0, -1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(-(float)d3d.viewport.X, -(float)d3d.viewport.Y, 0);
    } else {
        /* D3D row-vector matrices loaded as-is are the GL column-vector
           transforms.  D3D clip space has z in [0,w]; GL wants [-w,w]. */
        static const D3DMATRIX zfix = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 2, 0 }, { 0, 0, -1, 1 } } };
        D3DMATRIX proj, modelview;
        mat_mul(&proj, &d3d.transforms[1], &zfix);
        if (d3d.rt_texture) for (int i = 0; i < 4; i++) proj.m[i][1] = -proj.m[i][1];
        mat_mul(&modelview, &d3d.transforms[6], &d3d.transforms[0]);
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
    LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glBindAttribLocation);
    LOAD(glLinkProgram); LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog); LOAD(glUseProgram);
    LOAD(glDeleteShader); LOAD(glDeleteProgram); LOAD(glGetUniformLocation); LOAD(glUniform4fv);
    LOAD(glUniform1f); LOAD(glUniform1i); LOAD(glEnableVertexAttribArray); LOAD(glDisableVertexAttribArray);
    LOAD(glVertexAttribPointer); LOAD(glVertexAttrib4fv); LOAD(glActiveTexture); LOAD(glClientActiveTexture);
    LOAD(glMultiTexCoord4fv); LOAD(glUniform2fv); LOAD(glPointParameterfv); LOAD(glPointParameterf);
    LOAD(glSecondaryColorPointer);
    LOAD(glGenQueries); LOAD(glBeginQuery); LOAD(glEndQuery); LOAD(glGetQueryObjectuiv);
#undef LOAD
}

static GLuint compile_shader(GLenum kind, const char *src)
{
    GLuint sh = p_glCreateShader(kind);
    TRACE("D3D: compiling %s shader %u", kind == GL_VERTEX_SHADER ? "vertex" : "fragment", sh);
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
    TRACE("D3D: linking program %u (vs %u, fs %u)", prog, vs, fs);
    if (vs) p_glAttachShader(prog, vs);
    if (fs) p_glAttachShader(prog, fs);
    for (int i = 0; i < 16; i++) {
        char name[4];
        snprintf(name, sizeof(name), "v%d", i);
        p_glBindAttribLocation(prog, i, name);
    }
    p_glLinkProgram(prog);
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
    float (*consts)[4];      /* D3DVSD_CONST data: slot, then values */
    int *const_slots;
    unsigned nconsts;
    GLuint vs;               /* compiled vertex shader, 0 until first use */
    bool failed;
    ULONG stream_mask;
    int tess_normal_in, tess_normal_out, tess_uv_out;   /* tessellator outputs, -1 if unused */
} vshader;

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
    if (func) {
        /* The blob starts with one header dword whose high word counts instructions. */
        unsigned n = func[0] >> 16;
        if (!n || n > 136) n = 136;
        sh->ninstr = n;
        sh->code = malloc(n * 16);
        memcpy(sh->code, func + 1, n * 16);
    }
    *handle = (ULONG)sh | 1;
    TRACE("CreateVertexShader: %u instructions, usage %#x = %#x", sh->ninstr, usage, *handle);
    return D3D_OK;
}

/* A linked program: a vertex shader object and a fragment shader object
   (0 = fixed function for that stage) with its uniform locations. */
typedef struct program_entry {
    GLuint vs, fs, prog;
    GLint loc_c, loc_flip_y;
    GLint loc_tex[4], loc_cube[4], loc_vol[4], loc_tex_scale, loc_c0, loc_c1, loc_fc0, loc_fc1,
          loc_bump_env, loc_bump_lum;
    float (*last_c)[4];   /* the constants last uploaded to this program */
    struct program_entry *next;
} program_entry;

static program_entry *programs;

static void NTAPI D3DDevice_DeleteVertexShader(ULONG handle)
{
    if (!(handle & 1)) return;
    vshader *sh = (vshader *)(handle & ~1u);
    if (sh->vs) {
        /* GL reuses the name, so programs linked with it must go too. */
        for (program_entry **pp = &programs; *pp;) {
            program_entry *e = *pp;
            if (e->vs != sh->vs) { pp = &e->next; continue; }
            if (e->prog && e->prog == cur_program) program_off();
            if (e->prog) p_glDeleteProgram(e->prog);
            *pp = e->next;
            free(e->last_c);
            free(e);
        }
        p_glDeleteShader(sh->vs);
    }
    free(sh->code);
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

static LONG NTAPI D3DDevice_GetVertexShaderDeclaration(ULONG h, void *data, ULONG *size) { return D3DERR_INVALIDCALL; }
static LONG NTAPI D3DDevice_GetVertexShaderFunction(ULONG h, void *data, ULONG *size) { return D3DERR_INVALIDCALL; }

/* Vertex program memory (136 instruction slots), used for state shaders and
   for SelectVertexShader(NULL, address); ordinary programs are compiled from
   their own copy of the microcode. */
static uint32_t vp_mem[136][4];

static void vp_load(const uint32_t *code, unsigned n, ULONG address)
{
    if (address >= 136) return;
    if (n > 136 - address) n = 136 - address;
    memcpy(vp_mem[address], code, n * 16);
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
static void NTAPI D3DDevice_SelectVertexShader(ULONG handle, ULONG address)
{
    if (d3d.recording && handle) pb_record(OP_VERTEX_SHADER, &handle, 4);
    /* The shader was loaded into program memory earlier; just use it. */
    if (handle) d3d.vertex_shader = handle;
}
static void NTAPI D3DDevice_RunVertexStateShader(ULONG address, const float *data)
{
    if (address >= 136) return;
    vsh_run_state(vp_mem[address], 136 - address, data, d3d.vs_const);
}

static void NTAPI D3DDevice_SetVertexShaderInput(ULONG handle, UINT_ count, const ULONG *inputs)
{
    if (d3d.recording) { ULONG h[2] = { handle, count }; pb_record2(OP_VS_INPUT, h, 8, inputs, count * 12); }
    /* D3DSTREAM_INPUT: VertexBuffer, Stride, Offset */
    for (UINT_ i = 0; i < count && i < 16; i++) {
        d3d.streams[i].vb = (D3DResource *)inputs[i * 3];
        d3d.streams[i].stride = inputs[i * 3 + 1];
    }
    if (handle) d3d.vertex_shader = handle;
}


static GLuint vertex_shader_object(vshader *sh)
{
    if (sh->vs || sh->failed) return sh->vs;
    char *src = vsh_translate(sh->code, sh->ninstr);
    if (!src) {
        xlog("D3D: vertex program (%u instructions) could not be translated", sh->ninstr);
        sh->failed = true;
        return 0;
    }
    sh->vs = compile_shader(GL_VERTEX_SHADER, src);
    free(src);
    if (!sh->vs) sh->failed = true;
    return sh->vs;
}

static program_entry *program_for(GLuint vs, GLuint fs)
{
    for (program_entry *e = programs; e; e = e->next)
        if (e->vs == vs && e->fs == fs) return e;
    program_entry *e = calloc(1, sizeof(*e));
    e->vs = vs;
    e->fs = fs;
    e->prog = link_program(vs, fs);
    if (e->prog) {
#define U(name) p_glGetUniformLocation(e->prog, name)
        e->loc_c = U("c");
        e->loc_flip_y = U("flip_y");
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
#undef U
    }
    e->next = programs;
    programs = e;
    return e;
}

/* ---- pixel shaders: fragment programs from the combiner state ---------- */

/* Fragment programs are cached on the render states psh_translate reads,
   minus the combiner constants, which are uniforms. */
typedef struct fshader_entry {
    uint32_t key[64];
    GLuint fs;
    struct fshader_entry *next;
} fshader_entry;

static fshader_entry *fshaders;

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
        else if (t && m >= 0x01 && m <= 0x03)
            m = target == GL_TEXTURE_CUBE_MAP ? 0x03 : target == GL_TEXTURE_3D ? 0x02 : 0x01;
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

static void fshader_key(uint32_t *key)
{
    memset(key, 0, 64 * 4);
    memcpy(key, d3d.render_state, D3DRS_PS_MAX * 4);
    for (int i = D3DRS_PSCONSTANT0_0; i <= D3DRS_PSCONSTANT1_7; i++) key[i] = 0;
    key[D3DRS_PSFINALCOMBINERCONSTANT0] = key[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
    key[57] = adjusted_texture_modes();
    key[58] = RS(D3DRS_FOGENABLE);
    key[59] = RS(D3DRS_FOGTABLEMODE);
    key[60] = shadow_stages();
}

static GLuint fragment_shader_object(void)
{
    uint32_t key[64];
    fshader_key(key);
    for (fshader_entry *e = fshaders; e; e = e->next)
        if (!memcmp(e->key, key, sizeof(key))) return e->fs;
    fshader_entry *e = calloc(1, sizeof(*e));
    memcpy(e->key, key, sizeof(key));
    ULONG modes = RS(D3DRS_PSTEXTUREMODES);
    RS(D3DRS_PSTEXTUREMODES) = key[57];
    ULONG rs[D3DRS_MAX];   /* psh.c reads 4400 numbering */
    for (int i = 0; i < D3DRS_MAX; i++) rs[i] = RS(i);
    psh_shadow_stages = key[60];
    char *src = psh_translate(rs);
    RS(D3DRS_PSTEXTUREMODES) = modes;
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
static void apply_shader_textures(const program_entry *e)
{
    float scale[4][4];
    for (int s = 0; s < 4; s++) {
        p_glActiveTexture(GL_TEXTURE0 + s);
        scale[s][0] = scale[s][1] = scale[s][2] = scale[s][3] = 1;
        D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[s];
        if (!t) {
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            glBindTexture(GL_TEXTURE_3D, 0);
            continue;
        }
        GLenum target = tex_target(t);
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
        /* The program declares at most one sampler per stage, of the type its
           texture mode wants; point it at unit s whatever is bound there. */
        if (e->loc_tex[s] >= 0) p_glUniform1i(e->loc_tex[s], s);
        if (e->loc_cube[s] >= 0) p_glUniform1i(e->loc_cube[s], s);
        if (e->loc_vol[s] >= 0) p_glUniform1i(e->loc_vol[s], s);
    }
    p_glActiveTexture(GL_TEXTURE0);
    if (e->loc_tex_scale >= 0) p_glUniform4fv(e->loc_tex_scale, 4, &scale[0][0]);
}


/* The per-draw inputs of a fragment program. */
static void upload_ps_uniforms(const program_entry *e)
{
    float c[8][4];
    if (e->loc_c0 >= 0) {
        for (int i = 0; i < 8; i++) color4(c[i], RS(D3DRS_PSCONSTANT0_0 + i));
        p_glUniform4fv(e->loc_c0, 8, &c[0][0]);
    }
    if (e->loc_c1 >= 0) {
        for (int i = 0; i < 8; i++) color4(c[i], RS(D3DRS_PSCONSTANT1_0 + i));
        p_glUniform4fv(e->loc_c1, 8, &c[0][0]);
    }
    if (e->loc_fc0 >= 0) { color4(c[0], RS(D3DRS_PSFINALCOMBINERCONSTANT0)); p_glUniform4fv(e->loc_fc0, 1, c[0]); }
    if (e->loc_fc1 >= 0) { color4(c[0], RS(D3DRS_PSFINALCOMBINERCONSTANT1)); p_glUniform4fv(e->loc_fc1, 1, c[0]); }
    if (e->loc_bump_env >= 0) {
        for (int s = 0; s < 4; s++) {
            c[s][0] = tss_float(s, D3DTSS_BUMPENVMAT00);
            c[s][1] = tss_float(s, D3DTSS_BUMPENVMAT01);
            c[s][2] = tss_float(s, D3DTSS_BUMPENVMAT10);
            c[s][3] = tss_float(s, D3DTSS_BUMPENVMAT11);
        }
        p_glUniform4fv(e->loc_bump_env, 4, &c[0][0]);
    }
    if (e->loc_bump_lum >= 0) {
        float lum[4][2];
        for (int s = 0; s < 4; s++) {
            lum[s][0] = tss_float(s, D3DTSS_BUMPENVLSCALE);
            lum[s][1] = tss_float(s, D3DTSS_BUMPENVLOFFSET);
        }
        p_glUniform2fv(e->loc_bump_lum, 4, &lum[0][0]);
    }
    /* Fog parameters reach the program through gl_Fog. */
    color4(c[0], RS(D3DRS_FOGCOLOR));
    glFogfv(GL_FOG_COLOR, c[0]);
    glFogf(GL_FOG_START, rs_float(D3DRS_FOGSTART));
    glFogf(GL_FOG_END, rs_float(D3DRS_FOGEND));
    glFogf(GL_FOG_DENSITY, rs_float(D3DRS_FOGDENSITY));
    apply_shader_textures(e);
}

/* Fixed-function GL work (draws without shaders, glDrawPixels) needs
   program 0; draws leave their program bound. */
static void program_off(void)
{
    if (cur_program) p_glUseProgram(0);
    cur_program = 0;
}

/* Select the program for a draw: `vs` is the vertex shader object (0 for
   fixed function); the fragment side follows the title's pixel shader.
   Returns false when the draw must be skipped (a shader failed), else sets
   *out to the program in use, or NULL when everything is fixed function. */
static bool use_program(GLuint vs, program_entry **out)
{
    *out = NULL;
    GLuint fs = 0;
    if (d3d.pixel_shader) {
        fs = fragment_shader_object();
        if (!fs) return false;
    }
    if (!vs && !fs) { program_off(); return true; }
    program_entry *e = program_for(vs, fs);
    if (!e->prog) return false;
    /* The program stays bound after the draw: consecutive draws with the
       same program (most of a frame) skip the switch. */
    if (cur_program != e->prog) p_glUseProgram(e->prog);
    cur_program = e->prog;
    if (fs) upload_ps_uniforms(e);
    *out = e;
    return true;
}

static void end_program(program_entry *e)
{
    (void)e;
}

/* GL has no signed 11:11:10 vertex format, so NORMPACKED3 attributes are
   unpacked to float3 for the vertices a draw reads (one buffer per register). */
static const float *unpack_normpacked3(int slot, const UCHAR *src, ULONG stride, ULONG n)
{
    static float *buf[16];
    static ULONG cap[16];
    if (n > cap[slot]) {
        free(buf[slot]);
        cap[slot] = n + 1024;
        buf[slot] = malloc(cap[slot] * 12);
    }
    float *o = buf[slot];
    for (ULONG i = 0; i < n; i++, src += stride, o += 3) {
        uint32_t v;
        memcpy(&v, src, 4);
        o[0] = ((int32_t)(v << 21) >> 21) / 1023.0f;
        o[1] = ((int32_t)(v << 10) >> 21) / 1023.0f;
        o[2] = ((int32_t)v >> 22) / 511.0f;
    }
    return buf[slot];
}

/* How many vertices from the start of the streams a draw reads. */
static ULONG vertex_limit(ULONG first, ULONG count, const USHORT *indices)
{
    if (!indices) return first + count;
    ULONG m = 0;
    for (ULONG i = 0; i < count; i++)
        if (indices[first + i] >= m) m = indices[first + i] + 1u;
    return m;
}

/* Bind the generic attribute arrays of a declared vertex layout.  `up` is
   the user-pointer data for stream 0 (DrawVerticesUP), else streams come
   from SetStreamSource. */
static void bind_attributes(const vshader *sh, const UCHAR *up, ULONG up_stride, ULONG first_vertex, ULONG nverts)
{
    for (int r = 0; r < 16; r++) {
        const vattr *a = &sh->attr[r];
        if (a->stream < 0) {
            p_glDisableVertexAttribArray(r);
            static const float def[4] = { 0, 0, 0, 1 };
            p_glVertexAttrib4fv(r, def);
            continue;
        }
        const UCHAR *base;
        ULONG stride;
        if (up && a->stream == 0) {
            base = up;
            stride = up_stride;
        } else {
            D3DResource *vb = d3d.streams[a->stream].vb;
            if (!vb) { p_glDisableVertexAttribArray(r); continue; }
            base = resource_data(vb);
            stride = d3d.streams[a->stream].stride;
        }
        p_glEnableVertexAttribArray(r);
        if ((a->type & 0xF) == 6) {
            p_glVertexAttribPointer(r, 3, GL_FLOAT, GL_FALSE, 12,
                                    unpack_normpacked3(r, base + a->offset + first_vertex * stride, stride, nverts));
            continue;
        }
        /* D3DCOLOR is stored B, G, R, A and reaches the shader as (R, G, B, A). */
        p_glVertexAttribPointer(r, a->type == 0x40 ? GL_BGRA : a->components, a->gl_type, a->normalized, stride,
                                base + a->offset + first_vertex * stride);
    }
}

static void unbind_attributes(void)
{
    for (int r = 0; r < 16; r++) p_glDisableVertexAttribArray(r);
}

static void upload_constants(const vshader *sh, program_entry *e)
{
    float c[192][4];
    memcpy(c, d3d.vs_const, sizeof(c));
    for (unsigned i = 0; i < sh->nconsts; i++)
        if (sh->const_slots[i] < 192) memcpy(c[sh->const_slots[i]], sh->consts[i], 16);
    /* c[58] / c[59]: the viewport scale and offset the program's epilogue
       uses (what the library writes to NV097_SET_VIEWPORT_SCALE/OFFSET). */
    const D3DVIEWPORT8 *v = &d3d.viewport;
    float sx = 0.5f * v->Width, sy = -0.5f * v->Height;
    c[58][0] = sx; c[58][1] = sy; c[58][2] = d3d.zscale * (v->MaxZ - v->MinZ); c[58][3] = 0;
    c[59][0] = v->X + sx + d3d.screen_offset[0];
    c[59][1] = v->Y - sy + d3d.screen_offset[1];
    c[59][2] = d3d.zscale * v->MinZ; c[59][3] = 0;
    /* Most draws repeat the previous upload; skip the driver round trip. */
    if (e->loc_c >= 0 && (!e->last_c || memcmp(e->last_c, c, sizeof(c)))) {
        if (!e->last_c) e->last_c = malloc(sizeof(c));
        memcpy(e->last_c, c, sizeof(c));
        p_glUniform4fv(e->loc_c, 192, &c[0][0]);
    }
    if (e->loc_flip_y >= 0) p_glUniform1f(e->loc_flip_y, d3d.rt_texture ? -1.0f : 1.0f);
}

/* Draw with a programmable vertex shader: generic attributes + GLSL. */
static void draw_programmable_(vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                               ULONG count, const USHORT *indices)
{
    GLuint vs = vertex_shader_object(sh);
    if (!vs) return;
    apply_render_states(false, false);
    if (!d3d.pixel_shader) apply_textures();
    program_entry *e;
    if (!use_program(vs, &e)) return;
    TRACE("D3D: draw(program) prim %u count %u indices %p vs %u ps %#x z %u/%u/%u blend %u %u/%u atest %u cw %#x stencil %u vp %u,%u %ux%u %g-%g",
          PrimitiveType, count, indices ? indices + first : NULL, vs, d3d.pixel_shader,
          RS(D3DRS_ZENABLE), RS(D3DRS_ZFUNC), RS(D3DRS_ZWRITEENABLE), RS(D3DRS_ALPHABLENDENABLE),
          RS(D3DRS_SRCBLEND), RS(D3DRS_DESTBLEND), RS(D3DRS_ALPHATESTENABLE), RS(D3DRS_COLORWRITEENABLE),
          RS(D3DRS_STENCILENABLE), d3d.viewport.X, d3d.viewport.Y, d3d.viewport.Width, d3d.viewport.Height,
          d3d.viewport.MinZ, d3d.viewport.MaxZ);
    upload_constants(sh, e);
    bind_attributes(sh, up, up_stride, 0, vertex_limit(first, count, indices));
    if (indices)
        glDrawElements(gl_primitive(PrimitiveType), count, GL_UNSIGNED_SHORT, indices + first);
    else
        glDrawArrays(gl_primitive(PrimitiveType), first, count);
    unbind_attributes();
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
    apply_render_states(pretransformed, sh->attr[2].stream >= 0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(pretransformed ? 3 : pos->components, pos->gl_type, stride, base + pos->offset);
    if (sh->attr[2].stream >= 0) {
        const UCHAR *b; ULONG s;
        STREAM(&sh->attr[2], b, s);
        glEnableClientState(GL_NORMAL_ARRAY);
        if ((sh->attr[2].type & 0xF) == 6)
            glNormalPointer(GL_FLOAT, 12, unpack_normpacked3(2, b + sh->attr[2].offset, s,
                                                             vertex_limit(first, count, indices)));
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
        if (sh->attr[3].gl_type == GL_UNSIGNED_BYTE)
            glColorPointer(sh->attr[3].type == 0x40 ? GL_BGRA : 4, GL_UNSIGNED_BYTE, s, b + sh->attr[3].offset);
        else
            glColorPointer(sh->attr[3].components, sh->attr[3].gl_type, s, b + sh->attr[3].offset);
        if (lit) { glEnable(GL_COLOR_MATERIAL); glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE); }
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glDisable(GL_COLOR_MATERIAL);
        glColor4f(1, 1, 1, 1);
    }
    if (sh->attr[4].stream >= 0) {
        const UCHAR *b; ULONG s;
        STREAM(&sh->attr[4], b, s);
        const vattr *a = &sh->attr[4];
        secondary_color(lit, b + a->offset, a->type == 0x40 ? GL_BGRA : 3,
                        a->gl_type, s);
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
    TRACE("D3D: draw(declared) prim %u count %u stride %u indices %p shader %#x", PrimitiveType, count, stride,
          indices ? indices + first : NULL, d3d.vertex_shader);
    if (indices)
        glDrawElements(gl_primitive(PrimitiveType), count, GL_UNSIGNED_SHORT, indices + first);
    else
        glDrawArrays(gl_primitive(PrimitiveType), first, count);
    end_program(e);
}

static void draw_programmable(vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                              ULONG count, const USHORT *indices)
{
    Uint64 t0 = SDL_GetPerformanceCounter();
    draw_programmable_(sh, PrimitiveType, up, up_stride, first, count, indices);
    prof_draw_end(t0);
}

static void draw_declared(const vshader *sh, ULONG PrimitiveType, const UCHAR *up, ULONG up_stride, ULONG first,
                          ULONG count, const USHORT *indices)
{
    Uint64 t0 = SDL_GetPerformanceCounter();
    draw_declared_(sh, PrimitiveType, up, up_stride, first, count, indices);
    prof_draw_end(t0);
}

static void draw(ULONG PrimitiveType, const UCHAR *base, ULONG stride, ULONG first, ULONG count,
                 const USHORT *indices)
{
    if (d3d.vertex_shader & 1) {
        vshader *sh = (vshader *)(d3d.vertex_shader & ~1u);
        if (sh->code) draw_programmable(sh, PrimitiveType, base, stride, first, count, indices);
        else draw_declared(sh, PrimitiveType, base, stride, first, count, indices);
        return;
    }
    fvf_layout l = parse_fvf(d3d.vertex_shader);
    apply_render_states(l.pretransformed, l.normal_off >= 0);

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
        glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, base + l.diffuse_off);
        if (lit) {
            glEnable(GL_COLOR_MATERIAL);
            glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
        }
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glDisable(GL_COLOR_MATERIAL);
        glColor4f(1, 1, 1, 1);
    }
    secondary_color(lit, l.specular_off >= 0 ? base + l.specular_off : NULL, GL_BGRA, GL_UNSIGNED_BYTE, stride);
    unsigned units = d3d.pixel_shader ? 0xF : apply_textures();
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
    {
        int tsize[4];
        for (int u = 0; u < 4; u++) {
            ULONG tci = TSS(u, D3DTSS_TEXCOORDINDEX) & 0xFFFF;
            tsize[u] = tci < (ULONG)l.ntex ? l.tex_size[tci] : 2;
        }
        apply_texture_transforms(units, tsize);
    }

    program_entry *e;
    if (!use_program(0, &e)) return;
    TRACE("D3D: draw prim %u count %u base %p stride %u indices %p fvf %#x", PrimitiveType, count, base, stride,
          indices ? indices + first : NULL, d3d.vertex_shader);
    if (indices)
        glDrawElements(gl_primitive(PrimitiveType), count, GL_UNSIGNED_SHORT, indices + first);
    else
        glDrawArrays(gl_primitive(PrimitiveType), first, count);
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
    vshader tmp = *layout;
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
        if (real && !real->vs) real->vs = tmp.vs;
    } else {
        draw_declared(&tmp, 5, (const UCHAR *)verts, 256, 0, nidx, idx);
    }
}

static LONG NTAPI D3DDevice_DrawRectPatch(UINT_ Handle, const float *pNumSegs, const ULONG *info)
{
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
    pusher_drain();
    if (d3d.recording) { ULONG a[3] = { PrimitiveType, StartVertex, VertexCount }; pb_record(OP_DRAW, a, sizeof(a)); return; }
    D3DResource *vb = d3d.streams[0].vb;
    if (!vb) return;
    draw(PrimitiveType, resource_data(vb), d3d.streams[0].stride, StartVertex, VertexCount, NULL);
}

static void NTAPI D3DDevice_DrawVerticesUP(ULONG PrimitiveType, UINT_ VertexCount, const void *pData,
                                           UINT_ Stride)
{
    pusher_drain();
    if (d3d.recording) {
        ULONG h[3] = { PrimitiveType, VertexCount, Stride };
        pb_record2(OP_DRAW_UP, h, sizeof(h), pData, VertexCount * Stride);
        return;
    }
    draw(PrimitiveType, pData, Stride, 0, VertexCount, NULL);
}

static void NTAPI D3DDevice_DrawIndexedVertices(ULONG PrimitiveType, UINT_ VertexCount, const USHORT *pIndexData)
{
    pusher_drain();
    if (d3d.recording) {
        ULONG h[2] = { PrimitiveType, VertexCount };
        pb_record2(OP_DRAW_INDEXED, h, sizeof(h), pIndexData, VertexCount * 2);
        return;
    }
    D3DResource *vb = d3d.streams[0].vb;
    if (!vb) return;
    const UCHAR *base = (const UCHAR *)resource_data(vb) + d3d.base_vertex_index * d3d.streams[0].stride;
    draw(PrimitiveType, base, d3d.streams[0].stride, 0, VertexCount, pIndexData);
}

static void NTAPI D3DDevice_DrawIndexedVerticesUP(ULONG PrimitiveType, UINT_ VertexCount, const USHORT *pIndexData,
                                                  const void *pVertexData, UINT_ Stride)
{
    pusher_drain();
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
    draw(PrimitiveType, pVertexData, Stride, 0, VertexCount, pIndexData);
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

static void load_fbo_functions(void)
{
#define LOAD(n) p_##n = SDL_GL_GetProcAddress(#n)
    LOAD(glGenFramebuffers); LOAD(glBindFramebuffer); LOAD(glFramebufferTexture2D);
    LOAD(glFramebufferRenderbuffer); LOAD(glGenRenderbuffers); LOAD(glBindRenderbuffer);
    LOAD(glRenderbufferStorage); LOAD(glCheckFramebufferStatus);
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
static struct {
    GLuint fbo;                  /* the back buffer (multisampled when samples > 1) */
    GLuint resolve;              /* single-sample copy of a multisampled back buffer */
    GLuint tex;                  /* the finished frame: resolve's (or fbo's) color */
    int w, h, samples;
    float soft;                  /* weight of each neighbour in the downsample filter */
    GLuint prog;
    GLint u_tex, u_step, u_weight;
    void (APIENTRY *bind)(GLenum, GLuint);
    void (APIENTRY *blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
} window_fb;

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
    GLuint rb;
    p_glGenRenderbuffers(1, &rb);
    p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
    if (samples > 1 && storage_ms) storage_ms(GL_RENDERBUFFER, samples, format, w, h);
    else p_glRenderbufferStorage(GL_RENDERBUFFER, format, w, h);
    return rb;
}

/* A framebuffer with a color texture (and depth/stencil) of the back buffer's size. */
static GLuint make_texture_framebuffer(GLuint *tex)
{
    GLuint fb;
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, d3d.width, d3d.height, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glGenFramebuffers(1, &fb);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                make_renderbuffer(1, GL_DEPTH24_STENCIL8, d3d.width, d3d.height));
    return fb;
}

static void create_window_framebuffer(void)
{
    SDL_GL_GetDrawableSize(d3d.window, &window_fb.w, &window_fb.h);
    window_fb.blit = SDL_GL_GetProcAddress("glBlitFramebuffer");
    if (!p_glGenFramebuffers || !window_fb.blit) return;
    window_fb.samples = multisample_samples(d3d.multisample_type, &window_fb.soft);
    window_fb.samples = env_int("XBCOMPAT_MSAA", window_fb.samples);
    if (window_fb.samples <= 1) window_fb.samples = 1, window_fb.soft = 0;
    GLint max = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max);
    if (window_fb.samples > max) window_fb.samples = max > 1 ? max : 1;

    window_fb.resolve = make_texture_framebuffer(&window_fb.tex);
    if (window_fb.samples > 1) {
        p_glGenFramebuffers(1, &window_fb.fbo);
        p_glBindFramebuffer(GL_FRAMEBUFFER, window_fb.fbo);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                    make_renderbuffer(window_fb.samples, GL_RGBA8, d3d.width, d3d.height));
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                    make_renderbuffer(window_fb.samples, GL_DEPTH24_STENCIL8, d3d.width, d3d.height));
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            xlog("D3D: no %d-sample back buffer, drawing without antialiasing", window_fb.samples);
            window_fb.samples = 1;
            window_fb.soft = 0;
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
        window_fb.fbo = 0;
        return;
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    window_fb.bind = p_glBindFramebuffer;
    p_glBindFramebuffer = bind_framebuffer;
    xlog("D3D: %dx%d back buffer (%d sample%s) stretched over a %dx%d window", d3d.width, d3d.height,
         window_fb.samples, window_fb.samples > 1 ? "s" : "", window_fb.w, window_fb.h);
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

/* Point GL_READ_FRAMEBUFFER at the back buffer's pixels for glReadPixels
   (GL can't read a multisampled buffer directly). */
static void backbuffer_read_begin(GLbitfield mask)
{
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
static GLuint backbuffer_copy_texture(ULONG data, ULONG w, ULONG h)
{
    static struct { ULONG data, w, h; GLuint tex; } slot[8];
    static GLuint fb;
    if (!window_fb.fbo || !window_fb.blit || w > (ULONG)d3d.width || h > (ULONG)d3d.height) return 0;
    unsigned i = 0;
    while (i < 8 && slot[i].tex && !(slot[i].data == data && slot[i].w == w && slot[i].h == h)) i++;
    if (i == 8) i = data % 8;
    if (!slot[i].tex || slot[i].data != data || slot[i].w != w || slot[i].h != h) {
        if (!slot[i].tex) glGenTextures(1, &slot[i].tex);
        glBindTexture(GL_TEXTURE_2D, slot[i].tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        slot[i].data = data; slot[i].w = w; slot[i].h = h;
    }
    if (!fb) p_glGenFramebuffers(1, &fb);
    GLint draw = 0;
    glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &draw);
    backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
    window_fb.bind(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, fb);
    p_glFramebufferTexture2D(0x8CA9, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot[i].tex, 0);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    window_fb.blit(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    window_fb.bind(0x8CA9, draw);
    backbuffer_read_end();
    stats.readbacks++;
    return slot[i].tex;
}

static const char present_vs[] =
    "#version 120\n"
    "varying vec2 uv;\n"
    "void main() { uv = gl_Vertex.xy * 0.5 + 0.5; gl_Position = gl_Vertex; }\n";
/* A 3x3 tap filter, separable: each axis weighs its neighbours by weight. */
static const char present_fs[] =
    "#version 120\n"
    "uniform sampler2D tex;\n"
    "uniform vec2 texel, weight;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    vec3 kx = vec3(weight.x, 1.0 - 2.0 * weight.x, weight.x);\n"
    "    vec3 ky = vec3(weight.y, 1.0 - 2.0 * weight.y, weight.y);\n"
    "    vec3 c = vec3(0.0);\n"
    "    for (int y = 0; y < 3; y++)\n"
    "        for (int x = 0; x < 3; x++)\n"
    "            c += kx[x] * ky[y] * texture2D(tex, uv + vec2(float(x - 1), float(y - 1)) * texel).rgb;\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

static bool present_program(void)
{
    static bool failed;
    if (window_fb.prog || failed || !p_glCreateProgram) return window_fb.prog != 0;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, present_vs), fs = compile_shader(GL_FRAGMENT_SHADER, present_fs);
    GLuint prog = vs && fs ? p_glCreateProgram() : 0;
    GLint ok = 0;
    if (prog) {
        p_glAttachShader(prog, vs);
        p_glAttachShader(prog, fs);
        p_glLinkProgram(prog);
        p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    }
    if (vs) p_glDeleteShader(vs);
    if (fs) p_glDeleteShader(fs);
    if (!ok) {
        xlog("D3D: no present filter program, the frame goes out unfiltered");
        if (prog) p_glDeleteProgram(prog);
        failed = true;
        return false;
    }
    window_fb.prog = prog;
    window_fb.u_tex = p_glGetUniformLocation(prog, "tex");
    window_fb.u_step = p_glGetUniformLocation(prog, "texel");
    window_fb.u_weight = p_glGetUniformLocation(prog, "weight");
    return true;
}

static void present_window_framebuffer(void)
{
    if (!window_fb.fbo) return;
    resolve_window_framebuffer(GL_COLOR_BUFFER_BIT);
    static int flicker_override = -2;
    if (flicker_override == -2) flicker_override = env_int("XBCOMPAT_FLICKER", -1);
    int flicker = flicker_override >= 0 ? flicker_override : (int)d3d.flicker_filter;
    if (flicker > 5) flicker = 5;
    /* Flicker filter 5 blends each line with the ones above and below it
       1:2:1; the soft display (luma) filter softens along the line. */
    float wx = window_fb.soft + (d3d.soft_display ? 0.125f : 0.0f);
    float wy = window_fb.soft + flicker * 0.05f;
    if (wy > 0.3f) wy = 0.3f;

    if ((wx <= 0 && wy <= 0) || !present_program()) {
        GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        window_fb.bind(GL_READ_FRAMEBUFFER, window_fb.resolve ? window_fb.resolve : window_fb.fbo);
        window_fb.bind(GL_DRAW_FRAMEBUFFER, 0);
        window_fb.blit(0, 0, d3d.width, d3d.height, 0, 0, window_fb.w, window_fb.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        return;
    }

    GLint prev_prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    window_fb.bind(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, window_fb.w, window_fb.h);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST); glDisable(GL_CULL_FACE); glDisable(GL_COLOR_LOGIC_OP);
    for (int i = 0; i < 6; i++) glDisable(GL_CLIP_PLANE0 + i);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, window_fb.tex);
    p_glUseProgram(window_fb.prog);
    p_glUniform1i(window_fb.u_tex, 0);
    float step[2] = { 1.0f / d3d.width, 1.0f / d3d.height }, weight[2] = { wx, wy };
    p_glUniform2fv(window_fb.u_step, 1, step);
    p_glUniform2fv(window_fb.u_weight, 1, weight);
    glBegin(GL_TRIANGLE_STRIP);
    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(-1, 1); glVertex2f(1, 1);
    glEnd();
    p_glUseProgram(prev_prog);
    glPopAttrib();
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
    d3d.zscale = depth_format == 0x2C || depth_format == 0x2D || depth_format == 0x30 || depth_format == 0x31
                     ? 65535.0f : 16777215.0f;
    d3d.target = d3d.backbuffer;
    d3d.target_depth = d3d.depth;
    internal_addref_surface(d3d.target);
    internal_addref_surface(d3d.target_depth);
    d3d.rt_width = d3d.width;
    d3d.rt_height = d3d.height;
    d3d.rt_texture = false;
    d3d.target_zmax = d3d.zscale;
}

static void NTAPI D3DDevice_GetBackBuffer(LONG BackBuffer, ULONG Type, D3DSurface **pp)
{
    (void)BackBuffer; (void)Type;
    d3d.backbuffer->Common++;
    *pp = d3d.backbuffer;
}

static LONG NTAPI D3DDevice_GetDepthStencilSurface(D3DSurface **pp)
{
    d3d.depth->Common++;
    *pp = d3d.depth;
    return D3D_OK;
}

static void NTAPI D3DDevice_GetRenderTarget(D3DSurface **pp)
{
    d3d.target->Common++;
    *pp = d3d.target;
}

static GLuint texture_for(D3DPixelContainer *t);
static ULONG cube_face_bytes(D3DPixelContainer *t);

/* The GL texture behind a standalone render target surface, kept in the
   texture cache under the surface's own memory so destroy_resource frees it. */
static GLuint surface_rt_texture(D3DSurface *s, ULONG w, ULONG h)
{
    for (tex_entry *e = tex_cache; e; e = e->next)
        if (e->data == s->Data && e->format == s->Format && e->size == s->Size) return e->id;
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    tex_entry *e = malloc(sizeof(*e));
    *e = (tex_entry){ s->Data, s->Format, s->Size, id, GL_TEXTURE_2D, tex_cache };
    tex_cache = e;
    return id;
}

/* Copy a standalone render target's GL pixels into its memory.  Render
   targets are drawn flipped, so GL row 0 is the top row. */
static bool readback_rt_surface(D3DSurface *s)
{
    if (s->Parent || s == d3d.backbuffer) return false;
    stats.readbacks++;
    for (tex_entry *e = tex_cache; e; e = e->next) {
        if (e->data != s->Data || e->format != s->Format || e->size != s->Size) continue;
        ULONG w, h, pitch;
        container_size((D3DPixelContainer *)s, &w, &h, &pitch);
        uint8_t *tmp = malloc(w * h * 4), *px = (uint8_t *)(s->Data | CONTIG_BASE);
        glBindTexture(GL_TEXTURE_2D, e->id);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
        ULONG row = pitch < w * 4 ? pitch : w * 4;
        for (ULONG y = 0; y < h; y++) memcpy(px + y * pitch, tmp + y * w * 4, row);
        free(tmp);
        return true;
    }
    return false;
}

static void NTAPI D3DDevice_SetRenderTarget(D3DSurface *target, D3DSurface *z)
{
    pusher_drain();
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
    sync_device_surfaces();
    TRACE("D3D: render target %p (parent %p) depth %p", (void *)target, (void *)target->Parent, (void *)z);
    if (target == d3d.backbuffer || (!target->Parent && !p_glGenFramebuffers)) {
        if (target != d3d.backbuffer)
            xlog("D3D: no framebuffer objects, rendering a standalone surface into the back buffer");
        if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        d3d.rt_width = d3d.width;
        d3d.rt_height = d3d.height;
        d3d.rt_texture = false;
        d3d.viewport = (D3DVIEWPORT8){ 0, 0, d3d.rt_width, d3d.rt_height, 0, 1 };
        return;
    }
    if (!p_glGenFramebuffers) {
        xlog("D3D: no framebuffer objects, cannot render to texture");
        return;
    }
    ULONG w, h, pitch;
    container_size(target->Parent ? target->Parent : (D3DPixelContainer *)target, &w, &h, &pitch);
    if (!d3d.fbo) p_glGenFramebuffers(1, (GLuint *)&d3d.fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, d3d.fbo);
    /* A standalone surface (CreateRenderTarget) renders into a GL texture of
       its own; CopyRects reads it back. */
    GLuint tex = target->Parent ? texture_for(target->Parent) : surface_rt_texture(target, w, h);
    GLenum face_target = GL_TEXTURE_2D;
    if (target->Parent && tex_target(target->Parent) == GL_TEXTURE_CUBE_MAP) {
        /* A cube face surface sits face * cube_face_bytes past the cube's data. */
        ULONG face = (target->Data - target->Parent->res.Data) / cube_face_bytes(target->Parent);
        face_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + (face < 6 ? face : 0);
    }
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, face_target, tex, 0);
    d3d.target_zmax = z ? depth_format_max((z->Format >> 8) & 0xFF) : d3d.zscale;
    if (z && z->Parent && tex_target(z->Parent) == GL_TEXTURE_2D) {
        /* Depth into a texture (a shadow buffer). */
        GLuint dt = texture_for(z->Parent);
        bool d24 = depth_format_max((z->Format >> 8) & 0xFF) > 65535.0f;
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, d24 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                 GL_TEXTURE_2D, dt, 0);
        GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) xlog("D3D: depth texture framebuffer incomplete (%#x)", st);
        d3d.rt_width = w;
        d3d.rt_height = h;
        d3d.rt_texture = true;
        d3d.viewport = (D3DVIEWPORT8){ 0, 0, w, h, 0, 1 };
        return;
    }
    /* A depth buffer always comes along: titles clear z without checking. */
    static GLuint rb; static ULONG rb_w, rb_h;
    if (!rb || rb_w != w || rb_h != h) {
        if (!rb) p_glGenRenderbuffers(1, &rb);
        p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        rb_w = w; rb_h = h;
    }
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) xlog("D3D: render target framebuffer incomplete (%#x)", st);
    d3d.rt_width = w;
    d3d.rt_height = h;
    d3d.rt_texture = true;
    d3d.viewport = (D3DVIEWPORT8){ 0, 0, w, h, 0, 1 };
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
    if (s == d3d.backbuffer && !d3d.bb_cpu_dirty) {
        /* Hand out the real pixels: read the frame back.  Not while memory
           holds writes from an earlier lock: GL hasn't seen them yet (a draw
           flushes them first), so memory is already the newest frame, and
           reading GL back would erase them.  XFONT locks once per TextOut,
           so every string but a frame's last went missing. */
        uint8_t *px = (uint8_t *)(s->Data | CONTIG_BASE);
        uint8_t *tmp = malloc(w * h * 4);
        backbuffer_read_begin(GL_COLOR_BUFFER_BIT);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
        backbuffer_read_end();
        for (ULONG y = 0; y < h; y++) memcpy(px + y * pitch, tmp + (h - 1 - y) * w * 4, w * 4);
        free(tmp);
    }
    if (s == d3d.backbuffer && !(flags & 0x10 /* D3DLOCK_READONLY */)) d3d.bb_cpu_dirty = true;
    if (s->Parent && !(flags & 0x80)) tex_invalidate(s->Parent->res.Data);
    ULONG fmt = (s->Format >> 8) & 0xFF, offset = rect ? rect[1] * pitch + rect[0] * (pitch / w) : 0;
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

static void NTAPI D3DDevice_CopyRects(D3DSurface *src, const LONG *rects, UINT_ n, D3DSurface *dst,
                                      const LONG *points)
{
    pusher_drain();
    ULONG sw, sh, sp, dw, dh, dp;
    container_size((D3DPixelContainer *)src, &sw, &sh, &sp);
    container_size((D3DPixelContainer *)dst, &dw, &dh, &dp);
    bool lin;
    int bytes = format_bits((src->Format >> 8) & 0xFF, &lin) / 8;
    if (src == d3d.backbuffer) {
        ULONG locked[2];
        D3DSurface_LockRect(src, locked, NULL, 0x80);   /* refresh its pixels */
    } else {
        readback_rt_surface(src);
    }
    const uint8_t *s = (const uint8_t *)(src->Data | CONTIG_BASE);
    uint8_t *d = (uint8_t *)(dst->Data | CONTIG_BASE);
    bool dst_lin;
    int dst_bytes = format_bits((dst->Format >> 8) & 0xFF, &dst_lin) / 8;
    if (!n && !dst_lin && dst_bytes == bytes && sw == dw && sh == dh) {
        /* Into a swizzled texture level: the copy swizzles. */
        swizzle(s, sp, d, dw, dh, bytes);
    } else if (!n) {
        ULONG w = sw < dw ? sw : dw, h = sh < dh ? sh : dh;
        for (ULONG y = 0; y < h; y++) memcpy(d + y * dp, s + y * sp, w * bytes);
    } else {
        for (UINT_ i = 0; i < n; i++) {
            LONG x0 = rects[i * 4], y0 = rects[i * 4 + 1], x1 = rects[i * 4 + 2], y1 = rects[i * 4 + 3];
            LONG dx = points ? points[i * 2] : 0, dy = points ? points[i * 2 + 1] : 0;
            for (LONG y = y0; y < y1; y++)
                memcpy(d + (dy + y - y0) * dp + dx * bytes, s + y * sp + x0 * bytes, (x1 - x0) * bytes);
        }
    }
    if (dst->Parent) tex_invalidate(dst->Parent->res.Data);
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
    TRACE("D3D: texture %ux%ux%u fmt %#x levels %u%s = %p (%u bytes)", w, h, depth, fmt, levels,
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

static void NTAPI D3DDevice_GetVertexShader(ULONG *h) { *h = d3d.vertex_shader; }

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
    TRACE("D3D: vs constant %d x%u = %g %g %g %g", Register, count, data[0], data[1], data[2], data[3]);
    for (ULONG i = 0; i < count; i++) {
        int slot = constant_slot(Register + i);
        memcpy(d3d.vs_const[slot], data + i * 4, 16);
    }
}

static void NTAPI D3DDevice_GetVertexShaderConstant(LONG Register, float *data, ULONG count)
{
    for (ULONG i = 0; i < count; i++) memcpy(data + i * 4, d3d.vs_const[constant_slot(Register + i)], 16);
}

static void NTAPI D3DDevice_SetShaderConstantMode(ULONG mode) { d3d.constant_mode = mode; }
static void NTAPI D3DDevice_GetShaderConstantMode(ULONG *mode) { *mode = d3d.constant_mode; }

static void NTAPI D3DDevice_GetProjectionViewportMatrix(D3DMATRIX *out)
{
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

static void NTAPI D3DDevice_SetScissors(ULONG Count, BOOLEAN Exclusive, const D3DRECT *rects)
{
    if (Count > 8) Count = 8;
    d3d.scissors.count = Count;
    d3d.scissors.exclusive = Exclusive;
    if (Count) memcpy(d3d.scissors.rects, rects, Count * sizeof(D3DRECT));
}

static void NTAPI D3DDevice_GetScissors(ULONG *Count, ULONG *Exclusive, D3DRECT *rects)
{
    *Count = d3d.scissors.count;
    *Exclusive = d3d.scissors.exclusive;
    if (rects) memcpy(rects, d3d.scissors.rects, d3d.scissors.count * sizeof(D3DRECT));
}

/* Callbacks run "when the GPU gets there": we run them at the next sync point. */
static void run_callbacks(void)
{
    for (unsigned i = 0; i < d3d.ncallbacks; i++) {
        void (CDECLAPI *fn)(ULONG) = d3d.callbacks[i].fn;
        fn(d3d.callbacks[i].ctx);
    }
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

static void NTAPI D3DDevice_SetVerticalBlankCallback(PVOID fn) { d3d.vblank_callback = fn; }
static void NTAPI D3DDevice_SetSwapCallback(PVOID fn) { d3d.swap_callback = fn; }
static void NTAPI D3DDevice_GetDisplayFieldStatus(ULONG *st) { st[0] = 3; st[1] = d3d.frame; }
static void NTAPI D3DDevice_SetScreenSpaceOffset(float x, float y) { d3d.screen_offset[0] = x; d3d.screen_offset[1] = y; }
static void NTAPI D3DDevice_SetBackBufferScale(float x, float y) { d3d.backbuffer_scale[0] = x; d3d.backbuffer_scale[1] = y; }
static void NTAPI D3DDevice_GetBackBufferScale(float *x, float *y) { *x = d3d.backbuffer_scale[0]; *y = d3d.backbuffer_scale[1]; }
static void NTAPI D3DDevice_SetDebugMarker(ULONG v) { (void)v; }
static void NTAPI D3DDevice_SetTile(ULONG i, const void *t) { (void)i; (void)t; }
static void NTAPI D3DDevice_GetTile(ULONG i, ULONG *t) { (void)i; memset(t, 0, 24); }
static void NTAPI D3DDevice_KickPushBuffer(void) { pusher_drain(); }
static ULONG NTAPI D3DDevice_InsertFence(void) { return ++d3d.frame * 0 + 1; }
static BOOLEAN NTAPI D3DDevice_IsFencePending(ULONG f) { (void)f; return 0; }
static void NTAPI D3DDevice_BlockOnFence(ULONG f) { (void)f; }
static void NTAPI D3DDevice_SetGammaRamp(ULONG flags, const void *ramp) { (void)flags; (void)ramp; }
static void NTAPI D3DDevice_GetGammaRamp(USHORT *ramp) { for (int i = 0; i < 768; i++) ramp[i] = (i % 256) * 257; }
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
}

static void NTAPI D3DDevice_SetPixelShader(ULONG handle)
{
    if (d3d.recording) pb_record(OP_PIXEL_SHADER, &handle, 4);
    d3d.pixel_shader = handle;
    if (!handle) return;
    const ULONG *def = (const ULONG *)handle;
    /* A title that builds its own shader objects (LTCG, no CreatePixelShader
       to replace) passes the library's { RefCount, D3DOwned, pPSDef }. */
    if (!pool_owns(def) && def[0] < 0x10000 && def[1] <= 1 && def[2] >= 0x10000) {
        def = (const ULONG *)def[2];
        d3d.pixel_shader = (ULONG)def;
    }
    memcpy(d3d.render_state, def, D3DRS_PS_MAX * 4);
    RS(D3DRS_PSTEXTUREMODES) = def[54];
}

/* The library points its own shader object at the title's definition; the
   definition is used in place, and NULL turns pixel shaders off. */
static void NTAPI D3DDevice_SetPixelShaderProgram(const ULONG *def)
{
    D3DDevice_SetPixelShader((ULONG)def);
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
    apply_render_states(pretransformed, imm.used[2]);
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
    float vs_const[192][4], ps_const[16][4];
    D3DPalette *palettes[4];
    D3DSurface *target, *target_depth;
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
    memcpy(pb_saved.palettes, d3d.palettes, sizeof(pb_saved.palettes));
    pb_saved.target = d3d.target;
    pb_saved.target_depth = d3d.target_depth;
}

static LONG NTAPI D3DDevice_EndPushBuffer(void)
{
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
    memcpy(d3d.ps_const, pb_saved.ps_const, sizeof(pb_saved.ps_const));
    memcpy(d3d.palettes, pb_saved.palettes, sizeof(pb_saved.palettes));
    if (d3d.target != pb_saved.target || d3d.target_depth != pb_saved.target_depth)
        D3DDevice_SetRenderTarget(pb_saved.target, pb_saved.target_depth);
    TRACE("D3D: recorded push buffer %p: %u dwords", (void *)pb, pb_stream_of(pb, true)->n);
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

/* The op recorded at `offset`, or NULL (with one complaint) when there is
   no op of that kind there. */
static ULONG *pb_op_at(D3DPushBuffer *b, ULONG offset, unsigned op)
{
    pb_stream *pb = pb_stream_of(b, false);
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
        e[1] = offset;
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

static void NTAPI D3DPushBuffer_RunPushBuffer(D3DPushBuffer *pb, ULONG Offset, D3DPushBuffer *dest, D3DFixup *fx)
{
    ULONG *p = fixup_payload(pb, Offset, OP_RUN, 8);
    if (p) { p[0] = (ULONG)dest; p[1] = (ULONG)fx; }
}

static void NTAPI D3DPushBuffer_Verify(D3DPushBuffer *pb, ULONG StampResources) { (void)pb; (void)StampResources; }

/* ---- running ---- */

static void pb_interpret(const ULONG *p, unsigned n);

static void pb_run(D3DPushBuffer *b, D3DFixup *fx, int depth)
{
    pb_stream *pb = pb_stream_of(b, false);
    if (!pb || !pb->ops || depth > 8) return;
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
        case OP_PIXEL_SHADER: D3DDevice_SetPixelShader(a[0]); break;
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
        default: xlog("D3D: bad push buffer op %u", op); return;
        }
        i += len;
    }
}

static void NTAPI D3DDevice_RunPushBuffer(D3DPushBuffer *pb, D3DFixup *fx)
{
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

static void pb_interpret(const ULONG *p, unsigned n)
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
                            if (stride) draw(prim, inline_buf, stride, 0, inline_n / stride, NULL);
                        }
                    } else if (elems_n) {
                        D3DResource *vb = d3d.streams[0].vb;
                        if (vb) draw(prim, resource_data(vb), d3d.streams[0].stride, 0, elems_n, elems);
                    } else {
                        imm_flush(prim);
                    }
                    inline_n = elems_n = imm.n = 0;
                }
            } else if (m == 0x1810) {   /* NV097_DRAW_ARRAYS: count-1 << 24 | start */
                D3DResource *vb = d3d.streams[0].vb;
                if (vb) draw(prim, resource_data(vb), d3d.streams[0].stride, d & 0xFFFFFF, (d >> 24) + 1, NULL);
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
            } else if (m == 0x1EA4) {   /* NV097_SET_TRANSFORM_CONSTANT_LOAD */
                const_load = d;
            } else if (m >= 0xB80 && m < 0xC00) {   /* NV097_SET_TRANSFORM_CONSTANT */
                /* Consecutive dwords fill vec4s from the load index, which
                   moves on by the number of vec4s written. */
                unsigned pos = noinc ? k : (m - 0xB80) / 4;
                unsigned slot = const_load + pos / 4;
                if (slot < 192) d3d.vs_const[slot][pos % 4] = f;
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

static void NTAPI D3DDevice_EndPush(ULONG *end)
{
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
static ULONG *pusher_buf;

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
    if (!pusher_buf || !dev || busy) return;
    ULONG *put = (ULONG *)dev[0];
    if (put < pusher_buf || put > pusher_buf + PUSHER_BYTES / 4) {
        xlog("D3D: push buffer pointer %p is outside the device's buffer", (void *)put);
        dev[0] = (ULONG)pusher_buf;
        return;
    }
    unsigned n = put - pusher_buf;
    if (!n) return;
    busy = true;
    if (d3d.recording) {
        ULONG h = n;
        pb_record2(OP_RAW, &h, 4, pusher_buf, n * 4);
    } else {
        Uint64 t0 = SDL_GetPerformanceCounter();
        pb_interpret(pusher_buf, n);
        prof.drain += SDL_GetPerformanceCounter() - t0;
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

static void NTAPI D3DDevice_Nop(void) {}

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
    pusher_drain();
    if (!d3d.window) return D3DERR_INVALIDCALL;
    flush_cpu_backbuffer();
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
    free(px);
    free(flipped);
    xlog("D3D: PersistDisplay kept a %dx%d frame", w, h);
    return D3D_OK;
}

/* Nothing survives a reboot here; hand out the back buffer so the title has a surface to read. */
static LONG NTAPI D3DDevice_GetPersistedSurface(D3DSurface **pp)
{
    *pp = d3d.backbuffer;
    if (d3d.backbuffer) d3d.backbuffer->Common++;
    return d3d.backbuffer ? D3D_OK : D3DERR_INVALIDCALL;
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

/* Visibility tests are GL occlusion queries, one per index the title uses. */
#define VIS_SLOTS 256
static struct { ULONG index; GLuint query; bool used; } vis[VIS_SLOTS];
static GLuint vis_active;

static void NTAPI D3DDevice_BeginVisibilityTest(void)
{
    pusher_drain();
    if (!p_glGenQueries || vis_active) return;
    p_glGenQueries(1, &vis_active);
    p_glBeginQuery(0x8914 /* GL_SAMPLES_PASSED */, vis_active);
}

static LONG NTAPI D3DDevice_EndVisibilityTest(ULONG index)
{
    pusher_drain();
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
    return D3D_OK;
}

static LONG NTAPI D3DDevice_GetVisibilityTestResult(ULONG index, UINT_ *result, ULONGLONG *stamp)
{
    unsigned slot = index % VIS_SLOTS;
    if (stamp) *stamp = d3d.frame;
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
} state_block;

static void state_capture(state_block *b)
{
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
    memcpy(d3d.ps_const, b->ps_const, sizeof(b->ps_const));
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
    TRACE("D3D: CreateTexture2(%u, %u, %u, levels %u, usage %#x, fmt %#x, type %u)", w, h, depth, levels, usage, fmt,
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

static void NTAPI D3DDevice_GetModelView(D3DMATRIX *m)
{
    mat_mul(m, &d3d.transforms[6], &d3d.transforms[0]);
}

static void NTAPI D3DDevice_SetModelView(const D3DMATRIX *mv, const D3DMATRIX *inv, const D3DMATRIX *composite)
{
    (void)inv; (void)composite;
    static bool warned;
    if (mv && !warned) { xlog("D3D: SetModelView overrides are ignored"); warned = true; }
}

static void NTAPI D3DDevice_GetViewportOffsetAndScale(float *offset, float *scale)
{
    /* The screen-space transform the hardware applies after projection
       (CDevice::GetViewportOffsetAndScale): pixel centres at +0.53125 and
       z scaled to the depth buffer's range. */
    float zscale = d3d.depth ? (((d3d.depth->Format >> 8) & 0xFF) == 0x2C ? 65535.0f : 16777215.0f) : 1.0f;
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
static ULONG NTAPI D3D_SetFence(ULONG Flags)
{
    (void)Flags;
    pusher_drain();
    if (d3d.cpu_time) *d3d.cpu_time = fence + 1;
    return ++fence;
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
static LONG NTAPI D3DDevice_Reset(PVOID pp) { (void)pp; return D3D_OK; }
static void NTAPI SetRenderState_SampleAlpha(ULONG Value) { (void)Value; }

static ULONG NTAPI D3D_GetAdapterModeCount2(UINT_ Adapter, ULONG Format)
{
    (void)Format;
    return Direct3D_GetAdapterModeCount(Adapter);
}

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
    F("_D3DDevice_SetStipple@4", D3D_Nop4),
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
