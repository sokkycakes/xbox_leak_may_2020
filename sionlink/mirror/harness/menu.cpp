#include "menu.h"
#include "menuart.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

// Frolick's colors (xbcompat/tools/frolickdash/tools/genpage.py on its branch), ARGB
const uint32_t PanelTop = 0xd9424c5a, PanelBottom = 0xd9313b49;
const uint32_t TextColor = 0xffffffff, MetaColor = 0xffb4c0c8, DisabledColor = 0x6effffff;
const uint32_t FocusText = 0xff0a0a0a, FocusShadow = 0x5aebebeb;
const uint32_t FocusTop = 0xf4f4f5f6, FocusBottom = 0xf4c9ced3;
const uint32_t Lime = 0xffc4ff46, Unlit = 0xff68727e, RuleColor = 0x30ffffff;

// The panel in units of a 480-line picture's lines (more lines: all of it bigger)
const int PanelW = 460, HeaderH = 88, RowsY = 96, RowH = 34, Rows = 7;
const int LegendY = RowsY + Rows * RowH + 8, PanelH = LegendY + 42;

std::u32string Decode(const std::string& s)
{
    std::u32string out;
    for (size_t i = 0; i < s.size();)
    {
        uint8_t c = s[i];
        int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        char32_t v = n == 0 ? c : n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
        for (int k = 1; k <= n && i + k < s.size(); k++) v = v << 6 | (s[i + k] & 0x3F);
        out.push_back(v);
        i += n + 1;
    }
    return out;
}

void Encode(std::string& out, char32_t c)
{
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 0x3F)); }
    else { out += (char)(0xE0 | c >> 12); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
}

// coverage 0-255, placed from the pen on the baseline
struct Mask { int x = 0, y = 0, w = 0, h = 0; std::vector<uint8_t> a; };
// premultiplied ARGB
struct Picture { int w = 0, h = 0; std::vector<uint32_t> px; };

// A glyph from menuart.h made `sx` output pixels a master pixel across and `sy` down,
// each output pixel the average of the master pixels under it (the master is bigger).
Mask ScaleGlyph(const menuart::Glyph& g, double sx, double sy)
{
    Mask m;
    m.x = (int)floor(g.x * sx);
    m.y = (int)floor(g.y * sy);
    m.w = (int)ceil((g.x + g.w) * sx) - m.x;
    m.h = (int)ceil((g.y + g.h) * sy) - m.y;
    if (g.w == 0 || g.h == 0) { m.w = m.h = 0; return m; }
    m.a.assign((size_t)m.w * m.h, 0);
    const int rowBytes = (g.w + 1) / 2;
    auto cov = [&](int mx, int my) {
        uint8_t v = menuart::Bits[g.at + my * rowBytes + mx / 2];
        return (double)((mx & 1 ? v >> 4 : v & 15) * 17);
    };
    for (int j = 0; j < m.h; j++)
    {
        double y0 = (m.y + j) / sy, y1 = (m.y + j + 1) / sy;   // master rows, from the baseline
        for (int i = 0; i < m.w; i++)
        {
            double x0 = (m.x + i) / sx, x1 = (m.x + i + 1) / sx;
            double sum = 0;
            for (int my = std::max((int)floor(y0) - g.y, 0); my < std::min((int)ceil(y1) - g.y, (int)g.h); my++)
            {
                double oy = std::min(y1, (double)(g.y + my + 1)) - std::max(y0, (double)(g.y + my));
                if (oy <= 0) continue;
                for (int mx = std::max((int)floor(x0) - g.x, 0); mx < std::min((int)ceil(x1) - g.x, (int)g.w); mx++)
                {
                    double ox = std::min(x1, (double)(g.x + mx + 1)) - std::max(x0, (double)(g.x + mx));
                    if (ox > 0) sum += ox * oy * cov(mx, my);
                }
            }
            m.a[(size_t)j * m.w + i] = (uint8_t)std::min(255.0, sum * sx * sy + 0.5);
        }
    }
    return m;
}

