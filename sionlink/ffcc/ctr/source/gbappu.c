// A software GBA picture processor (see gbappu.h), written from the GBA's
// documented register layout (GBATEK). One scanline at a time: the sprites
// go into a line buffer of their own, the backgrounds are painted back to
// front a tile at a time (skipping transparent ones), the sprites are put
// in at their priority, then windows and color effects apply per pixel, as
// on the hardware. Only alpha blending needs the layer under the top one,
// so only then is it kept.

#include <string.h>

#include "gbappu.h"

#define IO16(o) (*(const uint16_t *)(ppu->io + (o)))

// A layer's pixel on this line: palette color (0x8000 = transparent)
#define CLEAR 0x8000

enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BACK };

// A pixel's tag: the priority and layer of what is there
#define BACK_TAG ((4 << 3) | L_BACK)

typedef struct {
    uint16_t color[GBA_W]; // BGR555, or CLEAR
    uint8_t prio[GBA_W];   // sprites only
    uint8_t semi[GBA_W];   // sprites only: semi-transparent
    uint8_t win[GBA_W];    // sprites only: inside the OBJ window
} line_buf;

static line_buf obj;
// What the sprites drew on this line: anything, and semi-transparent pixels
static int obj_dirty = 1, obj_semi, obj_prios; // obj_prios: a bit per priority drawn

// Backgrounds
// ===========

// Paints a text background's line onto dst: its opaque pixels replace
// what is there, and each one's tag is set to tagv. With allow, only pixels
// whose window lets the layer (bit) through are painted.
//
// With sec, layers are painted front to back instead, and only the top two
// count: a pixel's first layer goes to dst and tag, its second to sec and
// sectag, the rest are under them. layers counts them per pixel, full the
// pixels with two.
static inline __attribute__((always_inline)) void PaintTextBg(
    const gba_ppu *ppu, int n, int y, uint16_t *dst, uint8_t *tag, uint8_t tagv,
    const uint8_t *allow, uint8_t bit, uint16_t *sec, uint8_t *sectag, uint8_t *layers, int *full)
{
    uint16_t cnt = IO16(0x08 + n * 2);
    int hofs = IO16(0x10 + n * 4) & 0x1FF;
    int vofs = IO16(0x12 + n * 4) & 0x1FF;
    uint32_t chars = ((cnt >> 2) & 3) * 0x4000;
    uint32_t screen = ((cnt >> 8) & 0x1F) * 0x800;
    int size = cnt >> 14;
    int bpp8 = cnt & 0x80;
    int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    const uint16_t *pltt = ppu->pltt;

    // 32x32-tile screen blocks, laid out left to right, then down. The
    // line's row of map entries starts here (plus the block across).
    int sy = (y + vofs) & (h - 1);
    uint32_t row_at = screen + (sy >> 8) * (w >> 8) * 0x800 + ((sy & 255) >> 3) * 64;
    int ty0 = sy & 7;

    // A tile at a time: one map entry, one row of the tile
    int sx = hofs & (w - 1);
    for (int x = 0; x < GBA_W;)
    {
        int first = sx & 7;
        int count = 8 - first;
        if (x + count > GBA_W)
            count = GBA_W - x;
        uint32_t at = row_at + (sx >> 8) * 0x800 + ((sx & 255) >> 3) * 2;
        int x0 = x;
        x += count;
        sx = (sx + count) & (w - 1);
        if (at >= 0x10000)
            continue;
        uint16_t e = *(const uint16_t *)(ppu->vram + at);
        int ty = (e & 0x800) ? 7 - ty0 : ty0;
        int flip = (e & 0x400) ? 7 : 0;
        int tile = e & 0x3FF;
        if (bpp8)
        {
            uint32_t off = chars + tile * 64 + ty * 8;
            if (off >= 0x10000)
                continue;
            const uint8_t *row = ppu->vram + off;
            for (int i = 0; i < count; i++)
            {
                int index = row[(first + i) ^ flip];
                if (index && (!allow || (allow[x0 + i] & bit)))
                {
                    if (!sec)
                    {
                        dst[x0 + i] = pltt[index] & 0x7FFF;
                        tag[x0 + i] = tagv;
                    }
                    else if (layers[x0 + i] == 0)
                    {
                        dst[x0 + i] = pltt[index] & 0x7FFF;
                        tag[x0 + i] = tagv;
                        layers[x0 + i] = 1;
                    }
                    else if (layers[x0 + i] == 1)
                    {
                        sec[x0 + i] = pltt[index] & 0x7FFF;
                        sectag[x0 + i] = tagv;
                        layers[x0 + i] = 2;
                        ++*full;
                    }
                }
            }
        }
        else
        {
            uint32_t off = chars + tile * 32 + ty * 4;
            if (off >= 0x10000)
                continue;
            uint32_t bits = *(const uint32_t *)(ppu->vram + off);
            if (!bits)
                continue;
            const uint16_t *pal = pltt + ((e >> 12) << 4);
            if (flip)
            {
                // Pixel order reversed: the nibbles in reverse
                bits = __builtin_bswap32(bits);
                bits = ((bits >> 4) & 0x0F0F0F0F) | ((bits & 0x0F0F0F0F) << 4);
            }
            bits >>= first * 4;
            for (int i = 0; i < count; i++, bits >>= 4)
            {
                int index = bits & 15;
                if (index && (!allow || (allow[x0 + i] & bit)))
                {
                    if (!sec)
                    {
                        dst[x0 + i] = pal[index] & 0x7FFF;
                        tag[x0 + i] = tagv;
                    }
                    else if (layers[x0 + i] == 0)
                    {
                        dst[x0 + i] = pal[index] & 0x7FFF;
                        tag[x0 + i] = tagv;
                        layers[x0 + i] = 1;
                    }
                    else if (layers[x0 + i] == 1)
                    {
                        sec[x0 + i] = pal[index] & 0x7FFF;
                        sectag[x0 + i] = tagv;
                        layers[x0 + i] = 2;
                        ++*full;
                    }
                }
            }
        }
    }
}

