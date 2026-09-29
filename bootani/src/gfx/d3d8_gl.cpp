//
//  d3d8_gl.cpp
//
//  The Xbox Direct3D 8 subset used by the boot animation, implemented on
//  OpenGL 3.3 core.
//
//  Conventions:
//  - Everything is rendered into framebuffer objects with clip-space y
//    negated (see xbox_emit in shaders_glsl.cpp). GL window coordinates then
//    equal D3D screen coordinates numerically, so row 0 of every render
//    target is the top row, as on D3D. Uploaded textures and rendered
//    textures therefore share one layout and texture coordinates need no
//    flipping. Only the final blit to a window flips.
//  - With y negated, D3D's clockwise winding is GL's counter-clockwise one:
//    glFrontFace(GL_CCW) once, D3DCULL_CCW -> cull GL_BACK, D3DCULL_CW ->
//    cull GL_FRONT.
//  - Xbox "linear" texture formats (D3DFMT_LIN_*) are addressed with texel
//    coordinates; the shaders multiply by texScale = 1/size for those.
//  - Xbox shadow compare returns (texel FUNC r); GL computes (r FUNC texel),
//    so D3DRS_SHADOWFUNC is mirrored when programming the sampler.
//
#include "../compat/d3d8_compat.h"
#include "d3d8_gl.h"
#include "shaders_glsl.h"

#include <map>
#include <string>
#include <vector>
#include <utility>

namespace bootani_gl {

//------------------------------------------------------------------------------
// Resource implementations

struct TextureImpl
{
    GLuint    tex;
    GLenum    target;           // GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP
    D3DFORMAT format;
    UINT      width, height, levels;
    bool      linear;           // unnormalised texel addressing
    bool      depth;
    std::vector< std::vector<BYTE> > staging;   // [face * levels + level]
};

struct SurfaceImpl
{
    GLuint    rb;
    bool      depth;
    int       samples;
};

struct BufferImpl
{
    GLuint             buf;
    GLenum             target;
    std::vector<BYTE>  shadow;
};

static PresentHook g_present_hook = 0;
static void*       g_present_user = 0;
static int         g_backbuffer_samples = 1;

void SetPresentHook(PresentHook hook, void* user) { g_present_hook = hook; g_present_user = user; }
void SetBackBufferSamples(int samples) { g_backbuffer_samples = samples < 1 ? 1 : samples; }

static UINT BytesPerPixel(D3DFORMAT f)
{
    switch (f) {
    case D3DFMT_A8:          return 1;
    case D3DFMT_LIN_D16:
    case D3DFMT_LIN_R5G6B5:  return 2;
    default:                 return 4;
    }
}

static bool IsLinear(D3DFORMAT f)
{
    return f == D3DFMT_LIN_A8R8G8B8 || f == D3DFMT_LIN_X8R8G8B8 || f == D3DFMT_LIN_D16 ||
           f == D3DFMT_LIN_D24S8 || f == D3DFMT_LIN_R5G6B5;
}

static bool IsDepth(D3DFORMAT f)
{
    return f == D3DFMT_D24S8 || f == D3DFMT_LIN_D16 || f == D3DFMT_LIN_D24S8;
}

// Allocate storage for one image of a texture and apply the format swizzle.
static void DefineImage(TextureImpl* t, GLenum image_target, UINT level, const void* data)
{
    UINT w = t->width >> level;  if (!w) w = 1;
    UINT h = t->height >> level; if (!h) h = 1;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    switch (t->format) {
    case D3DFMT_A8:
        glTexImage2D(image_target, level, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, data);
        break;
    case D3DFMT_LIN_D16:
        glTexImage2D(image_target, level, GL_DEPTH_COMPONENT16, w, h, 0, GL_DEPTH_COMPONENT,
                     GL_UNSIGNED_SHORT, data);
        break;
    case D3DFMT_D24S8:
    case D3DFMT_LIN_D24S8:
        glTexImage2D(image_target, level, GL_DEPTH24_STENCIL8, w, h, 0, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, data);
        break;
    default:
        // A8R8G8B8 as a little-endian DWORD is B,G,R,A in memory.
        glTexImage2D(image_target, level, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, data);
        break;
    }
}

static void ApplyFormatSwizzle(TextureImpl* t)
{
    GLint swz[4] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
    if (t->format == D3DFMT_A8) {
        swz[0] = swz[1] = swz[2] = GL_ZERO; swz[3] = GL_RED;
    } else if (t->format == D3DFMT_X8R8G8B8 || t->format == D3DFMT_LIN_X8R8G8B8) {
        swz[3] = GL_ONE;
    }
    if (!t->depth) glTexParameteriv(t->target, GL_TEXTURE_SWIZZLE_RGBA, swz);
    glTexParameteri(t->target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
}

//------------------------------------------------------------------------------
// Shaders and programs

struct VertexElement { int reg; int components; UINT offset; };

struct VertexShaderRec
{
    std::string name;
    std::vector<VertexElement> streams[2];
};

struct Program
{
    GLuint prog;
    GLint  loc_c, loc_pc, loc_texScale, loc_alphaFunc, loc_alphaRef;
    GLint  loc_wvp, loc_rhw, loc_viewport, loc_hasDiffuse;
    GLint  loc_hasTex0, loc_colorOp, loc_alphaOp, loc_tfactor;
};

static GLuint CompileStage(GLenum type, const char* prelude, const char* body, const char* name)
{
    GLuint s = glCreateShader(type);
    const char* src[2] = { prelude, body };
    glShaderSource(s, 2, src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "bootani: %s shader '%s' failed to compile:\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "pixel", name, log);
    }
    return s;
}

static Program BuildProgram(const std::string& vs_name, const std::string& ps_name)
{
    Program p;
    memset(&p, 0, sizeof(p));
    const char* vs_body = FindVertexShader(vs_name.c_str());
    const char* ps_body = FindPixelShader(ps_name.c_str());
    if (!vs_body || !ps_body) {
        fprintf(stderr, "bootani: no GLSL translation for shader pair %s/%s\n",
                vs_name.c_str(), ps_name.c_str());
        return p;
    }
    GLuint vs = CompileStage(GL_VERTEX_SHADER, VertexShaderPrelude(), vs_body, vs_name.c_str());
    GLuint ps = CompileStage(GL_FRAGMENT_SHADER, PixelShaderPrelude(), ps_body, ps_name.c_str());
    p.prog = glCreateProgram();
    glAttachShader(p.prog, vs);
    glAttachShader(p.prog, ps);
    glLinkProgram(p.prog);
    glDeleteShader(vs);
    glDeleteShader(ps);
    GLint ok = 0;
    glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p.prog, sizeof(log), NULL, log);
        fprintf(stderr, "bootani: program %s/%s failed to link:\n%s\n",
                vs_name.c_str(), ps_name.c_str(), log);
    }
    glUseProgram(p.prog);
    for (int i = 0; i < 4; i++) {
        char n[8];
        snprintf(n, sizeof(n), "tex%d", i);
        GLint l = glGetUniformLocation(p.prog, n);
        if (l >= 0) glUniform1i(l, i);
    }
    p.loc_c         = glGetUniformLocation(p.prog, "c");
    p.loc_pc        = glGetUniformLocation(p.prog, "pcRaw");
    p.loc_texScale  = glGetUniformLocation(p.prog, "texScale");
    p.loc_alphaFunc = glGetUniformLocation(p.prog, "uAlphaFunc");
    p.loc_alphaRef  = glGetUniformLocation(p.prog, "uAlphaRef");
    p.loc_wvp       = glGetUniformLocation(p.prog, "uWVP");
    p.loc_rhw       = glGetUniformLocation(p.prog, "uRHW");
    p.loc_viewport  = glGetUniformLocation(p.prog, "uViewport");
    p.loc_hasDiffuse= glGetUniformLocation(p.prog, "uHasDiffuse");
    p.loc_hasTex0   = glGetUniformLocation(p.prog, "uHasTex0");
    p.loc_colorOp   = glGetUniformLocation(p.prog, "uColorOp");
    p.loc_alphaOp   = glGetUniformLocation(p.prog, "uAlphaOp");
    p.loc_tfactor   = glGetUniformLocation(p.prog, "uTFactor");
    return p;
}