// an ARGB picture at dw x dw, the same way
Picture ScaleImage(const uint32_t* src, int sw, int sh, int dw, int dh)
{
    Picture p;
    p.w = dw;
    p.h = dh;
    p.px.assign((size_t)dw * dh, 0);
    double fx = (double)sw / dw, fy = (double)sh / dh;
    for (int j = 0; j < dh; j++)
    {
        double y0 = j * fy, y1 = (j + 1) * fy;
        for (int i = 0; i < dw; i++)
        {
            double x0 = i * fx, x1 = (i + 1) * fx;
            double acc[4] = {};
            for (int sy = (int)y0; sy < std::min((int)ceil(y1), sh); sy++)
            {
                double oy = std::min(y1, sy + 1.0) - std::max(y0, (double)sy);
                for (int sx = (int)x0; sx < std::min((int)ceil(x1), sw); sx++)
                {
                    double o = (std::min(x1, sx + 1.0) - std::max(x0, (double)sx)) * oy;
                    uint32_t c = src[sy * sw + sx];
                    double a = (c >> 24) / 255.0;
                    acc[0] += o * (c >> 24);
                    acc[1] += o * a * ((c >> 16) & 255);
                    acc[2] += o * a * ((c >> 8) & 255);
                    acc[3] += o * a * (c & 255);
                }
            }
            double n = 1 / (fx * fy);
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) v = v << 8 | (uint32_t)std::min(255.0, acc[k] * n + 0.5);
            p.px[(size_t)j * dw + i] = v;
        }
    }
    return p;
}

struct Font
{
    std::unordered_map<char32_t, Mask> masks;
    std::unordered_map<char32_t, double> adv;
    Font() = default;
    Font(double px, double par)
    {
        double sy = px / menuart::Em, sx = sy / par;
        for (const menuart::Glyph& g : menuart::Glyphs)
        {
            masks[g.code] = ScaleGlyph(g, sx, sy);
            adv[g.code] = g.adv64 / 64.0 * sx;
        }
    }
    // what isn't in the font shows as a question mark
    char32_t Have(char32_t c) const { return adv.count(c) ? c : U'?'; }
    double Width(const std::u32string& s) const
    {
        double w = 0;
        for (char32_t c : s) w += adv.at(Have(c));
        return w;
    }
    // as much as fits in `max` pixels, with an ellipsis where it's cut
    std::u32string Fit(std::u32string s, double max) const
    {
        if (Width(s) <= max) return s;
        while (!s.empty() && Width(s + U"…") > max) s.pop_back();
        while (!s.empty() && s.back() == U' ') s.pop_back();
        return s + U"…";
    }
    // the words in lines of up to `max` pixels
    std::vector<std::u32string> Wrap(const std::u32string& s, double max) const
    {
        std::vector<std::u32string> lines;
        std::u32string line, word;
        auto flush = [&] {
            std::u32string t = line.empty() ? word : line + U" " + word;
            if (!line.empty() && Width(t) > max)
            {
                lines.push_back(line);
                line = word;
            }
            else
                line = t;
            word.clear();
        };
        for (char32_t c : s)
        {
            if (c == U' ') flush();
            else word.push_back(c);
        }
        flush();
        if (!line.empty()) lines.push_back(line);
        return lines;
    }
};

struct Canvas
{
    uint32_t* px;
    uint32_t stride;   // in pixels
    int w, h;

