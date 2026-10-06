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
    D3DRS_ZFUNC = 57, D3DRS_ALPHAFUNC = 58, D3DRS_ALPHABLENDENABLE = 59, D3DRS_ALPHATESTENABLE = 60,
    D3DRS_ALPHAREF = 61, D3DRS_SRCBLEND = 62, D3DRS_DESTBLEND = 63, D3DRS_ZWRITEENABLE = 64,
    D3DRS_SHADEMODE = 66, D3DRS_COLORWRITEENABLE = 67, D3DRS_BLENDOP = 74,
    D3DRS_FOGENABLE = 82, D3DRS_LIGHTING = 92, D3DRS_SPECULARENABLE = 93, D3DRS_COLORVERTEX = 95,
    D3DRS_DIFFUSEMATERIALSOURCE = 101, D3DRS_AMBIENTMATERIALSOURCE = 102, D3DRS_AMBIENT = 105,
    D3DRS_FILLMODE = 120, D3DRS_NORMALIZENORMALS = 123, D3DRS_ZENABLE = 124,
    D3DRS_STENCILENABLE = 125, D3DRS_FRONTFACE = 127, D3DRS_CULLMODE = 128,
    D3DRS_TEXTUREFACTOR = 129, D3DRS_MAX = 146,
};

/* ---- state ------------------------------------------------------------- */

static struct {
    SDL_Window *window;
    SDL_GLContext gl;
    int width, height;
    ULONG frame;
    ULONG *render_state;         /* the title's D3D__RenderState[] */
    ULONG render_state_fallback[D3DRS_MAX];
    ULONG *device;               /* the title's g_Device (or our own) */
    ULONG *device_ptr;           /* the title's g_pDevice */
    D3DMATRIX transforms[10];    /* VIEW, PROJECTION, TEXTURE0-3, WORLD0-3 */
    D3DVIEWPORT8 viewport;
    struct { D3DResource *vb; ULONG stride; } streams[16];
    ULONG *texture_state;        /* the title's D3D__TextureState[4][32] */
    ULONG texture_state_fallback[4 * 32];
    D3DResource *textures[4];
    ULONG vertex_shader;
    D3DResource *indices;
    ULONG base_vertex_index;
    struct { bool set; float diffuse[4], ambient[4], specular[4], position[4], direction[4];
             ULONG type; float range, attenuation[3]; bool enabled; } lights[8];
    float material[4][4];        /* diffuse, ambient, specular, emissive */
    float material_power;

    struct D3DSurface *backbuffer, *depth;        /* the device's own surfaces */
    struct D3DSurface *target, *target_depth;     /* current render target */
    int rt_width, rt_height;     /* size of the current render target */
    bool rt_texture;             /* rendering into a texture (GL rows run bottom-up) */
    ULONG fbo;                   /* framebuffer object for texture targets */
    struct { ULONG count; ULONG exclusive; D3DRECT rects[8]; } scissors;
    float screen_offset[2];
    float backbuffer_scale[2];
    ULONG constant_mode;
    float vs_const[192][4];      /* vertex shader constants, hardware numbering */
    float ps_const[16][4];
    ULONG pixel_shader;
    PVOID vblank_callback;
    struct { PVOID fn; ULONG ctx; } callbacks[64];
    unsigned ncallbacks;
    struct D3DPalette *palettes[4];
} d3d;

/* An Xbox surface: a pixel container plus the texture it belongs to. */
typedef struct D3DSurface {
    DWORD Common, Data, Lock, Format, Size;
    struct D3DPixelContainer *Parent;
} D3DSurface;

typedef struct D3DPalette { DWORD Common, Data, Lock; } D3DPalette;

extern int g_screenshot_frame;
extern const char *g_screenshot_path;
extern int g_exit_after_frames;

#define RS(i) (d3d.render_state[i])
#define TSS(stage, i) (d3d.texture_state[(stage) * 32 + (i)])

enum {
    D3DTSS_ADDRESSU = 0, D3DTSS_ADDRESSV = 1, D3DTSS_MAGFILTER = 3, D3DTSS_MINFILTER = 4,
    D3DTSS_COLOROP = 12, D3DTSS_COLORARG0 = 13, D3DTSS_COLORARG1 = 14, D3DTSS_COLORARG2 = 15,
    D3DTSS_ALPHAOP = 16, D3DTSS_ALPHAARG0 = 17, D3DTSS_ALPHAARG1 = 18, D3DTSS_ALPHAARG2 = 19,
    D3DTSS_TEXCOORDINDEX = 28, D3DTSS_BORDERCOLOR = 29, D3DTSS_COLORKEYCOLOR = 30,
};