//------------------------------------------------------------------------------
// Device state

static const int kStages = 4;
static const int kVSConstants = 96;

struct DeviceState
{
    DWORD  rs[D3DRS_MAX];
    DWORD  tss[kStages][D3DTSS_MAX];
    D3DMATRIX transforms[D3DTS_MAX];
    D3DVIEWPORT8 viewport;

    IDirect3DBaseTexture8*  textures[kStages];
    IDirect3DVertexBuffer8* streams[2];
    UINT                    strides[2];
    IDirect3DIndexBuffer8*  indices;

    DWORD vs;           // handle (bit 0 set) or FVF
    DWORD ps;           // handle or 0
    float vconst[kVSConstants][4];
    float pconst[8][4];

    std::map<DWORD, VertexShaderRec> vertex_shaders;
    std::map<DWORD, std::string>     pixel_shaders;
    DWORD next_handle;

    std::map<std::pair<std::string, std::string>, Program> programs;

    IDirect3DSurface8* color_target;
    IDirect3DSurface8* depth_target;
    IDirect3DSurface8* backbuffer;
    IDirect3DSurface8* backbuffer_depth;

    std::map<std::pair<std::string, std::string>, GLuint> fbos;
    GLuint vao;
    GLuint samplers[kStages];
    GLuint stream_ib;           // for DrawIndexedVertices
    GLuint resolve_fbo, resolve_rb;
    int    bb_width, bb_height;
};

static DeviceState* S = 0;
static IDirect3DDevice8 g_device;
static IDirect3D8 g_d3d;

static void InitDefaultState(DeviceState* s)
{
    // Defaults from the Xbox D3D runtime (d3d8/se/dxgcreate.cpp).
    memset(s->rs, 0, sizeof(s->rs));
    s->rs[D3DRS_ZENABLE]          = D3DZB_TRUE;
    s->rs[D3DRS_FILLMODE]         = D3DFILL_SOLID;
    s->rs[D3DRS_BACKFILLMODE]     = D3DFILL_SOLID;
    s->rs[D3DRS_ZWRITEENABLE]     = TRUE;
    s->rs[D3DRS_SRCBLEND]         = D3DBLEND_ONE;
    s->rs[D3DRS_DESTBLEND]        = D3DBLEND_ZERO;
    s->rs[D3DRS_CULLMODE]         = D3DCULL_CCW;
    s->rs[D3DRS_ZFUNC]            = D3DCMP_LESSEQUAL;
    s->rs[D3DRS_ALPHAFUNC]        = D3DCMP_ALWAYS;
    s->rs[D3DRS_LIGHTING]         = TRUE;
    s->rs[D3DRS_TEXTUREFACTOR]    = 0xffffffff;
    s->rs[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
    s->rs[D3DRS_MULTISAMPLEMASK]  = 0xffffffff;
    s->rs[D3DRS_SHADOWFUNC]       = D3DCMP_NEVER;
    for (int i = 0; i < kStages; i++) {
        DWORD* t = s->tss[i];
        memset(t, 0, sizeof(DWORD) * D3DTSS_MAX);
        t[D3DTSS_COLOROP]   = (i == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        t[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        t[D3DTSS_ALPHAOP]   = (i == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        t[D3DTSS_TEXCOORDINDEX] = i;
        t[D3DTSS_ADDRESSU] = t[D3DTSS_ADDRESSV] = t[D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
        t[D3DTSS_MAGFILTER] = t[D3DTSS_MINFILTER] = D3DTEXF_POINT;
        t[D3DTSS_MIPFILTER] = D3DTEXF_NONE;
        t[D3DTSS_MAXANISOTROPY] = 1;
    }
    for (int i = 0; i < D3DTS_MAX; i++) {
        memset(&s->transforms[i], 0, sizeof(D3DMATRIX));
        s->transforms[i]._11 = s->transforms[i]._22 = s->transforms[i]._33 = s->transforms[i]._44 = 1.f;
    }
    memset(s->vconst, 0, sizeof(s->vconst));
    memset(s->pconst, 0, sizeof(s->pconst));
}

//------------------------------------------------------------------------------
// Framebuffers

static std::string SurfaceKey(IDirect3DSurface8* s)
{
    if (!s || s->kind == IDirect3DSurface8::KIND_HEADER_ONLY) return "-";
    char buf[64];
    if (s->kind == IDirect3DSurface8::KIND_TEXTURE_LEVEL || s->kind == IDirect3DSurface8::KIND_CUBE_FACE)
        snprintf(buf, sizeof(buf), "t%u/%d/%d", s->owner->impl->tex, s->face, s->level);
    else
        snprintf(buf, sizeof(buf), "r%u", s->impl->rb);
    return buf;
}

static void Attach(GLenum attachment, IDirect3DSurface8* s)
{
    if (s->kind == IDirect3DSurface8::KIND_TEXTURE_LEVEL) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, s->owner->impl->tex, s->level);
    } else if (s->kind == IDirect3DSurface8::KIND_CUBE_FACE) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + s->face,
                               s->owner->impl->tex, s->level);
    } else {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, s->impl->rb);
    }
}

