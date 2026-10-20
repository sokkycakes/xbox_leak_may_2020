// The parts of Frolick's 3D world that are not panels: the screen backdrop
// texture, the ribbon ring that circles the scene (and the bumper around
// each pad) and the "atmo" dome, a multiply layer with slowly scrolling
// light stripes.  The numbers come from frolick_assets/main.tscn and the
// Go renderer's draw order (unlit, alpha blended, no depth write).
//
//   Background { backdrop XtxTexture { url "backdrop.xtx" } }
//   Transform { translation x y z  children [ Ribbon { radius 5.92 height 1.815 } ] }
//   Transform { translation x y z  children [ Dome { radius 6.97 height 4.26 } ] }
//   Transform { translation x y z  children [ Disc { radius 0.276 color r g b } ] }
//   Transform { translation x y z  children [ Sprite { url "circle.xtx" pixelSize 0.00066 color r g b } ] }
#include "std.h"
#include "xapp.h"
#include "node.h"
#include "Texture.h"

extern bool FrolickReadFile(const char* name, unsigned char** data, int* len);
extern float g_nEffectAlpha;
extern D3DXMATRIX g_matIdentity;

////////////////////////////////////////////////////////////////////////////
// XtxTexture: a Texture node from a .xtx (tools/mkassets.py: "XTX1",
// width, height, B G R A pixels), swizzled into an ordinary texture so the
// engine's own nodes (the Background backdrop) can sample it with 0..1
// coordinates.

class CXtxTexture : public CTexture
{
    DECLARE_NODE(CXtxTexture, CTexture)
public:
    CXtxTexture() : m_url(NULL), m_bDirty(true) {}
    ~CXtxTexture() { delete [] m_url; }

    TCHAR* m_url;
    bool m_bDirty;

    LPDIRECT3DTEXTURE8 GetTextureSurface();
    bool OnSetProperty(const PRD* pprd, const void* pvValue) { m_bDirty = true; return true; }

    DECLARE_NODE_PROPS()
};

IMPLEMENT_NODE("XtxTexture", CXtxTexture, CTexture)

START_NODE_PROPS(CXtxTexture, CTexture)
    NODE_PROP(pt_string, CXtxTexture, url)
END_NODE_PROPS()

// Loads a .xtx into a swizzled A8R8G8B8 texture; the same name hands back
// the same texture (AddRef'd), since the pads share their art.
// D3DX rounds the texture up to a power of two (swizzled textures must
// be), so the image fills only the top left w x h texels of it; pTexWidth
// and pTexHeight get the texture's real size for the caller's UVs.
static LPDIRECT3DTEXTURE8 LoadXtx(const char* szName, int* pWidth, int* pHeight, int* pTexWidth = NULL, int* pTexHeight = NULL)
{
    struct Cached { char* name; LPDIRECT3DTEXTURE8 p; int w, h, tw, th; };
    static Cached c_rg[32];
    static int c_n;
    for (int i = 0; i < c_n; i++)
        if (_stricmp(c_rg[i].name, szName) == 0)
        {
            *pWidth = c_rg[i].w; *pHeight = c_rg[i].h;
            if (pTexWidth) *pTexWidth = c_rg[i].tw;
            if (pTexHeight) *pTexHeight = c_rg[i].th;
            if (c_rg[i].p) c_rg[i].p->AddRef();
            return c_rg[i].p;
        }
    LPDIRECT3DTEXTURE8 p = NULL;
    int w = 0, h = 0, tw = 0, th = 0;
    unsigned char* data = NULL;
    int len = 0;
    if (FrolickReadFile(szName, &data, &len) && len >= 12 && memcmp(data, "XTX1", 4) == 0)
    {
        w = *(int*)(data + 4); h = *(int*)(data + 8);
        for (tw = 1; tw < w; tw <<= 1) ;
        for (th = 1; th < h; th <<= 1) ;
        if (len >= 12 + w * h * 4 &&
            SUCCEEDED(D3DXCreateTexture(XAppGetD3DDev(), tw, th, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &p)))
        {
            // the image in the top left of a power-of-two texture, the
            // rest transparent
            unsigned char* padded = new unsigned char[tw * th * 4];
            memset(padded, 0, tw * th * 4);
            for (int y = 0; y < h; y++)
                memcpy(padded + y * tw * 4, data + 12 + y * w * 4, w * 4);
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(p->LockRect(0, &lr, NULL, 0)))
            {
                XGSwizzleRect(padded, tw * 4, NULL, lr.pBits, tw, th, NULL, 4);
                p->UnlockRect(0);
            }
            delete [] padded;
        }
    }
    else
        TRACE(_T("Frolick: cannot read texture %S\n"), szName);
    delete [] data;
    if (c_n < sizeof c_rg / sizeof c_rg[0])
    {
        c_rg[c_n].name = new char[strlen(szName) + 1];
        strcpy(c_rg[c_n].name, szName);
        c_rg[c_n].p = p; c_rg[c_n].w = w; c_rg[c_n].h = h; c_rg[c_n].tw = tw; c_rg[c_n].th = th;
        c_n++;
        if (p) p->AddRef();
    }
    *pWidth = w; *pHeight = h;
    if (pTexWidth) *pTexWidth = tw;
    if (pTexHeight) *pTexHeight = th;
    return p;
}

