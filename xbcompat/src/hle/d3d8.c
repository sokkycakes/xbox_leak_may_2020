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
    ULONG vertex_shader;
    D3DResource *indices;
    ULONG base_vertex_index;
    struct { bool set; float diffuse[4], ambient[4], specular[4], position[4], direction[4];
             ULONG type; float range, attenuation[3]; bool enabled; } lights[8];
    float material[4][4];        /* diffuse, ambient, specular, emissive */
    float material_power;
} d3d;

extern int g_screenshot_frame;
extern const char *g_screenshot_path;
extern int g_exit_after_frames;

#define RS(i) (d3d.render_state[i])

/* ---- device ------------------------------------------------------------ */

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
}

void d3d_bind_globals(void)
{
    d3d.render_state = (ULONG *)hle_lookup("_D3D__RenderState");
    if (!d3d.render_state) d3d.render_state = d3d.render_state_fallback;
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
    if (g_screenshot_path && (int)d3d.frame == g_screenshot_frame)
        save_screenshot(g_screenshot_path);
    SDL_GL_SwapWindow(d3d.window);
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
    glViewport(v->X, d3d.height - (v->Y + v->Height), v->Width, v->Height);
    glDepthRange(v->MinZ, v->MaxZ);
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
            glScissor(r->x1, d3d.height - r->y2, r->x2 - r->x1, r->y2 - r->y1);
            glClear(mask);
        }
        glDisable(GL_SCISSOR_TEST);
    } else {
        glClear(mask);
    }
    glColorMask(1, 1, 1, 1);
}

/* ---- resources --------------------------------------------------------- */

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
        if (type == D3DCOMMON_TYPE_VERTEXBUFFER || type == D3DCOMMON_TYPE_INDEXBUFFER) {
            MmFreeContiguousMemory(resource_data(r));
            pool_free(r);
        }
    }
    return refs;
}

static ULONG NTAPI D3DResource_GetType(D3DResource *r)
{
    static const ULONG types[] = { 6 /* VERTEXBUFFER */, 7 /* INDEXBUFFER */, 10 /* PUSHBUFFER */,
                                   9 /* PALETTE */, 3 /* TEXTURE */, 1 /* SURFACE */, 11 /* FIXUP */ };
    return types[(r->Common & D3DCOMMON_TYPE_MASK) >> 16];
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

static void NTAPI D3DDevice_BlockUntilIdle(void) {}
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
        glFrontFace(cull == 0x900 ? GL_CCW : GL_CW);
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
        glOrtho(0, d3d.viewport.Width, d3d.viewport.Height, 0, 0, -1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(-(float)d3d.viewport.X, -(float)d3d.viewport.Y, 0);
    } else {
        /* D3D row-vector matrices loaded as-is are the GL column-vector
           transforms.  D3D clip space has z in [0,w]; GL wants [-w,w]. */
        static const D3DMATRIX zfix = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 2, 0 }, { 0, 0, -1, 1 } } };
        D3DMATRIX proj, modelview;
        mat_mul(&proj, &d3d.transforms[1], &zfix);
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
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);

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

/* ---- table ------------------------------------------------------------- */

#define F(dec, fn) { dec, (void *)fn }
static const struct hle_func funcs[] = {
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
    { NULL, NULL },
};

const struct hle_func *hle_find(const char *name)
{
    for (const struct hle_func *f = funcs; f->name; f++)
        if (!strcmp(f->name, name)) return f;
    return NULL;
}