static GLenum DepthAttachmentFor(IDirect3DSurface8* d)
{
    D3DFORMAT f = d->format;
    return (f == D3DFMT_LIN_D16) ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT;
}

static GLuint FramebufferFor(IDirect3DSurface8* color, IDirect3DSurface8* depth)
{
    std::pair<std::string, std::string> key(SurfaceKey(color), SurfaceKey(depth));
    std::map<std::pair<std::string, std::string>, GLuint>::iterator it = S->fbos.find(key);
    if (it != S->fbos.end()) return it->second;

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    bool has_color = color && color->kind != IDirect3DSurface8::KIND_HEADER_ONLY;
    if (has_color) {
        Attach(GL_COLOR_ATTACHMENT0, color);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    } else {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    if (depth) Attach(DepthAttachmentFor(depth), depth);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "bootani: framebuffer %s + %s incomplete (0x%x)\n",
                key.first.c_str(), key.second.c_str(), status);
    S->fbos[key] = fbo;
    return fbo;
}

static void ForgetFramebuffersUsing(const std::string& key_prefix)
{
    std::map<std::pair<std::string, std::string>, GLuint>::iterator it = S->fbos.begin();
    while (it != S->fbos.end()) {
        if (it->first.first.compare(0, key_prefix.size(), key_prefix) == 0 ||
            it->first.second.compare(0, key_prefix.size(), key_prefix) == 0) {
            glDeleteFramebuffers(1, &it->second);
            S->fbos.erase(it++);
        } else {
            ++it;
        }
    }
}

static void BindTargets()
{
    glBindFramebuffer(GL_FRAMEBUFFER, FramebufferFor(S->color_target, S->depth_target));
}

static SurfaceImpl* MakeRenderbuffer(GLenum internal, UINT w, UINT h, int samples)
{
    SurfaceImpl* impl = new SurfaceImpl;
    impl->depth = (internal == GL_DEPTH24_STENCIL8 || internal == GL_DEPTH_COMPONENT16);
    impl->samples = samples;
    glGenRenderbuffers(1, &impl->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, impl->rb);
    if (samples > 1) {
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internal, w, h);
    } else {
        glRenderbufferStorage(GL_RENDERBUFFER, internal, w, h);
    }
    return impl;
}

static IDirect3DSurface8* NewSurface(int kind, D3DFORMAT fmt, UINT w, UINT h)
{
    IDirect3DSurface8* s = new IDirect3DSurface8;
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->refs = 1;
    s->format = fmt;
    s->width = w;
    s->height = h;
    return s;
}

static void SetFullViewport()
{
    IDirect3DSurface8* ref = (S->color_target && S->color_target->kind != IDirect3DSurface8::KIND_HEADER_ONLY)
                             ? S->color_target : S->depth_target;
    if (!ref) ref = S->color_target;
    D3DVIEWPORT8 vp = { 0, 0, ref ? ref->width : 0, ref ? ref->height : 0, 0.f, 1.f };
    S->viewport = vp;
}

//------------------------------------------------------------------------------
// Draw-time state application

static GLenum GLCompare(DWORD f)
{
    switch (f) {
    case D3DCMP_NEVER:        return GL_NEVER;
    case D3DCMP_LESS:         return GL_LESS;
    case D3DCMP_EQUAL:        return GL_EQUAL;
    case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
    case D3DCMP_GREATER:      return GL_GREATER;
    case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
    default:                  return GL_ALWAYS;
    }
}

static GLenum GLCompareMirrored(DWORD f)
{
    switch (f) {
    case D3DCMP_LESS:         return GL_GREATER;
    case D3DCMP_LESSEQUAL:    return GL_GEQUAL;
    case D3DCMP_GREATER:      return GL_LESS;
    case D3DCMP_GREATEREQUAL: return GL_LEQUAL;
    default:                  return GLCompare(f);
    }
}

static GLenum GLBlend(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO:         return GL_ZERO;
    case D3DBLEND_ONE:          return GL_ONE;
    case D3DBLEND_SRCCOLOR:     return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    default:                    return GL_ONE;
    }
}

static GLenum GLAddress(DWORD a)
{
    switch (a) {
    case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
    case D3DTADDRESS_CLAMP:  return GL_CLAMP_TO_EDGE;
    case D3DTADDRESS_BORDER: return GL_CLAMP_TO_BORDER;
    default:                 return GL_REPEAT;
    }
}

static void ColorToFloat4(DWORD c, float out[4])
{
    out[0] = ((c >> 16) & 0xff) / 255.f;
    out[1] = ((c >> 8) & 0xff) / 255.f;
    out[2] = (c & 0xff) / 255.f;
    out[3] = ((c >> 24) & 0xff) / 255.f;
}

