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
// Fade(id, opacity, seconds), Pulse(id, dy, period, seconds),
// SetOpacity(id, n), SetData(id, k, text), SetValue(id, n), Reload().  OnPress and OnFocus are delivered from
// Advance, after the call that caused them, so script can trigger them.  Images and fonts are .xtx / .ttf files
// packed by tools/mkassets.py beside the scene.
#include "std.h"
#include "xapp.h"
#include "node.h"
#include "runner.h"
#include "Camera.h"
#include "activefile.h"
#include "Texture.h"
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

#define MAX_BELT_GAMES 256

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
    // or, with m_gridMode, the canvas rectangle (m_gx0, m_gy0, scaled by
    // m_gxs across and m_gys down to 0..1) draped over a 7x6 grid of world
    // points, row 0 at the top (the Belt's cover meshes); points outside
    // the rectangle extrapolate from the edge cells
    bool m_gridMode;
    D3DXVECTOR3 m_grid[6][7];
    float m_gx0, m_gy0, m_gxs, m_gys;
    float m_alpha;
    bool m_clipping; float m_clip[4];
    xui::Vec<XUIVERTEX> m_verts;
    xui::Vec<unsigned short> m_idx;

    CXuiBackend() : m_sx(1), m_sy(1), m_ox(0), m_oy(0), m_basis(false), m_flip(false), m_cw(1), m_ch(1),
        m_gridMode(false), m_gx0(0), m_gy0(0), m_gxs(1), m_gys(1), m_alpha(1), m_clipping(false) {}

    D3DXVECTOR3 GridPoint(float gx, float gy) const
    {
        float fx = gx * 6.0f, fy = gy * 5.0f;
        int cx = (int)floor(fx), cy = (int)floor(fy);
        if (cx < 0) cx = 0; if (cx > 5) cx = 5;
        if (cy < 0) cy = 0; if (cy > 4) cy = 4;
        float tx = fx - cx, ty = fy - cy;
        const D3DXVECTOR3& a = m_grid[cy][cx];
        const D3DXVECTOR3& b = m_grid[cy][cx + 1];
        const D3DXVECTOR3& c = m_grid[cy + 1][cx];
        const D3DXVECTOR3& d = m_grid[cy + 1][cx + 1];
        return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * (tx * ty);
    }

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

    // An Xbox texture file (.xbx: a save's SaveImage.xbx, a game's title
    // image out of its XBE), read the way the engine's materials read them
    // (Image.cpp ParseTexture: swizzled, so its coordinates are 0..1).
    xui::Texture native(const char* path, int* w, int* h)
    {
        *w = *h = 0;
        TCHAR szURL[MAX_PATH];
        Unicode(szURL, path, MAX_PATH);
        CActiveFile file;
        if (!file.Fetch(szURL, false, true) || file.GetContent() == NULL)
            return 0;
        LPDIRECT3DTEXTURE8 p = ParseTexture(szURL, file.GetContent(), file.GetContentLength(), 0, 0);
        if (p == NULL)
            return 0;
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(p->GetLevelDesc(0, &desc)))
        {
            *w = desc.Width;
            *h = desc.Height;
        }
        Tex t; t.p = p; t.us = 1.0f; t.vs = 1.0f;
        m_texes.push(t);
        return (xui::Texture)m_texes.n;
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
        if (m_gridMode)
        {
            D3DXVECTOR3 w = GridPoint((v.x - m_gx0) * m_gxs, (v.y - m_gy0) * m_gys);
            o.x = w.x; o.y = w.y; o.z = w.z;
        }
        else if (m_basis)
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
            // gradient brushes run past their box (a glow centred near a
            // corner): clamp, so the ramp's last stop fills the rest
            XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
            XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
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
    // The canvas y that sits at the panel's centre (default: the middle).
    // With a width wider than the panel's place this shows a window of the
    // canvas: the Blades pages show the 360's 16:9 window, canvas rows
    // 58.7..688.7, scaled to the HUD's height.
    float m_viewY;

    void Advance(float nSeconds);
    void Render();
    bool OnSetProperty(const PRD* pprd, const void* pvValue);

    int Navigate(const TCHAR* szDir);
    void Press();
    void Play(const TCHAR* szId, const TCHAR* szFrame);
    void SetText(const TCHAR* szId, const TCHAR* szText);
    void SetImage(const TCHAR* szId, const TCHAR* szPath);
    CObject* SetValue(CObject** rgparam, int nParam);
    CObject* Fade(CObject** rgparam, int nParam);
    CObject* Pulse(CObject** rgparam, int nParam);
    // Fade(id, opacity, seconds): the control's opacity eased to a value
    struct FadeAnim { xui::Node* n; float from, to, t, dur; };
    xui::Vec<FadeAnim> m_fades;
    // Pulse(id, dy, period, seconds): every period the control dips dy
    // and eases back over seconds (dy 0 stops it)
    struct PulseAnim { xui::Node* n; xui::Vec3 base; float dy, period, dur, t; };
    xui::Vec<PulseAnim> m_pulses;
    void Animate(float nSeconds);
    void SetFocus(const TCHAR* szId);
    CStrObject* GetFocus();
    CObject* SetVisible(CObject** rgparam, int nParam);
    CObject* SetOpacity(CObject** rgparam, int nParam);
    CObject* SetData(CObject** rgparam, int nParam);
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
    NODE_PROP(pt_number, CXuiPanel, viewY)
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
    NODE_FUN(SetData)
    NODE_FUN(SetValue)
    NODE_FUN(Fade)
    NODE_FUN(Pulse)
    NODE_FUN_VV(Reload)
