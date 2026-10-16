// Booting a received .nds (Download Play), shared by the ARM9 and ARM7 sides.
//
// The ARM9 has the whole file in main RAM. It checks the header, copies it
// to 0x02FFFE00, and passes the ARM7 a pointer to sl_boot over FIFO_USER_01.
// Then, over the IPC sync register:
//   ARM7: copies its binary into place, sync out = SLBOOT_SYNC_ARM7_COPIED
//   ARM9: (from ITCM) copies its binary, cleans caches, out = SLBOOT_SYNC_ARM9_COPIED
//   ARM7: out = 0;  ARM9: out = 0, jumps;  ARM7: jumps
// so both start with the sync register at 0, as after a real boot.

#ifndef SLBOOT_H
#define SLBOOT_H

#include <stdint.h>

#define SLBOOT_SYNC_ARM7_COPIED 0xA
#define SLBOOT_SYNC_ARM9_COPIED 0xB

typedef struct {
    uint32_t src7, dest7, size7, entry7;
} sl_boot;

#endif
