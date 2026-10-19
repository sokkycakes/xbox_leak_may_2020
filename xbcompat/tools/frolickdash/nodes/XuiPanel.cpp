// XuiPanel: an xapp node that draws an Xbox 360 .xui scene (through the
// XUI runtime in ../xui) as a flat panel in the 3D scene.  The panel is
// the scene's canvas, scaled so that its width is `width` world units and
// centred on the node's origin, drawn under the current world matrix, so
// a Transform parent places and tilts it like any mesh.
//
//   DEF padLibrary XuiPanel {
//       url "pads/library.xui"  skin "pads/frolick_skin.xui"  width 0.9
//       function OnPress(id) { ... }         // A on a focused control
//       function OnFocus(id) { ... }         // focus moved
//   }
//
// Script functions: Navigate("left"|"right"|"up"|"down") -> 1 if the
// focus moved, Press(), Play(id, frame), SetText(id, text),
// SetImage(id, path), SetFocus(id), GetFocus(), SetVisible(id, 0|1),
// SetOpacity(id, n), Reload().  OnPress and OnFocus are delivered from
// Advance, after the call that caused them, so script can trigger them.  Images and fonts are .xtx / .ttf files
// packed by tools/mkassets.py beside the scene.
#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"
#include "../xui/xui.h"

extern float g_nEffectAlpha;

////////////////////////////////////////////////////////////////////////////
// Resources through the engine's file layer (XIPs and the app directory).

class CXuiResources : public xui::Resources
{
public:
    char m_szBase[MAX_PATH]; // directory of the scene, "pads/"

    CXuiResources() { m_szBase[0] = 0; }

    void SetBaseFrom(const char* szScene)
    {
        strncpy(m_szBase, szScene, sizeof m_szBase - 1);
        m_szBase[sizeof m_szBase - 1] = 0;
        char* p = strrchr(m_szBase, '/');
        char* q = strrchr(m_szBase, '\\');
        if (q > p) p = q;
        if (p) p[1] = 0; else m_szBase[0] = 0;
    }

    bool read(const char* path, unsigned char** data, int* len)
    {
        char sz[MAX_PATH];
        const char* name = path;
        const char* s = strstr(name, "://"); // sharedres://name
        if (s) name = s + 3;
        while (*name == '/' || *name == '\\') name++;
        bool bare = strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
        _snprintf(sz, sizeof sz, "%s%s", bare ? m_szBase : "", name);
        sz[sizeof sz - 1] = 0;
        // PNGs are converted to .xtx at build time.
        int n = strlen(sz);
        if (n > 4 && _stricmp(sz + n - 4, ".png") == 0) strcpy(sz + n - 4, ".xtx");
        for (char* q = sz; *q; q++) if (*q == '\\') *q = '/';

        // A XIP entry can be fetched only once (the loader hands over its
        // buffer), so keep every file read for the panels that share it.
        for (int i = 0; i < c_cache.n; i++)
            if (_stricmp(c_cache[i].name, sz) == 0)
            {
                *len = c_cache[i].len;
                *data = new unsigned char[*len + 1];
                CopyMemory(*data, c_cache[i].data, *len);
                (*data)[*len] = 0;
                return true;
            }

        TCHAR szURL[MAX_PATH];
        Unicode(szURL, sz, MAX_PATH);
        CActiveFile file;
        if (!file.Fetch(szURL, false, true) || file.GetContent() == NULL)
        {
            // not beside the scene: try the root (the font lives there)
            if (!bare || strlen(m_szBase) == 0)
                return false;
            Unicode(szURL, name, MAX_PATH);
            file.Reset();
            if (!file.Fetch(szURL, false, true) || file.GetContent() == NULL)
                return false;
        }
        Cached c;
        c.name = xui::strdup2(sz);
        c.len = (int)file.GetContentLength();
        c.data = new unsigned char[c.len + 1];
        CopyMemory(c.data, file.GetContent(), c.len);
        c.data[c.len] = 0;
        c_cache.push(c);
        *len = c.len;
        *data = new unsigned char[*len + 1];
        CopyMemory(*data, c.data, *len + 1);
        return true;
    }