    void Blend(int x, int y, uint32_t argb, uint32_t cov = 255)
    {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint32_t a = (argb >> 24) * cov / 255;
        if (!a) return;
        a += a >> 7;   // of 256
        uint32_t& d = px[(size_t)y * stride + x];
        uint32_t rb = ((argb & 0xFF00FF) * a + (d & 0xFF00FF) * (256 - a)) >> 8 & 0xFF00FF;
        uint32_t g = ((argb & 0x00FF00) * a + (d & 0x00FF00) * (256 - a)) >> 8 & 0x00FF00;
        d = rb | g;
    }
    void Fill(int x0, int y0, int x1, int y1, uint32_t argb)
    {
        for (int y = std::max(y0, 0); y < std::min(y1, h); y++)
            for (int x = std::max(x0, 0); x < std::min(x1, w); x++) Blend(x, y, argb);
    }
    static uint32_t Mix(uint32_t a, uint32_t b, double t)
    {
        uint32_t v = 0;
        for (int s = 24; s >= 0; s -= 8)
        {
            double ca = (a >> s) & 255, cb = (b >> s) & 255;
            v |= (uint32_t)lround(ca + (cb - ca) * t) << s;
        }
        return v;
    }
    void Gradient(int x0, int y0, int x1, int y1, uint32_t top, uint32_t bottom)
    {
        for (int y = y0; y < y1; y++)
        {
            uint32_t c = Mix(top, bottom, y1 - y0 > 1 ? (double)(y - y0) / (y1 - y0 - 1) : 0);
            Fill(x0, y, x1, y + 1, c);
        }
    }
    // the game behind the menu, at 7/16
    void Dim()
    {
        for (int y = 0; y < h; y++)
        {
            uint32_t* row = px + (size_t)y * stride;
            for (int x = 0; x < w; x++)
            {
                uint32_t p = row[x];
                row[x] = ((p >> 1) & 0x7F7F7F) - ((p >> 4) & 0x0F0F0F);
            }
        }
    }
    // returns where the pen ends
    double Text(const Font& f, double x, int baseline, const std::u32string& s, uint32_t argb, uint8_t alpha = 255)
    {
        for (char32_t ch : s)
        {
            char32_t c = f.Have(ch);
            const Mask& m = f.masks.at(c);
            int ox = (int)lround(x) + m.x, oy = baseline + m.y;
            for (int j = 0; j < m.h; j++)
                for (int i = 0; i < m.w; i++)
                    if (uint32_t a = m.a[(size_t)j * m.w + i]) Blend(ox + i, oy + j, argb, a * alpha / 255);
            x += f.adv.at(c);
        }
        return x;
    }
    void Image(const Picture& p, int x0, int y0, uint8_t alpha = 255)
    {
        for (int j = 0; j < p.h; j++)
            for (int i = 0; i < p.w; i++)
            {
                int x = x0 + i, y = y0 + j;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                uint32_t s = p.px[(size_t)j * p.w + i];
                uint32_t a = (s >> 24) * alpha / 255;
                if (!a) continue;
                uint32_t& d = px[(size_t)y * stride + x];
                uint32_t out = 0;
                for (int sh = 16; sh >= 0; sh -= 8)
                {
                    // premultiplied: source + destination * (1 - alpha)
                    uint32_t sc = ((s >> sh) & 255) * alpha / 255, dc = (d >> sh) & 255;
                    out |= std::min(255u, sc + dc * (255 - a) / 255) << sh;
                }
                d = out;
            }
    }
};

}   // namespace

// everything drawn at the TV's size: rebuilt when the mode changes
struct GameMenu::Art
{
    uint32_t h = 0;
    double par = 0, k = 1;   // a pixel's width over its height; lines a unit
    Font title, body, small, letter;
    Picture a, b, icon;
};

GameMenu::GameMenu() = default;
GameMenu::~GameMenu() = default;

const char* GameMenu::LayoutName(TVLayout l)
{
    switch (l)
    {
    case TVLayout::TopOnly: return "Top screen only";
    case TVLayout::BottomOnly: return "Bottom screen only";
    case TVLayout::Stacked: return "Stacked";
    case TVLayout::SideBySide: return "Side by side";
    case TVLayout::BigTop: return "Top screen big";
    case TVLayout::BigBottom: return "Bottom screen big";
    default: break;
    }
    return "";
}