LPDIRECT3DTEXTURE8 CXtxTexture::GetTextureSurface()
{
    if (m_bDirty && m_url != NULL)
    {
        m_bDirty = false;
        if (m_pSurface != NULL)
        {
            m_pSurface->Release();
            m_pSurface = NULL;
        }
        char szName[MAX_PATH];
        Ansi(szName, m_url, sizeof szName);
        m_pSurface = LoadXtx(szName, &m_nImageWidth, &m_nImageHeight);
    }
    return m_pSurface;
}

////////////////////////////////////////////////////////////////////////////
// Common state for the unlit, blended meshes below.

struct WORLDVERTEX { float x, y, z; D3DCOLOR color; float u, v; };
#define WORLDVERTEX_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void BeginUnlit(DWORD* pdwLighting, DWORD srcBlend, DWORD destBlend)
{
    XAppGetRenderState(D3DRS_LIGHTING, pdwLighting);
    XAppSetRenderState(D3DRS_LIGHTING, FALSE);
    XAppSetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    XAppSetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    XAppSetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    XAppSetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    XAppSetRenderState(D3DRS_SRCBLEND, srcBlend);
    XAppSetRenderState(D3DRS_DESTBLEND, destBlend);
    XAppSetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    XAppSetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    XAppSetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    XAppSetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    XAppSetTexture(1, NULL);
    XAppGetD3DDev()->SetVertexShader(WORLDVERTEX_FVF);
}

static void EndUnlit(DWORD dwLighting)
{
    XAppSetTexture(0, NULL);
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    XAppSetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    XAppSetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    XAppSetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    XAppSetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    XAppSetRenderState(D3DRS_LIGHTING, dwLighting);
}

// A level cross-fade multiplies every node by g_nEffectAlpha: the texture
// factor carries it (white, alpha = g_nEffectAlpha) so a stage can modulate
// its alpha by it, or blend its colour towards white (a multiply that
// fades out), without touching the vertex colours.
static DWORD BeginEffectAlpha()
{
    DWORD dwSave;
    XAppGetRenderState(D3DRS_TEXTUREFACTOR, &dwSave);
    XAppSetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_COLORVALUE(1.0f, 1.0f, 1.0f, g_nEffectAlpha));
    return dwSave;
}
static void EndEffectAlpha(DWORD dwSave) { XAppSetRenderState(D3DRS_TEXTUREFACTOR, dwSave); }

////////////////////////////////////////////////////////////////////////////
// Ribbon: an open tube (Godot's CylinderMesh, no caps) with the ring's
// gradient material: opaque at the seam, fading to almost nothing on the
// far side.  Vertex colours carry the gradient; the Go renderer draws it
// as base colour times the gradient texture, alpha blended.

class CRibbon : public CNode
{
    DECLARE_NODE(CRibbon, CNode)
public:
    CRibbon();
    ~CRibbon() { delete [] m_verts; delete [] m_innerVerts; delete [] m_idx; }

    float m_radius;
    float m_height;
    D3DXVECTOR3 m_color;
    float m_alpha;
    int m_segments;
    bool m_inner;           // an opaque tube of the same size inside
    D3DXVECTOR3 m_innerColor;

    void Render();
    bool OnSetProperty(const PRD* pprd, const void* pvValue) { m_bDirty = true; return true; }

    DECLARE_NODE_PROPS()

protected:
    void Build();
    bool m_bDirty;
    WORLDVERTEX* m_verts;
    WORLDVERTEX* m_innerVerts;
    WORD* m_idx;
    int m_nVerts, m_nIdx;
};

