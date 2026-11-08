// Sion Link on Linux handhelds (the Anbernic RG DS under BananaOS first): the
// same calls as dslib/sllink.h, so a client joins the Sion's DS network just
// like a DS does, with no emulator.
//
// How it reaches the Sion: the handheld's Wi-Fi joins the Sion's DS access
// point as an ordinary station (found by the Nintendo vendor element in its
// beacon, like DSWifi finds it), then trades Sion Link packets as raw 802.3
// frames on a packet socket. A length field instead of an EtherType makes
// mac80211 send our payload right after the 802.11 header, exactly where a DS
// puts it, and it hands the Sion's LLC-less frames up the same way. So
// sionlinkd sees one more DS and needs no change.
//
// Joining uses the system's wpa_supplicant through its control socket (our
// own instance, own config), so the handheld's normal Wi-Fi has to be down
// first (the RTL8821CS can't be a station twice). Needs root (packet socket).

#ifndef SLLINUX_H
#define SLLINUX_H

#include <stdbool.h>
#include <stdint.h>

#include "sllink.h"

// Before SlInit: the Wi-Fi interface (default $SL_IFACE, else wlan0) and the
// owner name sent in hello (default $SL_NAME, else the host name).
void SlSetInterface(const char *ifname);
void SlSetName(const char *name);

// SlJoin() calls this between steps; returning true gives up (B on a DS).
void SlSetCancel(bool (*cancel)(void));

// Testing without a radio: SlJoin() skips wpa_supplicant and talks to this
// MAC straight away on the interface (a veth pair to a sionlinkd --if).
void SlSetDirectPeer(const uint8_t mac[6]);

// Leaves the Sion's network and stops our wpa_supplicant
void SlExit(void);

// The Sion's network as joined: BSSID, SSID, channel frequency in MHz
void SlNetwork(uint8_t bssid[6], char ssid[33], int *freq);

// How many networks the last scan heard
int SlHeard(void);

#endif
