// The Sion's Wi-Fi link (Sion Link over DS local wireless, common/slproto.h), from the Wi-Fi
// launcher (branch rb4kah, sionlink/ds). It finds a Sion and links to it by itself, then takes
// what the Sion sends: a line of text now and then, and programs over Download Play, booted
// from RAM (Sion games with DS players send their DS side that way too). It runs a step a
// frame while no game is mirrored; the mirror leaves it idle, as it needs all the DS's time.
#ifndef WIFILINK_H
#define WIFILINK_H

#include <nds.h>

typedef enum {
    WL_OFF,
    WL_FAILED,      // the radio didn't start
    WL_SEARCHING,
    WL_LINKING,     // joining, or saying hello until the Sion answers
    WL_LINKED,
} wl_state;

typedef struct {
    wl_state state;
    char host[24];      // the Sion's name (WL_LINKED)
    char motd[64];      // its line for the DSes
    char text[33];      // the last text it sent
    // Download Play
    bool receiving, booting;
    char name[33];
    u32 percent;
    const char* refused;   // why the last program couldn't start (NULL: it could)
} wl_status;

// looking for a Sion (the first time it starts DSWifi, which takes a moment)
void wlOn(void);
// says goodbye to the Sion, drops a download, the radio idle
void wlOff(void);
// once a frame while on: never blocks, but boots a program once it has all of it (and then
// doesn't return)
void wlTick(void);
// what it's doing; *changed set when that differs from the last call
const wl_status* wlStatus(bool* changed);

#endif