IMPLEMENT_NODE("Ribbon", CRibbon, CNode)

START_NODE_PROPS(CRibbon, CNode)
    NODE_PROP(pt_number, CRibbon, radius)
    NODE_PROP(pt_number, CRibbon, height)
    NODE_PROP(pt_vec3, CRibbon, color)
    NODE_PROP(pt_number, CRibbon, alpha)
    NODE_PROP(pt_integer, CRibbon, segments)
    NODE_PROP(pt_boolean, CRibbon, inner)
    NODE_PROP(pt_vec3, CRibbon, innerColor)
END_NODE_PROPS()

CRibbon::CRibbon() :
    m_radius(0.5f), m_height(0.02f), m_color(0.587f, 0.569f, 0.441f), m_alpha(1.0f), m_segments(128),
    m_inner(false), m_innerColor(0.785f, 0.775f, 0.691f),
    m_bDirty(true), m_verts(NULL), m_innerVerts(NULL), m_idx(NULL), m_nVerts(0), m_nIdx(0)
{
}

// The ring's gradient texture (_albedo002.png, 64 wide) sampled across u:
// grey level and alpha at each column.
static const struct { int x; int grey, alpha; } c_ribbonGradient[] = {
    { 0, 0xff, 0xff }, { 15, 0xff, 0xff }, { 16, 0xb5, 0xd7 }, { 20, 0x57, 0x95 }, { 24, 0x1b, 0x54 },
    { 28, 0x06, 0x2d }, { 32, 0x00, 0x17 }, { 36, 0x0c, 0x3d }, { 40, 0x2b, 0x69 }, { 44, 0x6f, 0xa8 },
    { 48, 0xd3, 0xe8 }, { 52, 0xff, 0xff }, { 63, 0xff, 0xff },
};

static void RibbonSample(float u, float* pGrey, float* pAlpha)
{
    float x = u * 63.0f;
    int n = sizeof c_ribbonGradient / sizeof c_ribbonGradient[0];
    for (int i = 0; i + 1 < n; i++)
    {
        if (x <= c_ribbonGradient[i + 1].x)
        {
            float t = (x - c_ribbonGradient[i].x) / (float)(c_ribbonGradient[i + 1].x - c_ribbonGradient[i].x);
            if (t < 0.0f) t = 0.0f;
            *pGrey = (c_ribbonGradient[i].grey + (c_ribbonGradient[i + 1].grey - c_ribbonGradient[i].grey) * t) / 255.0f;
            *pAlpha = (c_ribbonGradient[i].alpha + (c_ribbonGradient[i + 1].alpha - c_ribbonGradient[i].alpha) * t) / 255.0f;
            return;
        }
    }
    *pGrey = *pAlpha = 1.0f;
}

void CRibbon::Build()
{
    delete [] m_verts; delete [] m_innerVerts; delete [] m_idx;
    int n = m_segments < 8 ? 8 : m_segments;
    m_nVerts = (n + 1) * 2;
    m_nIdx = n * 6;
    m_verts = new WORLDVERTEX[m_nVerts];
    m_innerVerts = new WORLDVERTEX[m_nVerts];
    m_idx = new WORD[m_nIdx];
    D3DCOLOR cInner = D3DCOLOR_COLORVALUE(m_innerColor.x, m_innerColor.y, m_innerColor.z, 1.0f);
    for (int i = 0; i <= n; i++)
    {
        // Godot's cylinder: u around the circumference, x = sin, z = cos,
        // so the seam (u = 0) is at +z and the faded side at -z.
        float u = (float)i / n;
        float a = u * 2.0f * D3DX_PI;
        float grey, alpha;
        RibbonSample(u, &grey, &alpha);
        D3DCOLOR c = D3DCOLOR_COLORVALUE(m_color.x * grey, m_color.y * grey, m_color.z * grey, alpha * m_alpha);
        for (int k = 0; k < 2; k++)
        {
            WORLDVERTEX& v = m_verts[i * 2 + k];
            v.x = sinf(a) * m_radius;
            v.y = (k ? -0.5f : 0.5f) * m_height;
            v.z = cosf(a) * m_radius;
            v.color = c;
            v.u = u; v.v = (float)k;
            m_innerVerts[i * 2 + k] = v;
            m_innerVerts[i * 2 + k].color = cInner;
        }
    }
    for (int j = 0; j < n; j++)
    {
        WORD* p = m_idx + j * 6;
        p[0] = j * 2; p[1] = j * 2 + 1; p[2] = j * 2 + 2;
        p[3] = j * 2 + 2; p[4] = j * 2 + 1; p[5] = j * 2 + 3;
    }
    m_bDirty = false;
}

