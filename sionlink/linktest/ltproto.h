// Link Test: what the Sion game and its DS client say inside SL_GAME packets.
//
// The DS sends its input every frame; the Sion sends one snapshot of the
// whole field every frame to all players. Both carry what each side measured
// about the other direction, so each screen shows the link both ways.
// Round trip: the snapshot echoes the stamp of the last input the Sion got
// from each DS and how long the Sion held it, so the DS can subtract that.

#ifndef LTPROTO_H
#define LTPROTO_H

#include <stdint.h>

#define LT_APP_ID       0x5453544Cu // "LTST"
#define LT_MAX_PLAYERS  15
#define LT_FIELD_W      720 // the Sion's screen
#define LT_FIELD_H      480

enum lt_kind {
    LT_INPUT = 1, // DS -> Sion, every frame: lt_input
    LT_SNAP  = 2, // Sion -> all, every frame: lt_snap + lt_player[count]
    LT_NAMES = 3, // Sion -> all, every second: lt_names
};

// One second of what a receiver saw from its sender
typedef struct __attribute__((packed)) {
    uint16_t got;       // packets that arrived
    uint16_t lost;      // sequence numbers that never did
    uint16_t worst_gap; // longest time between two arrivals, ms
    uint16_t rtt_avg;   // round trip, ms (0: none measured)
    uint16_t rtt_max;
} lt_stats;

typedef struct __attribute__((packed)) {
    uint8_t  kind;      // LT_INPUT
    uint8_t  touching;
    uint16_t keys;      // libnds KEY_* held
    uint32_t seq;
    uint32_t stamp;     // DS clock, ms; echoed back in the snapshot
    uint8_t  touch_x, touch_y;
    uint16_t reserved;
    lt_stats seen;      // snapshots this DS got in the last full second
} lt_input;

typedef struct __attribute__((packed)) {
    uint8_t  id;        // player number (sionlinkd's)
    uint8_t  flags;     // bit 0: touching
    uint16_t x, y;      // on the LT_FIELD_W x LT_FIELD_H field
    uint16_t echo_hold; // ms the Sion held echo_stamp before this snapshot
    uint32_t echo_stamp; // stamp of the last lt_input from this player
    lt_stats seen;      // this player's inputs the Sion got in the last full second
} lt_player;

typedef struct __attribute__((packed)) {
    uint8_t  kind;      // LT_SNAP
    uint8_t  count;
    uint16_t reserved;
    uint32_t seq;
    uint32_t sion_ms;   // Sion clock
} lt_snap;

typedef struct __attribute__((packed)) {
    uint8_t  kind;      // LT_NAMES
    uint8_t  count;
    struct __attribute__((packed)) {
        uint8_t id;
        char name[11];
    } p[LT_MAX_PLAYERS];
} lt_names;

// Player colors (0xRRGGBB), by player number
static const uint32_t lt_colors[16] = {
    0x808080, 0xE84040, 0x4080F0, 0x40C060, 0xF0C030, 0xC060E0, 0x30C8C8, 0xF08030,
    0xF070B0, 0x90D040, 0x8080F0, 0xD0A070, 0x60E0A0, 0xE0E0E0, 0xA04060, 0x4060A0,
};

#endif
