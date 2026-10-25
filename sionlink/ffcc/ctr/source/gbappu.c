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
    uint16_t color[GBA_WIDE_MAX]; // BGR555, or CLEAR
    uint8_t prio[GBA_WIDE_MAX];   // sprites only
    uint8_t semi[GBA_WIDE_MAX];   // sprites only: semi-transparent
    uint8_t win[GBA_WIDE_MAX];    // sprites only: inside the OBJ window
} line_buf;

static line_buf obj;
// What the sprites drew on this line: anything, and semi-transparent pixels
static int obj_dirty = 1, obj_semi, obj_prios; // obj_prios: a bit per priority drawn

// This frame's width, and the extra pixels in it
static int width = GBA_W, extra;

// Wide layout
// ===========

#define CELLS_X 31 // a background's cells that can show: 30 + 1 when scrolled
#define CELLS_Y 21
#define MAX_PIECES 255

enum { EMPTY, PATTERN, PIECE };

typedef struct {
    int x0, x1, y0, y1;     // on the GBA screen
    int k0, k1, j0, j1;     // cells, inclusive
    int area, layer;
    int move;               // pixels to the right
    int split;              // stretched: the GBA x after the repeated column, else -1
} piece;

typedef struct {
    int on, hofs, vofs, fx, fy, w, h, bpp8;
    uint32_t chars, screen;
    int all;                  // one piece covering it all (a map): its move, else -1
    uint8_t kind[CELLS_Y][CELLS_X];
    uint8_t owner[CELLS_Y][CELLS_X]; // piece number + 1
    uint16_t entry[CELLS_Y][CELLS_X];
    uint8_t row_pattern[CELLS_Y];
} layer;