/* ---- device ------------------------------------------------------------ */

static void load_fbo_functions(void);
static void create_device_surfaces(ULONG format, ULONG depth_format);
static void run_callbacks(void);

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
    ULONG *rs = d3d.render_state;
    /* The title's image carries the library's own defaults table. */
    ULONG init = hle_lookup_prefix("?g_InitialRenderStates@D3D@@");
    (void)init;
    rs[D3DRS_ZFUNC] = GL_LEQUAL;
    rs[D3DRS_ALPHAFUNC] = GL_ALWAYS;
    rs[D3DRS_ALPHABLENDENABLE] = 0;
    rs[D3DRS_ALPHATESTENABLE] = 0;
    rs[D3DRS_SRCBLEND] = GL_ONE;
    rs[D3DRS_DESTBLEND] = GL_ZERO;
    rs[D3DRS_ZWRITEENABLE] = 1;
    rs[D3DRS_SHADEMODE] = GL_SMOOTH;
    rs[D3DRS_COLORWRITEENABLE] = 0x01010101;
    rs[D3DRS_BLENDOP] = GL_FUNC_ADD;
    rs[D3DRS_LIGHTING] = 1;
    rs[D3DRS_COLORVERTEX] = 1;
    rs[D3DRS_DIFFUSEMATERIALSOURCE] = 1;   /* D3DMCS_COLOR1 */
    rs[D3DRS_AMBIENTMATERIALSOURCE] = 0;   /* D3DMCS_MATERIAL */
    rs[D3DRS_FILLMODE] = GL_FILL;
    rs[D3DRS_CULLMODE] = 0x901;            /* D3DCULL_CCW */
    rs[D3DRS_FRONTFACE] = GL_CW;

    for (int st = 0; st < 4; st++) {
        TSS(st, D3DTSS_COLOROP) = st == 0 ? 4 /* MODULATE */ : 1 /* DISABLE */;
        TSS(st, D3DTSS_COLORARG1) = 2;  /* D3DTA_TEXTURE */
        TSS(st, D3DTSS_COLORARG2) = 1;  /* D3DTA_CURRENT */
        TSS(st, D3DTSS_ALPHAOP) = st == 0 ? 2 /* SELECTARG1 */ : 1;
        TSS(st, D3DTSS_ALPHAARG1) = 2;
        TSS(st, D3DTSS_ALPHAARG2) = 1;
        TSS(st, D3DTSS_ADDRESSU) = TSS(st, D3DTSS_ADDRESSV) = 1;  /* WRAP */
        TSS(st, D3DTSS_MAGFILTER) = TSS(st, D3DTSS_MINFILTER) = 1;
        TSS(st, D3DTSS_TEXCOORDINDEX) = st;
    }
}

void d3d_bind_globals(void)
{
    d3d.render_state = (ULONG *)hle_lookup("_D3D__RenderState");
    if (!d3d.render_state) d3d.render_state = d3d.render_state_fallback;
    d3d.texture_state = (ULONG *)hle_lookup("_D3D__TextureState");
    if (!d3d.texture_state) d3d.texture_state = d3d.texture_state_fallback;
    d3d.device = (ULONG *)hle_lookup_prefix("?g_Device@D3D@@");
    d3d.device_ptr = (ULONG *)hle_lookup_prefix("?g_pDevice@D3D@@");
    xlog("D3D: render states at %p, device at %p", (void *)d3d.render_state, (void *)d3d.device);
}