static void PaintTagged(const gba_ppu *ppu, int n, int y, uint16_t *dst, uint8_t *tag, uint8_t tagv)
{
    PaintTextBg(ppu, n, y, dst, tag, tagv, NULL, 0, NULL, NULL, NULL, NULL);
}

static void PaintWindowed(const gba_ppu *ppu, int n, int y, uint16_t *dst, uint8_t *tag, uint8_t tagv,
                          const uint8_t *allow)
{
    PaintTextBg(ppu, n, y, dst, tag, tagv, allow, 1 << n, NULL, NULL, NULL, NULL);
}

static void PaintUnder(const gba_ppu *ppu, int n, int y, uint16_t *dst, uint8_t *tag, uint8_t tagv,
                       uint16_t *sec, uint8_t *sectag, uint8_t *layers, int *full)
{
    PaintTextBg(ppu, n, y, dst, tag, tagv, NULL, 0, sec, sectag, layers, full);
}

static void PaintUnderWindowed(const gba_ppu *ppu, int n, int y, uint16_t *dst, uint8_t *tag,
                               uint8_t tagv, const uint8_t *allow, uint16_t *sec, uint8_t *sectag,
                               uint8_t *layers, int *full)
{
    PaintTextBg(ppu, n, y, dst, tag, tagv, allow, 1 << n, sec, sectag, layers, full);
}

// Sprites
// =======

