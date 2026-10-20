// Renderer: a port of frolick-xui's draw.go and text.go.  Figures are
// flattened and ear-clipped, fills become solid colours, gradient ramp
// textures or images, and text is rasterised one line at a time with
// stb_truetype into a white alpha texture that the diffuse colour tints.
#include "xui.h"
#include <math.h>
#include <stdio.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../vendor/stb_truetype.h"

namespace xui {

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float fmin2(float a, float b) { return a < b ? a : b; }
static float fmax2(float a, float b) { return a > b ? a : b; }

// ---- resources -------------------------------------------------------------

// .xtx: "XTX1", uint32 width, uint32 height, then width*height*4 bytes BGRA
// (tools/mkassets.py converts PNGs at build time; no decoder on the Xbox).
Image* Resources::image(const char* path)
{
    unsigned char* data; int len;
    if (!read(path, &data, &len)) return 0;
    Image* img = 0;
    if (len >= 12 && memcmp(data, "XTX1", 4) == 0) {
        unsigned int w = *(unsigned int*)(data + 4), h = *(unsigned int*)(data + 8);
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096 && len >= 12 + (int)(w * h * 4)) {
            img = new Image;
            img->w = (int)w; img->h = (int)h;
            img->pix = new unsigned char[w * h * 4];
            memcpy(img->pix, data + 12, w * h * 4);
        }
    }
    delete [] data;
    return img;
}

Element* Resources::scene(const char* path, char* err, int errlen)
{
    unsigned char* data; int len;
    if (!read(path, &data, &len)) {
        if (err) _snprintf(err, errlen, "%s: not found", path);
        return 0;
    }
    Element* e = ParseXUI((const char*)data, len, err, errlen);
    delete [] data;
    return e;
}

int utf8decode(const char* s, int* adv)
{
    const unsigned char* p = (const unsigned char*)s;
    int c = p[0], n = 1;
    if (c >= 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        c = ((c & 7) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f); n = 4;
    } else if (c >= 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        c = ((c & 15) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); n = 3;
    } else if (c >= 0xc0 && (p[1] & 0xc0) == 0x80) {
        c = ((c & 31) << 6) | (p[1] & 0x3f); n = 2;
    }
    *adv = n;
    return c;
}

// ---- fonts -----------------------------------------------------------------

struct Font {
    unsigned char* data;
    stbtt_fontinfo info;
    int ascent, descent, lineGap;
    bool ok;
    Font() : data(0), ascent(0), descent(0), lineGap(0), ok(false) {}
    ~Font() { delete [] data; }
};

Renderer::Renderer(Backend* b, Resources* r) : be(b), res(r), font(0), clipping(false) {}

Renderer::~Renderer()
{
    for (int i = 0; i < texes.n; i++) delete [] texes[i].key;
    for (int i = 0; i < fonts.n; i++) { delete [] fonts[i].name; delete fonts[i].f; }
    delete font;
}

static Font* loadFont(Resources* res, const char* path)
{
    unsigned char* data; int len;
    if (!res->read(path, &data, &len)) return 0;
    Font* f = new Font;
    f->data = data;
    if (stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->lineGap);
        f->ok = true;
    }
    return f;
}

void Renderer::setFont(const char* path)
{
    delete font;
    font = loadFont(res, path);
}

// fontFor maps a text element's Font name to a file: "Fira Sans" is
// FiraSans.ttf beside the scene or at the root.  A name with no file (the
// dashboard's Convection, say) draws in the default font, as before.
Font* Renderer::fontFor(const char* name)
{
    if (!name || !name[0]) return font;
    for (int i = 0; i < fonts.n; i++)
        if (strcmp(fonts[i].name, name) == 0) return fonts[i].f ? fonts[i].f : font;
    char file[256]; int n = 0;
    for (const char* p = name; *p && n < (int)sizeof file - 8; p++)
        if (*p != ' ') file[n++] = *p;
    strcpy(file + n, ".ttf");
    FontEntry e;
    e.name = strdup2(name);
    e.f = loadFont(res, file);
    if (e.f && !e.f->ok) { delete e.f; e.f = 0; }
    fonts.push(e);
    return e.f ? e.f : font;
}

// ---- texture cache ---------------------------------------------------------

Texture Renderer::lookup(const char* key, int* w, int* h)
{
    for (int i = 0; i < texes.n; i++)
        if (strcmp(texes[i].key, key) == 0) {
            if (w) *w = texes[i].w;
            if (h) *h = texes[i].h;
            return texes[i].tex;
        }
    return (Texture)-1;
}

void Renderer::remember(const char* key, Texture t, int w, int h)
{
    TexEntry e; e.key = strdup2(key); e.tex = t; e.w = w; e.h = h;
    texes.push(e);
}