static LONG NTAPI Direct3D_CreateDevice(UINT_ Adapter, ULONG DeviceType, PVOID pUnused, ULONG Flags,
                                       D3DPRESENT_PARAMETERS *pp, PVOID *ppDevice)
{
    d3d.width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    d3d.height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    xlog("D3D: CreateDevice %ux%u, format %#x, depth %s", d3d.width, d3d.height, pp->BackBufferFormat,
         pp->EnableAutoDepthStencil ? "yes" : "no");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) fatal("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    d3d.window = SDL_CreateWindow("xbcompat", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  d3d.width, d3d.height, SDL_WINDOW_OPENGL);
    if (!d3d.window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    d3d.gl = SDL_GL_CreateContext(d3d.window);
    if (!d3d.gl) fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(1);
    xlog("D3D: OpenGL %s on %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    for (int i = 0; i < 10; i++) identity(&d3d.transforms[i]);
    d3d.viewport = (D3DVIEWPORT8){ 0, 0, d3d.width, d3d.height, 0, 1 };
    default_render_states();
    d3d.material[0][0] = d3d.material[0][1] = d3d.material[0][2] = d3d.material[0][3] = 1;

    load_fbo_functions();
    create_device_surfaces(pp->BackBufferFormat, pp->EnableAutoDepthStencil ? pp->AutoDepthStencilFormat : 0);
    d3d.backbuffer_scale[0] = d3d.backbuffer_scale[1] = 1;

    ULONG *dev = d3d.device;
    if (!dev) dev = d3d.device = pool_alloc(4096);
    if (d3d.device_ptr) *d3d.device_ptr = (ULONG)dev;
    *ppDevice = dev;
    return D3D_OK;
}

/* ---- present and clears ---------------------------------------------- */

static void save_screenshot(const char *path)
{
    int w = d3d.width, h = d3d.height;
    uint8_t *px = malloc(w * h * 4);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    for (int y = 0; y < h; y++)
        memcpy((uint8_t *)s->pixels + y * s->pitch, px + (h - 1 - y) * w * 4, w * 4);
    if (SDL_SaveBMP(s, path) != 0) xlog("screenshot failed: %s", SDL_GetError());
    else xlog("D3D: saved frame %u to %s", d3d.frame, path);
    SDL_FreeSurface(s);
    free(px);
}

static ULONG NTAPI D3DDevice_Swap(ULONG Flags)
{
    (void)Flags;
    d3d.frame++;
    run_callbacks();
    if (g_screenshot_path && (int)d3d.frame == g_screenshot_frame)
        save_screenshot(g_screenshot_path);
    SDL_GL_SwapWindow(d3d.window);
    if (d3d.vblank_callback) {
        ULONG data[3] = { d3d.frame, d3d.frame, 1 /* D3DVBLANK_SWAPDONE */ };
        ((void (CDECLAPI *)(ULONG *))d3d.vblank_callback)(data);
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            xlog("window closed");
            exit(0);
        }
    }
    if (g_exit_after_frames && (int)d3d.frame >= g_exit_after_frames) {
        xlog("D3D: %u frames presented, exiting", d3d.frame);
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
    r->Common++;
    return r->Common & D3DCOMMON_REFCOUNT_MASK;
}

static ULONG NTAPI D3DResource_Release(D3DResource *r)
{
    ULONG refs = --r->Common & D3DCOMMON_REFCOUNT_MASK;
    if (refs == 0 && (r->Common & D3DCOMMON_D3DCREATED)) {
        ULONG type = r->Common & D3DCOMMON_TYPE_MASK;
        if (type == D3DCOMMON_TYPE_VERTEXBUFFER || type == D3DCOMMON_TYPE_INDEXBUFFER ||
            type == D3DCOMMON_TYPE_PALETTE) {
            MmFreeContiguousMemory(resource_data(r));
            pool_free(r);
        } else if (type == D3DCOMMON_TYPE_TEXTURE) {
            tex_invalidate(r->Data);
            MmFreeContiguousMemory(resource_data(r));
            pool_free(r);
        } else if (type == D3DCOMMON_TYPE_SURFACE) {
            D3DSurface *s = (D3DSurface *)r;
            if (s == d3d.backbuffer || s == d3d.depth) { r->Common++; return 1; }
            if (s->Parent) D3DResource_Release(&s->Parent->res);
            else MmFreeContiguousMemory(resource_data(r));
            pool_free(r);
        }
    }
    return refs;
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
    ULONG mem = (ULONG)base + r->Data;
    /* Push buffers keep a virtual address; everything else a GPU (physical) one. */
    r->Data = (r->Common & D3DCOMMON_TYPE_MASK) == 0x00020000 ? mem : (mem & 0x7FFFFFFF);
}

/* ---- textures ---------------------------------------------------------- */


typedef struct tex_entry {
    ULONG data, format, size;
    GLuint id;
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

static GLuint texture_for(D3DPixelContainer *t)
{
    for (tex_entry *e = tex_cache; e; e = e->next)
        if (e->data == t->res.Data && e->format == t->Format && e->size == t->Size) return e->id;

    ULONG fmt = (t->Format >> 8) & 0xFF;
    ULONG w, h, pitch = 0;
    if (t->Size) {
        w = (t->Size & 0xFFF) + 1;
        h = ((t->Size >> 12) & 0xFFF) + 1;
        pitch = ((t->Size >> 24) + 1) * 64;
    } else {
        w = 1u << ((t->Format >> 20) & 0xF);
        h = 1u << ((t->Format >> 24) & 0xF);
    }
    const uint8_t *src = (const uint8_t *)(t->res.Data | CONTIG_BASE);

    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    struct { ULONG fmt; int bpp; bool swizzled; GLenum gl_fmt, gl_type; bool force_alpha; } table[] = {
        { 0x06, 4, true,  GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, false },   /* A8R8G8B8 */
        { 0x07, 4, true,  GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, true },    /* X8R8G8B8 */
        { 0x12, 4, false, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, false },   /* LIN_A8R8G8B8 */
        { 0x1E, 4, false, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, true },    /* LIN_X8R8G8B8 */
        { 0x05, 2, true,  GL_RGB,  GL_UNSIGNED_SHORT_5_6_5, false },       /* R5G6B5 */
        { 0x11, 2, false, GL_RGB,  GL_UNSIGNED_SHORT_5_6_5, false },       /* LIN_R5G6B5 */
        { 0x02, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, false }, /* A1R5G5B5 */
        { 0x03, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, true },  /* X1R5G5B5 */
        { 0x10, 2, false, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, false }, /* LIN_A1R5G5B5 */
        { 0x04, 2, true,  GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, false }, /* A4R4G4B4 */
        { 0x1D, 2, false, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, false }, /* LIN_A4R4G4B4 */
        { 0x00, 1, true,  GL_LUMINANCE, GL_UNSIGNED_BYTE, false },         /* L8 */
        { 0x13, 1, false, GL_LUMINANCE, GL_UNSIGNED_BYTE, false },         /* LIN_L8 */
        { 0x19, 1, true,  GL_ALPHA, GL_UNSIGNED_BYTE, false },             /* A8 */
        { 0x1F, 1, false, GL_ALPHA, GL_UNSIGNED_BYTE, false },             /* LIN_A8 */
    };

    if (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F) {
        /* DXT1/3/5 are stored linearly in 4x4 blocks, like on the PC. */
        static const GLenum dxt[] = { 0x83F1, 0x83F2, 0x83F3 };  /* GL_COMPRESSED_RGBA_S3TC_DXT{1,3,5}_EXT */
        GLenum internal = dxt[fmt == 0x0C ? 0 : fmt == 0x0E ? 1 : 2];
        ULONG block = fmt == 0x0C ? 8 : 16;
        ULONG bytes = ((w + 3) / 4) * ((h + 3) / 4) * block;
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, bytes, src);
    } else {
        unsigned i = 0;
        while (i < sizeof(table) / sizeof(table[0]) && table[i].fmt != fmt) i++;
        if (i == sizeof(table) / sizeof(table[0])) {
            xlog("D3D: texture format %#x is not supported yet", fmt);
            uint32_t magenta = 0xFFFF00FF;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &magenta);
        } else {
            int bpp = table[i].bpp;
            uint8_t *px = malloc(w * h * bpp);
            if (table[i].swizzled) {
                unswizzle(src, px, w, h, bpp);
            } else {
                if (!pitch) pitch = w * bpp;
                for (ULONG y = 0; y < h; y++) memcpy(px + y * w * bpp, src + y * pitch, w * bpp);
            }
            if (table[i].force_alpha && bpp == 4)
                for (ULONG k = 0; k < w * h; k++) px[k * 4 + 3] = 0xFF;
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, table[i].force_alpha ? GL_RGB : GL_RGBA, w, h, 0,
                         table[i].gl_fmt, table[i].gl_type, px);
            free(px);
        }
    }
    TRACE("D3D: uploaded %ux%u texture format %#x", w, h, fmt);

    tex_entry *e = malloc(sizeof(*e));
    *e = (tex_entry){ t->res.Data, t->Format, t->Size, id, tex_cache };
    tex_cache = e;
    return id;
}

