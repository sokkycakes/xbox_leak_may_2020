// The parts of Frolick's 3D world that are not panels: the screen backdrop
// texture, the ribbon ring that circles the scene (and the bumper around
// each pad) and the "atmo" dome, a multiply layer with slowly scrolling
// light stripes.  The numbers come from frolick_assets/main.tscn and the
// Go renderer's draw order (unlit, alpha blended, no depth write).
//
//   Background { backdrop XtxTexture { url "backdrop.xtx" } }
//   Transform { translation x y z  children [ Ribbon { radius 5.92 height 1.815 } ] }
//   Transform { translation x y z  children [ Dome { radius 6.97 height 4.26 } ] }
#include "std.h"
#include "xapp.h"
#include "node.h"
#include "Texture.h"

extern bool FrolickReadFile(const char* name, unsigned char** data, int* len);

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
        unsigned char* data = NULL;
        int len = 0;
        if (FrolickReadFile(szName, &data, &len) && len >= 12 && memcmp(data, "XTX1", 4) == 0)
        {
            int w = *(int*)(data + 4), h = *(int*)(data + 8);
            if (len >= 12 + w * h * 4 && Create(w, h))
            {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(m_pSurface->LockRect(0, &lr, NULL, 0)))
                {
                    XGSwizzleRect(data + 12, w * 4, NULL, lr.pBits, w, h, NULL, 4);
                    m_pSurface->UnlockRect(0);
                }
            }
        }
        else
            TRACE(_T("XtxTexture: cannot read %s\n"), m_url);
        delete [] data;
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
    ~CRibbon() { delete [] m_verts; delete [] m_idx; }

    float m_radius;
    float m_height;
    D3DXVECTOR3 m_color;
    float m_alpha;
    int m_segments;

    void Render();
    bool OnSetProperty(const PRD* pprd, const void* pvValue) { m_bDirty = true; return true; }

    DECLARE_NODE_PROPS()

protected:
    void Build();
    bool m_bDirty;
    WORLDVERTEX* m_verts;
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
END_NODE_PROPS()

CRibbon::CRibbon() :
    m_radius(0.5f), m_height(0.02f), m_color(0.587f, 0.569f, 0.441f), m_alpha(1.0f), m_segments(128),
    m_bDirty(true), m_verts(NULL), m_idx(NULL), m_nVerts(0), m_nIdx(0)
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
    delete [] m_verts; delete [] m_idx;
    int n = m_segments < 8 ? 8 : m_segments;
    m_nVerts = (n + 1) * 2;
    m_nIdx = n * 6;
    m_verts = new WORLDVERTEX[m_nVerts];
    m_idx = new WORD[m_nIdx];
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
    XAppSetTexture(0, NULL);
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    XAppSetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    XAppGetD3DDev()->DrawIndexedVerticesUP(D3DPT_TRIANGLELIST, m_nIdx, m_idx, m_verts, sizeof (WORLDVERTEX));
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
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    XAppSetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    XAppSetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    XAppGetD3DDev()->DrawIndexedVerticesUP(D3DPT_TRIANGLELIST, m_nIdx, m_idx, m_verts, sizeof (WORLDVERTEX));
    XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    EndUnlit(dwLighting);
}
