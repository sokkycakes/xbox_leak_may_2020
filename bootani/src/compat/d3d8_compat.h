//
//  d3d8_compat.h
//
//  The subset of the Xbox Direct3D 8 API that the boot animation uses,
//  declared portably. The implementation (gfx/d3d8_gl.cpp) runs it on
//  OpenGL 3.3 core. Enumerant values are private to this layer: the
//  animation only ever uses them symbolically, except FVF codes and shader
//  handles which keep the D3D8 convention (shader handles have bit 0 set).
//
#ifndef BOOTANI_D3D8_COMPAT_H
#define BOOTANI_D3D8_COMPAT_H

#include "xbox_compat.h"

//------------------------------------------------------------------------------
// Basic types

typedef struct _D3DVECTOR { float x, y, z; } D3DVECTOR;

typedef struct _D3DCOLORVALUE { float r, g, b, a; } D3DCOLORVALUE;

typedef struct _D3DMATRIX {
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
} D3DMATRIX;

typedef DWORD D3DCOLOR;
#define D3DCOLOR_ARGB(a, r, g, b) \
    ((D3DCOLOR)((((a) & 0xff) << 24) | (((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))

typedef struct _D3DVIEWPORT8 {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT8;

typedef enum _D3DLIGHTTYPE { D3DLIGHT_POINT = 1, D3DLIGHT_SPOT = 2, D3DLIGHT_DIRECTIONAL = 3 } D3DLIGHTTYPE;

typedef struct _D3DLIGHT8 {
    D3DLIGHTTYPE  Type;
    D3DCOLORVALUE Diffuse;
    D3DCOLORVALUE Specular;
    D3DCOLORVALUE Ambient;
    D3DVECTOR     Position;
    D3DVECTOR     Direction;
    float         Range;
    float         Falloff;
    float         Attenuation0;
    float         Attenuation1;
    float         Attenuation2;
    float         Theta;
    float         Phi;
} D3DLIGHT8;

typedef struct _D3DLOCKED_RECT { INT Pitch; void* pBits; } D3DLOCKED_RECT;

//------------------------------------------------------------------------------
// Enumerations

typedef enum _D3DFORMAT {
    D3DFMT_UNKNOWN = 0,
    D3DFMT_A8R8G8B8,
    D3DFMT_X8R8G8B8,
    D3DFMT_A8,
    D3DFMT_D24S8,
    D3DFMT_INDEX16,
    // Xbox "linear" (unswizzled) formats. Textures in these formats are
    // addressed with texel (unnormalised) coordinates on the NV2A.
    D3DFMT_LIN_A8R8G8B8,
    D3DFMT_LIN_X8R8G8B8,
    D3DFMT_LIN_D16,
    D3DFMT_LIN_D24S8,
    D3DFMT_LIN_R5G6B5,
} D3DFORMAT;

typedef struct _D3DSURFACE_DESC {
    D3DFORMAT Format;
    DWORD     Usage;
    UINT      Size;
    UINT      Width;
    UINT      Height;
} D3DSURFACE_DESC;

typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1, D3DPT_LINELIST, D3DPT_LINESTRIP,
    D3DPT_TRIANGLELIST, D3DPT_TRIANGLESTRIP, D3DPT_TRIANGLEFAN
} D3DPRIMITIVETYPE;

typedef enum _D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW = 0, D3DTS_PROJECTION, D3DTS_WORLD, D3DTS_MAX
} D3DTRANSFORMSTATETYPE;

typedef enum _D3DRENDERSTATETYPE {
    D3DRS_ZENABLE = 0,
    D3DRS_FILLMODE,
    D3DRS_BACKFILLMODE,
    D3DRS_ZWRITEENABLE,
    D3DRS_ALPHATESTENABLE,
    D3DRS_SRCBLEND,
    D3DRS_DESTBLEND,
    D3DRS_CULLMODE,
    D3DRS_ZFUNC,
    D3DRS_ALPHAREF,
    D3DRS_ALPHAFUNC,
    D3DRS_DITHERENABLE,
    D3DRS_ALPHABLENDENABLE,
    D3DRS_FOGENABLE,
    D3DRS_EDGEANTIALIAS,
    D3DRS_STENCILENABLE,
    D3DRS_LIGHTING,
    D3DRS_TEXTUREFACTOR,
    D3DRS_COLORWRITEENABLE,
    D3DRS_MULTISAMPLEMASK,
    D3DRS_LOGICOP,
    D3DRS_YUVENABLE,
    D3DRS_SHADOWFUNC,
    D3DRS_SOLIDOFFSETENABLE,
    D3DRS_POLYGONOFFSETZOFFSET,
    D3DRS_POLYGONOFFSETZSLOPESCALE,
    D3DRS_MAX
} D3DRENDERSTATETYPE;

typedef enum _D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP = 0,
    D3DTSS_COLORARG1,
    D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP,
    D3DTSS_ALPHAARG1,
    D3DTSS_ALPHAARG2,
    D3DTSS_TEXCOORDINDEX,
    D3DTSS_ADDRESSU,
    D3DTSS_ADDRESSV,
    D3DTSS_ADDRESSW,
    D3DTSS_BORDERCOLOR,
    D3DTSS_MAGFILTER,
    D3DTSS_MINFILTER,
    D3DTSS_MIPFILTER,
    D3DTSS_MAXANISOTROPY,
    D3DTSS_TEXTURETRANSFORMFLAGS,
    D3DTSS_COLORKEYOP,
    D3DTSS_COLORSIGN,
    D3DTSS_ALPHAKILL,
    D3DTSS_MAX
} D3DTEXTURESTAGESTATETYPE;

enum { D3DZB_FALSE = 0, D3DZB_TRUE = 1 };
enum { D3DFILL_POINT = 1, D3DFILL_WIREFRAME, D3DFILL_SOLID };
enum { D3DCULL_NONE = 1, D3DCULL_CW, D3DCULL_CCW };
enum {
    D3DCMP_NEVER = 1, D3DCMP_LESS, D3DCMP_EQUAL, D3DCMP_LESSEQUAL,
    D3DCMP_GREATER, D3DCMP_NOTEQUAL, D3DCMP_GREATEREQUAL, D3DCMP_ALWAYS
};
enum {
    D3DBLEND_ZERO = 1, D3DBLEND_ONE, D3DBLEND_SRCCOLOR, D3DBLEND_INVSRCCOLOR,
    D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLEND_DESTALPHA,
    D3DBLEND_INVDESTALPHA, D3DBLEND_DESTCOLOR, D3DBLEND_INVDESTCOLOR
};
enum {
    D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE,
    D3DTOP_MODULATE2X, D3DTOP_ADD
};
enum { D3DTA_DIFFUSE = 0, D3DTA_CURRENT = 1, D3DTA_TEXTURE = 2, D3DTA_TFACTOR = 3 };
enum { D3DTADDRESS_WRAP = 1, D3DTADDRESS_MIRROR, D3DTADDRESS_CLAMP, D3DTADDRESS_BORDER };
enum { D3DTEXF_NONE = 0, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_ANISOTROPIC };
enum { D3DTTFF_DISABLE = 0 };
enum { D3DTCOLORKEYOP_DISABLE = 0 };
enum { D3DTALPHAKILL_DISABLE = 0 };
enum { D3DLOGICOP_NONE = 0 };
enum {
    D3DCOLORWRITEENABLE_RED = 1 << 16, D3DCOLORWRITEENABLE_GREEN = 1 << 8,
    D3DCOLORWRITEENABLE_BLUE = 1 << 0, D3DCOLORWRITEENABLE_ALPHA = 1 << 24,
    D3DCOLORWRITEENABLE_ALL = 0x01010101
};

#define D3DCLEAR_TARGET   0x00000001
#define D3DCLEAR_ZBUFFER  0x00000002
#define D3DCLEAR_STENCIL  0x00000004

#define D3DUSAGE_RENDERTARGET 0x00000001
#define D3DUSAGE_DEPTHSTENCIL 0x00000002
#define D3DUSAGE_WRITEONLY    0x00000008

#define D3DLOCK_DISCARD 0x00002000

typedef enum _D3DPOOL { D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1 } D3DPOOL;

typedef enum _D3DCUBEMAP_FACES {
    D3DCUBEMAP_FACE_POSITIVE_X = 0, D3DCUBEMAP_FACE_NEGATIVE_X,
    D3DCUBEMAP_FACE_POSITIVE_Y, D3DCUBEMAP_FACE_NEGATIVE_Y,
    D3DCUBEMAP_FACE_POSITIVE_Z, D3DCUBEMAP_FACE_NEGATIVE_Z
} D3DCUBEMAP_FACES;

typedef enum _D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE = 0,
    D3DMULTISAMPLE_2_SAMPLES_MULTISAMPLE_LINEAR,
    D3DMULTISAMPLE_2_SAMPLES_SUPERSAMPLE_HORIZONTAL_LINEAR,
} D3DMULTISAMPLE_TYPE;