END_NODE_FUN()

CXuiPanel::CXuiPanel() :
    m_url(NULL), m_skin(NULL), m_font(NULL),
    m_width(0.9f), m_height(0.0f), m_alpha(1.0f), m_mirror(false),
    m_origin(0.0f, 0.0f, 0.0f), m_axisX(0.0f, 0.0f, 0.0f), m_axisY(0.0f, 0.0f, 0.0f),
    m_canvasWidth(0.0f), m_canvasHeight(0.0f), m_viewY(0.0f),
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
    m_fades.clear();
    m_pulses.clear();
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
    Animate(nSeconds);
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
    m_be.m_oy = m_viewY > 0.0f ? m_viewY : ch / 2;
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

static void SetNodeOpacity(xui::Node* n, float a)
{
    xui::Value& v = n->props.set("Opacity");
    v.free();
    v.type = xui::TFloat;
    v.f = a;
}

CObject* CXuiPanel::Fade(CObject** rgparam, int nParam)
{
    if (nParam != 3)
        return NULL;
    xui::Node* n = Find(rgparam[0]->Deref()->ToStr()->GetSz());
    if (n == NULL)
        return NULL;
    FadeAnim a;
    a.n = n;
    a.from = xui::propF(n->props, "Opacity", 1.0f);
    a.to = rgparam[1]->Deref()->ToNum()->m_nValue;
    a.t = 0.0f;
    a.dur = rgparam[2]->Deref()->ToNum()->m_nValue;
    for (int i = 0; i < m_fades.n; i++)
    {
        if (m_fades[i].n == n)
        {
            m_fades[i] = a;
            return NULL;
        }
    }
    m_fades.push(a);
    return NULL;
}

CObject* CXuiPanel::Pulse(CObject** rgparam, int nParam)
{
    if (nParam != 4)
        return NULL;
    xui::Node* n = Find(rgparam[0]->Deref()->ToStr()->GetSz());
    if (n == NULL)
        return NULL;
    float dy = rgparam[1]->Deref()->ToNum()->m_nValue;
    int i;
    for (i = 0; i < m_pulses.n; i++)
    {
        if (m_pulses[i].n == n)
            break;
    }
    if (dy == 0.0f)
    {
        if (i < m_pulses.n)
        {
            // back to rest
            xui::Value& v = n->props.set("Position");
            v.free();
            v.type = xui::TVector;
            v.v = m_pulses[i].base;
            m_pulses[i] = m_pulses[m_pulses.n - 1];
            m_pulses.n -= 1;
        }
        return NULL;
    }
    PulseAnim a;
    a.n = n;
    xui::Vec3 zero = { 0.0f, 0.0f, 0.0f };
    a.base = i < m_pulses.n ? m_pulses[i].base : xui::propV(n->props, "Position", zero);
    a.dy = dy;
    a.period = rgparam[2]->Deref()->ToNum()->m_nValue;
    a.dur = rgparam[3]->Deref()->ToNum()->m_nValue;
    a.t = i < m_pulses.n ? m_pulses[i].t : 0.0f;
    if (i < m_pulses.n)
        m_pulses[i] = a;
    else
        m_pulses.push(a);
    return NULL;
}