// ---- affine ----------------------------------------------------------------

static Affine aff(float a, float b, float c, float d, float tx, float ty)
{
    Affine m; m.a = a; m.b = b; m.c = c; m.d = d; m.tx = tx; m.ty = ty; return m;
}

static Affine mul(const Affine& m, const Affine& n) // m * n (apply n first)
{
    return aff(m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b,
               m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
               m.a * n.tx + m.c * n.ty + m.tx, m.b * n.tx + m.d * n.ty + m.ty);
}

static void apply(const Affine& m, float x, float y, float* ox, float* oy)
{
    *ox = m.a * x + m.c * y + m.tx;
    *oy = m.b * x + m.d * y + m.ty;
}

static void rgba(Color c, float* r, float* g, float* b, float* a)
{
    *a = ((c >> 24) & 255) / 255.0f;
    *r = ((c >> 16) & 255) / 255.0f;
    *g = ((c >> 8) & 255) / 255.0f;
    *b = (c & 255) / 255.0f;
}

// ---- scene walk ------------------------------------------------------------

void Renderer::draw(Scene* s, float x, float y, float scale)
{
    node(s->root, aff(scale, 0, 0, scale, x, y), 1, BlendNormal);
    if (clipping) { clipping = false; be->clip(0); }
}

void Renderer::node(Node* n, const Affine& parent, float alpha, Blend blend)
{
    if (!propB(n->props, "Show", true)) return;
    alpha *= propF(n->props, "Opacity", 1);
    if (alpha <= 0.001f) return;
    switch (propU(n->props, "BlendMode", 0)) {
    case 3: blend = BlendAdd; break;
    case 2: blend = BlendMultiply; break;
    }
    Vec3 zero = { 0, 0, 0 }, one = { 1, 1, 1 };
    Vec3 pos = propV(n->props, "Position", zero);
    Vec3 sc = propV(n->props, "Scale", one);
    Vec3 pv = propV(n->props, "Pivot", zero);
    Affine m = mul(parent, aff(1, 0, 0, 1, pos.x + pv.x, pos.y + pv.y));
    Value* rot = n->props.get("Rotation");
    if (rot && rot->type == TQuat && rot->q.z != 0) {
        double ang = 2 * atan2((double)rot->q.z, (double)rot->q.w);
        float cs = (float)cos(ang), sn = (float)sin(ang);
        m = mul(m, aff(cs, sn, -sn, cs, 0, 0));
    }
    m = mul(mul(m, aff(sc.x, 0, 0, sc.y, 0, 0)), aff(1, 0, 0, 1, -pv.x, -pv.y));

    const char* cls = n->cls ? n->cls->name : "";
    if (strcmp(cls, "XuiFigure") == 0) figure(n, m, alpha, blend);
    else if (strcmp(cls, "XuiImage") == 0 || strcmp(cls, "XuiImagePresenter") == 0) image(n, m, alpha, blend);
    else if (strcmp(cls, "XuiText") == 0 || strcmp(cls, "XuiTextPresenter") == 0) text(n, m, alpha, blend);

    bool savedClipping = clipping; float saved[4];
    memcpy(saved, clipBuf, sizeof saved);
    if (propB(n->props, "ClipChildren", false)) {
        float x0, y0, x1, y1;
        apply(m, 0, 0, &x0, &y0);
        apply(m, n->width(), n->height(), &x1, &y1);
        float c[4] = { fmin2(x0, x1), fmin2(y0, y1), fmax2(x0, x1), fmax2(y0, y1) };
        if (savedClipping) {
            c[0] = fmax2(c[0], saved[0]); c[1] = fmax2(c[1], saved[1]);
            c[2] = fmin2(c[2], saved[2]); c[3] = fmin2(c[3], saved[3]);
        }
        memcpy(clipBuf, c, sizeof c);
        clipping = true;
        be->clip(clipBuf);
    }
    if (n->visual) node(n->visual, m, alpha, blend);
    for (int i = 0; i < n->children.n; i++) node(n->children[i], m, alpha, blend);
    if (clipping != savedClipping || memcmp(saved, clipBuf, sizeof saved) != 0) {
        clipping = savedClipping;
        memcpy(clipBuf, saved, sizeof saved);
        be->clip(clipping ? clipBuf : 0);
    }
}

// ---- figures ---------------------------------------------------------------