    struct Cached { char* name; unsigned char* data; int len; };
    static xui::Vec<Cached> c_cache;
};

xui::Vec<CXuiResources::Cached> CXuiResources::c_cache;

// The same file layer for the other Frolick nodes (textures at the XIP root).
bool FrolickReadFile(const char* name, unsigned char** data, int* len)
{
    static CXuiResources res;
    return res.read(name, data, len);
}

////////////////////////////////////////////////////////////////////////////
// D3D8 backend: linear textures, DrawIndexedVerticesUP, clipping by
// geometry (the Xbox has no scissor).

struct XUIVERTEX { float x, y, z; D3DCOLOR color; float u, v; };
#define XUIVERTEX_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

class CXuiBackend : public xui::Backend
{
public:
    struct Tex { LPDIRECT3DTEXTURE8 p; float us, vs; };
    xui::Vec<Tex> m_texes; // index + 1 is the xui::Texture handle
    float m_sx, m_sy, m_ox, m_oy; // scene pixels -> world units
    // or, with m_basis, the canvas mapped onto world corners: origin is the
    // canvas's top left, m_ax runs along its width, m_ay down its height
    bool m_basis, m_flip;
    D3DXVECTOR3 m_o, m_ax, m_ay;
    float m_cw, m_ch;
    float m_alpha;
    bool m_clipping; float m_clip[4];
    xui::Vec<XUIVERTEX> m_verts;
    xui::Vec<unsigned short> m_idx;

    CXuiBackend() : m_sx(1), m_sy(1), m_ox(0), m_oy(0), m_basis(false), m_flip(false), m_cw(1), m_ch(1), m_alpha(1), m_clipping(false) {}

    ~CXuiBackend()
    {
        for (int i = 0; i < m_texes.n; i++)
            if (m_texes[i].p) m_texes[i].p->Release();
    }