#define D3DTEXTURE_ALIGNMENT 128
#define D3DZ_MAX_D16 65535.0f

// Flexible vertex formats (D3D8 values).
#define D3DFVF_XYZ     0x002
#define D3DFVF_XYZRHW  0x004
#define D3DFVF_NORMAL  0x010
#define D3DFVF_DIFFUSE 0x040
#define D3DFVF_TEX1    0x100
#define D3DFVF_TEX2    0x200

// Vertex shader declaration tokens (encoding private to this layer).
#define D3DVSDT_FLOAT1 1
#define D3DVSDT_FLOAT2 2
#define D3DVSDT_FLOAT3 3
#define D3DVSDT_FLOAT4 4
#define D3DVSD_STREAM(n)       ((DWORD)(0x10000000u | (DWORD)(n)))
#define D3DVSD_REG(reg, type)  ((DWORD)(0x20000000u | ((DWORD)(type) << 8) | (DWORD)(reg)))
#define D3DVSD_END()           ((DWORD)0xFFFFFFFFu)

// Device creation (accepted and ignored).
#define D3D_SDK_VERSION 220
#define D3DADAPTER_DEFAULT 0
#define D3DDEVTYPE_HAL 1
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x40
#define D3DSWAPEFFECT_DISCARD 1
#define D3DPRESENTFLAG_WIDESCREEN 0x10
#define D3DPRESENTFLAG_INTERLACED 0x20
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000