// flatten turns XUI Bezier points into a polyline of x,y pairs.
static void flatten(Points* pts, bool closed, Vec<float>& out)
{
    int n = pts->pts.n;
    int segs = closed ? n : n - 1;
    out.push(pts->pts[0].p[0]); out.push(pts->pts[0].p[1]);
    for (int i = 0; i < segs; i++) {
        Point& a = pts->pts[i];
        Point& b = pts->pts[(i + 1) % n];
        const float* p0 = a.p; const float* c1 = a.c1; const float* c2 = a.c2; const float* p1 = b.p;
        bool c1p0 = c1[0] == p0[0] && c1[1] == p0[1];
        bool c2p1 = c2[0] == p1[0] && c2[1] == p1[1];
        bool c2p0 = c2[0] == p0[0] && c2[1] == p0[1];
        if (c1p0 && (c2p1 || c2p0)) { out.push(p1[0]); out.push(p1[1]); continue; }
        for (int k = 1; k <= 10; k++) {
            float t = k / 10.0f, u = 1 - t;
            float x = u * u * u * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t * t * t * p1[0];
            float y = u * u * u * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t * t * t * p1[1];
            out.push(x); out.push(y);
        }
    }
    if (closed && out.n > 2 && out[0] == out[out.n - 2] && out[1] == out[out.n - 1]) out.n -= 2;
    // drop consecutive duplicates
    int w = 2;
    for (int i = 2; i < out.n; i += 2)
        if (out[i] != out[w - 2] || out[i + 1] != out[w - 1]) { out[w] = out[i]; out[w + 1] = out[i + 1]; w += 2; }
    out.n = w;
}

static float cross(const float* a, const float* b, const float* c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

static bool inTri(const float* p, const float* a, const float* b, const float* c)
{
    return cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0;
}

// triangulate ear-clips a simple polygon (x,y pairs).
static void triangulate(const float* poly, int n, Vec<unsigned short>& out)
{
    int* idx = new int[n];
    float area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += poly[i * 2] * poly[j * 2 + 1] - poly[j * 2] * poly[i * 2 + 1];
        idx[i] = i;
    }
    if (area < 0)
        for (int lo = 0, hi = n - 1; lo < hi; lo++, hi--) { int t = idx[lo]; idx[lo] = idx[hi]; idx[hi] = t; }
    int left = n;
    for (int guard = 0; left > 3 && guard < n * n; guard++) {
        bool found = false;
        for (int i = 0; i < left; i++) {
            int ia = idx[(i + left - 1) % left], ib = idx[i], ic = idx[(i + 1) % left];
            const float* a = poly + ia * 2; const float* b = poly + ib * 2; const float* c = poly + ic * 2;
            if (cross(a, b, c) <= 0) continue;
            bool ear = true;
            for (int k = 0; k < left; k++) {
                int j = idx[k];
                if (j != ia && j != ib && j != ic && inTri(poly + j * 2, a, b, c)) { ear = false; break; }
            }
            if (!ear) continue;
            out.push((unsigned short)ia); out.push((unsigned short)ib); out.push((unsigned short)ic);
            for (int k = i; k + 1 < left; k++) idx[k] = idx[k + 1];
            left--;
            found = true;
            break;
        }
        if (!found) break;
    }
    if (left >= 3)
        for (int i = 1; i + 1 < left; i++) {
            out.push((unsigned short)idx[0]); out.push((unsigned short)idx[i]); out.push((unsigned short)idx[i + 1]);
        }
    delete [] idx;
}

// Brush transform for gradient and texture fills, as XuiTool writes it:
// Translation moves the brush, as a fraction of the figure's box (a y of
// 0.59 pulls a radial gradient's centre 0.59 boxes UP, so the Blades'
// button shadow is dark at its top edge, right under the button, and
// fades downward), Scale multiplies the box into brush space (a Scale of
// 0.4 stretches the gradient over 2.5 boxes: the Blades' pane edges are
// strips with a radial fill scaled 0.9,0.4, drawn by the 360 as a soft
// line the strip's whole height), Rotation turns about the centre.  All
// three checked against the GFWL XUI engine's rendering of the skin.
// The order is rotate, scale, then translate, so the Translation is not
// scaled: the GFWL Guide's buttons light their bottom edge with a radial
// fill translated 0,0.5 and scaled 1,-1, which the engine centres on the
// bottom edge (a mirrored scale before the move would put it on the top
// edge, a stray line inside the bar).  Rotation comes first: the Blades'
// Metapane rounds its top corners with 20x20 radial fills rotated 90 and
// translated 0.5,-0.5 (top left) and 0.5,0.5 (top right), whose centres
// land on the inner corner only this way.  A linear gradient runs along
// the box's width at Rotation 0 and from the box's bottom up at 90.
struct Brush {
    float tx, ty, sx, sy, cs, sn, pw, ph;
    void uv(float x, float y, float* u, float* v) const {
        float a = x / pw - 0.5f, b = y / ph - 0.5f;
        float a2 = a * cs - b * sn, b2 = a * sn + b * cs;
        *u = a2 * sx + tx + 0.5f; *v = b2 * sy + ty + 0.5f;
    }
};