    xui::Texture upload(const xui::Image& img)
    {
        // Linear textures need a pitch that is a multiple of 64 bytes, and
        // the Xbox addresses them in texels, so the 0..1 coordinates the
        // renderer hands over are scaled up by the image size.
        int w = (img.w + 15) & ~15, h = img.h;
        Tex t; t.p = NULL; t.us = (float)img.w; t.vs = (float)img.h;
        if (SUCCEEDED(XAppGetD3DDev()->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_MANAGED, &t.p)))
        {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(t.p->LockRect(0, &lr, NULL, 0)))
            {
                for (int y = 0; y < h; y++)
                {
                    BYTE* row = (BYTE*)lr.pBits + y * lr.Pitch;
                    CopyMemory(row, img.pix + y * img.w * 4, img.w * 4);
                    if (w > img.w) ZeroMemory(row + img.w * 4, (w - img.w) * 4);
                }
                t.p->UnlockRect(0);
            }
        }
        else
            TRACE(_T("XuiPanel: CreateTexture(%d,%d) failed\n"), w, h);
        m_texes.push(t);
        return (xui::Texture)m_texes.n;
    }

    void clip(const float* r)
    {
        m_clipping = r != NULL;
        if (r) CopyMemory(m_clip, r, sizeof m_clip);
    }

    static void lerp(const xui::Vertex& a, const xui::Vertex& b, float t, xui::Vertex& o)
    {
        o.x = a.x + (b.x - a.x) * t; o.y = a.y + (b.y - a.y) * t;
        o.u = a.u + (b.u - a.u) * t; o.v = a.v + (b.v - a.v) * t;
        o.r = a.r + (b.r - a.r) * t; o.g = a.g + (b.g - a.g) * t;
        o.b = a.b + (b.b - a.b) * t; o.a = a.a + (b.a - a.a) * t;
    }

    // Sutherland-Hodgman against one edge; axis 0 = x, 1 = y; keep side.
    static int clipEdge(const xui::Vertex* in, int n, xui::Vertex* out, int axis, float bound, bool keepLess)
    {
        int m = 0;
        for (int i = 0; i < n; i++)
        {
            const xui::Vertex& a = in[i];
            const xui::Vertex& b = in[(i + 1) % n];
            float va = axis ? a.y : a.x, vb = axis ? b.y : b.x;
            bool ina = keepLess ? va <= bound : va >= bound;
            bool inb = keepLess ? vb <= bound : vb >= bound;
            if (ina) out[m++] = a;
            if (ina != inb)
            {
                float t = (bound - va) / (vb - va);
                lerp(a, b, t, out[m++]);
            }
        }
        return m;
    }

    void emit(const xui::Vertex& v)
    {
        XUIVERTEX o;
        if (m_basis)
        {
            float fx = v.x / m_cw, fy = v.y / m_ch;
            if (m_flip) fx = 1.0f - fx;
            o.x = m_o.x + fx * m_ax.x + fy * m_ay.x;
            o.y = m_o.y + fx * m_ax.y + fy * m_ay.y;
            o.z = m_o.z + fx * m_ax.z + fy * m_ay.z;
        }
        else
        {
            o.x = (v.x - m_ox) * m_sx;
            o.y = (v.y - m_oy) * m_sy;
            o.z = 0.0f;
        }
        float a = v.a * m_alpha;
        o.color = D3DCOLOR_COLORVALUE(v.r, v.g, v.b, a < 0 ? 0 : a > 1 ? 1 : a);
        o.u = v.u; o.v = v.v;
        m_verts.push(o);
    }

    void triangles(xui::Texture tex, xui::Blend blend, const xui::Vertex* v, int nv, const unsigned short* idx, int ni)
    {
        m_verts.clear(); m_idx.clear();
        Tex t; t.p = NULL; t.us = t.vs = 1;
        if (tex > 0 && (int)tex <= m_texes.n) t = m_texes[tex - 1];
        if (!m_clipping)
        {
            for (int i = 0; i < nv; i++) emit(v[i]);
            for (int k = 0; k < ni; k++) m_idx.push(idx[k]);
        }
        else
        {
            xui::Vertex a[12], b[12];
            for (int k = 0; k + 2 < ni; k += 3)
            {
                a[0] = v[idx[k]]; a[1] = v[idx[k + 1]]; a[2] = v[idx[k + 2]];
                int n = 3;
                n = clipEdge(a, n, b, 0, m_clip[0], false);
                n = clipEdge(b, n, a, 0, m_clip[2], true);
                n = clipEdge(a, n, b, 1, m_clip[1], false);
                n = clipEdge(b, n, a, 1, m_clip[3], true);
                if (n < 3) continue;
                int base = m_verts.n;
                int i;
                for (i = 0; i < n; i++) emit(a[i]);
                for (i = 1; i + 1 < n; i++)
                {
                    m_idx.push((unsigned short)base);
                    m_idx.push((unsigned short)(base + i));
                    m_idx.push((unsigned short)(base + i + 1));
                }
            }
        }
        if (m_idx.n == 0) return;
        if (t.p && (t.us != 1 || t.vs != 1))
            for (int j = 0; j < m_verts.n; j++) { m_verts[j].u *= t.us; m_verts[j].v *= t.vs; }

        switch (blend)
        {
        case xui::BlendAdd:
            XAppSetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            XAppSetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            break;
        case xui::BlendMultiply:
            XAppSetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
            XAppSetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
            break;
        default:
            XAppSetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            XAppSetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            break;
        }
        XAppSetTexture(0, t.p);
        if (t.p)
        {
            XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            XAppSetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            XAppSetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            XAppSetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            XAppSetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        }
        else
        {
            XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            XAppSetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            XAppSetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        }
        XAppSetVertexShader(XUIVERTEX_FVF);
        XAppGetD3DDev()->DrawIndexedVerticesUP(D3DPT_TRIANGLELIST, m_idx.n, m_idx.p, m_verts.p, sizeof (XUIVERTEX));
    }
};

////////////////////////////////////////////////////////////////////////////

class CXuiPanel : public CNode
{
    DECLARE_NODE(CXuiPanel, CNode)
public:
    CXuiPanel();
    ~CXuiPanel();

    TCHAR* m_url;
    TCHAR* m_skin;
    TCHAR* m_font;
    float m_width;      // world units; the height follows the canvas
    float m_height;     // or set it explicitly
    float m_alpha;
    bool m_mirror;      // flip in X (the settings pad)
    // Or place the canvas on explicit world corners: origin is the canvas's
    // top left, axisX its full width, axisY its full height downward (the
    // way Frolick's hub panels are fixed, skewed quads in the world).  Set
    // when axisX is not zero; width/height are ignored then.
    D3DXVECTOR3 m_origin, m_axisX, m_axisY;
    float m_canvasWidth, m_canvasHeight; // read back by script