static void NTAPI D3DDevice_SetTexture(ULONG Stage, D3DResource *t)
{
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

/* Map one D3D texture op onto GL_COMBINE for either RGB or alpha. */
static void combine(bool alpha, ULONG op, ULONG arg1, ULONG arg2)
{
    GLenum mode_p = alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB;
    GLenum src0 = alpha ? GL_SOURCE0_ALPHA : GL_SOURCE0_RGB, src1 = alpha ? GL_SOURCE1_ALPHA : GL_SOURCE1_RGB;
    GLenum scale = alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE;
    GLenum mode = GL_MODULATE;
    float s = 1;
    switch (op) {
    case 1: mode = GL_REPLACE; arg1 = 1; break;   /* DISABLE: pass the current color */
    case 2: mode = GL_REPLACE; break;
    case 3: mode = GL_REPLACE; arg1 = arg2; break;
    case 4: mode = GL_MODULATE; break;
    case 5: mode = GL_MODULATE; s = 2; break;
    case 6: mode = GL_MODULATE; s = 4; break;
    case 7: mode = GL_ADD; break;
    case 8: mode = GL_ADD_SIGNED; break;
    case 9: mode = GL_ADD_SIGNED; s = 2; break;
    case 10: mode = GL_SUBTRACT; break;
    default:
        mode = GL_MODULATE;
    }
    glTexEnvi(GL_TEXTURE_ENV, mode_p, mode);
    glTexEnvi(GL_TEXTURE_ENV, src0, combine_source(arg1));
    glTexEnvi(GL_TEXTURE_ENV, src1, combine_source(arg2));
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

/* Only stage 0 for now; later stages fall back to pass-through. */
static bool apply_textures(void)
{
    D3DPixelContainer *t = (D3DPixelContainer *)d3d.textures[0];
    if (!t || TSS(0, D3DTSS_COLOROP) == 1) {
        glDisable(GL_TEXTURE_2D);
        return false;
    }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture_for(t));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(TSS(0, D3DTSS_ADDRESSU)));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(TSS(0, D3DTSS_ADDRESSV)));
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    float tf[4];
    color4(tf, RS(D3DRS_TEXTUREFACTOR));
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tf);
    combine(false, TSS(0, D3DTSS_COLOROP), TSS(0, D3DTSS_COLORARG1), TSS(0, D3DTSS_COLORARG2));
    ULONG aop = TSS(0, D3DTSS_ALPHAOP);
    combine(true, aop, TSS(0, D3DTSS_ALPHAARG1), TSS(0, D3DTSS_ALPHAARG2));
    return true;
}

