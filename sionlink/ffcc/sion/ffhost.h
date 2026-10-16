// ffhost: the GameCube's side of FFCC's GBA link (see ffhost.c). The link
// half (ffhost.c) runs the JoyBus handshake and moves words; the game half
// (demo.c) decides what the GameCube says.

#ifndef FFHOST_H
#define FFHOST_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PLAYERS 15

// Client link.h: command ids in byte 0 of every word, kind in its top bits
#define LINK_KIND_SECOND  0x40
#define LINK_KIND_DATA    0x80
#define LINK_CONTEXT      1
#define LINK_PAD          4
#define LINK_MESSAGE      5
#define LINK_ACK          6
#define LINK_NAK          7
#define LINK_FLUSH        8
#define LINK_CTRL_MODE    9
#define LINK_START        10
#define LINK_DATA         11
#define LINK_REQUEST      12
#define LINK_CHECK_CRC    13
#define LINK_STATE        14
#define LINK_BASE_POS     15
#define LINK_CANCEL       16
#define LINK_PARTY_POS    17
#define LINK_ENEMY_POS    18
#define LINK_PARTY_HP     19
#define LINK_EVENT        20
#define LINK_LETTER_REPLY 21
#define LINK_MAPOBJ_FLAGS 22
#define LINK_ITEM         23
#define LINK_MASK         24
#define LINK_STRENGTH     25
#define LINK_GIL          26
#define LINK_MODE         27
#define LINK_CMAKE_NAME   28
#define LINK_CMAKE_FOODS  29
#define LINK_EQUIP_SLOT   30
#define LINK_CMD_SLOT     31
#define LINK_TMP_ARTIFACT 32
#define LINK_TREASURE_POS 33
#define LINK_HIT_ENEMY    34

// A word as the client sees it: byte 0 first
#define WORD(b0, b1, b2, b3) \
    ((uint32_t)(uint8_t)(b0) | (uint32_t)(uint8_t)(b1) << 8 | (uint32_t)(uint8_t)(b2) << 16 | \
     (uint32_t)(uint8_t)(b3) << 24)
#define WORD16(b0, b1, v) WORD(b0, b1, (v) & 0xFF, ((v) >> 8) & 0xFF)

typedef struct unit unit;
typedef struct demo demo;

typedef struct {
    int id;                 // Sion Link player number (1-15)
    bool here;
    char name[21];
    int step;               // handshake step, CONNECTED once linked
    uint32_t host_id;
    uint16_t seq;
    bool waiting;           // a batch is out
    uint32_t sent_at;
    uint32_t first_sent_at; // when the batch first went out
    uint32_t srtt, rtt_max; // radio round trip, smoothed and worst, in ms
    uint32_t last_reply;    // when the DS last answered
    bool copy_due;          // the batch goes out twice (see send_batch)
    uint8_t batch[1024];
    int batch_len;
    int batch_writes;       // words of the batch from the queue
    uint32_t boot_at;
    uint16_t keys;
    uint32_t words, pads, batches, resends, written;

    unit *queue, *queue_tail; // words for the client, in order
    demo *game;
} player;

// The queue (ffhost.c)
void q_word(player *p, uint32_t w);
void q_msg(player *p, const uint8_t *data, int len);            // LINK_MESSAGE, waits for the ACK
void q_bulk(player *p, int type, const uint8_t *data, int len);  // LINK_DATA download, block by block
void q_crc(player *p, int type, const uint8_t *data, int len, int tag); // LINK_CHECK_CRC: demo_done(tag, has it)
void q_tag(player *p, int tag);                                  // demo_done(tag, true) once sent this far
void q_wait(player *p, int ack_arg);                             // waits for an ACK with this argument
void q_clear(player *p);
bool q_idle(const player *p);
uint16_t ff_crc16(const uint8_t *data, int len);
void logf_(const char *fmt, ...);
uint32_t now_ms(void);
player *ffhost_player(int id);   // a linked player, or NULL

// The game (demo.c)
void demo_start(player *p);                 // the client has linked
void demo_frame(player *p, uint32_t now);   // once a frame while linked
void demo_word(player *p, uint32_t w);      // a word from the client
void demo_ack(player *p, uint32_t w);       // an ACK or NAK nothing in the queue waited for
void demo_done(player *p, int tag, bool ok);  // a q_crc or q_tag finished
void demo_end(player *p);
void demo_command(const char *line);        // a line typed at the host
bool demo_init(const char *data_dir);

#endif