    void Advance(float nSeconds);
    void Render();
    bool OnSetProperty(const PRD* pprd, const void* pvValue);

    int Navigate(const TCHAR* szDir);
    void Press();
    void Play(const TCHAR* szId, const TCHAR* szFrame);
    void SetText(const TCHAR* szId, const TCHAR* szText);
    void SetImage(const TCHAR* szId, const TCHAR* szPath);
    void SetFocus(const TCHAR* szId);
    CStrObject* GetFocus();
    CObject* SetVisible(CObject** rgparam, int nParam);
    CObject* SetOpacity(CObject** rgparam, int nParam);
    void Reload();

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()

protected:
    void Load();
    xui::Node* Find(const TCHAR* szId);
    static void OnPressCB(xui::Scene* s, xui::Node* n, void* user);
    static void OnFocusCB(xui::Scene* s, xui::Node* n, void* user);
    void Fire(const TCHAR* szFunc, xui::Node* n);

    struct Event { const TCHAR* szFunc; char szId[128]; };
    xui::Vec<Event> m_events;

    bool m_bDirty;
    CXuiResources m_res;
    CXuiBackend m_be;
    xui::Renderer* m_pRenderer;
    xui::Element* m_pRoot;
    xui::Element* m_pSkin;
    xui::Scene* m_pScene;
    bool m_bFontLoaded;
};

IMPLEMENT_NODE("XuiPanel", CXuiPanel, CNode)

START_NODE_PROPS(CXuiPanel, CNode)
    NODE_PROP(pt_string, CXuiPanel, url)
    NODE_PROP(pt_string, CXuiPanel, skin)
    NODE_PROP(pt_string, CXuiPanel, font)
    NODE_PROP(pt_number, CXuiPanel, width)
    NODE_PROP(pt_number, CXuiPanel, height)
    NODE_PROP(pt_number, CXuiPanel, alpha)
    NODE_PROP(pt_boolean, CXuiPanel, mirror)
    NODE_PROP(pt_vec3, CXuiPanel, origin)
    NODE_PROP(pt_vec3, CXuiPanel, axisX)
    NODE_PROP(pt_vec3, CXuiPanel, axisY)
    NODE_PROP(pt_number, CXuiPanel, canvasWidth)
    NODE_PROP(pt_number, CXuiPanel, canvasHeight)
END_NODE_PROPS()

START_NODE_FUN(CXuiPanel, CNode)
    NODE_FUN_IS(Navigate)
    NODE_FUN_VV(Press)
    NODE_FUN_VSS(Play)
    NODE_FUN_VSS(SetText)
    NODE_FUN_VSS(SetImage)
    NODE_FUN_VS(SetFocus)
    NODE_FUN_SV(GetFocus)
    NODE_FUN(SetVisible)
    NODE_FUN(SetOpacity)
    NODE_FUN_VV(Reload)
END_NODE_FUN()

CXuiPanel::CXuiPanel() :
    m_url(NULL), m_skin(NULL), m_font(NULL),
    m_width(0.9f), m_height(0.0f), m_alpha(1.0f), m_mirror(false),
    m_origin(0.0f, 0.0f, 0.0f), m_axisX(0.0f, 0.0f, 0.0f), m_axisY(0.0f, 0.0f, 0.0f),
    m_canvasWidth(0.0f), m_canvasHeight(0.0f),
    m_bDirty(true), m_pRenderer(NULL), m_pRoot(NULL), m_pSkin(NULL), m_pScene(NULL), m_bFontLoaded(false)
{
}

CXuiPanel::~CXuiPanel()
{
    delete m_pScene;
    delete m_pRoot;
    delete m_pSkin;
    delete m_pRenderer;
    delete [] m_url;
    delete [] m_skin;
    delete [] m_font;
}

bool CXuiPanel::OnSetProperty(const PRD* pprd, const void* pvValue)
{
    if (pprd->nType == pt_string)
        m_bDirty = true;
    return true;
}

