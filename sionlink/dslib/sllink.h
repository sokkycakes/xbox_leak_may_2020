// Sion Link for DS game clients: finds the Sion, joins it and trades Sion
// Link packets (common/slproto.h). Used by the programs the Sion sends over
// Download Play; needs the Sion Link DSWifi build (ds/dswifi). 3DS clients
// get the same calls over the 3DS's local wireless from ctr/sl3ds.c, which
// adds SlExit().

#ifndef SLLINK_H
#define SLLINK_H

#include <stdbool.h>
#include <stdint.h>

#include "slproto.h"

// Starts wireless. False if it couldn't.
bool SlInit(void);

// Joins the first Sion in range, showing progress through status() (may be
// NULL). Returns false if B was pressed.
bool SlJoin(void (*status)(const char *text));

// Says hello as app (the game's ID) until welcomed, then every 3 s; call
// every frame. Returns the player number once welcomed, else 0.
int SlKeepAlive(uint32_t app, uint32_t now_ms);

// Next packet from the Sion, or false. *body stays valid until the next call.
bool SlNext(const sl_hdr **hdr, const uint8_t **body);

void SlSend(int type, const void *body, int len);

// Milliseconds since start; doesn't wrap like cpuGetTiming() (call it at
// least every two minutes). Needs cpuStartTiming(0).
uint32_t SlMillis(void);

// Ms since the Sion was last heard.
uint32_t SlQuietFor(uint32_t now_ms);

#ifdef __3DS__
// Leaves the Sion's network and stops wireless
void SlExit(void);

// Joins again if the link dropped (the 3DS slept, or the Sion has been quiet
// for quiet_ms), showing progress through status(); B doesn't stop it. Call
// every frame. True if it rejoined.
bool SlRejoin(void (*status)(const char *text), uint32_t quiet_ms);
#endif

#endif
