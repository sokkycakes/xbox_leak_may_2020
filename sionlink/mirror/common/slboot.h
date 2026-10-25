// Booting a received program (Download Play), shared by the ARM9 and ARM7.
//
// The Sion sends a packed image, not the whole .nds: the 0x200-byte header
// unchanged, then the ARM9 binary (padded to 4 bytes), then the ARM7 binary.
// The DS stages it in main RAM at SLBOOT_STAGE and boots it:
//
//   ARM9: checks the header, copies it to 0x02FFFE00. If the ARM7 binary
//         loads into ARM7 RAM (IWRAM/shared WRAM, as retail programs often
//         do), copies it into VRAM banks C+D and hands those to the ARM7 at
//         0x06000000. Passes the ARM7 an sl_boot over FIFO_USER_01.
//   ARM7: IWRAM case: copies a small stub to SLBOOT_STUB7 (VRAM) and runs
//         it, which copies the binary from VRAM into ARM7 RAM. Either way it
//         then sets sync out = SLBOOT_SYNC_ARM7_READY and waits.
//   ARM9: (from ITCM) moves its binary into place, then the ARM7 binary if
//         that loads into main RAM, cleans caches, sync out = ARM9_COPIED.
//   ARM7: sync out = 0, jumps.   ARM9: sees 0, sync out = 0, jumps.

#ifndef SLBOOT_H
#define SLBOOT_H

#include <stdint.h>

#define SLBOOT_SYNC_ARM7_READY  0xA
#define SLBOOT_SYNC_ARM9_COPIED 0xB

#define SLBOOT_STAGE     0x02060000u // packed image; the heap ends here
#define SLBOOT_STAGE_END 0x023FE000u // keep clear of the header and system area
#define SLBOOT_VRAM7     0x06000000u // VRAM C+D as the ARM7 sees them
#define SLBOOT_STUB7     0x0603F000u // last 4 KB of that: the ARM7 copy stub
#define SLBOOT_VRAM7_MAX (SLBOOT_STUB7 - SLBOOT_VRAM7)

typedef struct {
    uint32_t src7;   // 0: the ARM9 puts the ARM7 binary in main RAM itself
    uint32_t dest7, size7, entry7;
} sl_boot;

#endif