static void ApplyRasterState()
{
    const DWORD* rs = S->rs;
    glViewport(S->viewport.X, S->viewport.Y, S->viewport.Width, S->viewport.Height);
    glDepthRange(S->viewport.MinZ, S->viewport.MaxZ);

    if (rs[D3DRS_ZENABLE] && S->depth_target) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GLCompare(rs[D3DRS_ZFUNC]));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(rs[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);

    if (rs[D3DRS_ALPHABLENDENABLE]) {
        glEnable(GL_BLEND);
        glBlendFunc(GLBlend(rs[D3DRS_SRCBLEND]), GLBlend(rs[D3DRS_DESTBLEND]));
    } else {
        glDisable(GL_BLEND);
    }

    if (rs[D3DRS_CULLMODE] == D3DCULL_NONE) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(rs[D3DRS_CULLMODE] == D3DCULL_CCW ? GL_BACK : GL_FRONT);
    }

    DWORD cw = rs[D3DRS_COLORWRITEENABLE];
    glColorMask((cw & D3DCOLORWRITEENABLE_RED) != 0, (cw & D3DCOLORWRITEENABLE_GREEN) != 0,
                (cw & D3DCOLORWRITEENABLE_BLUE) != 0, (cw & D3DCOLORWRITEENABLE_ALPHA) != 0);

    if (rs[D3DRS_SOLIDOFFSETENABLE]) {
        float slope, units;
        memcpy(&slope, &rs[D3DRS_POLYGONOFFSETZSLOPESCALE], 4);
        memcpy(&units, &rs[D3DRS_POLYGONOFFSETZOFFSET], 4);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(slope, units);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    glPolygonMode(GL_FRONT_AND_BACK, rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? GL_LINE : GL_FILL);
}

