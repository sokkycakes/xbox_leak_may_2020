// 3DS Download Play, parent side: the Sion offers a child program (a CIA)
// to the 3DS Download Play app, the way a 3DS game does. Written from
// 3dbrew's Download Play page, with Azahar's emulation of the DLP system
// module as the reference for the packet layouts.
//
// It runs on a UDS network (uds.h) with Download Play's own communication
// ID and passphrase. Every DLP packet is a 16-byte header (type, size,
// a checksum keyed by the host's MAC, packet index, response ID) and a
// body, all big-endian:
//   - every 500 ms, five broadcast packets on channel 1 describe the child:
//     title ID, sizes, its short and long name and its 48x48 icon. The
//     Download Play app lists it from them.
//   - a 3DS that picks it joins as a client. On channel 2 the host accepts
//     it (auth), starts distribution, sends the CIA in blocks of 182
//     fragments of 1440 bytes (asking after each block whether more is
//     needed), then tells it to start the game with the "wireless reboot"
//     passphrase. The 3DS installs the CIA and boots it.
// Packets are acknowledged by the 3DS; the host repeats what isn't.

#ifndef DLP_H
#define DLP_H

#include <stdbool.h>
#include <stdint.h>

#include "uds.h"

#define DLP_COMM_ID     0x00002810u
#define DLP_PASSPHRASE  "0km@tsa$uhmy1a0sa" // sent with its NUL
#define DLP_BROADCAST_CHANNEL 1
#define DLP_CLIENT_CHANNEL    2

typedef struct dlp_host dlp_host;

// cia: the child CIA (kept by the caller). Fails (NULL) if it isn't a CIA
// with an icon.
dlp_host *DlpCreate(uds_host *uds, const uint8_t host_mac[6], const uds_keys *keys,
                    const uint8_t *cia, uint32_t cia_size, int max_clients, const char **error);
void DlpDestroy(dlp_host *d);

// Wire these to the UDS network's callbacks
void DlpNode(dlp_host *d, int node_id, bool joined);
void DlpData(dlp_host *d, int node_id, int channel, const uint8_t *data, int len);

// Call every few milliseconds. Distribution starts by itself a few seconds
// after the last 3DS joined (or at DlpStart()).
void DlpTick(dlp_host *d, uint64_t now_ms);
void DlpStart(dlp_host *d);

// True once every 3DS that took the child was told to start it. Keep
// ticking for a few seconds after, so a lost start reaches them again.
bool DlpDone(dlp_host *d);

// The child's title ID, and its name as ASCII
uint64_t DlpTitleId(dlp_host *d);
const char *DlpTitleName(dlp_host *d);

// The checksum every DLP packet carries (exposed for the tests)
uint32_t DlpChecksum(uint32_t key, const uint8_t *packet, int len);
uint32_t DlpChecksumKey(const uds_keys *keys, const uint8_t host_mac[6]);

#endif