void CRibbon::Render()
{
    if (m_bDirty)
        Build();
    DWORD dwLighting;
    BeginUnlit(&dwLighting, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA);
    DWORD dwFactor = BeginEffectAlpha();
    bool bFading = g_nEffectAlpha < 1.0f;
    XAppSetTexture(0, NULL);
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    XAppSetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    if (m_inner)
    {
        XAppSetRenderState(D3DRS_ALPHABLENDENABLE, bFading);
        XAppSetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        XAppGetD3DDev()->DrawIndexedVerticesUP(D3DPT_TRIANGLELIST, m_nIdx, m_idx, m_innerVerts, sizeof (WORLDVERTEX));
        XAppSetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        XAppSetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    }
    XAppGetD3DDev()->DrawIndexedVerticesUP(D3DPT_TRIANGLELIST, m_nIdx, m_idx, m_verts, sizeof (WORLDVERTEX));
    EndEffectAlpha(dwFactor);
    EndUnlit(dwLighting);
}

////////////////////////////////////////////////////////////////////////////
// Disc: a flat, unlit disc in the XZ plane (Godot's CylinderMesh of height
// 0.001 as the pads use it), or an annulus when innerRadius is set (the
// pad highlight ring).  Opaque, so it writes depth.

class CDisc : public CNode
{
    DECLARE_NODE(CDisc, CNode)
public:
    CDisc() : m_radius(0.5f), m_innerRadius(0.0f), m_color(1.0f, 1.0f, 1.0f), m_segments(96) {}

    float m_radius;
    float m_innerRadius;
    D3DXVECTOR3 m_color;
    int m_segments;

    void Render();

    DECLARE_NODE_PROPS()
};

IMPLEMENT_NODE("Disc", CDisc, CNode)

START_NODE_PROPS(CDisc, CNode)
    NODE_PROP(pt_number, CDisc, radius)
    NODE_PROP(pt_number, CDisc, innerRadius)
    NODE_PROP(pt_vec3, CDisc, color)
    NODE_PROP(pt_integer, CDisc, segments)
END_NODE_PROPS()

void CDisc::Render()
{
    int n = m_segments < 8 ? 8 : m_segments > 256 ? 256 : m_segments;
    WORLDVERTEX verts[2 * 257];
    WORD idx[256 * 6];
    D3DCOLOR c = D3DCOLOR_COLORVALUE(m_color.x, m_color.y, m_color.z, 1.0f);
    for (int i = 0; i <= n; i++)
    {
        float a = (float)i / n * 2.0f * D3DX_PI;
        WORLDVERTEX& o = verts[i * 2];
        o.x = sinf(a) * m_radius; o.y = 0.0f; o.z = cosf(a) * m_radius;
        o.color = c; o.u = o.v = 0.0f;
        WORLDVERTEX& q = verts[i * 2 + 1];
        q.x = sinf(a) * m_innerRadius; q.y = 0.0f; q.z = cosf(a) * m_innerRadius;
        q.color = c; q.u = q.v = 0.0f;
    }
    for (int j = 0; j < n; j++)
    {
        WORD* p = idx + j * 6;
        p[0] = j * 2; p[1] = j * 2 + 2; p[2] = j * 2 + 1;
        p[3] = j * 2 + 1; p[4] = j * 2 + 2; p[5] = j * 2 + 3;
    }
    DWORD dwLighting;
    BeginUnlit(&dwLighting, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA);
    DWORD dwFactor = BeginEffectAlpha();
    XAppSetRenderState(D3DRS_ALPHABLENDENABLE, g_nEffectAlpha < 1.0f);
    XAppSetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    XAppSetTexture(0, NULL);
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    XAppSetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    XAppGetD3DDev()->DrawIndexedVerticesUP(D3DPT_TRIANGLELIST, n * 6, idx, verts, sizeof (WORLDVERTEX));
    XAppSetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    EndEffectAlpha(dwFactor);
    EndUnlit(dwLighting);
}

