// The 3DS network on the Sion's own radio (sionlinkd --if3ds IFACE).
//
// hostapd runs the access point: this file writes its configuration and
// starts it for each UDS network (host/uds.h), and talks to it over its
// control socket. hostapd answers authentication and association and
// reports each station; the beacon carries the network's Nintendo elements
// (UdsBeaconTags(), refreshed when the node list changes). Our hostapd
// patch adds force_privacy, so the beacon says the network is encrypted
// as 3DS beacons do.
//
// Data frames (EtherType 0x876D, and EAPoL 0x888E for joining) go through
// a packet socket on IFACE: the kernel turns them into Ethernet frames, so
// the 8-byte SNAP header uds.c expects is put back on the way in and taken
// off on the way out. Every data frame on the air is CCMP-encrypted with
// the network's key (UdsCcmpKey()); this file installs it in the kernel
// with nl80211 for each 3DS that associates (pairwise) and for broadcasts
// (group), and the radio does the rest.

#ifndef RADIO3DS_H
#define RADIO3DS_H

#include <stdint.h>

#include "uds.h"

typedef struct radio3ds radio3ds;

// hostapd: the hostapd binary; run_dir: where its configuration, log and
// control socket go. group_key_idx: the key index broadcasts use (0-3).
// Returns NULL (and logs why) if IFACE can't be used.
radio3ds *R3Open(const char *ifname, const char *hostapd, const char *run_dir,
                 int channel, int group_key_idx, void (*log)(const char *fmt, ...));
void R3Close(radio3ds *r);

// The interface's MAC: the UDS host's MAC
const uint8_t *R3Mac(radio3ds *r);

// Starts (or restarts) the access point for this network
void R3Start(radio3ds *r, uds_host *u);

// The uds_config.tx callback (ctx = the radio3ds)
void R3Tx(void *ctx, int kind, const uint8_t dst[6], const uint8_t *body, int len);

// File descriptors to poll (-1 where there is none yet), and the work
// when one is ready (or every few milliseconds)
int R3Fds(radio3ds *r, int fds[3]);
void R3Poll(radio3ds *r);

#endif
