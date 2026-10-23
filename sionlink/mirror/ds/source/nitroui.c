// The app's own screens in the NitroLink look (see nitroui.h).
#include <stdlib.h>
#include <string.h>

#include "nitroui.h"

#define MIRROR_LZ_TICK() uiPoll()
#include "mirror_lz.h"
#include "nitroart.h"

#define W 256
#define H 192
#define PIXELS (W * H)

// sokky's colours: white text, the Figma's light box (#d9d9d9) with dark text on it, its
// border grey (#bdbdbd) for rules
#define C_TEXT     (RGB15(31, 31, 31) | BIT(15))
#define C_VALUE    (RGB15(25, 25, 25) | BIT(15))
#define C_DISABLED (RGB15(18, 18, 18) | BIT(15))
#define C_BOX      (RGB15(27, 27, 27) | BIT(15))
#define C_BOXTEXT  (RGB15(6, 6, 6) | BIT(15))
#define C_RULE     (RGB15(23, 23, 23) | BIT(15))

static bool shown;
static u16* back;   // the lower screen, made here and then copied in whole

bool uiShown(void)
{
    return shown;
}

// the art unpacked into dst (RAM: the unpacker writes bytes, VRAM takes 16 and 32 bits)
static bool unpackArt(const u8* lz, u32 len, u16* dst)
{
    return mirrorLzUnpack(lz, len, (u8*)dst, PIXELS * 2) != 0;
}

static void toVRAM(u16* vram, const u16* src)
{
    DC_FlushRange(src, PIXELS * 2);
    dmaCopyWords(3, src, vram, PIXELS * 2);
}

void uiShow(void)
{
    if (!back)
        back = malloc(PIXELS * 2);
    // the game's effects off: no fades, blending, windows, capture, mosaic, line changes
    irqDisable(IRQ_HBLANK);
    REG_MASTER_BRIGHT = 0;
    REG_MASTER_BRIGHT_SUB = 0;
    REG_BLDCNT = 0;
    REG_BLDCNT_SUB = 0;
    REG_MOSAIC = 0;
    REG_MOSAIC_SUB = 0;
    REG_DISPCAPCNT = 0;
    // both 2D engines on, the main one on top; the 3D engine as the game left it
    REG_POWERCNT = (REG_POWERCNT & (BIT(2) | BIT(3))) | BIT(0) | BIT(1) | BIT(9) | BIT(15);
    // bank A the main engine's bitmap, C the sub engine's; the others unmapped
    for (int b = 0; b < 9; b++)
        *(vu8*)(0x04000240 + (b < 7 ? b : b + 1)) = b == 0 ? 0x81 : b == 2 ? 0x84 : 0;
    REG_DISPCNT = MODE_5_2D | DISPLAY_BG3_ACTIVE;
    REG_DISPCNT_SUB = MODE_5_2D | DISPLAY_BG3_ACTIVE;
    REG_BG3CNT = BG_BMP16_256x256 | BG_BMP_BASE(0) | BG_PRIORITY(0);
    REG_BG3CNT_SUB = BG_BMP16_256x256 | BG_BMP_BASE(0) | BG_PRIORITY(0);
    REG_BG3PA = 256;
    REG_BG3PB = 0;
    REG_BG3PC = 0;
    REG_BG3PD = 256;
    REG_BG3X = 0;
    REG_BG3Y = 0;
    REG_BG3PA_SUB = 256;
    REG_BG3PB_SUB = 0;
    REG_BG3PC_SUB = 0;
    REG_BG3PD_SUB = 256;
    REG_BG3X_SUB = 0;
    REG_BG3Y_SUB = 0;
    shown = true;
    if (back && unpackArt(artTopLz, sizeof artTopLz, back))
        toVRAM(BG_GFX, back);
    else
        dmaFillHalfWords(RGB15(10, 10, 9) | BIT(15), BG_GFX, PIXELS * 2);
}

void uiLeave(void)
{
    shown = false;
    free(back);
    back = NULL;
}

// ---- the lower screen

