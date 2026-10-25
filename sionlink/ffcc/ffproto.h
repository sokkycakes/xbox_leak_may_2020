// FFCC link: the GameCube <-> GBA JoyBus traffic of Final Fantasy Crystal
// Chronicles, carried inside SL_GAME packets between a host on the Sion and
// the game's GBA client running on a DS.
//
// The DS emulates the GBA's JOY port. The host sends the commands a
// GameCube would put on the wire, in order; the DS applies each to its JOY
// registers, raises the client's serial interrupt the way the GBA would,
// and answers with what the GameCube would have read back. Batches carry a
// sequence number: the DS runs each batch once and repeats its answer if
// the batch comes again (the host resends when an answer is lost).

#ifndef FFPROTO_H
#define FFPROTO_H

#include <stdint.h>

#define FF_APP_ID   0x43434646u // "FFCC"
#define FF_MAX_OPS  100

enum ff_kind {
    FF_BOOT  = 1, // host -> DS: ff_boot, what the GameCube writes into a multiboot image
    FF_JOY   = 2, // host -> DS: ff_joy + ff_op[count]
    FF_REPLY = 3, // DS -> host: ff_joy + ff_result[count]
};

// JoyBus commands (the GameCube's SI command byte)
enum ff_cmd {
    FF_CMD_STATUS = 0x00, // read JOYSTAT
    FF_CMD_READ   = 0x14, // read the GBA's JOY_TRANS
    FF_CMD_WRITE  = 0x15, // write the GBA's JOY_RECV
    FF_CMD_RESET  = 0xFF, // JOY reset
};

typedef struct __attribute__((packed)) {
    uint8_t  kind;      // FF_BOOT
    uint8_t  boot_mode;
    uint8_t  player_no;
    uint8_t  reserved;
    uint32_t idle_word; // multiboot header 0xAC
    uint32_t host_id;   // multiboot header 0xC8
} ff_boot;

typedef struct __attribute__((packed)) {
    uint8_t  kind;      // FF_JOY or FF_REPLY
    uint8_t  count;
    uint16_t seq;
} ff_joy;

typedef struct __attribute__((packed)) {
    uint8_t  cmd;       // ff_cmd
    uint8_t  reserved[3];
    uint32_t data;      // FF_CMD_WRITE: the word written
} ff_op;

typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint8_t  stat;      // JOYSTAT as the command found it (for a read, bit 3
                        // says whether the GBA had written a new word)
    uint16_t reserved;
    uint32_t data;      // FF_CMD_READ: the word read
} ff_result;

#endif