static Brush brush(Object* fill, float pw, float ph)
{
    Brush b;
    Vec3 zero = { 0, 0, 0 }, one = { 1, 1, 1 };
    Vec3 t = propV(*fill, "Translation", zero);
    Vec3 s = propV(*fill, "Scale", one);
    double rot = propF(*fill, "Rotation", 0) * M_PI / 180;
    b.tx = t.x; b.ty = t.y;
    b.sx = s.x == 0 ? 1 : s.x; b.sy = s.y == 0 ? 1 : s.y;
    b.cs = (float)cos(rot); b.sn = (float)sin(rot);
    b.pw = pw; b.ph = ph;
    return b;
}

void Renderer::figure(Node* n, const Affine& m, float alpha, Blend blend)
{
    Value* pv = n->props.get("Points");
    Points* pts = pv && pv->type == TCustom ? pv->pts : 0;
    float w = n->width(), h = n->height();
    Vec<float> poly;
    float pw = w, ph = h, sx = 1, sy = 1;
    bool closed = propB(n->props, "Closed", true);
    if (pts && pts->pts.n > 0) {
        pw = pts->w; ph = pts->h;
        if (pw <= 0) pw = 1;
        if (ph <= 0) ph = 1;
        sx = w / pw; sy = h / ph;
        flatten(pts, closed, poly);
    } else {
        poly.push(0); poly.push(0); poly.push(w); poly.push(0);
        poly.push(w); poly.push(h); poly.push(0); poly.push(h);
        pw = w > 0 ? w : 1; ph = h > 0 ? h : 1;
    }
    int np = poly.n / 2;
    Affine fm = mul(m, aff(sx, 0, 0, sy, 0, 0));

    Value* fv = n->props.get("Fill");
    Object* fill = fv && fv->type == TObject ? fv->o : 0;
    unsigned int ftype = fill ? propU(*fill, "FillType", 1) : 0;
    if (ftype != 0 && np >= 3) {
        Vec<unsigned short> idx;
        triangulate(poly.p, np, idx);
        Texture tex = 0;
        Color col = 0xffffffff;
        bool useBrush = false;
        Brush br;
        switch (ftype) {
        case 1:
            col = propC(*fill, "FillColor", 0xff000000);
            break;
        case 2: case 3: {
            Value* gv = fill->get("Gradient");
            Object* g = gv && gv->type == TObject ? gv->o : 0;
            bool radial = ftype == 3 || (g && propB(*g, "Radial", false));
            tex = gradient(g, radial);
            br = brush(fill, pw, ph); useBrush = true;
            break;
        }
        case 4: {
            int tw, th;
            tex = imageTex(propS(*fill, "TextureFileName"), &tw, &th);
            br = brush(fill, pw, ph); useBrush = true;
            break;
        }
        }
        float cr, cg, cb, ca;
        rgba(col, &cr, &cg, &cb, &ca);
        Vertex* verts = new Vertex[np];
        for (int i = 0; i < np; i++) {
            float x = poly[i * 2], y = poly[i * 2 + 1];
            Vertex& v = verts[i];
            apply(fm, x, y, &v.x, &v.y);
            if (useBrush) br.uv(x, y, &v.u, &v.v); else { v.u = x / pw; v.v = y / ph; }
            v.r = cr; v.g = cg; v.b = cb; v.a = ca * alpha;
        }
        be->triangles(tex, blend, verts, np, idx.p, idx.n);
        delete [] verts;
    }
    Value* sv = n->props.get("Stroke");
    if (sv && sv->type == TObject) {
        float sw = propF(*sv->o, "StrokeWidth", 0);
        Color sc = propC(*sv->o, "StrokeColor", 0);
        if (sw > 0 && (sc >> 24) != 0 && np >= 2) stroke(poly.p, np, closed, fm, sw, sc, alpha, blend);
    }
}

