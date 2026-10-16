// A software GBA picture processor (see gbappu.h), written from the GBA's
// documented register layout (GBATEK). One scanline at a time: each
// background and the sprites are drawn into their own line buffers, then
// mixed per pixel by priority, windows and color effects, as the hardware
// does.

#include <string.h>

#include "gbappu.h"

#define IO16(o) (*(const uint16_t *)(ppu->io + (o)))

// A layer's pixel on this line: palette color (0x8000 = transparent)
#define CLEAR 0x8000

enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BACK };

typedef struct {
    uint16_t color[GBA_W]; // BGR555, or CLEAR
    uint8_t prio[GBA_W];   // sprites only
    uint8_t semi[GBA_W];   // sprites only: semi-transparent
    uint8_t win[GBA_W];    // sprites only: inside the OBJ window
} line_buf;

static line_buf bg[4], obj;

// Backgrounds
// ===========

static void DrawTextBg(const gba_ppu *ppu, int n, int y, line_buf *out)
{
    uint16_t cnt = IO16(0x08 + n * 2);
    int hofs = IO16(0x10 + n * 4) & 0x1FF;
    int vofs = IO16(0x12 + n * 4) & 0x1FF;
    const uint8_t *chars = ppu->vram + ((cnt >> 2) & 3) * 0x4000;
    uint32_t screen = ((cnt >> 8) & 0x1F) * 0x800;
    int size = cnt >> 14;
    int bpp8 = cnt & 0x80;
    int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;

    int sy = (y + vofs) & (h - 1);
    for (int x = 0; x < GBA_W; x++)
    {
        int sx = (x + hofs) & (w - 1);
        // 32x32-tile screen blocks, laid out left to right, then down
        int block = (sx >> 8) + ((sy >> 8) * (w >> 8));
        uint32_t at = screen + block * 0x800 + (((sy & 255) >> 3) * 32 + ((sx & 255) >> 3)) * 2;
        if (at >= 0x10000)
        {
            out->color[x] = CLEAR;
            continue;
        }
        uint16_t e = ppu->vram[at] | (ppu->vram[at + 1] << 8);
        int tx = sx & 7, ty = sy & 7;
        if (e & 0x400)
            tx = 7 - tx;
        if (e & 0x800)
            ty = 7 - ty;
        int tile = e & 0x3FF;
        uint32_t off;
        int index;
        if (bpp8)
        {
            off = (chars - ppu->vram) + tile * 64 + ty * 8 + tx;
            index = off < 0x10000 ? ppu->vram[off] : 0;
        }
        else
        {
            off = (chars - ppu->vram) + tile * 32 + ty * 4 + (tx >> 1);
            index = off < 0x10000 ? (ppu->vram[off] >> ((tx & 1) * 4)) & 15 : 0;
            if (index)
                index |= (e >> 12) << 4;
        }
        out->color[x] = index ? ppu->pltt[index] & 0x7FFF : CLEAR;
    }
}

// Sprites
// =======

static const uint8_t obj_w[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t obj_h[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static void DrawObjs(const gba_ppu *ppu, int y, line_buf *out)
{
    uint16_t dispcnt = IO16(0);
    int map1d = dispcnt & 0x40;
    for (int x = 0; x < GBA_W; x++)
    {
        out->color[x] = CLEAR;
        out->prio[x] = 4;
        out->semi[x] = 0;
        out->win[x] = 0;
    }
    if (!(dispcnt & 0x1000) && !(dispcnt & 0x8000))
        return;

    for (int i = 0; i < 128; i++)
    {
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
                continue;
            }
            if (!(dispcnt & 0x1000))
                continue;
            if (prio < out->prio[x])
            {
                out->color[x] = ppu->pltt[0x100 + index] & 0x7FFF;
                out->prio[x] = prio;
                out->semi[x] = mode == 1;
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

static uint16_t Fade(uint16_t c, int evy, int up)
{
    int ch[3] = { c & 31, (c >> 5) & 31, (c >> 10) & 31 };
    for (int i = 0; i < 3; i++)
        ch[i] = up ? ch[i] + (31 - ch[i]) * evy / 16 : ch[i] - ch[i] * evy / 16;
    return ch[0] | (ch[1] << 5) | (ch[2] << 10);
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

    // Text backgrounds: all four in mode 0, BG0 and BG1 in mode 1
    int text_bgs = mode == 0 ? 4 : mode == 1 ? 2 : 0;

    for (int y = 0; y < GBA_H; y++)
    {
        int on[4] = { 0 };
        for (int n = 0; n < text_bgs; n++)
            if (dispcnt & (0x100 << n))
            {
                on[n] = 1;
                DrawTextBg(ppu, n, y, &bg[n]);
            }
        DrawObjs(ppu, y, &obj);
        int in0v = InRange(y, win0v >> 8, win0v & 0xFF);
        int in1v = InRange(y, win1v >> 8, win1v & 0xFF);

        uint16_t *dst = out + y * GBA_W;
        for (int x = 0; x < GBA_W; x++)
        {
            int allow = 0x3F;
            if (windows)
            {
                if ((dispcnt & 0x2000) && in0v && InRange(x, win0h >> 8, win0h & 0xFF))
                    allow = winin & 0x3F;
                else if ((dispcnt & 0x4000) && in1v && InRange(x, win1h >> 8, win1h & 0xFF))
                    allow = (winin >> 8) & 0x3F;
                else if ((dispcnt & 0x8000) && obj.win[x])
                    allow = (winout >> 8) & 0x3F;
                else
                    allow = winout & 0x3F;
            }

            // The top two layers here
            int layer[2] = { L_BACK, L_BACK };
            uint16_t color[2] = { backdrop, backdrop };
            int found = 0;
            for (int p = 0; p < 4 && found < 2; p++)
            {
                if ((allow & 0x10) && obj.color[x] != CLEAR && obj.prio[x] == p)
                {
                    layer[found] = L_OBJ;
                    color[found++] = obj.color[x];
                }
                for (int n = 0; n < 4 && found < 2; n++)
                {
                    if (!on[n] || !(allow & (1 << n)) || bg[n].color[x] == CLEAR)
                        continue;
                    if ((IO16(0x08 + n * 2) & 3) != p)
                        continue;
                    layer[found] = n;
                    color[found++] = bg[n].color[x];
                }
            }

            uint16_t c = color[0];
            if (allow & 0x20)
            {
                int top1 = bldcnt & (1 << layer[0]);
                int bot2 = (bldcnt >> 8) & (1 << layer[1]);
                if (layer[0] == L_OBJ && obj.semi[x] && bot2)
                    c = Blend(color[0], color[1], eva, evb);
                else if (effect == 1 && top1 && bot2)
                    c = Blend(color[0], color[1], eva, evb);
                else if (effect >= 2 && top1)
                    c = Fade(color[0], evy, effect == 2);
            }
            dst[x] = c;
        }
    }
}