// The fades and pulses, a frame on.
void CXuiPanel::Animate(float nSeconds)
{
    for (int i = 0; i < m_fades.n; )
    {
        FadeAnim& a = m_fades[i];
        a.t += nSeconds;
        float u = a.dur > 0.0f ? a.t / a.dur : 1.0f;
        if (u >= 1.0f)
        {
            SetNodeOpacity(a.n, a.to);
            m_fades[i] = m_fades[m_fades.n - 1];
            m_fades.n -= 1;
            continue;
        }
        SetNodeOpacity(a.n, a.from + (a.to - a.from) * u);
        i++;
    }
    for (int j = 0; j < m_pulses.n; j++)
    {
        PulseAnim& a = m_pulses[j];
        a.t += nSeconds;
        float phase = a.period > 0.0f ? (float)fmod(a.t, a.period) : 0.0f;
        float off = 0.0f;
        if (a.dur > 0.0f && phase < a.dur)
        {
            // down and back as a half sine, the time eased out so the dip
            // is quick and the return settles
            float u = phase / a.dur;
            float e = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
            off = a.dy * (float)sin(3.14159265f * e);
        }
        xui::Value& v = a.n->props.set("Position");
        v.free();
        v.type = xui::TVector;
        v.v = a.base;
        v.v.y += off;
    }
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

// SetData(id, k, text): the control's data slot k, shown by the presenters
// of its visual bound to it (DataAssociation k), as the 360 dashboard
// fills a list item's or meta panel's strings.
CObject* CXuiPanel::SetData(CObject** rgparam, int nParam)
{
    if (nParam != 3)
        return NULL;
    // A property name is kept by pointer, so the key must outlive this call.
    static const char* const rgszKey[] = { "Data0", "Data1", "Data2", "Data3", "Data4", "Data5", "Data6", "Data7", "Data8", "Data9" };
    int k = (int)rgparam[1]->Deref()->ToNum()->m_nValue;
    if (k < 0 || k > 9)
        return NULL;
    SetString(Find(rgparam[0]->Deref()->ToStr()->GetSz()), rgszKey[k], rgparam[2]->Deref()->ToStr()->GetSz());
    return NULL;
}

// SetValue(id, n): a progress bar's Value (a gauge's fill).
CObject* CXuiPanel::SetValue(CObject** rgparam, int nParam)
{
    if (nParam != 2)
        return NULL;
    xui::Node* n = Find(rgparam[0]->Deref()->ToStr()->GetSz());
    if (n == NULL)
        return NULL;
    xui::Value& v = n->props.set("Value");
    v.free();
    v.type = xui::TFloat;
    v.f = rgparam[1]->Deref()->ToNum()->m_nValue;
    // the bar's visual is held at the frame the value picks between
    // Normal and EndNormal (the scene set it so when it loaded; a value
    // set later moves it)
    if (n->visual != NULL && n->visual->timeline)
    {
        xui::Node* vis = n->visual;
        float lo = xui::propN(n->props, "RangeMin", 0), hi = xui::propN(n->props, "RangeMax", 100);
        float f = hi > lo ? (v.f - lo) / (hi - lo) : 0;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        double a = vis->frameTime("Normal", 0), b = vis->frameTime("EndNormal", vis->lastKey());
        vis->time = a + (b - a) * f;
        vis->playing = false;
        vis->apply();
    }
    return NULL;
}

void CXuiPanel::Reload()
{
    m_bDirty = true;
}

////////////////////////////////////////////////////////////////////////////
// Belt: GameTap's game belt (art/gametap/game-belt-render.md), the library
// view.
//
// The belt is the 13 scene nodes gsb_0shape .. gsb_12shape of GameTap's
// beta2scene.gr2, each drawn with its own cover mesh gsb_NshapeShape (42
// vertices, 60 triangles) at its sampled world matrix; tools/mkbelt.py
// packs them from the decoded JSON.  At frame 1000 the covers sit on a
// horizontal ring facing its axis, 19.5 degrees apart, the focused cover
// gsb_6shape on the ring's -x side pulled in and larger.  That ring is
// placed in Frolick's world lining the inside of the rail ring: its axis on
// centre at the covers' height, the regular covers' circle scaled to
// radius, GameTap's -x turned to -z so the focused cover is on the far wall
// straight ahead of the carousel camera, y up, GameTap's -z (its right) to
// +x.  The camera is Frolick's own; nothing here moves it.
//
// Motion is the sampled clip and nothing else: Belt_Right plays frames
// 1000..1020 at speed 2.5 (0.267 s), sliding covers 1..10 onto the next
// cover's pose (covers 0, 11 and 12 have no keys and stay); Belt_Left plays
// it backwards.  scroll_belt_right / scroll_belt_left hold the in-progress
// lock (gametap.dll 0x6052b910 / 0x6052baf0): a press while a step plays
// is dropped.  When a step ends the covers are back on their frame-1000
// slots, so the selection moves and each slot takes the next game.  Covers
// draw in shape-list order (all gsb* share drawOrder 280), depth tested.
//
// Each cover wears Frolick's tile art: the carousel_card.xui scene draped
// over the cover mesh (a 7x6 vertex grid; the card keeps its own aspect,
// matched to the cover's width and centred on it), and the focused cover
// the carousel_focus.xui overlay (its card rectangle on the cover, the
// arrows, A pill, title and back hint riding with it).  GameTap's cover
// materials are not used.
//
//   DEF theBelt Belt
//   {
//       url "belt.bin"  card "pads/carousel_card.xui"  focus "pads/carousel_focus.xui"
//       skin "pads/frolick_skin.xui"  font "font.ttf"
//       icon "pads/carousel_icon.xui"  icons "icon_grid.png;icon_disc.png"
//                                   reserved tiles' icons, game 0 first
//       start 2                     the game focused when the belt first opens
//       centre x y z  radius r      the ring the belt lines: its axis at
//                                   the regular covers' height, and their
//                                   radius; the focused cover faces +z
//       coverScale 2                every cover grown about its own centre
//       focusPull 5.6               the focused slot pulled this far off the
//                                   ring toward the camera (a cover sliding
//                                   in or out follows, by its angle from
//                                   the slot)
//       skip 2                      the slots either side of the focused one
//                                   sit behind the pulled-in cover from the
//                                   camera, so this many on each side hold
//                                   nothing: the previous and next game are
//                                   on the first slots past them, and the
//                                   rest follow outward slot by slot.  On a
//                                   step a game crosses the empty slots into
//                                   or out of the focus within the step time
//                                   (the clip's poses, slot to slot), while
//                                   the others move one slot.
//       count 9                     games on the belt
//       function OnStep() { ... }   a step finished; GetSelected() tells which
//   }
//   theBelt.Open();  theBelt.Close();  theBelt.Step(1);  theBelt.SetTitle("...");

struct BELTHDR { char magic[4]; int nNodes, nFrames, nVerts, nIdx; };
struct BELTVERTEX { float x, y, z, nx, ny, nz, u, v; };

class CBelt : public CNode
{
    DECLARE_NODE(CBelt, CNode)
public:
    CBelt();
    ~CBelt();

    TCHAR* m_url;
    TCHAR* m_card;
    TCHAR* m_focus;
    TCHAR* m_skin;
    TCHAR* m_font;
    TCHAR* m_icon;          // a scene with an XuiImage "icon", drawn centred on a tile
    TCHAR* m_cover;         // a scene with an XuiImage "image": a game's own cover art on its tile
    TCHAR* m_icons;         // the tiles' icon images, ';' separated, game 0 first
    TCHAR* m_rgImage[MAX_BELT_GAMES];  // a game's own cover image (SetGameImage), over m_icons
    int m_start;            // the game focused when the belt first opens
    D3DXVECTOR3 m_centre;   // the belt ring's axis, at the regular covers' height
    float m_radius;         // the regular covers' ring radius in the world
    float m_coverScale;     // every cover grown about its own centre
    float m_focusPull;      // the focused slot pulled this far toward the camera
    int m_skip;             // empty slots either side of the focus
    float m_stepDuration;   // 20 frames at 30 fps, speed 2.5
    float m_fadeDuration;   // the covers fade with Frolick's camera flight
    int m_count;

    void Advance(float nSeconds);
    void Render();
    bool OnSetProperty(const PRD* pprd, const void* pvValue);

    void Open();
    void Close();
    void Step(int nDir);
    int GetSelected();
    int IsBusy();
    void SetTitle(const TCHAR* szText);
    void SetGameImage(int game, const TCHAR* szPath);

    DECLARE_NODE_PROPS()
    DECLARE_NODE_FUNCTIONS()

protected:
    void Load();
    bool LoadData();
    void Pose(int node, float frame, D3DXMATRIX* pm);
    void CoverGrid(float pos, float pull, D3DXVECTOR3 grid[6][7]);
    float SlotOf(float k);
    void DrawScene(xui::Scene* pScene, float alpha, float rx, float ry, float rw, float rh);

    BELTHDR m_hdr;
    float m_circleX, m_circleZ, m_circleR, m_coverY;
    float m_slotAngle;          // the neighbour slots' angle from the focused one
    BELTVERTEX* m_verts;        // nNodes * nVerts, in each bone's space
    unsigned short* m_indices;  // nNodes * nIdx
    float* m_poses;             // nNodes * nFrames * 16
    bool m_bDirty;

    CXuiResources m_res;
    CXuiBackend m_be;
    xui::Renderer* m_pRenderer;
    xui::Element* m_pCardRoot;
    xui::Element* m_pFocusRoot;
    xui::Element* m_pSkinRoot;
    xui::Element* m_pIconRoot;
    xui::Element* m_pCoverRoot;
    xui::Scene* m_pCard;
    xui::Scene* m_pFocus;
    xui::Scene* m_pIcon;
    xui::Scene* m_pCover;
    bool m_bFontLoaded;
    bool m_bStarted;
    bool IconFor(int game, TCHAR* sz, int len);
    bool CoverFor(int game, TCHAR* sz, int len);

    int m_selected;
    bool m_open;
    int m_fadeDir;          // +1 opening, -1 closing, 0 still
    float m_fade;           // 0 at the hub, 1 in the belt
    int m_stepDir;          // 0 still, -1 Belt_Right (left pressed), +1 Belt_Left
    XTIME m_stepStart;
    bool m_bStepDone;
};

IMPLEMENT_NODE("Belt", CBelt, CNode)

START_NODE_PROPS(CBelt, CNode)
    NODE_PROP(pt_string, CBelt, url)
    NODE_PROP(pt_string, CBelt, card)
    NODE_PROP(pt_string, CBelt, focus)
    NODE_PROP(pt_string, CBelt, skin)
    NODE_PROP(pt_string, CBelt, font)
    NODE_PROP(pt_string, CBelt, icon)
    NODE_PROP(pt_string, CBelt, cover)
    NODE_PROP(pt_string, CBelt, icons)
    NODE_PROP(pt_integer, CBelt, start)
    NODE_PROP(pt_vec3, CBelt, centre)
    NODE_PROP(pt_number, CBelt, radius)
    NODE_PROP(pt_number, CBelt, coverScale)
    NODE_PROP(pt_number, CBelt, focusPull)
    NODE_PROP(pt_integer, CBelt, skip)
    NODE_PROP(pt_number, CBelt, stepDuration)
    NODE_PROP(pt_number, CBelt, fadeDuration)
    NODE_PROP(pt_integer, CBelt, count)
END_NODE_PROPS()

START_NODE_FUN(CBelt, CNode)
    NODE_FUN_VV(Open)
    NODE_FUN_VV(Close)
    NODE_FUN_VI(Step)
    NODE_FUN_IV(GetSelected)
    NODE_FUN_IV(IsBusy)
    NODE_FUN_VS(SetTitle)
    NODE_FUN_VIS(SetGameImage)
END_NODE_FUN()

CBelt::CBelt() :
    m_url(NULL), m_card(NULL), m_focus(NULL), m_skin(NULL), m_font(NULL), m_icon(NULL), m_cover(NULL), m_icons(NULL), m_start(0),
    m_centre(0.0f, 0.0f, 0.0f), m_radius(1.0f), m_coverScale(1.0f), m_focusPull(0.0f), m_skip(0),
    m_stepDuration(20.0f / 30.0f / 2.5f), m_fadeDuration(0.75f), m_count(0),
    m_circleX(0.0f), m_circleZ(0.0f), m_circleR(1.0f), m_coverY(0.0f), m_slotAngle(0.5f),
    m_verts(NULL), m_indices(NULL), m_poses(NULL), m_bDirty(true),
    m_pRenderer(NULL), m_pCardRoot(NULL), m_pFocusRoot(NULL), m_pSkinRoot(NULL), m_pIconRoot(NULL), m_pCoverRoot(NULL),
    m_pCard(NULL), m_pFocus(NULL), m_pIcon(NULL), m_pCover(NULL), m_bFontLoaded(false), m_bStarted(false),
    m_selected(0), m_open(false), m_fadeDir(0), m_fade(0.0f), m_stepDir(0), m_stepStart(0.0), m_bStepDone(false)
{
    ZeroMemory(&m_hdr, sizeof m_hdr);
    ZeroMemory(m_rgImage, sizeof m_rgImage);
}

CBelt::~CBelt()
{
    for (int i = 0; i < MAX_BELT_GAMES; i++)
        delete [] m_rgImage[i];
    delete m_pCard;
    delete m_pFocus;
    delete m_pIcon;
    delete m_pCover;
    delete m_pCardRoot;
    delete m_pFocusRoot;
    delete m_pSkinRoot;
    delete m_pIconRoot;
    delete m_pCoverRoot;
    delete m_pRenderer;
    delete [] m_verts;
    delete [] m_indices;
    delete [] m_poses;
    delete [] m_url;
    delete [] m_card;
    delete [] m_focus;
    delete [] m_skin;
    delete [] m_font;
    delete [] m_icon;
    delete [] m_cover;
    delete [] m_icons;
}

bool CBelt::OnSetProperty(const PRD* pprd, const void* pvValue)
{
    if (pprd->nType == pt_string)
        m_bDirty = true;
    return true;
}

bool CBelt::LoadData()
{
    delete [] m_verts; m_verts = NULL;
    delete [] m_indices; m_indices = NULL;
    delete [] m_poses; m_poses = NULL;
    if (m_url == NULL)
        return false;
    char sz[MAX_PATH];
    ToAnsi(sz, m_url, MAX_PATH);
    unsigned char* data = NULL;
    int len = 0;
    if (!FrolickReadFile(sz, &data, &len) || len < (int)sizeof (BELTHDR) + 16 || memcmp(data, "BLT3", 4) != 0)
    {
        TRACE(_T("Belt: cannot read %s\n"), m_url);
        delete [] data;
        return false;
    }
    CopyMemory(&m_hdr, data, sizeof m_hdr);
    const float* f = (const float*)(data + sizeof m_hdr);
    m_circleX = f[0]; m_circleZ = f[1]; m_circleR = f[2]; m_coverY = f[3];
    const unsigned char* b = (const unsigned char*)(f + 4);
    int nv = m_hdr.nNodes * m_hdr.nVerts, ni = m_hdr.nNodes * m_hdr.nIdx, np = m_hdr.nNodes * m_hdr.nFrames * 16;
    int need = (int)sizeof (BELTHDR) + 16 + m_hdr.nNodes * (m_hdr.nVerts * (int)sizeof (BELTVERTEX)
        + m_hdr.nIdx * (int)sizeof (unsigned short) + m_hdr.nFrames * 16 * (int)sizeof (float));
    if (m_hdr.nNodes <= 0 || m_hdr.nFrames < 2 || len < need)
    {
        TRACE(_T("Belt: %s is short\n"), m_url);
        delete [] data;
        return false;
    }
    m_verts = new BELTVERTEX[nv];
    m_indices = new unsigned short[ni];
    m_poses = new float[np];
    for (int n = 0; n < m_hdr.nNodes; n++)
    {
        CopyMemory(m_verts + n * m_hdr.nVerts, b, m_hdr.nVerts * sizeof (BELTVERTEX));
        b += m_hdr.nVerts * sizeof (BELTVERTEX);
        CopyMemory(m_indices + n * m_hdr.nIdx, b, m_hdr.nIdx * sizeof (unsigned short));
        b += m_hdr.nIdx * sizeof (unsigned short);
        CopyMemory(m_poses + n * m_hdr.nFrames * 16, b, m_hdr.nFrames * 16 * sizeof (float));
        b += m_hdr.nFrames * 16 * sizeof (float);
    }
    delete [] data;

    // the neighbour slot's angle round the ring from the focused slot
    // (GameTap's -x), from cover 5 at frame 0
    if (m_hdr.nNodes > 5)
    {
        const float* w = m_poses + 5 * m_hdr.nFrames * 16;
        const BELTVERTEX* v = m_verts + 5 * m_hdr.nVerts;
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        for (int i = 0; i < m_hdr.nVerts; i++) { cx += v[i].x; cy += v[i].y; cz += v[i].z; }
        cx /= m_hdr.nVerts; cy /= m_hdr.nVerts; cz /= m_hdr.nVerts;
        float qx = w[0] * cx + w[4] * cy + w[8] * cz + w[12];
        float qz = w[2] * cx + w[6] * cy + w[10] * cz + w[14];
        m_slotAngle = (float)fabs(atan2(qz - m_circleZ, -(qx - m_circleX)));
        if (m_slotAngle < 0.05f) m_slotAngle = 0.5f;
    }
    return true;
}

void CBelt::Load()
{
    m_bDirty = false;
    delete m_pCard; m_pCard = NULL;
    delete m_pFocus; m_pFocus = NULL;
    delete m_pIcon; m_pIcon = NULL;
    delete m_pCover; m_pCover = NULL;
    delete m_pCardRoot; m_pCardRoot = NULL;
    delete m_pFocusRoot; m_pFocusRoot = NULL;
    delete m_pSkinRoot; m_pSkinRoot = NULL;
    delete m_pIconRoot; m_pIconRoot = NULL;
    delete m_pCoverRoot; m_pCoverRoot = NULL;
    if (m_pRenderer == NULL)
        m_pRenderer = new xui::Renderer(&m_be, &m_res);
    if (!LoadData())
        return;

    char sz[MAX_PATH], err[256];
    if (m_card != NULL)
    {
        ToAnsi(sz, m_card, MAX_PATH);
        m_res.SetBaseFrom(sz);
        m_pCardRoot = m_res.scene(sz, err, sizeof err);
        if (m_pCardRoot == NULL)
            TRACE(_T("Belt: %s: %S\n"), m_card, err);
    }
    if (m_focus != NULL)
    {
        ToAnsi(sz, m_focus, MAX_PATH);
        m_pFocusRoot = m_res.scene(sz, err, sizeof err);
        if (m_pFocusRoot == NULL)
            TRACE(_T("Belt: %s: %S\n"), m_focus, err);
    }
    if (m_skin != NULL && m_skin[0])
    {
        ToAnsi(sz, m_skin, MAX_PATH);
        m_pSkinRoot = m_res.scene(sz, err, sizeof err);
    }
    if (m_icon != NULL && m_icon[0])
    {
        ToAnsi(sz, m_icon, MAX_PATH);
        m_pIconRoot = m_res.scene(sz, err, sizeof err);
        if (m_pIconRoot == NULL)
            TRACE(_T("Belt: %s: %S\n"), m_icon, err);
    }
    if (m_cover != NULL && m_cover[0])
    {
        ToAnsi(sz, m_cover, MAX_PATH);
        m_pCoverRoot = m_res.scene(sz, err, sizeof err);
        if (m_pCoverRoot == NULL)
            TRACE(_T("Belt: %s: %S\n"), m_cover, err);
    }
    if (!m_bFontLoaded)
    {
        ToAnsi(sz, m_font != NULL && m_font[0] ? m_font : _T("font.ttf"), MAX_PATH);
        m_pRenderer->setFont(sz);
        m_bFontLoaded = true;
    }
    if (m_pCardRoot != NULL)
        m_pCard = new xui::Scene(m_pCardRoot, m_pSkinRoot);
    if (m_pFocusRoot != NULL)
        m_pFocus = new xui::Scene(m_pFocusRoot, m_pSkinRoot);
    if (m_pIconRoot != NULL)
        m_pIcon = new xui::Scene(m_pIconRoot, m_pSkinRoot);
    if (m_pCoverRoot != NULL)
        m_pCover = new xui::Scene(m_pCoverRoot, m_pSkinRoot);
}

// A game's own cover image (SetGameImage), empty for none.
bool CBelt::CoverFor(int game, TCHAR* sz, int len)
{
    if (game < 0 || game >= MAX_BELT_GAMES || m_rgImage[game] == NULL || m_rgImage[game][0] == 0)
        return false;
    lstrcpyn(sz, m_rgImage[game], len);
    return true;
}

// The icon image of a game's tile: entry game of the ';' separated icons
// list, empty for none.
bool CBelt::IconFor(int game, TCHAR* sz, int len)
{
    if (m_icons == NULL || game < 0)
        return false;
    const TCHAR* p = m_icons;
    for (int i = 0; i < game; i++)
    {
        while (*p && *p != ';') p++;
        if (*p == 0) return false;
        p++;
    }
    int n = 0;
    while (*p && *p != ';' && n < len - 1)
        sz[n++] = *p++;
    sz[n] = 0;
    return n > 0;
}

// scroll_belt_right / scroll_belt_left for one step: nothing while the lock
// holds.  nDir +1 selects the next game (Belt_Left: covers slide toward the
// focus from the right), -1 the previous one (Belt_Right).  GameTap's
// per-step sounds (gameBeltRightEvent_N) have nothing to play yet.
void CBelt::Step(int nDir)
{
    if (!m_open || m_stepDir != 0 || m_fadeDir != 0 || nDir == 0)
        return;
    int n = m_selected + (nDir > 0 ? 1 : -1);
    if (n < 0 || n >= m_count)
        return;
    m_stepDir = nDir > 0 ? 1 : -1;
    m_stepStart = XAppGetNow();
}

void CBelt::Open()
{
    if (m_bDirty)
        Load();
    if (!m_bStarted)
    {
        m_bStarted = true;
        if (m_start >= 0)
            m_selected = m_start;
    }
    // the list may have shrunk since (a disc out, a game deleted)
    if (m_selected >= m_count)
        m_selected = m_count - 1;
    if (m_selected < 0)
        m_selected = 0;
    m_open = true;
    m_fadeDir = 1;
}

void CBelt::Close()
{
    if (!m_open)
        return;
    m_fadeDir = -1;
    m_stepDir = 0;
}

int CBelt::GetSelected() { return m_selected; }
int CBelt::IsBusy() { return m_fadeDir != 0 || m_stepDir != 0; }

// A game's cover image (its title image out of its XBE, T:\GameImages);
// "" goes back to the icons list.
void CBelt::SetGameImage(int game, const TCHAR* szPath)
{
    if (game < 0 || game >= MAX_BELT_GAMES)
        return;
    delete [] m_rgImage[game];
    m_rgImage[game] = NULL;
    if (szPath != NULL && szPath[0])
    {
        m_rgImage[game] = new TCHAR [_tcslen(szPath) + 1];
        _tcscpy(m_rgImage[game], szPath);
    }
}

void CBelt::SetTitle(const TCHAR* szText)
{
    if (m_bDirty)
        Load();
    if (m_pFocus == NULL)
        return;
    xui::Node* n = m_pFocus->find("title");
    if (n != NULL)
        SetString(n, "Text", szText);
}

void CBelt::Advance(float nSeconds)
{
    if (m_bDirty)
        Load();
    if (m_fadeDir != 0)
    {
        m_fade += m_fadeDir * nSeconds / (m_fadeDuration > 0.0f ? m_fadeDuration : 0.001f);
        if (m_fade >= 1.0f) { m_fade = 1.0f; m_fadeDir = 0; }
        if (m_fade <= 0.0f) { m_fade = 0.0f; m_fadeDir = 0; m_open = false; }
    }
    if (m_stepDir != 0 && XAppGetNow() - m_stepStart >= m_stepDuration)
    {
        m_selected += m_stepDir;
        m_stepDir = 0;
        m_bStepDone = true;
    }
    if (m_pFocus != NULL && m_open)
        m_pFocus->update(nSeconds);
    if (m_bStepDone)
    {
        m_bStepDone = false;
        CallFunction(this, _T("OnStep"));
    }
}

// A cover's world matrix at a fractional frame of the clip, lerped between
// the two samples around it (the samples are one frame apart).
void CBelt::Pose(int node, float frame, D3DXMATRIX* pm)
{
    int last = m_hdr.nFrames - 1;
    if (frame < 0.0f) frame = 0.0f;
    if (frame > last) frame = (float)last;
    int i = (int)floor(frame);
    if (i >= last) i = last - 1;
    float t = frame - i;
    const float* a = m_poses + (node * m_hdr.nFrames + i) * 16;
    const float* b = a + 16;
    float* o = (float*)pm;
    for (int k = 0; k < 16; k++)
        o[k] = a[k] + (b[k] - a[k]) * t;
}

// The slot a game sits on by its place k from the focused one (0): the
// skipped slots either side of the focus are jumped, and in between (a
// game on its way into or out of the focus) the slot runs linearly, so the
// crossing takes the same share of a step as any other move.
float CBelt::SlotOf(float k)
{
    float a = (float)fabs(k);
    float slot = a < 1.0f ? a * (1 + m_skip) : a + m_skip;
    return 6.0f + (k < 0.0f ? -slot : slot);
}

// The cover's 7x6 vertex grid in Frolick's world for a cover at slot
// position pos (slot n at n, on its way to n+1 at n + f: cover n's mesh at
// its frame f of the clip, which is how GameTap moves a cover between
// those two slots), row 0 at the top (the mesh lists vertex col*6+row with
// row 0 at the bottom).  pull is how much of focusPull applies.  GameTap's
// world into Frolick's: the regular covers' circle onto the ring, -x to
// -z, y up, -z to +x.
void CBelt::CoverGrid(float pos, float pull, D3DXVECTOR3 grid[6][7])
{
    int last = m_hdr.nNodes - 1;
    if (pos < 0.0f) pos = 0.0f;
    if (pos > last) pos = (float)last;
    int node = (int)pos;
    if (node >= last) node = last;
    float frame = (pos - node) * (m_hdr.nFrames - 1);
    D3DXMATRIX m;
    Pose(node, frame, &m);
    float scale = m_radius / m_circleR;
    const BELTVERTEX* v = m_verts + node * m_hdr.nVerts;
    D3DXVECTOR3 c(0.0f, 0.0f, 0.0f);
    int i;
    for (i = 0; i < 42; i++)
        c += D3DXVECTOR3(v[i].x, v[i].y, v[i].z);
    c /= 42.0f;

    // the pull off the ring toward the camera
    D3DXVECTOR3 off(0.0f, 0.0f, 0.0f);
    if (m_focusPull != 0.0f && pull > 0.0f)
    {
        D3DXVECTOR3 qc;
        D3DXVec3TransformCoord(&qc, &c, &m);
        D3DXVECTOR3 wc(m_centre.x - (qc.z - m_circleZ) * scale, 0.0f, m_centre.z + (qc.x - m_circleX) * scale);
        D3DXVECTOR3 eye = theCamera.m_position;
        off = D3DXVECTOR3(eye.x - wc.x, 0.0f, -eye.z - wc.z);
        D3DXVec3Normalize(&off, &off);
        off *= m_focusPull * pull;
    }
    for (int col = 0; col < 7; col++)
        for (int row = 0; row < 6; row++)
        {
            const BELTVERTEX& l = v[col * 6 + row];
            D3DXVECTOR3 p = c + (D3DXVECTOR3(l.x, l.y, l.z) - c) * m_coverScale, q;
            D3DXVec3TransformCoord(&q, &p, &m);
            grid[5 - row][col] = D3DXVECTOR3(
                m_centre.x - (q.z - m_circleZ) * scale,
                m_centre.y + (q.y - m_coverY) * scale,
                m_centre.z + (q.x - m_circleX) * scale) + off;
        }
}

// The scene's rectangle (rx, ry, rw by rh) lands on the cover in m_be.m_grid:
// as wide as the cover, keeping its own aspect, centred on the cover's
// height.  The rest of the canvas rides along (the title under the focus).
void CBelt::DrawScene(xui::Scene* pScene, float alpha, float rx, float ry, float rw, float rh)
{
    D3DXVECTOR3 w = m_be.m_grid[3][6] - m_be.m_grid[3][0], h = m_be.m_grid[5][3] - m_be.m_grid[0][3];
    float coverH = rw * D3DXVec3Length(&h) / D3DXVec3Length(&w);   // the cover's height in canvas units
    m_be.m_gridMode = true;
    m_be.m_gx0 = rx; m_be.m_gy0 = ry - 0.5f * (coverH - rh);
    m_be.m_gxs = 1.0f / rw;
    m_be.m_gys = 1.0f / coverH;
    m_be.m_alpha = alpha;
    m_pRenderer->draw(pScene, 0, 0, 1);
    m_be.m_gridMode = false;
}

void CBelt::Render()
{
    if (m_bDirty)
        Load();
    if (!m_open || m_verts == NULL || m_pCard == NULL || !m_visible)
        return;
    float alpha = m_fade * g_nEffectAlpha;
    if (alpha <= 0.01f)
        return;

    // the clip's frame, and which game each cover carries
    float u = 0.0f;
    if (m_stepDir != 0)
    {
        u = (float)(XAppGetNow() - m_stepStart) / m_stepDuration;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
    }
    // Each game sits by its place k from the selected one; a step moves
    // every place by one (Belt_Right, left pressed: the belt slides right,
    // places grow).  Its slot position follows SlotOf, so a game crosses
    // the skipped slots within the step.
    int lastSlot = m_hdr.nNodes - 1;
    int games[32], n = 0;
    float places[32], depth[32];
    int order[32];
    int i, j, k;
    D3DXVECTOR3 eye = theCamera.m_position;
    eye.z = -eye.z;
    D3DXVECTOR3 grid[6][7];
    for (k = -lastSlot; k <= lastSlot && n < 32; k++)
    {
        int game = m_selected + k;
        if (game < 0 || game >= m_count)
            continue;
        float x = (float)k + (m_stepDir < 0 ? u : m_stepDir > 0 ? -u : 0.0f);
        float pos = SlotOf(x);
        if (pos < 0.0f || pos > lastSlot)
            continue;
        float w = 1.0f - (float)fabs(x);
        CoverGrid(pos, w > 0.0f ? w : 0.0f, grid);
        D3DXVECTOR3 c = grid[3][3] - eye;
        games[n] = game;
        places[n] = x;
        depth[n] = D3DXVec3Length(&c);
        order[n] = n;
        n++;
    }
    // back to front: the art is blended, so the farthest cover first
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && depth[order[j]] > depth[order[j - 1]]; j--)
        {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }

    DWORD dwLighting;
    XAppGetRenderState(D3DRS_LIGHTING, &dwLighting);
    XAppSetRenderState(D3DRS_LIGHTING, FALSE);
    XAppSetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    XAppSetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
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
    D3DXMATRIX identity;
    D3DXMatrixIdentity(&identity);
    XAppSetTransform(D3DTS_WORLD, &identity);

    float cw = m_pCard->root->width(), ch = m_pCard->root->height();
    for (i = 0; i < n; i++)
    {
        int e = order[i];
        int game = games[e];
        float x = places[e];
        float w = 1.0f - (float)fabs(x);
        CoverGrid(SlotOf(x), w > 0.0f ? w : 0.0f, m_be.m_grid);
        DrawScene(m_pCard, alpha, 0.0f, 0.0f, cw > 0 ? cw : 1.0f, ch > 0 ? ch : 1.0f);

        // a game's cover art across the card, else a reserved tile's
        // icon, centred on it
        TCHAR szIcon[MAX_PATH];
        if (m_pCover != NULL && CoverFor(game, szIcon, MAX_PATH))
        {
            SetString(m_pCover->find("image"), "ImagePath", szIcon);
            DrawScene(m_pCover, alpha, 0.0f, 0.0f, cw > 0 ? cw : 1.0f, ch > 0 ? ch : 1.0f);
        }
        else if (m_pIcon != NULL && IconFor(game, szIcon, MAX_PATH))
        {
            SetString(m_pIcon->find("icon"), "ImagePath", szIcon);
            DrawScene(m_pIcon, alpha, 0.0f, 0.0f, cw > 0 ? cw : 1.0f, ch > 0 ? ch : 1.0f);
        }

        // the focus overlay rides the cover in the focus slot: its card
        // rectangle (192,40 640x440 of 1024x768) is the cover
        float fa = 1.0f - 2.0f * (float)fabs(x);
        if (m_pFocus != NULL && fa > 0.01f)
            DrawScene(m_pFocus, alpha * fa, 192.0f, 40.0f, 640.0f, 440.0f);
    }

    XAppSetTexture(0, NULL);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    XAppSetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    XAppSetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
    XAppSetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    XAppSetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    XAppSetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    XAppSetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    XAppSetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    XAppSetRenderState(D3DRS_LIGHTING, dwLighting);
}