void Renderer::stroke(const float* poly, int n, bool closed, const Affine& m, float w, Color c, float alpha, Blend blend)
{
    float cr, cg, cb, ca;
    rgba(c, &cr, &cg, &cb, &ca);
    ca *= alpha;
    int segs = closed ? n : n - 1;
    Vertex* verts = new Vertex[segs * 4];
    unsigned short* idx = new unsigned short[segs * 6];
    int nv = 0, ni = 0;
    for (int i = 0; i < segs; i++) {
        float ax, ay, bx, by;
        apply(m, poly[i * 2], poly[i * 2 + 1], &ax, &ay);
        apply(m, poly[((i + 1) % n) * 2], poly[((i + 1) % n) * 2 + 1], &bx, &by);
        float dx = bx - ax, dy = by - ay;
        float l = (float)sqrt(dx * dx + dy * dy);
        if (l == 0) continue;
        float nx = -dy / l * w / 2, ny = dx / l * w / 2;
        float ex = dx / l * w / 2, ey = dy / l * w / 2; // extend to cover joins
        float q[8] = { ax - ex + nx, ay - ey + ny, bx + ex + nx, by + ey + ny,
                       bx + ex - nx, by + ey - ny, ax - ex - nx, ay - ey - ny };
        int base = nv;
        for (int k = 0; k < 4; k++) {
            Vertex& v = verts[nv++];
            v.x = q[k * 2]; v.y = q[k * 2 + 1]; v.u = v.v = 0;
            v.r = cr; v.g = cg; v.b = cb; v.a = ca;
        }
        idx[ni++] = (unsigned short)base; idx[ni++] = (unsigned short)(base + 1); idx[ni++] = (unsigned short)(base + 2);
        idx[ni++] = (unsigned short)base; idx[ni++] = (unsigned short)(base + 2); idx[ni++] = (unsigned short)(base + 3);
    }
    if (nv > 0) be->triangles(0, blend, verts, nv, idx, ni);
    delete [] verts;
    delete [] idx;
}

// ---- gradients -------------------------------------------------------------

struct Stop { float pos; Color c; };

static void stopAt(const Stop* stops, int n, float t, unsigned char* bgra)
{
    Color c;
    if (t <= stops[0].pos) c = stops[0].c;
    else {
        c = stops[n - 1].c;
        for (int i = 1; i < n; i++)
            if (t <= stops[i].pos) {
                const Stop& a = stops[i - 1]; const Stop& b = stops[i];
                float f = b.pos > a.pos ? (t - a.pos) / (b.pos - a.pos) : 0;
                unsigned int out = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    float x = (float)((a.c >> sh) & 255), y = (float)((b.c >> sh) & 255);
                    out |= ((unsigned int)(x + (y - x) * f + 0.5f) & 255) << sh;
                }
                c = out;
                break;
            }
    }
    bgra[0] = (unsigned char)(c & 255); bgra[1] = (unsigned char)((c >> 8) & 255);
    bgra[2] = (unsigned char)((c >> 16) & 255); bgra[3] = (unsigned char)(c >> 24);
}

Texture Renderer::gradient(Object* g, bool radial)
{
    Stop stops[16]; int ns = 0;
    if (g) {
        Value* cols = g->get("StopColor");
        Value* poss = g->get("StopPos");
        int nc = cols && cols->type == TList ? cols->l->v.n : 0;
        for (int i = 0; i < nc && ns < 16; i++) {
            Stop s;
            Value& cv = cols->l->v[i];
            s.c = cv.type == TColor ? cv.c : 0;
            s.pos = nc > 1 ? (float)i / (nc - 1) : 0;
            if (poss && poss->type == TList && i < poss->l->v.n && poss->l->v[i].type == TFloat) s.pos = poss->l->v[i].f;
            stops[ns++] = s;
        }
    }
    if (ns == 0) {
        stops[0].pos = 0; stops[0].c = 0xffffffff; stops[1].pos = 1; stops[1].c = 0xff000000; ns = 2;
    }
    char key[512];
    int kl = _snprintf(key, sizeof key, radial ? "rad:" : "lin:");
    for (int i = 0; i < ns && kl < (int)sizeof key - 32; i++)
        kl += _snprintf(key + kl, sizeof key - kl, "%g/%x;", stops[i].pos, stops[i].c);
    key[sizeof key - 1] = 0;
    Texture t = lookup(key);
    if (t != (Texture)-1) return t;
    Image img;
    if (radial) {
        const int sz = 128;
        img.w = img.h = sz;
        img.pix = new unsigned char[sz * sz * 4];
        for (int y = 0; y < sz; y++)
            for (int x = 0; x < sz; x++) {
                float dx = (x + 0.5f) / sz * 2 - 1, dy = (y + 0.5f) / sz * 2 - 1;
                stopAt(stops, ns, (float)sqrt(dx * dx + dy * dy), img.pix + (y * sz + x) * 4);
            }
    } else {
        const int sz = 256;
        img.w = sz; img.h = 1;
        img.pix = new unsigned char[sz * 4];
        for (int x = 0; x < sz; x++) stopAt(stops, ns, (x + 0.5f) / sz, img.pix + x * 4);
    }
    t = be->upload(img);
    remember(key, t, img.w, img.h);
    return t;
}