////////////////////////////////////////////////////////////////////////////
// Sprite: Godot's Sprite3D / Label3D with billboard on: a textured quad
// that always faces the camera, upright, centred on the node's origin and
// sized by the texture's pixels times pixelSize, tinted by color.  Used
// for the pad emblems and the pad labels (depthTest false: drawn over
// everything, like Label3D's no_depth_test).

extern D3DXMATRIX g_matView;

class CSprite : public CNode
{
    DECLARE_NODE(CSprite, CNode)
public:
    CSprite() : m_url(NULL), m_pixelSize(0.001f), m_color(1.0f, 1.0f, 1.0f), m_alpha(1.0f), m_depthTest(true),
                m_offsetX(0.0f), m_offsetY(0.0f), m_offsetZ(0.0f), m_moveTime(0.0f),
                m_shown(0.0f, 0.0f, 0.0f), m_from(0.0f, 0.0f, 0.0f), m_target(0.0f, 0.0f, 0.0f), m_moveStart(0.0f),
                m_pTexture(NULL), m_w(0), m_h(0), m_tw(0), m_th(0), m_bDirty(true) {}
    ~CSprite() { delete [] m_url; if (m_pTexture) m_pTexture->Release(); }

    TCHAR* m_url;
    float m_pixelSize;
    D3DXVECTOR3 m_color;
    float m_alpha;
    bool m_depthTest;
    // A world-space offset from the node's origin that script can set
    // (label_settings.offsetX = 0.2); the sprite eases there over moveTime
    // seconds (Frolick's ease-in-out), so a label can slide into the
    // frame while the camera flies.
    float m_offsetX, m_offsetY, m_offsetZ, m_moveTime;

    void Render();
    bool OnSetProperty(const PRD* pprd, const void* pvValue) { m_bDirty = true; return true; }

    DECLARE_NODE_PROPS()

protected:
    D3DXVECTOR3 m_shown, m_from, m_target;
    XTIME m_moveStart;
    LPDIRECT3DTEXTURE8 m_pTexture;
    int m_w, m_h, m_tw, m_th;
    bool m_bDirty;
};

IMPLEMENT_NODE("Sprite", CSprite, CNode)

START_NODE_PROPS(CSprite, CNode)
    NODE_PROP(pt_string, CSprite, url)
    NODE_PROP(pt_number, CSprite, pixelSize)
    NODE_PROP(pt_vec3, CSprite, color)
    NODE_PROP(pt_number, CSprite, alpha)
    NODE_PROP(pt_boolean, CSprite, depthTest)
    NODE_PROP(pt_number, CSprite, offsetX)
    NODE_PROP(pt_number, CSprite, offsetY)
    NODE_PROP(pt_number, CSprite, offsetZ)
    NODE_PROP(pt_number, CSprite, moveTime)
END_NODE_PROPS()