void GameMenu::TakeBanner(const uint8_t* rom, uint32_t len)
{
    if (len < 0x200) return;
    // the header's title: 12 characters
    title.clear();
    for (int i = 0; i < 12 && rom[i]; i++) title += (char)rom[i];
    while (!title.empty() && title.back() == ' ') title.pop_back();
    uint32_t off;
    memcpy(&off, rom + 0x68, 4);
    if (!off || off + 0x440 > len) return;
    const uint8_t* bn = rom + off;
    // the icon: 32x32, 4x4 tiles of 8x8 at 4 bits a pixel, 16 BGR555 colors (0 clear)
    uint32_t pal[16];
    for (int i = 0; i < 16; i++)
    {
        uint16_t c = bn[0x220 + i * 2] | bn[0x221 + i * 2] << 8;
        auto x5 = [](uint32_t v) { return v << 3 | v >> 2; };
        pal[i] = (i ? 0xFF000000u : 0) | x5(c & 31) << 16 | x5((c >> 5) & 31) << 8 | x5((c >> 10) & 31);
    }
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
        {
            uint8_t v = bn[0x20 + ((y / 8) * 4 + x / 8) * 32 + (y % 8) * 4 + (x % 8) / 2];
            icon[y * 32 + x] = pal[x & 1 ? v >> 4 : v & 15];
        }
    hasIcon = true;
    // the English title's first line (the second is the publisher)
    std::string t;
    for (int i = 0; i < 128; i++)
    {
        char32_t c = bn[0x340 + i * 2] | bn[0x341 + i * 2] << 8;
        if (!c || c == U'\n') break;
        Encode(t, c);
    }
    if (!t.empty()) title = t;
}

void GameMenu::Open()
{
    open = true;
    page = 0;
    focus = 0;
    prev = 0;
    blocked = 0xFFFF;   // whatever is held as it opens counts once it's let go
    toastFrames = 0;
}

void GameMenu::Toast(const std::string& text)
{
    toast = text;
    toastFrames = 100;
}

MenuAction GameMenu::Update(uint16_t keys)
{
    if (!open) return MenuAction::None;
    struct Shown { int page, focus, confirmFocus; TVLayout layout; bool swap; };
    const Shown was {page, focus, confirmFocus, layout, swap};
    MenuAction a = Act(keys);
    if (a != MenuAction::None || page != was.page || focus != was.focus || confirmFocus != was.confirmFocus ||
        layout != was.layout || swap != was.swap)
        changes++;
    return a;
}

MenuAction GameMenu::Act(uint16_t keys)
{
    blocked &= keys;
    uint16_t k = keys & ~blocked;
    uint16_t edge = k & ~prev;
    prev = k;
    // the d-pad repeats while it's held: after 0.4 s, ten times a second
    uint16_t fire = edge & (MK_OK | MK_BACK | MK_CLOSE);
    for (int d = 0; d < 4; d++)
    {
        uint16_t bit = 1 << d;
        if (!(k & bit)) continue;
        if (edge & bit) { fire |= bit; repeatIn[d] = 24; }
        else if (--repeatIn[d] <= 0) { fire |= bit; repeatIn[d] = 6; }
    }
    auto close = [&](MenuAction a) {
        open = false;
        return a;
    };
    if (fire & MK_CLOSE) return close(MenuAction::Resume);
    if (page)
    {
        // a yes or no
        if (fire & MK_BACK) page = 0;
        else if (fire & (MK_UP | MK_DOWN)) confirmFocus ^= 1;
        else if (fire & MK_OK)
        {
            if (confirmFocus == 0) return close((MenuAction)page);
            page = 0;
        }
        return MenuAction::None;
    }
    if (fire & MK_BACK) return close(MenuAction::Resume);
    auto enabled = [&](int r) { return Enabled(r); };
    if (fire & (MK_UP | MK_DOWN))
    {
        int step = fire & MK_UP ? Rows - 1 : 1;
        do focus = (focus + step) % Rows;
        while (!enabled(focus));
    }
    const int layouts = (int)TVLayout::Count;
    if (focus == 1 && (fire & (MK_LEFT | MK_RIGHT | MK_OK)))
        layout = (TVLayout)(((int)layout + (fire & MK_LEFT ? layouts - 1 : 1)) % layouts);
    else if (focus == 2 && (fire & (MK_LEFT | MK_RIGHT | MK_OK)))
        swap = !swap;
    else if (fire & MK_OK)
    {
        switch (focus)
        {
        case 0: return close(MenuAction::Resume);
        case 3: return close(MenuAction::QuickSave);
        case 4: page = (int)MenuAction::QuickLoad; confirmFocus = 0; break;
        case 5: page = (int)MenuAction::Reset; confirmFocus = 0; break;
        case 6: page = (int)MenuAction::Dashboard; confirmFocus = 0; break;
        }
    }
    return MenuAction::None;
}