// ---- images ----------------------------------------------------------------

Texture Renderer::imageTex(const char* path, int* w, int* h)
{
    *w = *h = 0;
    if (!path || !path[0]) return 0;
    char key[300];
    _snprintf(key, sizeof key, "img:%s", path); key[sizeof key - 1] = 0;
    Texture t = lookup(key, w, h);
    if (t != (Texture)-1) return t;
    t = 0;
    // the console's own texture files (.xbx) are the host's to decode
    const char* ext = strrchr(path, '.');
    if (ext && (ext[1] == 'x' || ext[1] == 'X') && (ext[2] == 'b' || ext[2] == 'B') && (ext[3] == 'x' || ext[3] == 'X') && !ext[4]) {
        t = be->native(path, w, h);
        remember(key, t, *w, *h);
        return t;
    }
    Image* img = res->image(path);
    if (img) {
        t = be->upload(*img);
        *w = img->w; *h = img->h;
        delete img;
    }
    remember(key, t, *w, *h);
    return t;
}

// What a presenter shows: its owner's Text / ImagePath, or, when the
// presenter is bound to a data slot (DataAssociation k, the dashboard's
// list and meta data on the 360), the owner's Data<k>, which script sets
// (XuiPanel.SetData); unbound slots show nothing.
static const char* dataSlot(Node* n)
{
    unsigned k = propU(n->props, "DataAssociation", 0);
    if (k == 0) return propS(n->owner->props, strcmp(n->cls->name, "XuiTextPresenter") == 0 ? "Text" : "ImagePath");
    char key[16];
    sprintf(key, "Data%u", k);
    return propS(n->owner->props, key);
}

void Renderer::image(Node* n, const Affine& m, float alpha, Blend blend)
{
    const char* path = propS(n->props, "ImagePath");
    if (n->owner && n->cls && strcmp(n->cls->name, "XuiImagePresenter") == 0) {
        // the owner's ImagePath, or the data slot the presenter is bound to
        // (DataAssociation k: the owner's Data<k>, set by script)
        const char* p = dataSlot(n);
        if (p[0]) path = p;
    }
    int iw, ih;
    Texture tex = imageTex(path, &iw, &ih);
    if (tex == 0) return;
    float w = n->width(), h = n->height();
    float x0 = 0, y0 = 0, x1 = w, y1 = h;
    unsigned int mode = propU(n->props, "SizeMode", 0);
    if ((mode & 4) && iw > 0 && ih > 0) { // keep aspect, fit, centred
        float s = fmin2(w / iw, h / ih);
        x0 = (w - iw * s) / 2; y0 = (h - ih * s) / 2;
        x1 = x0 + iw * s; y1 = y0 + ih * s;
    } else if ((mode & 16) && iw > 0) { // centre at native size
        x0 = (w - iw) / 2; y0 = (h - ih) / 2;
        x1 = x0 + iw; y1 = y0 + ih;
    }
    quad(tex, blend, m, x0, y0, x1, y1, 0, 0, 1, 1, 0xffffffff, alpha);
}

void Renderer::quad(Texture tex, Blend blend, const Affine& m, float x0, float y0, float x1, float y1,
                    float u0, float v0, float u1, float v1, Color c, float alpha)
{
    float cr, cg, cb, ca;
    rgba(c, &cr, &cg, &cb, &ca);
    ca *= alpha;
    Vertex v[4];
    apply(m, x0, y0, &v[0].x, &v[0].y); v[0].u = u0; v[0].v = v0;
    apply(m, x1, y0, &v[1].x, &v[1].y); v[1].u = u1; v[1].v = v0;
    apply(m, x1, y1, &v[2].x, &v[2].y); v[2].u = u1; v[2].v = v1;
    apply(m, x0, y1, &v[3].x, &v[3].y); v[3].u = u0; v[3].v = v1;
    for (int i = 0; i < 4; i++) { v[i].r = cr; v[i].g = cg; v[i].b = cb; v[i].a = ca; }
    static const unsigned short idx[6] = { 0, 1, 2, 0, 2, 3 };
    be->triangles(tex, blend, v, 4, idx, 6);
}

// ---- text ------------------------------------------------------------------

enum { styleOneLine = 0x10, styleRight = 0x200, styleCenterH = 0x400, styleCenterV = 0x1000, styleShadow = 0x4000 };

// The XUI engine lays a line out with whole-pixel advances (each glyph's
// advance rounded up) and no kerning: lines measured against the GFWL
// engine's own renders come out the same width only this way.
static float advOf(float a) { return (float)ceil(a); }