static layer layers_wide[4];
static piece pieces[MAX_PIECES];
static int npieces;
static int plain;            // no layout this frame: everything moves extra / 2
static int obj_move[128];

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
//
// The span: len pixels from GBA screen x src on (any value: the map wraps)
// go to x dx on. With owner, only that piece's cells are painted.
static inline __attribute__((always_inline)) void PaintTextBg(
    const gba_ppu *ppu, int n, int y, int dx, int len, int src, int owner,
    uint16_t *dst, uint8_t *tag, uint8_t tagv,
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
    const layer *l = &layers_wide[n];
    int cy = (y + l->fy) >> 3;
    int sx = (src + hofs) & (w - 1);
    int end = dx + len;
    for (int x = dx; x < end;)
    {
        int first = sx & 7;
        int count = 8 - first;
        if (x + count > end)
            count = end - x;
        uint32_t at = row_at + (sx >> 8) * 0x800 + ((sx & 255) >> 3) * 2;
        int x0 = x;
        int cx = (src + (x - dx) + l->fx) >> 3;
        x += count;
        sx = (sx + count) & (w - 1);
        if (at >= 0x10000)
            continue;
        if (owner && (cx < 0 || cx >= CELLS_X || l->owner[cy][cx] != owner))
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

// Where a background's line goes, and how (see PaintTextBg)
typedef struct {
    uint16_t *dst;
    uint8_t *tag;
    uint8_t tagv;
    const uint8_t *allow;
    uint16_t *sec;
    uint8_t *sectag;
    uint8_t *layers;
    int *full;
} paint_to;

static void PaintSpan(const gba_ppu *ppu, int n, int y, int dx, int len, int src, int owner,
                      const paint_to *t)
{
    if (len <= 0)
        return;
    // Each way of painting gets its own copy of the loop
    if (!t->sec)
    {
        if (t->allow)
            PaintTextBg(ppu, n, y, dx, len, src, owner, t->dst, t->tag, t->tagv, t->allow, 1 << n,
                        NULL, NULL, NULL, NULL);
        else
            PaintTextBg(ppu, n, y, dx, len, src, owner, t->dst, t->tag, t->tagv, NULL, 0,
                        NULL, NULL, NULL, NULL);
    }
    else if (t->allow)
        PaintTextBg(ppu, n, y, dx, len, src, owner, t->dst, t->tag, t->tagv, t->allow, 1 << n,
                    t->sec, t->sectag, t->layers, t->full);
    else
        PaintTextBg(ppu, n, y, dx, len, src, owner, t->dst, t->tag, t->tagv, NULL, 0,
                    t->sec, t->sectag, t->layers, t->full);
}

// Where the pattern at GBA x src comes from on cell row cy: that cell if
// it is pattern, else one a period of the backdrop (64 pixels) or a few
// away. NO_PATTERN if none is.
#define NO_PATTERN (-100000)
static int PatternAt(const layer *l, int cy, int src)
{
    static const int away[] = { 0, -64, 64, -128, 128, -192, 192, -256, 256 };
    for (int i = 0; i < 9; i++)
    {
        int s = src + away[i];
        int cx = (s + l->fx) >> 3;
        if (cx >= 0 && cx < CELLS_X && l->kind[cy][cx] == PATTERN)
            return s;
    }
    return NO_PATTERN;
}

// Paints the pattern of a background's line from x to end: each tile from
// where PatternAt says, in runs that come from one place
static void PaintPattern(const gba_ppu *ppu, int n, int y, int x, int end, const paint_to *t)
{
    const layer *l = &layers_wide[n];
    int cy = (y + l->fy) >> 3;
    int run_x = x, run_src = NO_PATTERN, run_len = 0;
    while (x < end)
    {
        int src = x - extra / 2;
        int count = 8 - ((src + l->hofs) & 7);
        if (x + count > end)
            count = end - x;
        int from = PatternAt(l, cy, src);
        if (from != NO_PATTERN && run_src != NO_PATTERN && from == run_src + run_len)
            run_len += count;
        else
        {
            if (run_src != NO_PATTERN)
                PaintSpan(ppu, n, y, run_x, run_len, run_src, 0, t);
            run_x = x;
            run_src = from;
            run_len = count;
        }
        x += count;
    }
    if (run_src != NO_PATTERN)
        PaintSpan(ppu, n, y, run_x, run_len, run_src, 0, t);
}

// Paints a background's line: each piece where it moved to, then the
// pattern across what their boxes leave
static void PaintLayer(const gba_ppu *ppu, int n, int y, const paint_to *t)
{
    const layer *l = &layers_wide[n];
    if (plain || l->all != -1)
    {
        PaintSpan(ppu, n, y, 0, width, plain ? -(extra / 2) : -l->all, 0, t);
        return;
    }
    static int16_t box[MAX_PIECES][2];
    int boxes = 0;
    for (int i = 0; i < npieces; i++)
    {
        const piece *p = &pieces[i];
        if (p->layer != n || y < p->y0 || y >= p->y1)
            continue;
        int x0, x1;
        if (p->split < 0)
        {
            PaintSpan(ppu, n, y, p->x0 + p->move, p->x1 - p->x0, p->x0, i + 1, t);
            x0 = p->x0 + p->move;
            x1 = p->x1 + p->move;
        }
        else
        {
            // Stretched: the left part, the column before the split over
            // and over, the right part
            PaintSpan(ppu, n, y, p->x0, p->split - p->x0, p->x0, i + 1, t);
            for (int x = p->split; x < p->split + extra; x += 8)
                PaintSpan(ppu, n, y, x, 8, p->split - 8, i + 1, t);
            PaintSpan(ppu, n, y, p->split + extra, p->x1 - p->split, p->split, i + 1, t);
            x0 = p->x0;
            x1 = p->x1 + extra;
        }
        // The boxes in order from the left
        int at = boxes++;
        while (at > 0 && box[at - 1][0] > x0)
        {
            box[at][0] = box[at - 1][0];
            box[at][1] = box[at - 1][1];
            at--;
        }
        box[at][0] = x0;
        box[at][1] = x1;
    }
    if (!l->row_pattern[(y + l->fy) >> 3])
        return;
    int x = 0;
    for (int i = 0; i < boxes; i++)
    {
        if (box[i][0] > x)
            PaintPattern(ppu, n, y, x, box[i][0], t);
        if (box[i][1] > x)
            x = box[i][1];
    }
    if (x < width)
        PaintPattern(ppu, n, y, x, width, t);
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
        for (int x = 0; x < width; x++)
            out->color[x] = CLEAR;
        memset(out->prio, 4, width);
        memset(out->semi, 0, width);
        memset(out->win, 0, width);
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
        ox += obj_move[i];

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
            if (x < 0 || x >= width)
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


// The layout
// ----------

static int EmptyTile(const gba_ppu *ppu, const layer *l, uint16_t e)
{
    int size = l->bpp8 ? 64 : 32;
    uint32_t off = l->chars + (e & 0x3FF) * size;
    if (off + size > 0x10000)
        return 1;
    const uint32_t *p = (const uint32_t *)(ppu->vram + off);
    for (int i = 0; i < size / 4; i++)
        if (p[i])
            return 0;
    return 1;
}

static int PatternTile(const gba_ppu *ppu, const layer *l, uint16_t e)
{
    uint32_t off = l->chars + (e & 0x3FF) * (l->bpp8 ? 64 : 32);
    for (int i = 0; i < ppu->nback; i++)
        if (off >= ppu->back[i * 2] && off < ppu->back[i * 2 + 1])
            return 1;
    return 0;
}

// How far something from GBA x0 to x1 moves on its own: with the edge it
// touches (half the extra when both), else in proportion to its margins
static int MarginMove(int x0, int x1)
{
    int l = x0 > 0 ? x0 : 0, r = x1 < GBA_W ? GBA_W - x1 : 0;
    if (l + r == 0)
        return extra / 2;
    return (extra * l * 2 + (l + r)) / (2 * (l + r));
}

// How far something from x0 to x1 inside piece p moves
static int MoveWith(const piece *p, int x0, int x1)
{
    if (p->split < 0)
        return p->move;
    if (x1 <= p->split)
        return 0;
    if (x0 >= p->split)
        return extra;
    return extra / 2;
}

// The piece a box overlaps most (the smaller on a tie), or -1
static int Overlapping(int x0, int x1, int y0, int y1)
{
    int best = -1, best_area = 0;
    for (int i = 0; i < npieces; i++)
    {
        const piece *p = &pieces[i];
        int w = (x1 < p->x1 ? x1 : p->x1) - (x0 > p->x0 ? x0 : p->x0);
        int h = (y1 < p->y1 ? y1 : p->y1) - (y0 > p->y0 ? y0 : p->y0);
        if (p->layer < 0 || w <= 0 || h <= 0)
            continue;
        if (w * h > best_area || (w * h == best_area && p->area < pieces[best].area))
        {
            best = i;
            best_area = w * h;
        }
    }
    return best;
}

// Where a piece as wide as the screen can stretch: after a column that
// is the same as the next one all the way down (a window's plain middle),
// the one nearest the middle. -1 if none is.
static int FindSplit(const layer *l, int i)
{
    const piece *p = &pieces[i];
    int best = -1, best_d = 1 << 30;
    for (int k = p->k0 + 1; k + 1 < p->k1; k++)
    {
        int same = 1;
        for (int j = p->j0; j <= p->j1 && same; j++)
        {
            if (l->kind[j][k] == EMPTY && l->kind[j][k + 1] == EMPTY)
                continue;
            same = l->entry[j][k] == l->entry[j][k + 1] && l->owner[j][k] == i + 1
                   && l->owner[j][k + 1] == i + 1;
        }
        int split = (k + 1) * 8 - l->fx;
        int d = split > GBA_W / 2 ? split - GBA_W / 2 : GBA_W / 2 - split;
        if (same && d < best_d)
        {
            best = split;
            best_d = d;
        }
    }
    return best;
}

// Groups a background's cells of kind PIECE into pieces. 0 if there are
// too many.
static int FindPieces(layer *l, int n)
{
    static uint8_t stack[CELLS_X * CELLS_Y][2];
    for (int j = 0; j < CELLS_Y; j++)
        for (int k = 0; k < CELLS_X; k++)
        {
            if (l->kind[j][k] != PIECE || l->owner[j][k])
                continue;
            if (npieces == MAX_PIECES)
                return 0;
            piece *p = &pieces[npieces++];
            p->k0 = p->k1 = k;
            p->j0 = p->j1 = j;
            p->layer = n;
            int top = 0;
            l->owner[j][k] = npieces;
            stack[top][0] = j;
            stack[top++][1] = k;
            while (top)
            {
                --top;
                int cj = stack[top][0], ck = stack[top][1];
                if (ck < p->k0) p->k0 = ck;
                if (ck > p->k1) p->k1 = ck;
                if (cj < p->j0) p->j0 = cj;
                if (cj > p->j1) p->j1 = cj;
                for (int dj = -1; dj <= 1; dj++)
                    for (int dk = -1; dk <= 1; dk++)
                    {
                        int nj = cj + dj, nk = ck + dk;
                        if (nj < 0 || nj >= CELLS_Y || nk < 0 || nk >= CELLS_X
                            || l->kind[nj][nk] != PIECE || l->owner[nj][nk])
                            continue;
                        l->owner[nj][nk] = npieces;
                        stack[top][0] = nj;
                        stack[top++][1] = nk;
                    }
            }
            p->x0 = p->k0 * 8 - l->fx;
            p->x1 = p->k1 * 8 + 8 - l->fx;
            p->y0 = p->j0 * 8 - l->fy;
            p->y1 = p->j1 * 8 + 8 - l->fy;
            if (p->x0 < 0) p->x0 = 0;
            if (p->x1 > GBA_W) p->x1 = GBA_W;
            if (p->y0 < 0) p->y0 = 0;
            if (p->y1 > GBA_H) p->y1 = GBA_H;
            p->area = (p->x1 - p->x0) * (p->y1 - p->y0);
            p->move = 0;
            p->split = -1;
        }
    return 1;
}

static int win_move[2];

static void Layout(const gba_ppu *ppu)
{
    uint16_t dispcnt = IO16(0);
    int mode = dispcnt & 7;
    npieces = 0;
    plain = !extra;
    for (int n = 0; n < 4; n++)
    {
        layer *l = &layers_wide[n];
        uint16_t cnt = IO16(0x08 + n * 2);
        l->on = (dispcnt & (0x100 << n)) && (mode == 0 || (mode == 1 && n < 2));
        l->hofs = IO16(0x10 + n * 4) & 0x1FF;
        l->vofs = IO16(0x12 + n * 4) & 0x1FF;
        l->fx = l->hofs & 7;
        l->fy = l->vofs & 7;
        l->w = (cnt >> 14) & 1 ? 512 : 256;
        l->h = (cnt >> 14) & 2 ? 512 : 256;
        l->bpp8 = cnt & 0x80;
        l->chars = ((cnt >> 2) & 3) * 0x4000;
        l->screen = ((cnt >> 8) & 0x1F) * 0x800;
        l->all = -1;
        if (plain || !l->on)
            continue;

        // What each cell that can show holds
        int same = 1, holes = 0, first = -1;
        memset(l->owner, 0, sizeof(l->owner));
        for (int j = 0; j < CELLS_Y; j++)
            for (int k = 0; k < CELLS_X; k++)
            {
                l->kind[j][k] = EMPTY;
                l->entry[j][k] = 0;
                if ((k == CELLS_X - 1 && !l->fx) || (j == CELLS_Y - 1 && !l->fy))
                    continue;
                int mx = ((l->hofs & ~7) + k * 8) & (l->w - 1);
                int my = ((l->vofs & ~7) + j * 8) & (l->h - 1);
                uint32_t at = l->screen + (my >> 8) * (l->w >> 8) * 0x800 + (mx >> 8) * 0x800
                              + ((my & 255) >> 3) * 64 + ((mx & 255) >> 3) * 2;
                if (at >= 0x10000)
                {
                    holes = 1;
                    continue;
                }
                uint16_t e = *(const uint16_t *)(ppu->vram + at);
                l->entry[j][k] = e;
                if (EmptyTile(ppu, l, e))
                    holes = 1;
                else
                    l->kind[j][k] = PatternTile(ppu, l, e) ? PATTERN : PIECE;
                if (first < 0)
                    first = e;
                else if (e != first)
                    same = 0;
            }
        // A background of one tile all over is a pattern too
        if (same && !holes)
            memset(l->kind, PATTERN, sizeof(l->kind));
        int pattern = 0;
        for (int j = 0; j < CELLS_Y; j++)
        {
            l->row_pattern[j] = 0;
            for (int k = 0; k < CELLS_X; k++)
                l->row_pattern[j] |= l->kind[j][k] == PATTERN;
            pattern |= l->row_pattern[j];
        }

        int before = npieces;
        if (!FindPieces(l, n))
        {
            plain = 1;
            break;
        }
        // Nothing but one piece (a map under a window): it moves with the
        // biggest piece, below
        if (!holes && !pattern && npieces == before + 1)
        {
            pieces[before].layer = -1;
            l->all = before;
        }
    }

    if (plain)
    {
        // Everything moves half the extra
        for (int i = 0; i < 128; i++)
            obj_move[i] = extra / 2;
        win_move[0] = win_move[1] = extra / 2;
        return;
    }

    // Pieces that overlap (a window and the text in it, on another
    // background) form a group, which moves as one: by its box's margins,
    // or, as wide as the screen, stretched where its biggest piece can be
    static uint8_t group[MAX_PIECES];
    for (int i = 0; i < npieces; i++)
        group[i] = i;
    for (int i = 0; i < npieces; i++)
        for (int c = 0; c < i; c++)
        {
            const piece *p = &pieces[i], *q = &pieces[c];
            if (p->layer < 0 || q->layer < 0 || p->x1 <= q->x0 || q->x1 <= p->x0
                || p->y1 <= q->y0 || q->y1 <= p->y0)
                continue;
            int gi = i, gc = c;
            while (group[gi] != gi)
                gi = group[gi];
            while (group[gc] != gc)
                gc = group[gc];
            if (gi != gc)
                group[gi > gc ? gi : gc] = gi > gc ? gc : gi;
        }
    int biggest = -1;
    for (int i = 0; i < npieces; i++)
    {
        int g = i;
        while (group[g] != g)
            g = group[g];
        group[i] = g;
        if (pieces[i].layer >= 0 && (biggest < 0 || pieces[i].area > pieces[biggest].area))
            biggest = i;
    }
    for (int g = 0; g < npieces; g++)
    {
        if (group[g] != g || pieces[g].layer < 0)
            continue;
        int x0 = GBA_W, x1 = 0, big = g;
        for (int i = g; i < npieces; i++)
            if (group[i] == g)
            {
                if (pieces[i].x0 < x0) x0 = pieces[i].x0;
                if (pieces[i].x1 > x1) x1 = pieces[i].x1;
                if (pieces[i].area > pieces[big].area) big = i;
            }
        piece *b = &pieces[big];
        b->split = x0 <= 0 && x1 >= GBA_W ? FindSplit(&layers_wide[b->layer], big) : -1;
        b->move = b->split >= 0 ? 0 : MarginMove(x0, x1);
        for (int i = g; i < npieces; i++)
            if (group[i] == g && i != big)
            {
                pieces[i].move = MoveWith(b, pieces[i].x0, pieces[i].x1);
                pieces[i].split = -1;
            }
    }
    for (int n = 0; n < 4; n++)
    {
        layer *l = &layers_wide[n];
        if (l->all != -1)
            l->all = biggest >= 0 ? MoveWith(&pieces[biggest], 0, GBA_W) : extra / 2;
    }

    // Sprites: those that touch or overlap form a group (a banner of
    // several), which moves with the piece it overlaps most, else by its
    // own margins
    static int16_t ob[128][4];
    static uint8_t og[128];
    for (int j = 0; j < obj_count; j++)
    {
        const uint16_t *a = ppu->oam + obj_list[j] * 4;
        int shape = (a[0] >> 14) & 3;
        int w = obj_w[shape][a[1] >> 14], h = obj_h[shape][a[1] >> 14];
        if ((a[0] & 0x300) == 0x300)
        {
            w *= 2;
            h *= 2;
        }
        int y0 = a[0] & 0xFF;
        if (y0 + h > 256)
            y0 -= 256;
        int x0 = a[1] & 0x1FF;
        if (x0 >= 256)
            x0 -= 512;
        ob[j][0] = x0;
        ob[j][1] = x0 + w;
        ob[j][2] = y0;
        ob[j][3] = y0 + h;
        og[j] = j;
        if (x0 + w <= 0 || x0 >= GBA_W || y0 + h <= 0 || y0 >= GBA_H)
            continue; // off the screen: alone
        for (int c = 0; c < j; c++)
        {
            if (ob[c][1] < x0 || x0 + w < ob[c][0] || ob[c][3] < y0 || y0 + h < ob[c][2]
                || ob[c][1] <= 0 || ob[c][0] >= GBA_W || ob[c][3] <= 0 || ob[c][2] >= GBA_H)
                continue;
            int gj = j, gc = c;
            while (og[gj] != gj)
                gj = og[gj];
            while (og[gc] != gc)
                gc = og[gc];
            if (gj != gc)
                og[gj > gc ? gj : gc] = gj > gc ? gc : gj;
        }
    }
    for (int j = 0; j < obj_count; j++)
    {
        int g = j;
        while (og[g] != g)
            g = og[g];
        og[j] = g;
    }
    for (int g = 0; g < obj_count; g++)
    {
        if (og[g] != g)
            continue;
        int x0 = ob[g][0], x1 = ob[g][1], y0 = ob[g][2], y1 = ob[g][3];
        for (int j = g + 1; j < obj_count; j++)
            if (og[j] == g)
            {
                if (ob[j][0] < x0) x0 = ob[j][0];
                if (ob[j][1] > x1) x1 = ob[j][1];
                if (ob[j][2] < y0) y0 = ob[j][2];
                if (ob[j][3] > y1) y1 = ob[j][3];
            }
        int p = Overlapping(x0, x1, y0, y1);
        int move = p >= 0                     ? MoveWith(&pieces[p], x0, x1)
                   : x1 > 0 && x0 < GBA_W ? MarginMove(x0, x1)
                                          : extra / 2;
        for (int j = g; j < obj_count; j++)
            if (og[j] == g)
                obj_move[obj_list[j]] = move;
    }


    // Windows
    for (int i = 0; i < 2; i++)
    {
        uint16_t wh = IO16(0x40 + i * 2), wv = IO16(0x44 + i * 2);
        int x0 = wh >> 8, x1 = wh & 0xFF, y0 = wv >> 8, y1 = wv & 0xFF;
        if (x1 > GBA_W || x1 < x0)
            x1 = GBA_W;
        if (y1 > GBA_H || y1 < y0)
            y1 = GBA_H;
        int p = Overlapping(x0, x1, y0, y1);
        win_move[i] = p >= 0 ? MoveWith(&pieces[p], x0, x1) : MarginMove(x0, x1);
    }
}

// Mixing
// ======

static int InRange(int v, int a, int b)
{
    return a <= b ? v >= a && v < b : v >= a || v < b;
}

// Sets the pixels a window's horizontal range covers (left <= x < right,
// wrapping around when right < left), moved as the layout says. Past the
// GBA screen's edges, a window covers what its edge column does.
static void WindowSpan(uint8_t *allow, uint16_t h, int move, uint8_t mask)
{
    int a = h >> 8, b = h & 0xFF;
    for (int x = 0; x < width; x++)
    {
        int gx = x - move;
        gx = gx < 0 ? 0 : gx >= GBA_W ? GBA_W - 1 : gx;
        if (InRange(gx, a, b))
            allow[x] = mask;
    }
}

static uint8_t all_layers[GBA_WIDE_MAX];

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
    extra = ppu->extra > 0 && ppu->extra <= GBA_WIDE_MAX - GBA_W ? ppu->extra & ~7 : 0;
    width = GBA_W + extra;
    if (dispcnt & 0x80)
    {
        for (int i = 0; i < width * GBA_H; i++)
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
        memset(all_layers, 0x3F, GBA_WIDE_MAX);

    ListObjs(ppu);
    Layout(ppu);
    obj_dirty = 1;
    for (int y = 0; y < GBA_H; y++)
    {
        DrawObjs(ppu, y, &obj);
        uint16_t *dst = out + y * width;

        // Which layers each pixel's window lets through (bit 5: effects)
        static uint8_t allow_line[GBA_WIDE_MAX];
        const uint8_t *allow = allow_line;
        if (windows)
        {
            memset(allow_line, winout & 0x3F, width);
            if (dispcnt & 0x8000)
                for (int x = 0; x < width; x++)
                    if (obj.win[x])
                        allow_line[x] = (winout >> 8) & 0x3F;
            if ((dispcnt & 0x4000) && InRange(y, win1v >> 8, win1v & 0xFF))
                WindowSpan(allow_line, win1h, win_move[1], (winin >> 8) & 0x3F);
            if ((dispcnt & 0x2000) && InRange(y, win0v >> 8, win0v & 0xFF))
                WindowSpan(allow_line, win0h, win_move[0], winin & 0x3F);
        }
        else
            allow = all_layers;

        static uint8_t tag[GBA_WIDE_MAX], sectag[GBA_WIDE_MAX], layers[GBA_WIDE_MAX];
        static uint16_t sec[GBA_WIDE_MAX];
        int blend = effect == 1 || obj_semi;
        if (!blend)
        {
            // Only the top layer matters: paint the backgrounds back to
            // front, then put the sprites in at their priority (above
            // backgrounds of the same one). Each pixel's tag is the
            // priority and layer on top.
            for (int x = 0; x < width; x++)
                dst[x] = backdrop;
            memset(tag, BACK_TAG, width);
            for (int k = count - 1; k >= 0; k--)
            {
                uint8_t tagv = (prio[k] << 3) | order[k];
                paint_to t = { dst, tag, tagv, windows ? allow : NULL, NULL, NULL, NULL, NULL };
                PaintLayer(ppu, order[k], y, &t);
            }
            if (obj_dirty)
                for (int x = 0; x < width; x++)
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
            memset(layers, 0, width);
            int full = 0, k = 0;
            for (int p = 0; p < 4 && full < width; p++)
            {
                if (obj_prios & (1 << p))
                    for (int x = 0; x < width; x++)
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
                for (; k < count && prio[k] == p && full < width; k++)
                {
                    uint8_t tagv = (p << 3) | order[k];
                    paint_to t = { dst, tag, tagv, windows ? allow : NULL, sec, sectag, layers, &full };
                    PaintLayer(ppu, order[k], y, &t);
                }
                for (; k < count && prio[k] == p; k++)
                    ;
            }
            if (full < width)
                for (int x = 0; x < width; x++)
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
            for (int x = 0; x < width; x++)
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
            for (int x = 0; x < width; x++)
                if ((allow[x] & 0x20) && (bldcnt & (1 << (tag[x] & 7))))
                    dst[x] = Fade(dst[x], evy, effect == 2);
    }
}