#define D3D_OK S_OK

typedef struct _D3DPRESENT_PARAMETERS {
    UINT      BackBufferWidth;
    UINT      BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT      BackBufferCount;
    DWORD     MultiSampleType;
    DWORD     SwapEffect;
    BOOL      EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD     Flags;
    UINT      FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS;

//------------------------------------------------------------------------------
// Precompiled shaders. On the Xbox these are NV2A microcode blobs; here the
// blob is a tag naming the GLSL translation ("XVU:name" / "XPU:name").

typedef struct _D3DPIXELSHADERDEF { char Name[1]; } D3DPIXELSHADERDEF;
typedef struct _D3DPIXELSHADERDEF_FILE { char Tag[4]; D3DPIXELSHADERDEF Psd; } D3DPIXELSHADERDEF_FILE;

//------------------------------------------------------------------------------
// Resources

namespace bootani_gl { struct TextureImpl; struct BufferImpl; struct SurfaceImpl; }

class IDirect3DResource8
{
public:
    ULONG AddRef()  { return ++m_refs; }
    ULONG Release();
    virtual ~IDirect3DResource8() {}
protected:
    IDirect3DResource8() : m_refs(1) {}
    ULONG m_refs;
};

class IDirect3DSurface8;

class IDirect3DBaseTexture8 : public IDirect3DResource8
{
public:
    bootani_gl::TextureImpl* impl;
protected:
    IDirect3DBaseTexture8() : impl(NULL) {}
    ~IDirect3DBaseTexture8();
};

class IDirect3DTexture8 : public IDirect3DBaseTexture8
{
public:
    HRESULT GetLevelDesc(UINT level, D3DSURFACE_DESC* desc);
    HRESULT LockRect(UINT level, D3DLOCKED_RECT* locked, const void* rect, DWORD flags);
    HRESULT UnlockRect(UINT level);
    HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface8** surface);
};

class IDirect3DCubeTexture8 : public IDirect3DBaseTexture8
{
public:
    HRESULT GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** surface);
};

// Surfaces are plain data so that the animation can memset one and hand it
// to XGSetSurfaceHeader (the "fake" colour target of the shadow pass).
class IDirect3DSurface8
{
public:
    // Kind of surface.
    enum Kind { KIND_NONE = 0, KIND_TEXTURE_LEVEL, KIND_CUBE_FACE, KIND_RENDERTARGET,
                KIND_DEPTH, KIND_HEADER_ONLY };
    int                      kind;
    ULONG                    refs;
    IDirect3DBaseTexture8*   owner;       // texture this is a level/face of
    int                      face;
    int                      level;
    bootani_gl::SurfaceImpl* impl;        // standalone render target / depth buffer
    D3DFORMAT                format;
    UINT                     width, height;

    ULONG AddRef()  { return ++refs; }
    ULONG Release();
    HRESULT LockRect(D3DLOCKED_RECT* locked, const void* rect, DWORD flags);
    HRESULT UnlockRect();
};
typedef IDirect3DSurface8 D3DSurface;

class IDirect3DVertexBuffer8 : public IDirect3DResource8
{
public:
    bootani_gl::BufferImpl* impl;
    HRESULT Lock(UINT offset, UINT size, BYTE** data, DWORD flags);
    HRESULT Unlock();
    IDirect3DVertexBuffer8() : impl(NULL) {}
    ~IDirect3DVertexBuffer8();
};