static int measure(Font* f, float scale, const char* s, int len)
{
    float x = 0; int prev = 0;
    for (int i = 0; i < len;) {
        int adv; int cp = utf8decode(s + i, &adv); i += adv;
        int aw, lsb; stbtt_GetCodepointHMetrics(&f->info, cp, &aw, &lsb);
                x += advOf(aw * scale);
        prev = cp;
    }
    return (int)ceil(x);
}

// lineTex rasterises one line as white with alpha coverage (cached).
Texture Renderer::lineTex(Font* font, const char* line, float px, int* w, int* h, int* adv)
{
    char key[600];
    _snprintf(key, sizeof key, "txt:%p:%g:%s", (void*)font, px, line); key[sizeof key - 1] = 0;
    Texture t = lookup(key, w, h);
    float scale = stbtt_ScaleForMappingEmToPixels(&font->info, px);
    int len = (int)strlen(line);
    *adv = measure(font, scale, line, len);
    if (t != (Texture)-1) return t;
    int asc = (int)ceil(font->ascent * scale), desc = (int)ceil(-font->descent * scale);
    Image img;
    img.w = *adv + 2; if (img.w < 1) img.w = 1;
    img.h = asc + desc + 1; if (img.h < 1) img.h = 1;
    img.pix = new unsigned char[img.w * img.h * 4];
    memset(img.pix, 0, img.w * img.h * 4);
    unsigned char* cov = new unsigned char[img.w * img.h];
    memset(cov, 0, img.w * img.h);
    float x = 1; int prev = 0;
    for (int i = 0; i < len;) {
        int cadv; int cp = utf8decode(line + i, &cadv); i += cadv;
                int aw, lsb; stbtt_GetCodepointHMetrics(&font->info, cp, &aw, &lsb);
        int ix0, iy0, ix1, iy1;
        float xs = x - (float)floor(x);
        stbtt_GetCodepointBitmapBoxSubpixel(&font->info, cp, scale, scale, xs, 0, &ix0, &iy0, &ix1, &iy1);
        int gx = (int)floor(x) + ix0, gy = asc + iy0;
        int gw = ix1 - ix0, gh = iy1 - iy0;
        if (gw > 0 && gh > 0 && gx >= 0 && gy >= 0 && gx + gw <= img.w && gy + gh <= img.h)
            stbtt_MakeCodepointBitmapSubpixel(&font->info, cov + gy * img.w + gx, gw, gh, img.w, scale, scale, xs, 0, cp);
        x += advOf(aw * scale);
        prev = cp;
    }
    for (int i = 0; i < img.w * img.h; i++) {
        unsigned char* p = img.pix + i * 4;
        p[0] = p[1] = p[2] = 255; p[3] = cov[i];
    }
    delete [] cov;
    t = be->upload(img);
    remember(key, t, img.w, img.h);
    *w = img.w; *h = img.h;
    return t;
}

// A button glyph is a one-letter XuiText laid over the button's circle
// image (Abutton_Normal.png, A-Button.png ...).  The skins place the
// letter's box for the Convection font; with any other font the letter
// lands off the circle's centre, so it is centred on the circle instead:
// returns the circle's centre in the text's own coordinates.
static bool glyphTarget(Node* n, float* tx, float* ty)
{
    Node* p = n->parent;
    if (!p) return false;
    for (int i = 0; i < p->children.n; i++) {
        Node* c = p->children[i];
        if (!c->cls || strcmp(c->cls->name, "XuiImage") != 0) continue;
        const char* path = propS(c->props, "ImagePath");
        const char* f = strrchr(path, '/'); f = f ? f + 1 : path;
        if (!f[0] || (f[1] != 'b' && f[1] != '-' ) || !strstr(f, "utton")) continue;
        Vec3 zero = { 0, 0, 0 }, one = { 1, 1, 1 };
        Vec3 pos = propV(c->props, "Position", zero), sc = propV(c->props, "Scale", one), pv = propV(c->props, "Pivot", zero);
        float px = pos.x + pv.x + sc.x * (c->width() / 2 - pv.x), py = pos.y + pv.y + sc.y * (c->height() / 2 - pv.y);
        // into the text's frame (its Position, Pivot and Scale)
        Vec3 tp = propV(n->props, "Position", zero), ts = propV(n->props, "Scale", one), tv = propV(n->props, "Pivot", zero);
        if (ts.x == 0 || ts.y == 0) return false;
        *tx = (px - tp.x - tv.x) / ts.x + tv.x;
        *ty = (py - tp.y - tv.y) / ts.y + tv.y;
        return true;
    }
    return false;
}