/* ---- state setters ----------------------------------------------------- */

static void NTAPI D3DDevice_SetStreamSource(UINT_ Stream, D3DResource *vb, UINT_ Stride)
{
    if (Stream >= 16) return;
    d3d.streams[Stream].vb = vb;
    d3d.streams[Stream].stride = Stride;
}

static void NTAPI D3DDevice_SetVertexShader(ULONG Handle)
{
    d3d.vertex_shader = Handle;
    if (Handle & 1) xlog("D3D: programmable vertex shaders are not supported yet (%#x)", Handle);
}

static void NTAPI D3DDevice_SetTransform(ULONG State, const D3DMATRIX *m)
{
    if (State < 10) d3d.transforms[State] = *m;
}

static void NTAPI D3DDevice_SetViewport(const D3DVIEWPORT8 *v)
{
    if (v) d3d.viewport = *v;
    else d3d.viewport = (D3DVIEWPORT8){ 0, 0, d3d.width, d3d.height, 0, 1 };
}

static void FASTCALL SetRenderState_Simple(ULONG Method, ULONG Value)
{
    /* Inline header code already stored the value in D3D__RenderState. */
    (void)Method; (void)Value;
}

static LONG NTAPI SetRenderState_ParameterCheck(ULONG State, ULONG Value)
{
    (void)State; (void)Value;
    return D3D_OK;
}

static void NTAPI SetRenderStateNotInline(ULONG State, ULONG Value)
{
    if (State < D3DRS_MAX) RS(State) = Value;
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
    if (Index < 8) d3d.lights[Index].enabled = Enable;
    return D3D_OK;
}

static void NTAPI D3DDevice_SetMaterial(const float *m)
{
    /* D3DMATERIAL8: Diffuse, Ambient, Specular, Emissive, Power */
    memcpy(d3d.material, m, 64);
    d3d.material_power = m[16];
}

static void NTAPI D3DDevice_BlockUntilIdle(void) { run_callbacks(); }
static void NTAPI D3DDevice_BlockUntilVerticalBlank(void) {}
static BOOLEAN NTAPI D3DDevice_IsBusy(void) { return 0; }
static void NTAPI D3DDevice_SetFlickerFilter(ULONG v) { (void)v; }
static void NTAPI D3DDevice_SetSoftDisplayFilter(ULONG v) { (void)v; }

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
    apply_viewport();

    if (RS(D3DRS_ZENABLE)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(RS(D3DRS_ZFUNC));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(RS(D3DRS_ZWRITEENABLE) ? GL_TRUE : GL_FALSE);

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
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, d3d.material[0]);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, d3d.material[1]);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, d3d.material[2]);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, d3d.material[3]);
        glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, d3d.material_power > 128 ? 128 : d3d.material_power);
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
    int pos_size, pos_off, normal_off, diffuse_off, specular_off, tex_off[4], tex_size[4], ntex;
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
    return l;
}