static const uint8_t obj_w[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t obj_h[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

// The sprites that can show this frame, in OAM order
static uint8_t obj_list[128];
static int obj_count;

static void ListObjs(const gba_ppu *ppu)
{
    obj_count = 0;
    for (int i = 0; i < 128; i++)
    {
        uint16_t a0 = ppu->oam[i * 4];
        if ((a0 & 0x300) == 0x200 || (a0 >> 14) == 3 || ((a0 >> 10) & 3) == 3)
            continue; // hidden, or a shape or mode that doesn't exist
        obj_list[obj_count++] = i;
    }
}

static void DrawObjs(const gba_ppu *ppu, int y, line_buf *out)
{
    uint16_t dispcnt = IO16(0);
    int map1d = dispcnt & 0x40;
    if (obj_dirty)
    {
        for (int x = 0; x < GBA_W; x++)
            out->color[x] = CLEAR;
        memset(out->prio, 4, GBA_W);
        memset(out->semi, 0, GBA_W);
        memset(out->win, 0, GBA_W);
        obj_dirty = obj_semi = obj_prios = 0;
    }
    if (!(dispcnt & 0x1000) && !(dispcnt & 0x8000))
        return;

    for (int j = 0; j < obj_count; j++)
    {
        int i = obj_list[j];
        const uint16_t *a = ppu->oam + i * 4;
        uint16_t a0 = a[0], a1 = a[1], a2 = a[2];
        int affine = a0 & 0x100;
        if (!affine && (a0 & 0x200))
            continue; // hidden
        int shape = (a0 >> 14) & 3;
        if (shape == 3)
            continue;
        int mode = (a0 >> 10) & 3;
        if (mode == 3)
            continue;
        int w = obj_w[shape][a1 >> 14], h = obj_h[shape][a1 >> 14];
        int bw = w, bh = h; // the box it covers on screen
        if (affine && (a0 & 0x200))
        {
            bw *= 2;
            bh *= 2;
        }
        int oy = a0 & 0xFF;
        if (oy + bh > 256)
            oy -= 256;
        if (y < oy || y >= oy + bh)
            continue;
        int ox = a1 & 0x1FF;
        if (ox >= 256)
            ox -= 512;

        int bpp8 = a0 & 0x2000;
        int tile = a2 & 0x3FF;
        int prio = (a2 >> 10) & 3;
        int pal = (a2 >> 12) << 4;

        int pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine)
        {
            int p = (a1 >> 9) & 31;
            pa = (int16_t)ppu->oam[p * 16 + 3];
            pb = (int16_t)ppu->oam[p * 16 + 7];
            pc = (int16_t)ppu->oam[p * 16 + 11];
            pd = (int16_t)ppu->oam[p * 16 + 15];
        }
        int row = y - oy;
        for (int bx = 0; bx < bw; bx++)
        {
            int x = ox + bx;
            if (x < 0 || x >= GBA_W)
                continue;
            int tx, ty;
            if (affine)
            {
                // Texture coordinates around the sprite's center, 8.8 fixed
                int dx = bx - bw / 2, dy = row - bh / 2;
                tx = ((pa * dx + pb * dy) >> 8) + w / 2;
                ty = ((pc * dx + pd * dy) >> 8) + h / 2;
                if (tx < 0 || tx >= w || ty < 0 || ty >= h)
                    continue;
            }
            else
            {
                tx = (a1 & 0x1000) ? w - 1 - bx : bx;
                ty = (a1 & 0x2000) ? h - 1 - row : row;
            }
            // Tile number in 32-byte units
            int t;
            if (map1d)
                t = tile + ((ty >> 3) * (w >> 3) + (tx >> 3)) * (bpp8 ? 2 : 1);
            else
                t = tile + (ty >> 3) * 32 + (tx >> 3) * (bpp8 ? 2 : 1);
            t &= 0x3FF;
            uint32_t base = 0x10000 + t * 32;
            int index;
            if (bpp8)
                index = ppu->vram[(base + (ty & 7) * 8 + (tx & 7)) & 0x17FFF];
            else
            {
                index = (ppu->vram[(base + (ty & 7) * 4 + ((tx & 7) >> 1)) & 0x17FFF] >> ((tx & 1) * 4)) & 15;
                if (index)
                    index |= pal;
            }
            if (!index)
                continue;
            if (mode == 2)
            {
                out->win[x] = 1;
                obj_dirty = 1;
                continue;
            }
            if (!(dispcnt & 0x1000))
                continue;
            if (prio < out->prio[x])
            {
                out->color[x] = ppu->pltt[0x100 + index] & 0x7FFF;
                out->prio[x] = prio;
                out->semi[x] = mode == 1;
                obj_dirty = 1;
                obj_semi |= mode == 1;
                obj_prios |= 1 << prio;
            }
        }
    }
}

// Mixing
// ======

static int InRange(int v, int a, int b)
{
    return a <= b ? v >= a && v < b : v >= a || v < b;
}

// Sets the pixels a window's horizontal range covers (left <= x < right,
// wrapping around when right < left)
static void WindowSpan(uint8_t *allow, uint16_t h, uint8_t mask)
{
    int a = h >> 8, b = h & 0xFF;
    for (int x = 0; x < GBA_W; x++)
        if (InRange(x, a, b))
            allow[x] = mask;
}

static uint8_t all_layers[GBA_W];

static uint16_t Blend(uint16_t a, uint16_t b, int eva, int evb)
{
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return r | (g << 5) | (bl << 10);
}

// Brightness fades go through a table of the 32 channel levels
static uint8_t fade_lut[32];
static int fade_evy = -1, fade_up;

static uint16_t Fade(uint16_t c, int evy, int up)
{
    if (evy != fade_evy || up != fade_up)
    {
        for (int i = 0; i < 32; i++)
            fade_lut[i] = up ? i + (31 - i) * evy / 16 : i - i * evy / 16;
        fade_evy = evy;
        fade_up = up;
    }
    return fade_lut[c & 31] | (fade_lut[(c >> 5) & 31] << 5) | (fade_lut[(c >> 10) & 31] << 10);
}

void GbaPpuDraw(const gba_ppu *ppu, uint16_t *out)
{
    uint16_t dispcnt = IO16(0);
    if (dispcnt & 0x80)
    {
        for (int i = 0; i < GBA_W * GBA_H; i++)
            out[i] = 0x7FFF; // forced blank shows white
        return;
    }
    int mode = dispcnt & 7;
    uint16_t win0h = IO16(0x40), win1h = IO16(0x42), win0v = IO16(0x44), win1v = IO16(0x46);
    uint16_t winin = IO16(0x48), winout = IO16(0x4A);
    uint16_t bldcnt = IO16(0x50), bldalpha = IO16(0x52);
    int evy = IO16(0x54) & 31;
    int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31;
    if (evy > 16) evy = 16;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    int windows = dispcnt & 0xE000;
    int effect = (bldcnt >> 6) & 3;
    uint16_t backdrop = ppu->pltt[0] & 0x7FFF;

    // Text backgrounds: all four in mode 0, BG0 and BG1 in mode 1. The
    // enabled ones in drawing order: by priority, then by number.
    int text_bgs = mode == 0 ? 4 : mode == 1 ? 2 : 0;
    int order[4], prio[4], count = 0;
    for (int p = 0; p < 4; p++)
        for (int n = 0; n < text_bgs; n++)
            if ((dispcnt & (0x100 << n)) && (IO16(0x08 + n * 2) & 3) == p)
            {
                prio[count] = p;
                order[count++] = n;
            }

    if (!all_layers[0])
        memset(all_layers, 0x3F, GBA_W);

    ListObjs(ppu);
    for (int y = 0; y < GBA_H; y++)
    {
        DrawObjs(ppu, y, &obj);
        uint16_t *dst = out + y * GBA_W;

        // Which layers each pixel's window lets through (bit 5: effects)
        static uint8_t allow_line[GBA_W];
        const uint8_t *allow = allow_line;
        if (windows)
        {
            memset(allow_line, winout & 0x3F, GBA_W);
            if (dispcnt & 0x8000)
                for (int x = 0; x < GBA_W; x++)
                    if (obj.win[x])
                        allow_line[x] = (winout >> 8) & 0x3F;
            if ((dispcnt & 0x4000) && InRange(y, win1v >> 8, win1v & 0xFF))
                WindowSpan(allow_line, win1h, (winin >> 8) & 0x3F);
            if ((dispcnt & 0x2000) && InRange(y, win0v >> 8, win0v & 0xFF))
                WindowSpan(allow_line, win0h, winin & 0x3F);
        }
        else
            allow = all_layers;

        static uint8_t tag[GBA_W], sectag[GBA_W], layers[GBA_W];
        static uint16_t sec[GBA_W];
        int blend = effect == 1 || obj_semi;
        if (!blend)
        {
            // Only the top layer matters: paint the backgrounds back to
            // front, then put the sprites in at their priority (above
            // backgrounds of the same one). Each pixel's tag is the
            // priority and layer on top.
            for (int x = 0; x < GBA_W; x++)
                dst[x] = backdrop;
            memset(tag, BACK_TAG, GBA_W);
            for (int k = count - 1; k >= 0; k--)
            {
                uint8_t tagv = (prio[k] << 3) | order[k];
                if (windows)
                    PaintWindowed(ppu, order[k], y, dst, tag, tagv, allow);
                else
                    PaintTagged(ppu, order[k], y, dst, tag, tagv);
            }
            if (obj_dirty)
                for (int x = 0; x < GBA_W; x++)
                    if (obj.color[x] != CLEAR && (allow[x] & 0x10) && obj.prio[x] <= (tag[x] >> 3))
                    {
                        dst[x] = obj.color[x];
                        tag[x] = (obj.prio[x] << 3) | L_OBJ;
                    }
        }
        else
        {
            // Alpha blending needs the top two layers: paint front to back,
            // each priority's sprites first, until every pixel has two.
            // What is left gets the backdrop.
            memset(layers, 0, GBA_W);
            int full = 0, k = 0;
            for (int p = 0; p < 4 && full < GBA_W; p++)
            {
                if (obj_prios & (1 << p))
                    for (int x = 0; x < GBA_W; x++)
                    {
                        if (obj.color[x] == CLEAR || obj.prio[x] != p || !(allow[x] & 0x10))
                            continue;
                        if (layers[x] == 0)
                        {
                            dst[x] = obj.color[x];
                            tag[x] = (p << 3) | L_OBJ;
                            layers[x] = 1;
                        }
                        else if (layers[x] == 1)
                        {
                            sec[x] = obj.color[x];
                            sectag[x] = (p << 3) | L_OBJ;
                            layers[x] = 2;
                            full++;
                        }
                    }
                for (; k < count && prio[k] == p && full < GBA_W; k++)
                {
                    uint8_t tagv = (p << 3) | order[k];
                    if (windows)
                        PaintUnderWindowed(ppu, order[k], y, dst, tag, tagv, allow, sec, sectag, layers, &full);
                    else
                        PaintUnder(ppu, order[k], y, dst, tag, tagv, sec, sectag, layers, &full);
                }
                for (; k < count && prio[k] == p; k++)
                    ;
            }
            if (full < GBA_W)
                for (int x = 0; x < GBA_W; x++)
                {
                    if (layers[x] == 0)
                    {
                        dst[x] = backdrop;
                        tag[x] = BACK_TAG;
                    }
                    if (layers[x] < 2)
                    {
                        sec[x] = backdrop;
                        sectag[x] = BACK_TAG;
                    }
                }
        }

        if (blend)
        {
            for (int x = 0; x < GBA_W; x++)
            {
                if (!(allow[x] & 0x20))
                    continue;
                int l0 = tag[x] & 7, l1 = sectag[x] & 7;
                int top1 = bldcnt & (1 << l0);
                int bot2 = (bldcnt >> 8) & (1 << l1);
                if (l0 == L_OBJ && obj.semi[x] && bot2)
                    dst[x] = Blend(dst[x], sec[x], eva, evb);
                else if (effect == 1 && top1 && bot2)
                    dst[x] = Blend(dst[x], sec[x], eva, evb);
                else if (effect >= 2 && top1)
                    dst[x] = Fade(dst[x], evy, effect == 2);
            }
        }
        else if (effect >= 2 && evy)
            for (int x = 0; x < GBA_W; x++)
                if ((allow[x] & 0x20) && (bldcnt & (1 << (tag[x] & 7))))
                    dst[x] = Fade(dst[x], evy, effect == 2);
    }
}