void CSprite::Render()
{
    if (m_bDirty)
    {
        m_bDirty = false;
        if (m_pTexture) { m_pTexture->Release(); m_pTexture = NULL; }
        if (m_url != NULL)
        {
            char szName[MAX_PATH];
            Ansi(szName, m_url, sizeof szName);
            m_pTexture = LoadXtx(szName, &m_w, &m_h, &m_tw, &m_th);
        }
    }
    float alpha = m_alpha * g_nEffectAlpha;
    if (m_pTexture == NULL || alpha <= 0.0f)
        return;

    // The node's origin in world space, and the view's right and up in
    // world space (the view's rotation is orthogonal, so its transpose is
    // its inverse; the engine's z mirror does not change that).
    D3DXMATRIX world;
    XAppGetTransform(D3DTS_WORLD, &world);
    D3DXVECTOR3 origin(world._41, world._42, world._43);
    D3DXVECTOR3 target(m_offsetX, m_offsetY, m_offsetZ);
    if (target != m_target)
    {
        m_from = m_shown;
        m_target = target;
        m_moveStart = XAppGetNow();
    }
    float t = m_moveTime > 0.0f ? (float)(XAppGetNow() - m_moveStart) / m_moveTime : 1.0f;
    if (t > 1.0f) t = 1.0f;
    float f = t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    D3DXVec3Lerp(&m_shown, &m_from, &m_target, f);
    origin += m_shown;
    D3DXVECTOR3 right(g_matView._11, g_matView._21, g_matView._31);
    D3DXVECTOR3 up(g_matView._12, g_matView._22, g_matView._32);
    float hw = m_w * m_pixelSize * 0.5f, hh = m_h * m_pixelSize * 0.5f;
    float u1 = m_tw > 0 ? (float)m_w / m_tw : 1.0f, v1 = m_th > 0 ? (float)m_h / m_th : 1.0f;
    D3DCOLOR c = D3DCOLOR_COLORVALUE(m_color.x, m_color.y, m_color.z, alpha);
    WORLDVERTEX v[4];
    for (int i = 0; i < 4; i++)
    {
        float sx = (i & 1) ? hw : -hw, sy = (i & 2) ? -hh : hh;
        v[i].x = origin.x + right.x * sx + up.x * sy;
        v[i].y = origin.y + right.y * sx + up.y * sy;
        v[i].z = origin.z + right.z * sx + up.z * sy;
        v[i].color = c;
        v[i].u = (i & 1) ? u1 : 0.0f;
        v[i].v = (i & 2) ? v1 : 0.0f;
    }
    static const WORD idx[6] = { 0, 1, 2, 2, 1, 3 };

    DWORD dwLighting, dwZEnable;
    XAppGetRenderState(D3DRS_ZENABLE, &dwZEnable);
    BeginUnlit(&dwLighting, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA);
    if (!m_depthTest)
        XAppSetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    XAppSetTransform(D3DTS_WORLD, &g_matIdentity);
    XAppSetTexture(0, m_pTexture);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    XAppSetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    XAppSetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    XAppSetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    XAppSetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    XAppGetD3DDev()->DrawIndexedVerticesUP(D3DPT_TRIANGLELIST, 6, idx, v, sizeof (WORLDVERTEX));
    XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    XAppSetTransform(D3DTS_WORLD, &world);
    XAppSetRenderState(D3DRS_ZENABLE, dwZEnable);
    EndUnlit(dwLighting);
}

////////////////////////////////////////////////////////////////////////////
// Dome: Frolick's "atmo" hemisphere, seen from inside and multiplied over
// what is behind it: a slight darkening with thin light stripes, one per
// latitude, scrolling slowly.  The stripe column of _albedo000.png is
// rebuilt here as a 16 x 2048 linear texture (two periods, so a scrolled
// strip never wraps inside a triangle).

class CDome : public CNode
{
    DECLARE_NODE(CDome, CNode)
public:
    CDome();
    ~CDome();

    float m_radius;     // at the equator
    float m_height;     // of the dome above its centre
    float m_scroll;     // texture v per second
    float m_textureScale; // Godot uv1_scale.y
    int m_segments;
    int m_rings;

    void Render();
    bool OnSetProperty(const PRD* pprd, const void* pvValue) { m_bDirty = true; return true; }

    DECLARE_NODE_PROPS()

protected:
    void Build();
    bool m_bDirty;
    LPDIRECT3DTEXTURE8 m_pTexture;
    WORLDVERTEX* m_verts;
    float* m_vbase;     // each vertex's Godot v (0 at the pole, 0.5 at the rim)
    WORD* m_idx;
    int m_nVerts, m_nIdx;
};

IMPLEMENT_NODE("Dome", CDome, CNode)

START_NODE_PROPS(CDome, CNode)
    NODE_PROP(pt_number, CDome, radius)
    NODE_PROP(pt_number, CDome, height)
    NODE_PROP(pt_number, CDome, scroll)
    NODE_PROP(pt_number, CDome, textureScale)
    NODE_PROP(pt_integer, CDome, segments)
    NODE_PROP(pt_integer, CDome, rings)
END_NODE_PROPS()

CDome::CDome() :
    m_radius(6.97f), m_height(4.26f), m_scroll(0.015f), m_textureScale(0.97f), m_segments(48), m_rings(24),
    m_bDirty(true), m_pTexture(NULL), m_verts(NULL), m_vbase(NULL), m_idx(NULL), m_nVerts(0), m_nIdx(0)
{
}

CDome::~CDome()
{
    if (m_pTexture) m_pTexture->Release();
    delete [] m_verts; delete [] m_vbase; delete [] m_idx;
}

#define DOME_TEX_W 16
#define DOME_TEX_H 2048

// The stripe rows of the 988-row column: base grey 0xec, 16-row bands of
// white with 3-row soft edges at these image rows.
static const int c_domeBands[] = { 76, 163, 250, 313, 375, 597, 659, 722, 809, 896 };