// a fresh backdrop to draw on: the RAM copy, or with no memory for it, VRAM itself
static u16* begin(void)
{
    if (!uiShown())
        uiShow();
    if (back && unpackArt(artBottomLz, sizeof artBottomLz, back))
        return back;
    dmaFillHalfWords(RGB15(14, 14, 13) | BIT(15), BG_GFX_SUB, PIXELS * 2);
    return BG_GFX_SUB;
}

static void end(u16* fb)
{
    if (fb == back)
        toVRAM(BG_GFX_SUB, back);
}

static void fill(u16* fb, int x0, int y0, int x1, int y1, u16 c)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++)
            fb[y * W + x] = c;
}

static u32 nextChar(const char** s)
{
    const u8* p = (const u8*)*s;
    u32 c = *p++;
    int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    if (n)
        c &= n == 1 ? 0x1F : n == 2 ? 0x0F : 0x07;
    for (; n && (*p & 0xC0) == 0x80; n--)
        c = c << 6 | (*p++ & 0x3F);
    *s = (const char*)p;
    return c;
}

static const font_glyph* glyph(u32 c)
{
    int lo = 0, hi = (int)(sizeof fontGlyphs / sizeof fontGlyphs[0]) - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (fontGlyphs[mid].code == c)
            return &fontGlyphs[mid];
        if (fontGlyphs[mid].code < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return c == '?' ? NULL : glyph('?');
}

// width of the first n bytes of s (all of it for n < 0)
static int widthN(const char* s, int n)
{
    const char* end = n < 0 ? s + strlen(s) : s + n;
    int w = 0;
    while (s < end && *s)
    {
        const font_glyph* g = glyph(nextChar(&s));
        if (g)
            w += g->adv;
    }
    return w;
}

static int width(const char* s)
{
    return widthN(s, -1);
}

static int drawGlyph(u16* fb, int x, int top, const font_glyph* g, u16 c)
{
    const u8* bits = fontBits + g->bits;
    int stride = (g->w + 7) / 8;
    for (int r = 0; r < g->h; r++)
    {
        int y = top + g->y + r;
        if (y < 0 || y >= H)
            continue;
        for (int i = 0; i < g->w; i++)
        {
            int px = x + g->x + i;
            if (px >= 0 && px < W && (bits[r * stride + i / 8] & (0x80 >> (i & 7))))
                fb[y * W + px] = c;
        }
    }
    return g->adv;
}

// the first n bytes of s (all of it for n < 0) with the line's top at y; cut short with an
// ellipsis past maxW
static int drawN(u16* fb, int x, int y, const char* s, int n, u16 c, int maxW)
{
    const char* end = n < 0 ? s + strlen(s) : s + n;
    const font_glyph* dots = glyph(0x2026);
    bool cut = widthN(s, n) > maxW;
    int x0 = x, room = maxW - (cut && dots ? dots->adv : 0);
    while (s < end && *s)
    {
        const font_glyph* g = glyph(nextChar(&s));
        if (!g)
            continue;
        if (cut && x + g->adv - x0 > room)
            break;
        x += drawGlyph(fb, x, y, g, c);
    }
    if (cut && dots)
        x += drawGlyph(fb, x, y, dots, c);
    return x - x0;
}

static int draw(u16* fb, int x, int y, const char* s, u16 c, int maxW)
{
    return drawN(fb, x, y, s, -1, c, maxW);
}

// the next line of s that fits maxW, broken at a space: its length in bytes (*next: where
// the line after starts)
static int wrapLine(const char* s, int maxW, const char** next)
{
    const char* p = s;
    const char* lastBreak = NULL;
    int w = 0;
    while (*p)
    {
        const char* at = p;
        u32 c = nextChar(&p);
        if (c == ' ')
            lastBreak = at;
        const font_glyph* g = glyph(c);
        w += g ? g->adv : 0;
        if (w > maxW && at > s)
        {
            if (lastBreak)
            {
                *next = lastBreak + 1;
                return lastBreak - s;
            }
            *next = at;
            return at - s;
        }
    }
    *next = p;
    return p - s;
}

#define LINE_TOP(slot) (MIRROR_MENU_ROW_Y + (slot) * MIRROR_MENU_ROW_H)

void uiMessage(const char* line1, const char* line2)
{
    u16* fb = begin();
    // in the middle, where the Figma has its "Waiting for link..."
    int y = H / 2 - (line2 ? FONT_LINE + 1 : FONT_LINE / 2);
    draw(fb, (W - width(line1)) / 2, y, line1, C_TEXT, W - 32);
    if (line2)
        draw(fb, (W - width(line2)) / 2, y + FONT_LINE + 2, line2, C_TEXT, W - 32);
    end(fb);
}

// a button for the legend: a light disc with its letter, then what it does
static int legendButton(u16* fb, int x, int y, const char* letter, const char* what)
{
    for (int dy = -5; dy <= 5; dy++)
        for (int dx = -5; dx <= 5; dx++)
            if (dx * dx + dy * dy <= 30)
                fb[(y + 6 + dy) * W + x + 5 + dx] = C_BOX;
    draw(fb, x + 6 - width(letter) / 2, y, letter, C_BOXTEXT, 12);
    return 14 + draw(fb, x + 14, y, what, C_TEXT, 90);
}

void uiMenu(const mirror_menu* m, u32 len)
{
    // the strings after it: title, question, A's and B's words, then label and value a row
    const char* text = (const char*)(m + 1);
    const char* limit = (const char*)m + len;
    const char* str[4 + 2 * 32];
    int nstr = 0, rows = m->rows < 32 ? m->rows : 32;
    for (const char* p = text; nstr < 4 + 2 * rows && p < limit; nstr++)
    {
        const char* z = memchr(p, 0, limit - p);
        if (!z)
            break;
        str[nstr] = p;
        p = z + 1;
    }
    for (; nstr < 4 + 2 * rows; nstr++)
        str[nstr] = "";

    u16* fb = begin();
    // the game, and a rule under it
    draw(fb, 18, 8, str[0], C_TEXT, W - 36);
    fill(fb, MIRROR_MENU_X0, MIRROR_MENU_ROW_Y - 4, MIRROR_MENU_X1, MIRROR_MENU_ROW_Y - 3, C_RULE);

    // a question, above its rows
    int y = MIRROR_MENU_ROW_Y + 2;
    for (const char* q = str[1]; *q && y + FONT_LINE <= LINE_TOP(m->firstSlot);)
    {
        const char* next;
        int n = wrapLine(q, MIRROR_MENU_X1 - MIRROR_MENU_X0 - 8, &next);
        drawN(fb, MIRROR_MENU_X0 + 4, y, q, n, C_TEXT, W);
        y += FONT_LINE + 1;
        q = next;
    }

    for (int i = 0; i < rows && m->firstSlot + i < MIRROR_MENU_SLOTS; i++)
    {
        int top = LINE_TOP(m->firstSlot + i);
        bool focused = i == m->focus, enabled = !(m->disabled & (1u << i));
        if (focused)
            fill(fb, MIRROR_MENU_X0, top + 1, MIRROR_MENU_X1, top + MIRROR_MENU_ROW_H, C_BOX);
        u16 c = !enabled ? C_DISABLED : focused ? C_BOXTEXT : C_TEXT;
        const char* label = str[4 + 2 * i];
        const char* value = str[5 + 2 * i];
        int ty = top + (MIRROR_MENU_ROW_H - FONT_LINE) / 2 + 1;
        int lw = draw(fb, MIRROR_MENU_X0 + 6, ty, label, c, MIRROR_MENU_X1 - MIRROR_MENU_X0 - 12);
        if (*value)
        {
            int room = MIRROR_MENU_X1 - MIRROR_MENU_X0 - 12 - lw - 8;
            int vw = width(value) < room ? width(value) : room;
            draw(fb, MIRROR_MENU_X1 - 6 - vw, ty, value, enabled && !focused ? C_VALUE : c, room);
        }
    }

    // what A and B do
    int ly = LINE_TOP(MIRROR_MENU_SLOTS) + 2;
    int x = MIRROR_MENU_X0 + 4;
    x += legendButton(fb, x, ly, "A", str[2]) + 12;
    legendButton(fb, x, ly, "B", str[3]);
    end(fb);
}
