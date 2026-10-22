// Sion Link screen mirroring: one bundle per emulated frame.
//
// A bundle replays on a real DS what the emulated game did to its display
// hardware. It holds the 3D/geometry I/O writes up to the previous VBlank
// start, then the 2D state (registers, VRAM mapping, palettes, OAM, VRAM)
// as of the start of the frame being shown, and the register changes made
// partway down that frame. Both end up on screen in the same frame. It is
// sent once the frame's visible lines are done. Everything is little-endian
// and 4-byte aligned.
#ifndef MIRROR_FORMAT_H
#define MIRROR_FORMAT_H

#include <stdint.h>

#define MIRROR_MAGIC 0x4252494Du  // "MIRB"

typedef struct {
    uint32_t magic;
    uint32_t frame;
    uint32_t size;      // whole bundle, header included
    uint32_t flags;
} mirror_hdr;

#define MIRROR_FLAG_FULL 1  // first bundle: everything is sent
#define MIRROR_FLAG_HOLD 2  // show this frame for two VBlanks (30 fps mode)

enum {
    MREC_END   = 0,
    MREC_GX    = 1,  // u32 run count, then runs: mirror_run + count x u32 values
    MREC_REGS  = 2,  // u32 count, then count x mirror_reg
    MREC_VRAMCNT = 3, // u8 value[9] for banks A..I, 3 bytes padding
    MREC_PAL   = 4,  // u32 count, then count x (u32 offset, 32 bytes)
    MREC_OAM   = 5,  // same layout as MREC_PAL
    MREC_VRAM  = 6,  // u32 count, then count x (u32 bank<<16 | block, 512 bytes)
    MREC_LINES = 7,  // u32 count, then count x mirror_line, sorted by line
    MREC_GXDELTA = 8, // an MREC_GX payload built from the previous bundle's (see below)
};

// MREC_GXDELTA: u32 payload length in words, then ops until it is complete:
//   u32 n (bit 31 clear) followed by n literal words, or
//   u32 0x80000000|n, u32 src: copy n words from word src of the previous
//   bundle's GX payload (the one before this bundle, after its own delta was
//   undone). A bundle without 3D commands leaves no previous payload.

typedef struct {
    uint16_t type;
    uint16_t pad;
    uint32_t len;   // payload bytes after this header
} mirror_rec;

// A run of writes to one address (the geometry FIFO gets long ones).
typedef struct {
    uint16_t off;   // address - 0x04000000
    uint16_t info;  // bits 0-13: value count; bits 14-15: 0 = 32-bit, 1 = 16-bit, 2 = 8-bit
} mirror_run;

// A 2D register value that changes partway down the screen (HBlank effects):
// write it in the HBlank before `line`.
typedef struct {
    uint16_t line;
    uint16_t off;
    uint16_t val;
    uint16_t pad;
} mirror_line;

typedef struct {
    uint16_t off;   // address - 0x04000000
    uint16_t val;
} mirror_reg;

// Live link, DS -> Sion, once a frame: the pad and touch screen.
#define MIRROR_INPUT_MAGIC 0x4E494C53u  // "SLIN"
typedef struct {
    uint32_t magic;
    uint32_t keys;      // libnds keysHeld(): bit set = held (KEY_A = bit 0 ... KEY_TOUCH = bit 12)
    uint16_t touchX, touchY;
    uint32_t applied;   // bundles applied so far, | MIRROR_INPUT_NEED_FULL
} mirror_input;
// the DS lost track of the stream (a damaged bundle, or it ran out of memory):
// send a full bundle (MIRROR_FLAG_FULL) so it can start over from cleared hardware
#define MIRROR_INPUT_NEED_FULL 0x80000000u

#define MIRROR_ROW   32   // palette/OAM granularity
#define MIRROR_BLOCK 512  // VRAM granularity

#endif
