// Sion Link wire protocol, shared by the DS app and the Sion host.
//
// Transport: DS local wireless "direct" data frames. The Sion is an access
// point whose beacon carries Nintendo's vendor element (OUI 00:09:BF) with
// game ID SL_GAME_ID, so DSWifi's multiplayer client mode lists it.
//
// The Sion's Intel radio reaches a DS with broadcasts only: the DS never
// hears its one-to-one frames. So the DS joins without waiting for auth and
// assoc replies (our DSWifi patch), hostapd accepts it without hearing it
// acknowledge them (our hostapd patch), and everything from the Sion is a
// broadcast data frame naming the DS it is for:
//   DS -> Sion: [client AID (1 byte)] [pad (1 byte)] [sl_hdr] [body]
//   Sion -> DS: [to: DS MAC, or FF:FF:FF:FF:FF:FF for all] [sl_hdr] [body]
// No CMD/REPLY multiplayer frames are used, so the host has no microsecond
// timing to meet. Broadcasts aren't retried by the radio. All fields are
// little-endian.

#ifndef SLPROTO_H
#define SLPROTO_H

#include <stdint.h>

#define SL_GAME_ID      0x53494F4Eu // "SION"
#define SL_MAGIC0       'S'
#define SL_MAGIC1       'L'
#define SL_VERSION      2

#define SL_MAX_BODY     1024

enum sl_type {
    SL_HELLO   = 1, // DS -> Sion: sl_hello
    SL_WELCOME = 2, // Sion -> DS: sl_welcome
    SL_PING    = 3, // DS -> Sion: sl_ping
    SL_PONG    = 4, // Sion -> DS: sl_ping (echoed)
    SL_TEXT    = 5, // Sion -> DS: UTF-8 text, shown on the DS
    SL_BYE     = 6, // either way
};

typedef struct __attribute__((packed)) {
    uint8_t  magic[2];
    uint8_t  version;
    uint8_t  type;
    uint16_t seq;
    uint16_t len;   // body length in bytes
} sl_hdr;

typedef struct __attribute__((packed)) {
    char     name[20];  // DS owner name, UTF-8, NUL padded
} sl_hello;

typedef struct __attribute__((packed)) {
    char     host[24];  // console name
    char     motd[64];  // one line shown on the DS
    uint8_t  client_id; // number the Sion gave this DS
    uint8_t  clients;   // DSes linked right now
    uint16_t reserved;
} sl_welcome;

typedef struct __attribute__((packed)) {
    uint32_t stamp;     // sender's clock, echoed back unchanged
    uint32_t count;
} sl_ping;

_Static_assert(sizeof(sl_hdr) == 8, "sl_hdr");
_Static_assert(sizeof(sl_welcome) == 92, "sl_welcome");

#endif