class IDirect3DIndexBuffer8 : public IDirect3DResource8
{
public:
    bootani_gl::BufferImpl* impl;
    HRESULT Lock(UINT offset, UINT size, BYTE** data, DWORD flags);
    HRESULT Unlock();
    IDirect3DIndexBuffer8() : impl(NULL) {}
    ~IDirect3DIndexBuffer8();
};

typedef IDirect3DBaseTexture8   D3DBaseTexture;
typedef IDirect3DTexture8*      LPDIRECT3DTEXTURE8;
typedef IDirect3DCubeTexture8*  LPDIRECT3DCUBETEXTURE8;
typedef IDirect3DSurface8*      LPDIRECT3DSURFACE8;
typedef IDirect3DVertexBuffer8* LPDIRECT3DVERTEXBUFFER8;
typedef IDirect3DIndexBuffer8*  LPDIRECT3DINDEXBUFFER8;

//------------------------------------------------------------------------------
// Device

class IDirect3DDevice8
{
public:
    ULONG   Release();

    HRESULT BeginScene();
    HRESULT EndScene();
    HRESULT Present(const void*, const void*, void*, const void*);
    HRESULT Clear(DWORD count, const void* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);

    HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value);
    HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
    HRESULT SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
    HRESULT SetViewport(const D3DVIEWPORT8* viewport);

    HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture8* texture);
    HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer8* vb, UINT stride);
    HRESULT SetIndices(IDirect3DIndexBuffer8* ib, UINT base_vertex);

    HRESULT CreateVertexShader(const DWORD* decl, const DWORD* function, DWORD* handle, DWORD usage);
    HRESULT CreatePixelShader(const D3DPIXELSHADERDEF* def, DWORD* handle);
    HRESULT DeleteVertexShader(DWORD handle);
    HRESULT DeletePixelShader(DWORD handle);
    HRESULT SetVertexShader(DWORD handle_or_fvf);
    HRESULT SetPixelShader(DWORD handle);
    HRESULT SetVertexShaderConstant(INT reg, const void* data, DWORD count);
    HRESULT SetPixelShaderConstant(DWORD reg, const void* data, DWORD count);

    HRESULT DrawPrimitive(D3DPRIMITIVETYPE type, UINT start_vertex, UINT prim_count);
    HRESULT DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT min_index, UINT num_vertices,
                                 UINT start_index, UINT prim_count);
    // Xbox extension: draw with indices from CPU memory; count is the index count.
    HRESULT DrawIndexedVertices(D3DPRIMITIVETYPE type, UINT index_count, const WORD* indices);

    HRESULT CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, DWORD pool,
                          IDirect3DTexture8** out);
    HRESULT CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, DWORD pool,
                              IDirect3DCubeTexture8** out);
    HRESULT CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, DWORD pool,
                               IDirect3DVertexBuffer8** out);
    HRESULT CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT fmt, DWORD pool,
                              IDirect3DIndexBuffer8** out);
    HRESULT CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, DWORD multisample, BOOL lockable,
                               IDirect3DSurface8** out);
    HRESULT CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, DWORD multisample,
                                      IDirect3DSurface8** out);

    HRESULT GetRenderTarget(IDirect3DSurface8** out);
    HRESULT GetDepthStencilSurface(IDirect3DSurface8** out);
    HRESULT SetRenderTarget(IDirect3DSurface8* color, IDirect3DSurface8* depth);
    HRESULT CopyRects(IDirect3DSurface8* src, const void* src_rects, UINT count,
                      IDirect3DSurface8* dst, const void* dst_points);

    void    BlockUntilVerticalBlank() {}
};

class IDirect3D8
{
public:
    ULONG   Release();
    HRESULT CreateDevice(UINT adapter, DWORD type, void* window, DWORD flags,
                         D3DPRESENT_PARAMETERS* params, IDirect3DDevice8** device);
    void    SetPushBufferSize(DWORD, DWORD) {}
};

IDirect3D8* Direct3DCreate8(UINT sdk_version);

//------------------------------------------------------------------------------
// XGraphics helpers. Our textures are stored linearly, so "swizzling" is a
// copy of the rectangle into the locked level.

void XGSwizzleRect(const void* src, DWORD pitch, const void* rect, void* dst,
                   DWORD width, DWORD height, const void* point, DWORD bytes_per_pixel);
void XGSetSurfaceHeader(UINT width, UINT height, D3DFORMAT fmt, IDirect3DSurface8* surface,
                        UINT data, UINT pitch);

#endif // BOOTANI_D3D8_COMPAT_H