static BYTE DomeRow(int row)
{
    for (int i = 0; i < sizeof c_domeBands / sizeof c_domeBands[0]; i++)
    {
        int d = row - c_domeBands[i];
        if (d >= 0 && d < 16) return 0xff;
        if ((d >= -3 && d < 0) || (d >= 16 && d < 19)) return 0xef;
    }
    return 0xec;
}

void CDome::Build()
{
    if (m_pTexture == NULL &&
        SUCCEEDED(XAppGetD3DDev()->CreateTexture(DOME_TEX_W, DOME_TEX_H, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED, &m_pTexture)))
    {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(m_pTexture->LockRect(0, &lr, NULL, 0)))
        {
            for (int y = 0; y < DOME_TEX_H; y++)
            {
                // texel row -> image row, top of the image at v = 1 (the Go shader flips v)
                int period = DOME_TEX_H / 2;
                int row = (int)((1.0f - (float)(y % period) / period) * 987.0f + 0.5f);
                BYTE g = DomeRow(row);
                DWORD* p = (DWORD*)((BYTE*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < DOME_TEX_W; x++)
                    p[x] = D3DCOLOR_ARGB(0xff, g, g, g);
            }
            m_pTexture->UnlockRect(0);
        }
    }

    delete [] m_verts; delete [] m_vbase; delete [] m_idx;
    int n = m_segments < 8 ? 8 : m_segments;
    int r = m_rings < 2 ? 2 : m_rings;
    // rings 0..r cover the dome (pole to rim); ring r + 1 is the flat rim
    // disc Godot's hemisphere closes the shape with, collapsed to the centre
    m_nVerts = (n + 1) * (r + 2);
    m_nIdx = n * (r + 1) * 6;
    m_verts = new WORLDVERTEX[m_nVerts];
    m_vbase = new float[m_nVerts];
    m_idx = new WORD[m_nIdx];
    for (int j = 0; j <= r + 1; j++)
    {
        float v = j <= r ? 0.5f * j / r : 1.0f;  // Godot's sphere v
        float lat = j <= r ? v * D3DX_PI : D3DX_PI * 0.5f;
        float w = j <= r ? sinf(lat) : 0.0f;
        float y = j <= r ? cosf(lat) : 0.0f;
        for (int i = 0; i <= n; i++)
        {
            float u = (float)i / n;
            float a = u * 2.0f * D3DX_PI;
            WORLDVERTEX& o = m_verts[j * (n + 1) + i];
            o.x = sinf(a) * m_radius * w;
            o.y = y * m_height;
            o.z = cosf(a) * m_radius * w;
            o.color = 0xffffffff;
            o.u = DOME_TEX_W / 2; o.v = 0.0f;
            m_vbase[j * (n + 1) + i] = v;
        }
    }
    for (int jj = 0; jj <= r; jj++)
        for (int ii = 0; ii < n; ii++)
        {
            WORD* p = m_idx + (jj * n + ii) * 6;
            WORD a = jj * (n + 1) + ii, b = a + n + 1;
            p[0] = a; p[1] = b; p[2] = a + 1;
            p[3] = a + 1; p[4] = b; p[5] = b + 1;
        }
    m_bDirty = false;
}

void CDome::Render()
{
    if (m_bDirty)
        Build();
    if (m_pTexture == NULL)
        return;
    float t = (float)XAppGetNow() * m_scroll;
    t -= floorf(t);
    int period = DOME_TEX_H / 2;
    for (int i = 0; i < m_nVerts; i++)
        m_verts[i].v = (m_vbase[i] * m_textureScale + t) * period;  // 0 .. 2 periods, monotonic down the dome

    DWORD dwLighting;
    BeginUnlit(&dwLighting, D3DBLEND_ZERO, D3DBLEND_SRCCOLOR);  // multiply
    XAppSetTexture(0, m_pTexture);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    XAppSetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    XAppSetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    // a multiply fades out towards white: texture * a + white * (1 - a)
    DWORD dwFactor = BeginEffectAlpha();
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDFACTORALPHA);
    XAppSetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    XAppSetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    XAppGetD3DDev()->DrawIndexedVerticesUP(D3DPT_TRIANGLELIST, m_nIdx, m_idx, m_verts, sizeof (WORLDVERTEX));
    XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    EndEffectAlpha(dwFactor);
    EndUnlit(dwLighting);
}