static void ApplyTextures(Program& p, bool fixed_function)
{
    float scales[kStages][2];
    for (int i = 0; i < kStages; i++) {
        scales[i][0] = scales[i][1] = 1.f;
        IDirect3DBaseTexture8* t = S->textures[i];
        glActiveTexture(GL_TEXTURE0 + i);
        GLuint smp = S->samplers[i];
        glBindSampler(i, smp);
        if (!t) {
            glBindTexture(GL_TEXTURE_2D, 0);
            continue;
        }
        TextureImpl* ti = t->impl;
        glBindTexture(ti->target, ti->tex);
        if (ti->linear) {
            scales[i][0] = 1.f / ti->width;
            scales[i][1] = 1.f / ti->height;
        }
        const DWORD* ts = S->tss[i];
        bool has_mips = ti->levels > 1 && ts[D3DTSS_MIPFILTER] != D3DTEXF_NONE;
        bool lin_min = ts[D3DTSS_MINFILTER] >= D3DTEXF_LINEAR;
        bool lin_mip = ts[D3DTSS_MIPFILTER] >= D3DTEXF_LINEAR;
        GLenum minf = has_mips ? (lin_min ? (lin_mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                                          : (lin_mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST))
                               : (lin_min ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, minf);
        glSamplerParameteri(smp, GL_TEXTURE_MAG_FILTER,
                            ts[D3DTSS_MAGFILTER] >= D3DTEXF_LINEAR ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(smp, GL_TEXTURE_WRAP_S, GLAddress(ts[D3DTSS_ADDRESSU]));
        glSamplerParameteri(smp, GL_TEXTURE_WRAP_T, GLAddress(ts[D3DTSS_ADDRESSV]));
        glSamplerParameteri(smp, GL_TEXTURE_WRAP_R, GLAddress(ts[D3DTSS_ADDRESSW]));
        float border[4];
        ColorToFloat4(ts[D3DTSS_BORDERCOLOR], border);
        glSamplerParameterfv(smp, GL_TEXTURE_BORDER_COLOR, border);
        if (ti->depth && !fixed_function) {
            glSamplerParameteri(smp, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glSamplerParameteri(smp, GL_TEXTURE_COMPARE_FUNC, GLCompareMirrored(S->rs[D3DRS_SHADOWFUNC]));
        } else {
            glSamplerParameteri(smp, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }
    }
    if (p.loc_texScale >= 0) glUniform2fv(p.loc_texScale, kStages, &scales[0][0]);
}

static Program& UseProgram(const std::string& vs, const std::string& ps)
{
    std::pair<std::string, std::string> key(vs, ps);
    std::map<std::pair<std::string, std::string>, Program>::iterator it = S->programs.find(key);
    if (it == S->programs.end())
        it = S->programs.insert(std::make_pair(key, BuildProgram(vs, ps))).first;
    glUseProgram(it->second.prog);
    return it->second;
}

static void MulMatrix(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX* r)
{
    D3DMATRIX t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                        a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    *r = t;
}

static void EnableAttrib(int loc, int comps, GLenum type, GLboolean norm, UINT stride, UINT offset)
{
    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, comps, type, norm, stride, (const void*)(size_t)offset);
}

// Program, uniforms, textures and vertex layout for the next draw.
static bool PrepareDraw()
{
    BindTargets();
    ApplyRasterState();

    bool ff_vertex = (S->vs & 1) == 0;
    std::string vs_name, ps_name;
    const VertexShaderRec* vrec = 0;
    if (ff_vertex) {
        if (S->vs == 0) return false;
        vs_name = "fixed";
    } else {
        std::map<DWORD, VertexShaderRec>::iterator it = S->vertex_shaders.find(S->vs);
        if (it == S->vertex_shaders.end()) return false;
        vrec = &it->second;
        vs_name = vrec->name;
    }
    bool ff_pixel = true;
    if (S->ps) {
        std::map<DWORD, std::string>::iterator it = S->pixel_shaders.find(S->ps);
        if (it != S->pixel_shaders.end()) { ps_name = it->second; ff_pixel = false; }
    }
    if (ff_pixel) ps_name = "fixed";

    Program& p = UseProgram(vs_name, ps_name);
    if (!p.prog) return false;

    if (p.loc_c >= 0) glUniform4fv(p.loc_c, kVSConstants, &S->vconst[0][0]);
    if (p.loc_pc >= 0) glUniform4fv(p.loc_pc, 8, &S->pconst[0][0]);
    if (p.loc_alphaFunc >= 0) {
        glUniform1i(p.loc_alphaFunc, S->rs[D3DRS_ALPHATESTENABLE] ? (GLint)S->rs[D3DRS_ALPHAFUNC] : 0);
        glUniform1f(p.loc_alphaRef, (float)(S->rs[D3DRS_ALPHAREF] & 0xff));
    }
    ApplyTextures(p, ff_pixel);

    if (ff_pixel) {
        const DWORD* t = S->tss[0];
        GLint col[3] = { (GLint)t[D3DTSS_COLOROP], (GLint)t[D3DTSS_COLORARG1], (GLint)t[D3DTSS_COLORARG2] };
        GLint alp[3] = { (GLint)t[D3DTSS_ALPHAOP], (GLint)t[D3DTSS_ALPHAARG1], (GLint)t[D3DTSS_ALPHAARG2] };
        glUniform3iv(p.loc_colorOp, 1, col);
        glUniform3iv(p.loc_alphaOp, 1, alp);
        float tf[4];
        ColorToFloat4(S->rs[D3DRS_TEXTUREFACTOR], tf);
        glUniform4fv(p.loc_tfactor, 1, tf);
        glUniform1i(p.loc_hasTex0, S->textures[0] ? 1 : 0);
    }

    // Vertex layout.
    for (int loc = 0; loc < 5; loc++) {
        glDisableVertexAttribArray(loc);
        glVertexAttrib4f(loc, 0.f, 0.f, 0.f, 1.f);
    }
    if (ff_vertex) {
        DWORD fvf = S->vs;
        IDirect3DVertexBuffer8* vb = S->streams[0];
        if (!vb) return false;
        glBindBuffer(GL_ARRAY_BUFFER, vb->impl->buf);
        UINT stride = S->strides[0], off = 0;
        bool rhw = (fvf & D3DFVF_XYZRHW) != 0;
        EnableAttrib(0, rhw ? 4 : 3, GL_FLOAT, GL_FALSE, stride, off);
        off += rhw ? 16 : 12;
        if (fvf & D3DFVF_NORMAL) off += 12;
        bool diffuse = (fvf & D3DFVF_DIFFUSE) != 0;
        if (diffuse) { EnableAttrib(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, off); off += 4; }
        int ntex = (fvf >> 8) & 0xf;
        for (int i = 0; i < ntex && i < 2; i++) { EnableAttrib(1 + i, 2, GL_FLOAT, GL_FALSE, stride, off); off += 8; }

        D3DMATRIX wv, wvp;
        MulMatrix(S->transforms[D3DTS_WORLD], S->transforms[D3DTS_VIEW], &wv);
        MulMatrix(wv, S->transforms[D3DTS_PROJECTION], &wvp);
        // D3D row-vector matrices uploaded as-is are GLSL column-major M
        // with (M * v) == (v * D3DMATRIX).
        glUniformMatrix4fv(p.loc_wvp, 1, GL_FALSE, &wvp._11);
        glUniform1i(p.loc_rhw, rhw ? 1 : 0);
        glUniform4f(p.loc_viewport, (float)S->viewport.X, (float)S->viewport.Y,
                    (float)S->viewport.Width, (float)S->viewport.Height);
        glUniform1i(p.loc_hasDiffuse, diffuse ? 1 : 0);
    } else {
        for (int s = 0; s < 2; s++) {
            if (vrec->streams[s].empty()) continue;
            IDirect3DVertexBuffer8* vb = S->streams[s];
            if (!vb) return false;
            glBindBuffer(GL_ARRAY_BUFFER, vb->impl->buf);
            for (size_t e = 0; e < vrec->streams[s].size(); e++) {
                const VertexElement& el = vrec->streams[s][e];
                EnableAttrib(el.reg, el.components, GL_FLOAT, GL_FALSE, S->strides[s], el.offset);
            }
        }
    }
    return true;
}

static GLenum GLPrimitive(D3DPRIMITIVETYPE t)
{
    switch (t) {
    case D3DPT_POINTLIST:     return GL_POINTS;
    case D3DPT_LINELIST:      return GL_LINES;
    case D3DPT_LINESTRIP:     return GL_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN:   return GL_TRIANGLE_FAN;
    default:                  return GL_TRIANGLES;
    }
}

static UINT VertexCount(D3DPRIMITIVETYPE t, UINT prims)
{
    switch (t) {
    case D3DPT_POINTLIST:     return prims;
    case D3DPT_LINELIST:      return prims * 2;
    case D3DPT_LINESTRIP:     return prims + 1;
    case D3DPT_TRIANGLELIST:  return prims * 3;
    default:                  return prims + 2;
    }
}

} // namespace bootani_gl

using namespace bootani_gl;

//------------------------------------------------------------------------------
// Resources

ULONG IDirect3DResource8::Release()
{
    ULONG r = --m_refs;
    if (r == 0) delete this;
    return r;
}

IDirect3DBaseTexture8::~IDirect3DBaseTexture8()
{
    if (impl) {
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "t%u/", impl->tex);
        if (S) ForgetFramebuffersUsing(prefix);
        glDeleteTextures(1, &impl->tex);
        delete impl;
    }
}

HRESULT IDirect3DTexture8::GetLevelDesc(UINT level, D3DSURFACE_DESC* desc)
{
    desc->Format = impl->format;
    desc->Usage = 0;
    desc->Width = impl->width >> level;
    desc->Height = impl->height >> level;
    desc->Size = desc->Width * desc->Height * BytesPerPixel(impl->format);
    return D3D_OK;
}

static std::vector<BYTE>& Staging(TextureImpl* t, int face, UINT level)
{
    std::vector<BYTE>& v = t->staging[face * t->levels + level];
    UINT w = t->width >> level;  if (!w) w = 1;
    UINT h = t->height >> level; if (!h) h = 1;
    v.resize(w * h * BytesPerPixel(t->format));
    return v;
}

static void UploadLevel(TextureImpl* t, int face, UINT level)
{
    std::vector<BYTE>& v = Staging(t, face, level);
    glBindTexture(t->target, t->tex);
    GLenum image_target = (t->target == GL_TEXTURE_CUBE_MAP) ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
    DefineImage(t, image_target, level, &v[0]);
    // Keep the staging copy only while the level may be locked again.
    std::vector<BYTE>().swap(v);
}

HRESULT IDirect3DTexture8::LockRect(UINT level, D3DLOCKED_RECT* locked, const void*, DWORD)
{
    std::vector<BYTE>& v = Staging(impl, 0, level);
    UINT w = impl->width >> level; if (!w) w = 1;
    locked->pBits = &v[0];
    locked->Pitch = w * BytesPerPixel(impl->format);
    return D3D_OK;
}

HRESULT IDirect3DTexture8::UnlockRect(UINT level)
{
    UploadLevel(impl, 0, level);
    return D3D_OK;
}

HRESULT IDirect3DTexture8::GetSurfaceLevel(UINT level, IDirect3DSurface8** surface)
{
    IDirect3DSurface8* s = NewSurface(IDirect3DSurface8::KIND_TEXTURE_LEVEL, impl->format,
                                      impl->width >> level, impl->height >> level);
    s->owner = this;
    s->level = level;
    AddRef();
    *surface = s;
    return D3D_OK;
}

HRESULT IDirect3DCubeTexture8::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface8** surface)
{
    IDirect3DSurface8* s = NewSurface(IDirect3DSurface8::KIND_CUBE_FACE, impl->format,
                                      impl->width >> level, impl->height >> level);
    s->owner = this;
    s->face = face;
    s->level = level;
    AddRef();
    *surface = s;
    return D3D_OK;
}

