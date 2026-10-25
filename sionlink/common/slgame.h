// Sion Link game socket: how a game on the Sion talks to its DS players.
//
// sionlinkd owns the radio. A game connects to its Unix socket
// (SOCK_SEQPACKET, SLGAME_SOCKET by default) and registers with its app ID
// and the DS client it wants players to run. From then on:
//   - every DS that links with the Sion Link app is sent the client
//     (Download Play) and boots it;
//   - the client links again, saying hello with the game's app ID, and the
//     game hears SLGAME_JOIN;
//   - SL_GAME packets from that DS reach the game as SLGAME_PACKET, and the
//     game's SLGAME_SEND goes out as SL_GAME to one DS or all of them.
// One game at a time; when it disconnects, its players stay linked but
// nothing is passed on until a game registers again.
//
// Each message is one packet: a one-byte op, then its body.

#ifndef SLGAME_H
#define SLGAME_H

#include <stdint.h>

#define SLGAME_SOCKET   "/run/sionlink/game.sock"
#define SLGAME_ALL      0xFF  // SLGAME_SEND to every player
#define SLGAME_MAX      (1 + 1 + 1024)

enum slgame_op {
    // game -> sionlinkd
    SLGAME_REGISTER = 'R', // u32 app ID, then the client .nds path (NUL ends it)
    SLGAME_SEND     = 'S', // u8 player (or SLGAME_ALL), then the SL_GAME body
    // sionlinkd -> game
    SLGAME_JOIN     = 'J', // u8 player, then the DS owner's name (20 bytes)
    SLGAME_LEAVE    = 'L', // u8 player
    SLGAME_PACKET   = 'P', // u8 player, then the SL_GAME body
    SLGAME_NOTE     = 'N', // text for the game's log (e.g. a client couldn't be sent)
};

#endif
