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
#define MIRROR_FLAG_LZ   4  // live link: the body is packed (mirror_lz.h)
#define MIRROR_FLAG_MENU 8  // live link: the game's menu (MREC_MENU), nothing else; read even
                            // while the DS waits for a full bundle

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
    MREC_MENU  = 9,  // mirror_menu, then its text (see below); only to a DS that asks for it
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

// MREC_MENU: the game's menu is up on the TV (the game stands still), and the DS shows it on
// its lower screen in its own look, rows in slots MIRROR_MENU_ROW_H high from
// MIRROR_MENU_ROW_Y. A tap on a slot comes back as the touch in the DS's reports. After
// mirror_menu come NUL-ended UTF-8 strings: the game's title, the question (empty on the
// list), what A and B do, then each row's label and value; zeros to a multiple of 4.
// open = 0: the menu closed; a full bundle (the game's screens again) follows.
typedef struct {
    uint8_t open;
    uint8_t rows;
    uint8_t firstSlot;  // the slot the first row is in (a question goes above)
    uint8_t focus;      // the row with the focus
    uint32_t disabled;  // a bit per row: shown greyed, does nothing
} mirror_menu;
#define MIRROR_MENU_ROW_Y 28
#define MIRROR_MENU_ROW_H 18
#define MIRROR_MENU_SLOTS 8
#define MIRROR_MENU_X0    14    // the rows' boxes, across
#define MIRROR_MENU_X1    242

// Live link, DS -> Sion, once a frame: the pad and touch screen, and how the DS is doing.
#define MIRROR_INPUT_MAGIC 0x4E494C53u  // "SLIN"
typedef struct {
    uint32_t magic;
    uint32_t keys;      // libnds keysHeld(): bit set = held (KEY_A = bit 0 ... KEY_TOUCH = bit 12)
    uint16_t touchX, touchY;
    uint32_t applied;   // bundles applied so far, | MIRROR_INPUT_NEED_FULL
    // what the DS has run into so far (wrapping counters), for the Sion's log
    uint16_t damaged, noMemory;   // bundles that came in damaged, bundles it had no memory for
    uint16_t fulls, stalls;       // full refreshes applied, times the picture stopped with no data coming
    // how the DSpico link is doing (wrapping counters; times in 1/32768 s)
    uint32_t silent;              // event reads the DSpico didn't answer
    uint16_t irqReads, timedReads;  // event reads because it raised its IRQ line, because time went by
    uint16_t recovered, restarted;  // lost OUT completions recovered, OUT transfers armed again
    uint32_t blocks;              // 512-byte blocks taken
    uint32_t armedTime;           // time those transfers waited for their data
    uint16_t busyMax;             // longest the DS went without looking at USB since its last report
    uint16_t timedHits;           // timed event reads that found events (the IRQ line didn't say)
    uint32_t shownFrame;          // the frame (mirror_hdr.frame) of the bundle on screen now, 0 before
                                  // the first; the DS reports each new one, so the Sion knows how far
                                  // behind the game the DS is
} mirror_input;
// the DS lost track of the stream (a damaged bundle, or it ran out of memory):
// send a full bundle (MIRROR_FLAG_FULL) so it can start over from cleared hardware
#define MIRROR_INPUT_NEED_FULL 0x80000000u
// this DS shows the game's menu (MREC_MENU)
#define MIRROR_INPUT_MENUS 0x40000000u
#define MIRROR_INPUT_COUNT 0x3FFFFFFFu   // the bundles applied

#define MIRROR_ROW   32   // palette/OAM granularity
#define MIRROR_BLOCK 512  // VRAM granularity

#endif