static void draw(ULONG PrimitiveType, const UCHAR *base, ULONG stride, ULONG first, ULONG count,
                 const USHORT *indices)
{
    if (d3d.vertex_shader & 1) return;
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
    ULONG tci = TSS(0, D3DTSS_TEXCOORDINDEX) & 0xFFFF;
    if (apply_textures() && tci < (ULONG)l.ntex) {
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(l.tex_size[tci], GL_FLOAT, stride, base + l.tex_off[tci]);
    } else {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoord2f(0, 0);
    }

    if (indices)
        glDrawElements(gl_primitive(PrimitiveType), count, GL_UNSIGNED_SHORT, indices + first);
    else
        glDrawArrays(gl_primitive(PrimitiveType), first, count);
}

static void NTAPI D3DDevice_DrawVertices(ULONG PrimitiveType, UINT_ StartVertex, UINT_ VertexCount)
{
    D3DResource *vb = d3d.streams[0].vb;
    if (!vb) return;
    draw(PrimitiveType, resource_data(vb), d3d.streams[0].stride, StartVertex, VertexCount, NULL);
}

static void NTAPI D3DDevice_DrawVerticesUP(ULONG PrimitiveType, UINT_ VertexCount, const void *pData,
                                           UINT_ Stride)
{
    draw(PrimitiveType, pData, Stride, 0, VertexCount, NULL);
}

static void NTAPI D3DDevice_DrawIndexedVertices(ULONG PrimitiveType, UINT_ VertexCount, const USHORT *pIndexData)
{
    D3DResource *vb = d3d.streams[0].vb;
    if (!vb) return;
    const UCHAR *base = (const UCHAR *)resource_data(vb) + d3d.base_vertex_index * d3d.streams[0].stride;
    draw(PrimitiveType, base, d3d.streams[0].stride, 0, VertexCount, pIndexData);
}

static void NTAPI D3DDevice_DrawIndexedVerticesUP(ULONG PrimitiveType, UINT_ VertexCount, const USHORT *pIndexData,
                                                  const void *pVertexData, UINT_ Stride)
{
    draw(PrimitiveType, pVertexData, Stride, 0, VertexCount, pIndexData);
}

