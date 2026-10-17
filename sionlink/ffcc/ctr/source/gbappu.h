// A software GBA picture processor: what the GBA's display hardware would
// draw from its registers, palettes, VRAM and OAM, one 240x160 frame at a
// time. Covers what the FFCC client uses and a little more: mode 0 text
// backgrounds (4 and 8 bpp, every size), regular and affine sprites, the
// OBJ window, WIN0/WIN1, alpha blending (semi-transparent sprites too) and
// brightness fades. Bitmap modes, affine backgrounds and mosaic aren't
// drawn.
//
// It can also draw the picture wider than the GBA's (extra): windows,
// menus and sprites near an edge move out to it, the rest spread out in
// proportion, and the background pattern fills what opens up.

#ifndef GBAPPU_H
#define GBAPPU_H

#include <stdint.h>

#define GBA_W 240
#define GBA_H 160
#define GBA_WIDE_MAX 272 // widest picture GbaPpuDraw draws

typedef struct {
    const uint8_t *io;    // the 0x400 bytes at 0x04000000
    const uint16_t *pltt; // 0x200 halfwords at 0x05000000
    const uint8_t *vram;  // 0x18000 bytes at 0x06000000
    const uint16_t *oam;  // 0x200 halfwords at 0x07000000
    int extra;            // pixels added to the width, a multiple of 8 (0: none)
    const uint32_t *back; // VRAM byte ranges [from, to) of background pattern tiles
    int nback;            // how many ranges
} gba_ppu;

// Draws the frame as BGR555 colors, GBA_W + extra per row
void GbaPpuDraw(const gba_ppu *ppu, uint16_t *out);

#endif