ULONG IDirect3DSurface8::Release()
{
    if (kind == KIND_HEADER_ONLY) return 1;     // caller-owned memory
    ULONG r = --refs;
    if (r == 0) {
        if (owner) owner->Release();
        if (impl) {
            char prefix[32];
            snprintf(prefix, sizeof(prefix), "r%u", impl->rb);
            if (S) ForgetFramebuffersUsing(prefix);
            glDeleteRenderbuffers(1, &impl->rb);
            delete impl;
        }
        delete this;
    }
    return r;
}

HRESULT IDirect3DSurface8::LockRect(D3DLOCKED_RECT* locked, const void*, DWORD)
{
    TextureImpl* t = owner->impl;
    std::vector<BYTE>& v = Staging(t, face, level);
    locked->pBits = &v[0];
    locked->Pitch = (t->width >> level) * BytesPerPixel(t->format);
    return D3D_OK;
}

HRESULT IDirect3DSurface8::UnlockRect()
{
    UploadLevel(owner->impl, face, level);
    return D3D_OK;
}

static BufferImpl* NewBuffer(GLenum target, UINT length)
{
    BufferImpl* b = new BufferImpl;
    b->target = target;
    b->shadow.assign(length, 0);
    glGenBuffers(1, &b->buf);
    glBindBuffer(target, b->buf);
    glBufferData(target, length, &b->shadow[0], GL_DYNAMIC_DRAW);
    return b;
}

IDirect3DVertexBuffer8::~IDirect3DVertexBuffer8()
{
    if (impl) { glDeleteBuffers(1, &impl->buf); delete impl; }
}

HRESULT IDirect3DVertexBuffer8::Lock(UINT offset, UINT, BYTE** data, DWORD)
{
    *data = &impl->shadow[offset];
    return D3D_OK;
}

HRESULT IDirect3DVertexBuffer8::Unlock()
{
    glBindBuffer(GL_ARRAY_BUFFER, impl->buf);
    glBufferSubData(GL_ARRAY_BUFFER, 0, impl->shadow.size(), &impl->shadow[0]);
    return D3D_OK;
}

IDirect3DIndexBuffer8::~IDirect3DIndexBuffer8()
{
    if (impl) { glDeleteBuffers(1, &impl->buf); delete impl; }
}

HRESULT IDirect3DIndexBuffer8::Lock(UINT offset, UINT, BYTE** data, DWORD)
{
    *data = &impl->shadow[offset];
    return D3D_OK;
}

HRESULT IDirect3DIndexBuffer8::Unlock()
{
    // Index buffers are bound through the VAO; bind it so the upload doesn't
    // disturb whatever is current.
    glBindVertexArray(S->vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, impl->buf);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, impl->shadow.size(), &impl->shadow[0]);
    return D3D_OK;
}

//------------------------------------------------------------------------------
// Device

IDirect3D8* Direct3DCreate8(UINT) { return &g_d3d; }

ULONG IDirect3D8::Release() { return 0; }

HRESULT IDirect3D8::CreateDevice(UINT, DWORD, void*, DWORD, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** device)
{
    S = new DeviceState;
    InitDefaultState(S);
    S->next_handle = 1;
    memset(S->textures, 0, sizeof(S->textures));
    memset(S->streams, 0, sizeof(S->streams));
    memset(S->strides, 0, sizeof(S->strides));
    S->indices = 0;
    S->vs = S->ps = 0;
    S->bb_width = pp->BackBufferWidth;
    S->bb_height = pp->BackBufferHeight;

    glGenVertexArrays(1, &S->vao);
    glBindVertexArray(S->vao);
    glGenSamplers(kStages, S->samplers);
    glGenBuffers(1, &S->stream_ib);
    glFrontFace(GL_CCW);
    glDisable(GL_DITHER);

    int samples = g_backbuffer_samples;
    S->backbuffer = NewSurface(IDirect3DSurface8::KIND_RENDERTARGET, D3DFMT_A8R8G8B8,
                               pp->BackBufferWidth, pp->BackBufferHeight);
    S->backbuffer->impl = MakeRenderbuffer(GL_RGBA8, pp->BackBufferWidth, pp->BackBufferHeight, samples);
    S->backbuffer_depth = NewSurface(IDirect3DSurface8::KIND_DEPTH, D3DFMT_D24S8,
                                     pp->BackBufferWidth, pp->BackBufferHeight);
    S->backbuffer_depth->impl = MakeRenderbuffer(GL_DEPTH24_STENCIL8, pp->BackBufferWidth,
                                                 pp->BackBufferHeight, samples);
    S->resolve_fbo = 0;
    S->resolve_rb = 0;
    if (samples > 1) {
        glGenRenderbuffers(1, &S->resolve_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, S->resolve_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, pp->BackBufferWidth, pp->BackBufferHeight);
        glGenFramebuffers(1, &S->resolve_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, S->resolve_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, S->resolve_rb);
    }

    S->color_target = S->backbuffer;
    S->depth_target = pp->EnableAutoDepthStencil ? S->backbuffer_depth : 0;
    S->color_target->AddRef();
    if (S->depth_target) S->depth_target->AddRef();
    SetFullViewport();

    *device = &g_device;
    return D3D_OK;
}