static void ToAnsi(char* sz, const TCHAR* wsz, int n)
{
    if (wsz == NULL) { sz[0] = 0; return; }
    Ansi(sz, wsz, n);
    sz[n - 1] = 0;
}

void CXuiPanel::Load()
{
    m_bDirty = false;
    delete m_pScene; m_pScene = NULL;
    delete m_pRoot; m_pRoot = NULL;
    delete m_pSkin; m_pSkin = NULL;
    if (m_pRenderer == NULL)
        m_pRenderer = new xui::Renderer(&m_be, &m_res);
    if (m_url == NULL)
        return;

    char sz[MAX_PATH], err[256];
    ToAnsi(sz, m_url, MAX_PATH);
    m_res.SetBaseFrom(sz);
    m_pRoot = m_res.scene(sz, err, sizeof err);
    if (m_pRoot == NULL)
    {
        TRACE(_T("XuiPanel: %s: %S\n"), m_url, err);
        return;
    }
    if (m_skin != NULL && m_skin[0])
    {
        ToAnsi(sz, m_skin, MAX_PATH);
        m_pSkin = m_res.scene(sz, err, sizeof err);
        if (m_pSkin == NULL)
            TRACE(_T("XuiPanel: %s: %S\n"), m_skin, err);
    }
    if (!m_bFontLoaded)
    {
        ToAnsi(sz, m_font != NULL && m_font[0] ? m_font : _T("font.ttf"), MAX_PATH);
        m_pRenderer->setFont(sz);
        m_bFontLoaded = true;
    }
    m_pScene = new xui::Scene(m_pRoot, m_pSkin);
    m_pScene->user = this;
    m_pScene->onPress = OnPressCB;
    m_pScene->onFocus = OnFocusCB;
    m_canvasWidth = m_pScene->root->width();
    m_canvasHeight = m_pScene->root->height();
}

void CXuiPanel::Advance(float nSeconds)
{
    if (m_bDirty)
        Load();
    if (m_pScene != NULL)
        m_pScene->update(nSeconds);
    // Deliver queued events now, outside any script call that caused them.
    for (int i = 0; i < m_events.n; i++)
    {
        TCHAR szId[128];
        Unicode(szId, m_events[i].szId, 128);
        CObject* param = new CStrObject(szId);
        CallFunction(this, m_events[i].szFunc, 1, &param);
    }
    m_events.clear();
}

void CXuiPanel::Render()
{
    if (m_bDirty)
        Load();
    if (m_pScene == NULL || !m_visible)
        return;
    float alpha = m_alpha * g_nEffectAlpha;
    if (alpha <= 0.0f)
        return;

    float cw = m_canvasWidth > 0 ? m_canvasWidth : 1.0f;
    float ch = m_canvasHeight > 0 ? m_canvasHeight : 1.0f;
    float s = m_width / cw;
    m_be.m_sx = m_mirror ? -s : s;
    m_be.m_sy = -(m_height > 0 ? m_height / ch : s);
    m_be.m_ox = cw / 2;
    m_be.m_oy = ch / 2;
    m_be.m_basis = m_axisX.x != 0.0f || m_axisX.y != 0.0f || m_axisX.z != 0.0f;
    m_be.m_flip = m_mirror;
    m_be.m_o = m_origin; m_be.m_ax = m_axisX; m_be.m_ay = m_axisY;
    m_be.m_cw = cw; m_be.m_ch = ch;
    m_be.m_alpha = alpha;

    DWORD dwLighting;
    XAppGetRenderState(D3DRS_LIGHTING, &dwLighting);
    XAppSetRenderState(D3DRS_LIGHTING, FALSE);
    XAppSetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    XAppSetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    XAppSetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    XAppSetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    XAppSetRenderState(D3DRS_EDGEANTIALIAS, FALSE);
    XAppSetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    XAppSetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    XAppSetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    XAppSetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    XAppSetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    XAppSetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    XAppSetTexture(1, NULL);

    m_pRenderer->draw(m_pScene, 0, 0, 1);

    XAppSetTexture(0, NULL);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    XAppSetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    XAppSetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    XAppSetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    XAppSetRenderState(D3DRS_LIGHTING, dwLighting);
}