static void NTAPI D3DDevice_SetIndices(D3DResource *ib, UINT_ BaseVertexIndex)
{
    d3d.indices = ib;
    d3d.base_vertex_index = BaseVertexIndex;
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
static void (APIENTRY *p_glBindFramebuffer)(GLenum, GLuint);
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
    d3d.target = d3d.backbuffer;
    d3d.target_depth = d3d.depth;
    d3d.rt_width = d3d.width;
    d3d.rt_height = d3d.height;
    d3d.rt_texture = false;
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

static void NTAPI D3DDevice_SetRenderTarget(D3DSurface *target, D3DSurface *z)
{
    if (!target) target = d3d.target;
    d3d.target = target;
    d3d.target_depth = z;
    if (!target->Parent) {
        /* The back buffer (or a plain surface we cannot render into yet). */
        if (target != d3d.backbuffer)
            xlog("D3D: rendering into a standalone surface is not supported; using the back buffer");
        if (p_glBindFramebuffer) p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        d3d.rt_width = d3d.width;
        d3d.rt_height = d3d.height;
        d3d.rt_texture = false;
        return;
    }
    if (!p_glGenFramebuffers) {
        xlog("D3D: no framebuffer objects, cannot render to texture");
        return;
    }
    ULONG w, h, pitch;
    container_size(target->Parent, &w, &h, &pitch);
    if (!d3d.fbo) p_glGenFramebuffers(1, (GLuint *)&d3d.fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, d3d.fbo);
    GLuint tex = texture_for(target->Parent);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    /* A depth buffer always comes along: titles clear z without checking. */
    static GLuint rb; static ULONG rb_w, rb_h;
    if (!rb || rb_w != w || rb_h != h) {
        if (!rb) p_glGenRenderbuffers(1, &rb);
        p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
        p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        rb_w = w; rb_h = h;
    }
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) xlog("D3D: render target framebuffer incomplete (%#x)", st);
    d3d.rt_width = w;
    d3d.rt_height = h;
    d3d.rt_texture = true;
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
    if (s == d3d.backbuffer) {
        /* Hand out the real pixels: read the frame back. */
        uint8_t *px = (uint8_t *)(s->Data | CONTIG_BASE);
        uint8_t *tmp = malloc(w * h * 4);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
        for (ULONG y = 0; y < h; y++) memcpy(px + y * pitch, tmp + (h - 1 - y) * w * 4, w * 4);
        free(tmp);
    }
    if (s->Parent && !(flags & 0x80)) tex_invalidate(s->Parent->res.Data);
    locked[0] = pitch;
    locked[1] = (s->Data | CONTIG_BASE) + (rect ? rect[1] * pitch + rect[0] * (pitch / w) : 0);
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
    ULONG sw, sh, sp, dw, dh, dp;
    container_size((D3DPixelContainer *)src, &sw, &sh, &sp);
    container_size((D3DPixelContainer *)dst, &dw, &dh, &dp);
    bool lin;
    int bytes = format_bits((src->Format >> 8) & 0xFF, &lin) / 8;
    if (src == d3d.backbuffer) {
        ULONG locked[2];
        D3DSurface_LockRect(src, locked, NULL, 0x80);   /* refresh its pixels */
    }
    const uint8_t *s = (const uint8_t *)(src->Data | CONTIG_BASE);
    uint8_t *d = (uint8_t *)(dst->Data | CONTIG_BASE);
    if (!n) {
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

static void NTAPI D3DTexture_GetLevelDesc(D3DPixelContainer *t, UINT_ level, ULONG *desc)
{
    ULONG w, h, pitch;
    container_size(t, &w, &h, &pitch);
    for (UINT_ i = 0; i < level; i++) { if (w > 1) w >>= 1; if (h > 1) h >>= 1; }
    ULONG fmt = (t->Format >> 8) & 0xFF;
    desc[0] = fmt;
    desc[1] = 3;   /* D3DRTYPE_TEXTURE */
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
    if (!linear) pitch = w * bits / 8;
    if (!(flags & 0x80)) tex_invalidate(t->res.Data);
    locked[0] = pitch;
    locked[1] = (t->res.Data | CONTIG_BASE) + level_offset(t, level) + face * cube_face_bytes(t) +
                (rect ? rect[1] * pitch + rect[0] * bits / 8 : 0);
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

static void NTAPI D3DVolumeTexture_LockBox(D3DPixelContainer *t, UINT_ level, ULONG *locked, const LONG *box,
                                           ULONG flags)
{
    (void)box;
    ULONG w, h, pitch;
    container_size(t, &w, &h, &pitch);
    ULONG rl[2];
    lock_level(t, 0, level, rl, NULL, flags);
    locked[0] = rl[0];                    /* RowPitch */
    locked[1] = rl[0] * (h >> level);     /* SlicePitch */
    locked[2] = rl[1];
}

/* ---- index buffers, palettes, memory --------------------------------- */

static LONG NTAPI D3DDevice_CreateIndexBuffer(UINT_ Length, ULONG Usage, ULONG Format, ULONG Pool, D3DResource **pp)
{
    (void)Usage; (void)Format; (void)Pool;
    D3DResource *ib = pool_alloc(sizeof(*ib));
    PVOID mem = MmAllocateContiguousMemoryEx(Length, 0, 0x7FFFFFFF, 0, 4);
    if (!ib || !mem) return 0x8007000E;
    ib->Common = 1 | D3DCOMMON_TYPE_INDEXBUFFER | D3DCOMMON_D3DCREATED;
    ib->Data = (ULONG)mem & 0x7FFFFFFF;
    ib->Lock = 0;
    *pp = ib;
    return D3D_OK;
}

static void NTAPI D3DIndexBuffer_Lock(D3DResource *ib, UINT_ Offset, UINT_ Size, PVOID *ppbData, ULONG Flags)
{
    (void)Size; (void)Flags;
    *ppbData = (UCHAR *)resource_data(ib) + Offset;
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

static void NTAPI D3DDevice_GetRenderState(ULONG State, ULONG *v) { *v = State < D3DRS_MAX ? RS(State) : 0; }
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

static void NTAPI D3DDevice_GetDeviceCaps(ULONG *caps)
{
    memset(caps, 0, 212);
    caps[0] = 1;            /* D3DDEVTYPE_HAL */
    caps[27] = caps[28] = 4096;   /* MaxTextureWidth/Height */
    caps[29] = 512;         /* MaxVolumeExtent */
    caps[30] = 8192;        /* MaxTextureRepeat */
    caps[31] = 4096;        /* MaxTextureAspectRatio */
    caps[32] = 4;           /* MaxAnisotropy */
    caps[44] = 0x8;         /* FVFCaps: texcoord count 8 */
    caps[46] = 8;           /* MaxActiveLights */
    caps[47] = 2;           /* MaxUserClipPlanes */
    caps[48] = 4;           /* MaxVertexBlendMatrices */
    caps[50] = 1 | 0x100;   /* VertexShaderVersion 1.1-ish */
    caps[51] = 192;         /* MaxVertexShaderConst */
    caps[52] = 1 | 0x100;   /* PixelShaderVersion */
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
static void NTAPI D3DDevice_SetSwapCallback(PVOID fn) { (void)fn; }
static void NTAPI D3DDevice_GetDisplayFieldStatus(ULONG *st) { st[0] = 3; st[1] = d3d.frame; }
static LONG NTAPI D3DDevice_PersistDisplay(void) { return D3D_OK; }
static void NTAPI D3DDevice_SetScreenSpaceOffset(float x, float y) { d3d.screen_offset[0] = x; d3d.screen_offset[1] = y; }
static void NTAPI D3DDevice_SetBackBufferScale(float x, float y) { d3d.backbuffer_scale[0] = x; d3d.backbuffer_scale[1] = y; }
static void NTAPI D3DDevice_GetBackBufferScale(float *x, float *y) { *x = d3d.backbuffer_scale[0]; *y = d3d.backbuffer_scale[1]; }
static void NTAPI D3DDevice_SetDebugMarker(ULONG v) { (void)v; }
static void NTAPI D3DDevice_SetTile(ULONG i, const void *t) { (void)i; (void)t; }
static void NTAPI D3DDevice_GetTile(ULONG i, ULONG *t) { (void)i; memset(t, 0, 24); }
static void NTAPI D3DDevice_KickPushBuffer(void) {}
static ULONG NTAPI D3DDevice_InsertFence(void) { return ++d3d.frame * 0 + 1; }
static BOOLEAN NTAPI D3DDevice_IsFencePending(ULONG f) { (void)f; return 0; }
static void NTAPI D3DDevice_BlockOnFence(ULONG f) { (void)f; }
static void NTAPI D3DDevice_SetGammaRamp(ULONG flags, const void *ramp) { (void)flags; (void)ramp; }
static void NTAPI D3DDevice_GetGammaRamp(USHORT *ramp) { for (int i = 0; i < 768; i++) ramp[i] = (i % 256) * 257; }
static void NTAPI D3DDevice_EnableOverlay(BOOLEAN on) { (void)on; }
static void NTAPI D3DDevice_UpdateOverlay(D3DSurface *s, const void *a, const void *b, BOOLEAN c, ULONG d) {}
static BOOLEAN NTAPI D3DDevice_GetOverlayUpdateStatus(void) { return 1; }
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

/* A handle is the address of a copy of the D3DPIXELSHADERDEF; the def is
   51 render states (D3DRS_PSALPHAINPUTS0 .. D3DRS_PSFINALCOMBINERCONSTANTS)
   and SetPixelShader loads it into the title's render state array, like
   the real library. */
#define PSDEF_DWORDS 51

static void NTAPI D3DDevice_CreatePixelShader(const ULONG *def, ULONG *handle)
{
    ULONG *copy = pool_alloc(PSDEF_DWORDS * 4);
    memcpy(copy, def, PSDEF_DWORDS * 4);
    *handle = (ULONG)copy;
}

static void NTAPI D3DDevice_SetPixelShader(ULONG handle)
{
    d3d.pixel_shader = handle;
    if (handle) memcpy(d3d.render_state, (const void *)handle, PSDEF_DWORDS * 4);
}

static void NTAPI D3DDevice_SetPixelShaderProgram(const ULONG *def)
{
    ULONG h;
    D3DDevice_CreatePixelShader(def, &h);
    D3DDevice_SetPixelShader(h);
}

static void NTAPI D3DDevice_GetPixelShader(ULONG *handle) { *handle = d3d.pixel_shader; }
static void NTAPI D3DDevice_DeletePixelShader(ULONG handle) { if (handle) pool_free((void *)handle); }
static void NTAPI D3DDevice_GetPixelShaderFunction(ULONG handle, ULONG *def)
{
    if (handle) memcpy(def, (const void *)handle, PSDEF_DWORDS * 4);
}

static void NTAPI D3DDevice_SetPixelShaderConstant(ULONG Register, const float *data, ULONG count)
{
    for (ULONG i = 0; i < count && Register + i < 16; i++) memcpy(d3d.ps_const[Register + i], data + i * 4, 16);
}

static void NTAPI D3DDevice_GetPixelShaderConstant(ULONG Register, float *data, ULONG count)
{
    for (ULONG i = 0; i < count && Register + i < 16; i++) memcpy(data + i * 4, d3d.ps_const[Register + i], 16);
}

/* ---- table ------------------------------------------------------------- */

#define F(dec, fn) { dec, (void *)fn }
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