ULONG IDirect3DDevice8::Release()
{
    // Program and framebuffer objects die with the GL context.
    return 0;
}

HRESULT IDirect3DDevice8::BeginScene() { return D3D_OK; }
HRESULT IDirect3DDevice8::EndScene()   { return D3D_OK; }

HRESULT IDirect3DDevice8::Present(const void*, const void*, void*, const void*)
{
    GLuint src = FramebufferFor(S->backbuffer, S->backbuffer_depth);
    GLuint out = src;
    if (S->resolve_fbo) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, S->resolve_fbo);
        glBlitFramebuffer(0, 0, S->bb_width, S->bb_height, 0, 0, S->bb_width, S->bb_height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        out = S->resolve_fbo;
    }
    if (g_present_hook) g_present_hook(out, S->bb_width, S->bb_height, g_present_user);
    return D3D_OK;
}

HRESULT IDirect3DDevice8::Clear(DWORD, const void*, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    BindTargets();
    glViewport(S->viewport.X, S->viewport.Y, S->viewport.Width, S->viewport.Height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(S->viewport.X, S->viewport.Y, S->viewport.Width, S->viewport.Height);
    GLbitfield mask = 0;
    bool has_color = S->color_target && S->color_target->kind != IDirect3DSurface8::KIND_HEADER_ONLY;
    if ((flags & D3DCLEAR_TARGET) && has_color) {
        float c[4];
        ColorToFloat4(color, c);
        glClearColor(c[0], c[1], c[2], c[3]);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if ((flags & D3DCLEAR_ZBUFFER) && S->depth_target) {
        glClearDepth(z);
        glDepthMask(GL_TRUE);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if ((flags & D3DCLEAR_STENCIL) && S->depth_target && S->depth_target->format != D3DFMT_LIN_D16) {
        glClearStencil(stencil);
        mask |= GL_STENCIL_BUFFER_BIT;
    }
    if (mask) glClear(mask);
    glDisable(GL_SCISSOR_TEST);
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetRenderState(D3DRENDERSTATETYPE state, DWORD value)
{
    if (state < D3DRS_MAX) S->rs[state] = value;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    if (stage < (DWORD)kStages && type < D3DTSS_MAX) S->tss[stage][type] = value;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    if (state < D3DTS_MAX) S->transforms[state] = *m;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetViewport(const D3DVIEWPORT8* vp)
{
    S->viewport = *vp;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetTexture(DWORD stage, IDirect3DBaseTexture8* texture)
{
    if (stage >= (DWORD)kStages) return E_FAIL;
    if (texture) texture->AddRef();
    if (S->textures[stage]) S->textures[stage]->Release();
    S->textures[stage] = texture;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetStreamSource(UINT stream, IDirect3DVertexBuffer8* vb, UINT stride)
{
    if (stream >= 2) return E_FAIL;
    if (vb) vb->AddRef();
    if (S->streams[stream]) S->streams[stream]->Release();
    S->streams[stream] = vb;
    S->strides[stream] = stride;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetIndices(IDirect3DIndexBuffer8* ib, UINT)
{
    if (ib) ib->AddRef();
    if (S->indices) S->indices->Release();
    S->indices = ib;
    return D3D_OK;
}

static const char* TagName(const void* blob, const char* expected_tag)
{
    const char* s = (const char*)blob;
    if (strncmp(s, expected_tag, 4) == 0) return s + 4;
    return s;
}

HRESULT IDirect3DDevice8::CreateVertexShader(const DWORD* decl, const DWORD* function, DWORD* handle, DWORD)
{
    VertexShaderRec rec;
    rec.name = TagName(function, "XVU:");
    int stream = 0;
    UINT offsets[2] = { 0, 0 };
    for (const DWORD* d = decl; *d != D3DVSD_END(); d++) {
        DWORD tok = *d;
        if ((tok & 0xF0000000u) == 0x10000000u) {
            stream = tok & 0xff;
        } else if ((tok & 0xF0000000u) == 0x20000000u) {
            VertexElement el;
            el.reg = tok & 0xff;
            el.components = (tok >> 8) & 0xff;
            el.offset = offsets[stream];
            offsets[stream] += el.components * 4;
            rec.streams[stream].push_back(el);
        }
    }
    DWORD h = (S->next_handle++ << 1) | 1;
    S->vertex_shaders[h] = rec;
    *handle = h;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::CreatePixelShader(const D3DPIXELSHADERDEF* def, DWORD* handle)
{
    DWORD h = (S->next_handle++ << 1) | 1;
    S->pixel_shaders[h] = def->Name;
    *handle = h;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::DeleteVertexShader(DWORD handle) { S->vertex_shaders.erase(handle); return D3D_OK; }
HRESULT IDirect3DDevice8::DeletePixelShader(DWORD handle)  { S->pixel_shaders.erase(handle);  return D3D_OK; }

HRESULT IDirect3DDevice8::SetVertexShader(DWORD handle) { S->vs = handle; return D3D_OK; }
HRESULT IDirect3DDevice8::SetPixelShader(DWORD handle)  { S->ps = handle; return D3D_OK; }

HRESULT IDirect3DDevice8::SetVertexShaderConstant(INT reg, const void* data, DWORD count)
{
    for (DWORD i = 0; i < count; i++) {
        INT r = reg + (INT)i;
        if (r >= 0 && r < kVSConstants) memcpy(S->vconst[r], (const float*)data + 4 * i, 16);
    }
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetPixelShaderConstant(DWORD reg, const void* data, DWORD count)
{
    for (DWORD i = 0; i < count; i++)
        if (reg + i < 8) memcpy(S->pconst[reg + i], (const float*)data + 4 * i, 16);
    return D3D_OK;
}

HRESULT IDirect3DDevice8::DrawPrimitive(D3DPRIMITIVETYPE type, UINT start, UINT prims)
{
    glBindVertexArray(S->vao);
    if (!PrepareDraw()) return E_FAIL;
    glDrawArrays(GLPrimitive(type), start, VertexCount(type, prims));
    return D3D_OK;
}

HRESULT IDirect3DDevice8::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, UINT, UINT, UINT start_index, UINT prims)
{
    glBindVertexArray(S->vao);
    if (!S->indices || !PrepareDraw()) return E_FAIL;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, S->indices->impl->buf);
    glDrawElements(GLPrimitive(type), VertexCount(type, prims), GL_UNSIGNED_SHORT,
                   (const void*)(size_t)(start_index * 2));
    return D3D_OK;
}

HRESULT IDirect3DDevice8::DrawIndexedVertices(D3DPRIMITIVETYPE type, UINT count, const WORD* indices)
{
    glBindVertexArray(S->vao);
    if (!PrepareDraw()) return E_FAIL;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, S->stream_ib);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * sizeof(WORD), indices, GL_STREAM_DRAW);
    glDrawElements(GLPrimitive(type), count, GL_UNSIGNED_SHORT, 0);
    return D3D_OK;
}

static TextureImpl* NewTexture(GLenum target, UINT w, UINT h, UINT levels, D3DFORMAT fmt)
{
    TextureImpl* t = new TextureImpl;
    t->target = target;
    t->format = fmt;
    t->width = w;
    t->height = h;
    if (levels == 0) { levels = 1; UINT m = w > h ? w : h; while (m >>= 1) levels++; }
    t->levels = levels;
    t->linear = IsLinear(fmt);
    t->depth = IsDepth(fmt);
    t->staging.resize((target == GL_TEXTURE_CUBE_MAP ? 6 : 1) * levels);
    glGenTextures(1, &t->tex);
    glBindTexture(target, t->tex);
    for (UINT l = 0; l < levels; l++) {
        if (target == GL_TEXTURE_CUBE_MAP)
            for (int f = 0; f < 6; f++) DefineImage(t, GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, NULL);
        else
            DefineImage(t, GL_TEXTURE_2D, l, NULL);
    }
    ApplyFormatSwizzle(t);
    return t;
}

HRESULT IDirect3DDevice8::CreateTexture(UINT w, UINT h, UINT levels, DWORD, D3DFORMAT fmt, DWORD,
                                        IDirect3DTexture8** out)
{
    IDirect3DTexture8* t = new IDirect3DTexture8;
    t->impl = NewTexture(GL_TEXTURE_2D, w, h, levels, fmt);
    *out = t;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::CreateCubeTexture(UINT edge, UINT levels, DWORD, D3DFORMAT fmt, DWORD,
                                            IDirect3DCubeTexture8** out)
{
    IDirect3DCubeTexture8* t = new IDirect3DCubeTexture8;
    t->impl = NewTexture(GL_TEXTURE_CUBE_MAP, edge, edge, levels, fmt);
    *out = t;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::CreateVertexBuffer(UINT length, DWORD, DWORD, DWORD, IDirect3DVertexBuffer8** out)
{
    IDirect3DVertexBuffer8* vb = new IDirect3DVertexBuffer8;
    vb->impl = NewBuffer(GL_ARRAY_BUFFER, length);
    *out = vb;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::CreateIndexBuffer(UINT length, DWORD, D3DFORMAT, DWORD, IDirect3DIndexBuffer8** out)
{
    glBindVertexArray(S->vao);
    IDirect3DIndexBuffer8* ib = new IDirect3DIndexBuffer8;
    ib->impl = NewBuffer(GL_ELEMENT_ARRAY_BUFFER, length);
    *out = ib;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, DWORD, BOOL, IDirect3DSurface8** out)
{
    IDirect3DSurface8* s = NewSurface(IDirect3DSurface8::KIND_RENDERTARGET, fmt, w, h);
    s->impl = MakeRenderbuffer(GL_RGBA8, w, h, 1);
    *out = s;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, DWORD, IDirect3DSurface8** out)
{
    IDirect3DSurface8* s = NewSurface(IDirect3DSurface8::KIND_DEPTH, fmt, w, h);
    s->impl = MakeRenderbuffer(fmt == D3DFMT_LIN_D16 ? GL_DEPTH_COMPONENT16 : GL_DEPTH24_STENCIL8, w, h, 1);
    *out = s;
    return D3D_OK;
}

HRESULT IDirect3DDevice8::GetRenderTarget(IDirect3DSurface8** out)
{
    *out = S->color_target;
    if (*out) (*out)->AddRef();
    return D3D_OK;
}

HRESULT IDirect3DDevice8::GetDepthStencilSurface(IDirect3DSurface8** out)
{
    *out = S->depth_target;
    if (*out) (*out)->AddRef();
    return D3D_OK;
}

HRESULT IDirect3DDevice8::SetRenderTarget(IDirect3DSurface8* color, IDirect3DSurface8* depth)
{
    if (color) color->AddRef();
    if (depth) depth->AddRef();
    if (S->color_target) S->color_target->Release();
    if (S->depth_target) S->depth_target->Release();
    S->color_target = color;
    S->depth_target = depth;
    SetFullViewport();
    return D3D_OK;
}

HRESULT IDirect3DDevice8::CopyRects(IDirect3DSurface8* src, const void*, UINT, IDirect3DSurface8* dst, const void*)
{
    GLuint read = FramebufferFor(src, 0);
    GLuint draw = FramebufferFor(dst, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    glBlitFramebuffer(0, 0, src->width, src->height, 0, 0, dst->width, dst->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return D3D_OK;
}

//------------------------------------------------------------------------------
// XGraphics

void XGSwizzleRect(const void* src, DWORD pitch, const void*, void* dst, DWORD width, DWORD height,
                   const void*, DWORD bpp)
{
    DWORD row = width * bpp;
    if (!pitch) pitch = row;
    for (DWORD y = 0; y < height; y++)
        memcpy((BYTE*)dst + y * row, (const BYTE*)src + y * pitch, row);
}

void XGSetSurfaceHeader(UINT width, UINT height, D3DFORMAT fmt, IDirect3DSurface8* surface, UINT, UINT)
{
    memset(surface, 0, sizeof(*surface));
    surface->kind = IDirect3DSurface8::KIND_HEADER_ONLY;
    surface->refs = 1;
    surface->format = fmt;
    surface->width = width;
    surface->height = height;
}