////////////////////////////////////////////////////////////////////////////
// Script interface

xui::Node* CXuiPanel::Find(const TCHAR* szId)
{
    if (m_bDirty)
        Load();  // script may reach the panel before its first frame
    if (m_pScene == NULL || szId == NULL)
        return NULL;
    char sz[128];
    ToAnsi(sz, szId, sizeof sz);
    return m_pScene->find(sz);
}

void CXuiPanel::Fire(const TCHAR* szFunc, xui::Node* n)
{
    Event e;
    e.szFunc = szFunc;
    strncpy(e.szId, n != NULL ? n->id() : "", sizeof e.szId - 1);
    e.szId[sizeof e.szId - 1] = 0;
    m_events.push(e);
}

void CXuiPanel::OnPressCB(xui::Scene* s, xui::Node* n, void* user)
{
    ((CXuiPanel*)user)->Fire(_T("OnPress"), n);
}

void CXuiPanel::OnFocusCB(xui::Scene* s, xui::Node* n, void* user)
{
    ((CXuiPanel*)user)->Fire(_T("OnFocus"), n);
}

int CXuiPanel::Navigate(const TCHAR* szDir)
{
    if (m_pScene == NULL)
        return 0;
    char sz[16];
    ToAnsi(sz, szDir, sizeof sz);
    return m_pScene->navigate(sz) ? 1 : 0;
}

void CXuiPanel::Press()
{
    if (m_pScene != NULL)
        m_pScene->press();
}

void CXuiPanel::Play(const TCHAR* szId, const TCHAR* szFrame)
{
    xui::Node* n = (szId == NULL || szId[0] == 0) && m_pScene != NULL ? m_pScene->root : Find(szId);
    if (n == NULL)
        return;
    char sz[64];
    ToAnsi(sz, szFrame, sizeof sz);
    n->play(sz);
}

static void SetString(xui::Node* n, const char* szProp, const TCHAR* szValue)
{
    if (n == NULL)
        return;
    char sz[1024];
    ToAnsi(sz, szValue, sizeof sz);
    xui::Value& v = n->props.set(szProp);
    v.free();
    v.type = xui::TString;
    v.s = xui::strdup2(sz);
}

void CXuiPanel::SetText(const TCHAR* szId, const TCHAR* szText)
{
    SetString(Find(szId), "Text", szText);
}

void CXuiPanel::SetImage(const TCHAR* szId, const TCHAR* szPath)
{
    SetString(Find(szId), "ImagePath", szPath);
}

void CXuiPanel::SetFocus(const TCHAR* szId)
{
    xui::Node* n = Find(szId);
    if (n != NULL)
        m_pScene->setFocus(n);
}

CStrObject* CXuiPanel::GetFocus()
{
    TCHAR szId[128];
    Unicode(szId, m_pScene != NULL && m_pScene->focus != NULL ? m_pScene->focus->id() : "", 128);
    return new CStrObject(szId);
}

// SetVisible(id, 0|1) and SetOpacity(id, n) take the id first, which no
// fixed signature offers, so they unpack their parameters themselves.
CObject* CXuiPanel::SetVisible(CObject** rgparam, int nParam)
{
    if (nParam != 2)
        return NULL;
    xui::Node* n = Find(rgparam[0]->Deref()->ToStr()->GetSz());
    if (n == NULL)
        return NULL;
    xui::Value& v = n->props.set("Show");
    v.free();
    v.type = xui::TBool;
    v.b = rgparam[1]->Deref()->ToNum()->m_nValue != 0.0f;
    return NULL;
}

CObject* CXuiPanel::SetOpacity(CObject** rgparam, int nParam)
{
    if (nParam != 2)
        return NULL;
    xui::Node* n = Find(rgparam[0]->Deref()->ToStr()->GetSz());
    if (n == NULL)
        return NULL;
    xui::Value& v = n->props.set("Opacity");
    v.free();
    v.type = xui::TFloat;
    v.f = rgparam[1]->Deref()->ToNum()->m_nValue;
    return NULL;
}

void CXuiPanel::Reload()
{
    m_bDirty = true;
}