void Renderer::text(Node* n, const Affine& m, float alpha, Blend blend)
{
    Font* font = fontFor(propS(n->props, "Font"));
    if (!font || !font->ok) return;
    const char* src = propS(n->props, "Text");
    if (n->owner && n->cls && strcmp(n->cls->name, "XuiTextPresenter") == 0) src = dataSlot(n);
    char txt[1024]; int tl = 0;
    for (const char* p = src; *p && tl < (int)sizeof txt - 1; p++) if (*p != '\r') txt[tl++] = *p;
    txt[tl] = 0;
    if (tl == 0) return;
    float size = propF(n->props, "PointSize", 14);
    unsigned int style = propU(n->props, "TextStyle", 0);
    // Draw glyphs at the on-screen scale so text stays sharp.
    float scale = (float)sqrt(fabs(m.a * m.d - m.b * m.c));
    if (scale <= 0) return;
    // PointSize is in points: the XUI engine sizes the em at 96 dpi (the
    // GFWL engine's legend and header glyphs and baselines, and sokky's
    // real-console capture of the legend, all measure 4/3 PointSize per em).
    float px = size * (96.0f / 72.0f) * scale;
    float fs = stbtt_ScaleForMappingEmToPixels(&font->info, px);
    float w = n->width(), h = n->height();

    // wrap into lines (word wrap unless one-line style)
    char* lines[64]; int nl = 0;
    bool wrap = (style & styleOneLine) == 0 && w > 0;
    char* para = txt;
    while (para && nl < 64) {
        char* nx = strchr(para, '\n');
        if (nx) *nx++ = 0;
        if (!wrap) lines[nl++] = para;
        else {
            // split words in place: build lines by inserting terminators
            char* line = 0; char* p = para;
            while (*p && nl < 64) {
                while (*p == ' ') p++;
                if (!*p) break;
                char* word = p;
                while (*p && *p != ' ') p++;
                if (line) {
                    int tryLen = (int)(p - line);
                    if (measure(font, fs, line, tryLen) > w * scale) {
                        word[-1] = 0; lines[nl++] = line; line = word;
                    }
                } else line = word;
            }
            if (line) { *p = 0; lines[nl++] = line; }
            else lines[nl++] = para;
        }
        para = nx;
    }

    float lh = (float)ceil((font->ascent - font->descent + font->lineGap) * fs) / scale;
    float total = lh * nl;
    float y = 0;
    if (style & styleCenterV) y = (h - total) / 2;
    Color col = propC(n->props, "TextColor", 0xffffffff);
    Color shadow = propC(n->props, "DropShadowColor", 0);
    Affine inv = aff(1 / scale, 0, 0, 1 / scale, 0, 0);
    for (int i = 0; i < nl; i++) {
        int iw, ih, adv;
        Texture tex = lineTex(font, lines[i], px, &iw, &ih, &adv);
        float lw = adv / scale;
        float x = 0;
        if (style & styleRight) x = w - lw;
        else if (style & styleCenterH) x = (w - lw) / 2;
        // the line texture has a pixel of padding left of the pen and its
        // baseline at the ascent rounded up; take both back out so the pen
        // starts at the box's left edge and the baseline sits exactly one
        // ascent below its top (the button glyphs' letters sat a pixel
        // right and low in their circles otherwise)
        float ox = x - 1 / scale, oy = y - ((float)ceil(font->ascent * fs) - font->ascent * fs) / scale;
        int gcp, glen; gcp = utf8decode(lines[i], &glen);
        float gx, gy; int x0, y0, x1, y1;
        if (nl == 1 && glen > 0 && !lines[i][glen] && glyphTarget(n, &gx, &gy) && stbtt_GetCodepointBox(&font->info, gcp, &x0, &y0, &x1, &y1)) {
            // pen and baseline that put the letter's ink centre on the circle's
            float baseline = gy + (y0 + y1) / 2.0f * fs / scale;
            ox = gx - (x0 + x1) / 2.0f * fs / scale - 1 / scale;
            oy = baseline - (float)ceil(font->ascent * fs) / scale;
        }
        Affine lm = mul(mul(m, aff(1, 0, 0, 1, ox, oy)), inv);
        if ((style & styleShadow) && (shadow >> 24) != 0) {
            Affine sm = mul(mul(m, aff(1, 0, 0, 1, ox + 1.5f, oy + 1.5f)), inv);
            quad(tex, blend, sm, 0, 0, (float)iw, (float)ih, 0, 0, 1, 1, shadow, alpha);
        }
        quad(tex, blend, lm, 0, 0, (float)iw, (float)ih, 0, 0, 1, 1, col, alpha);
        y += lh;
    }
}

} // namespace xui