void GameMenu::Draw(uint32_t* dst, uint32_t pitch, uint32_t w, uint32_t h, double par)
{
    if (!open && toastFrames <= 0) return;
    if (!art || art->h != h || art->par != par)
    {
        art = std::make_unique<Art>();
        art->h = h;
        art->par = par;
        double k = art->k = h / 480.0;
        art->title = Font(26 * k, par);
        art->body = Font(21 * k, par);
        art->small = Font(17 * k, par);
        art->letter = Font(16 * k, par);
        int bh = (int)lround(24 * k), bw = (int)lround(24 * k / par);
        art->a = ScaleImage(menuart::ButtonA, menuart::ButtonAW, menuart::ButtonAH, bw, bh);
        art->b = ScaleImage(menuart::ButtonB, menuart::ButtonBW, menuart::ButtonBH, bw, bh);
        if (hasIcon) art->icon = ScaleImage(icon, 32, 32, (int)lround(64 * k / par), (int)lround(64 * k));
    }
    const Art& A = *art;
    Canvas c {dst, pitch / 4, (int)w, (int)h};
    const double k = A.k;
    auto ux = [&](double u) { return u * k / par; };   // units across to pixels

    if (!open)
    {
        // the toast: a slate bar low on the TV, fading out
        uint8_t alpha = (uint8_t)std::min(255, toastFrames * 255 / 15);
        std::u32string t = Decode(toast);
        double tw = A.body.Width(t);
        int bw = (int)(tw + ux(48)), bh = (int)lround(40 * k);
        int x0 = ((int)w - bw) / 2, y0 = (int)(h * 0.95) - bh - (int)lround(8 * k);
        auto fade = [&](uint32_t col) { return (col & 0xFFFFFF) | ((col >> 24) * alpha / 255) << 24; };
        c.Gradient(x0, y0, x0 + bw, y0 + bh, fade(PanelTop), fade(PanelBottom));
        c.Text(A.body, x0 + ux(24), y0 + (int)lround(27 * k), t, TextColor, alpha);
        return;
    }

    c.Dim();
    int px0 = (int)((w - ux(PanelW)) / 2), py0 = (int)((h - PanelH * k) / 2);
    auto X = [&](double u) { return px0 + (int)lround(ux(u)); };
    auto Y = [&](double u) { return py0 + (int)lround(u * k); };
    c.Gradient(X(0), Y(0), X(PanelW), Y(PanelH), PanelTop, PanelBottom);

    // the game and the DS link
    double tx = 20;
    if (hasIcon)
    {
        c.Image(A.icon, X(20), Y(12));
        tx = 100;
    }
    c.Text(A.title, X(tx), Y(46), A.title.Fit(Decode(title), ux(PanelW - tx - 20)), TextColor);
    // a dot, lit while the DS is there
    int dot = std::max(2, (int)lround(5 * k));
    int dcx = X(tx + 5), dcy = Y(64);
    for (int y = -dot; y <= dot; y++)
        for (int x = -(int)lround(dot / par); x <= (int)lround(dot / par); x++)
        {
            double r = sqrt((x * par) * (x * par) + y * y);
            if (r <= dot + 0.5) c.Blend(dcx + x, dcy + y, linked ? Lime : Unlit, (uint32_t)std::min(255.0, (dot + 0.5 - r) * 255 + 128));
        }
    c.Text(A.small, X(tx + 16), Y(70), A.small.Fit(Decode(link), ux(PanelW - tx - 36)), MetaColor);
    c.Fill(X(16), Y(HeaderH), X(PanelW - 16), Y(HeaderH) + std::max(1, (int)lround(k)), RuleColor);

    auto row = [&](int i, const std::u32string& label, const std::u32string& value, bool focused, bool enabled) {
        int top = RowsY + i * RowH;
        if (focused) c.Gradient(X(12), Y(top), X(PanelW - 12), Y(top + RowH), FocusTop, FocusBottom);
        uint32_t col = !enabled ? DisabledColor : focused ? FocusText : TextColor;
        if (focused) c.Text(A.body, X(26), Y(top + 23) + 1, label, FocusShadow);
        c.Text(A.body, X(26), Y(top + 23), label, col);
        if (!value.empty())
        {
            double vw = A.body.Width(value);
            if (focused) c.Text(A.body, X(PanelW - 26) - vw, Y(top + 23) + 1, value, FocusShadow);
            c.Text(A.body, X(PanelW - 26) - vw, Y(top + 23), value, enabled && !focused ? MetaColor : col);
        }
    };
    // a button: the Blades' orb with its letter on it, then what it does
    auto button = [&](double x, int y, const Picture& orb, char32_t letter, const char* what) {
        c.Image(orb, (int)x, y);
        std::u32string l(1, letter);
        double lx = x + (orb.w - A.letter.Width(l)) / 2;
        int ly = y + (int)lround(17.5 * k);
        c.Text(A.letter, lx + std::max(1.0, k / par), ly + std::max(1, (int)lround(k)), l, 0x80000000);
        c.Text(A.letter, lx, ly, l, TextColor);
        return c.Text(A.small, x + orb.w + ux(6), y + (int)lround(18 * k), Decode(what), TextColor);
    };
    auto legend = [&](const char* aText, const char* bText) {
        int y = Y(LegendY + 7);
        double x = button(X(24), y, A.a, U'A', aText);
        button(x + ux(22), y, A.b, U'B', bText);
    };

    if (page == 0)
    {
        std::u32string lay = Decode(LayoutName(layout));
        const std::u32string labels[Rows] = {U"Resume", U"Screen layout", U"Swap screens", U"Quick save", U"Quick load",
                                             U"Reset game", U"Back to dashboard"};
        const std::u32string values[Rows] = {U"", focus == 1 ? U"‹  " + lay + U"  ›" : lay, swap ? U"On" : U"Off",
                                             U"", Decode(stateWhen), U"", U""};
        for (int i = 0; i < Rows; i++) row(i, labels[i], values[i], i == focus, Enabled(i));
        legend("Select", "Resume");
        return;
    }
    // asking first: what it does, then yes or no
    std::string q, yes;
    switch ((MenuAction)page)
    {
    case MenuAction::QuickLoad:
        q = "Load the quick save from " + stateWhen + "? The game goes back to then, its own save too.";
        yes = "Load it";
        break;
    case MenuAction::Reset:
        q = "Reset the game? Anything since the game last saved is lost.";
        yes = "Reset";
        break;
    default:
        q = "Quit the game and go back to the dashboard? Anything since the game last saved is lost.";
        yes = "Quit";
        break;
    }
    std::vector<std::u32string> lines = A.body.Wrap(Decode(q), ux(PanelW - 52));
    for (size_t i = 0; i < lines.size(); i++) c.Text(A.body, X(26), Y(RowsY + 26 + i * 28), lines[i], TextColor);
    int first = (int)std::max<size_t>(3, lines.size() + 1);
    row(first, Decode(yes), U"", confirmFocus == 0, true);
    row(first + 1, U"Cancel", U"", confirmFocus == 1, true);
    legend("OK", "Cancel");
}
