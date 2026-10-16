// The host side of the 3DS's local wireless ("UDS"): the network a 3DS game
// opens for others to join, played by the Sion. Written from 3dbrew's NWM
// documentation, with Azahar's emulation as a cross-check.
//
// The 3DSes find the network by its beacon: two vendor elements (OUI
// 00:1F:32) carrying the network's description in the clear (communication
// ID, network ID, node counts, application data, under a SHA-1) and the
// list of nodes encrypted with a key from the 3DS's NWM module. A 3DS joins
// with open authentication and association, then sends an EAPoL-Start
// frame with its user name; the host gives it a node number in an
// EAPoL-Logoff frame. After that everything is a "secure data" frame: an
// 802.2 SNAP header (EtherType 0x876D), a 14-byte header naming the data
// channel and the source and destination nodes, and the application's
// bytes. On the air every data frame is CCMP-encrypted with a key derived
// from the network's passphrase (UdsCcmpKey()); the radio does that.
//
// This file knows the protocol only. A backend moves frame bodies to and
// from the air: in Azahar (the cloud test) bodies travel as they are; on
// the Sion's radio, hostapd answers authentication and association,
// beacons carry UdsBeaconTags() and data goes through the kernel.

#ifndef UDS_H
#define UDS_H

#include <stdbool.h>
#include <stdint.h>

#define UDS_MAX_NODES   16
#define UDS_HOST_NODE   1
#define UDS_BROADCAST   0xFFFF
#define UDS_MAX_PAYLOAD 0x5C6 // largest application payload in one frame
#define UDS_ETH_DATA    0x876D
#define UDS_ETH_EAPOL   0x888E
#define UDS_CLIENT      3     // EAPoL-Start connection types
#define UDS_SPECTATOR   4

// Frame kinds (numbered as Azahar's room protocol numbers them)
enum uds_kind {
    UDS_BEACON = 0,
    UDS_DATA = 1,     // body starts with the 8-byte SNAP header
    UDS_AUTH = 2,
    UDS_ASSOC_RESP = 3,
    UDS_DEAUTH = 4,
};

// Keys the 3DS's system modules hold (keys.txt, never in the repo)
typedef struct {
    uint8_t beacon[16];  // NWM's beacon key
    uint8_t uds_data[16]; // keyslot 0x2D normal key: CCMP key derivation
    uint8_t dlp[16];     // keyslot 0x39 normal key with the DLP keyY
    uint8_t dlp_iv[16];  // DLP module's checksum IV
    bool have_beacon, have_uds_data, have_dlp;
} uds_keys;

// Reads "name = hex" lines: beacon_key, uds_data_key, dlp_key, dlp_iv.
// Returns false if the file couldn't be read.
bool UdsLoadKeys(const char *path, uds_keys *k);

typedef struct uds_host uds_host;

typedef struct {
    uint32_t comm_id;         // local-WLAN communication ID
    uint8_t id8;
    uint8_t max_nodes;        // including the host
    // Network tag bytes 0x09 and 0x13, meaning unknown: both 1 in a real
    // 3DS's Download Play beacon, 0 otherwise
    uint8_t tag_unk9, tag_unk13;
    uint8_t channel;          // Wi-Fi channel
    uint8_t mac[6];           // the host's MAC
    const char *name;         // the host's user name (ASCII)
    const void *passphrase;
    int passphrase_len;
    const uds_keys *keys;
    // Sends a frame body to dst (FF:FF:FF:FF:FF:FF for everyone)
    void (*tx)(void *ctx, int kind, const uint8_t dst[6], const uint8_t *body, int len);
    // A node joined (true) or left
    void (*node)(void *ctx, int node_id, bool joined, const uint8_t mac[6], const char *name);
    // Application data for the host on a data channel
    void (*data)(void *ctx, int node_id, int channel, const uint8_t *data, int len);
    void *ctx;
} uds_config;

uds_host *UdsCreate(const uds_config *cfg);
void UdsDestroy(uds_host *u);

// Call every few milliseconds: sends a beacon every 100 ms (Azahar backend)
void UdsTick(uds_host *u, uint64_t now_ms);

// A frame body from the air, sent by src
void UdsReceive(uds_host *u, int kind, const uint8_t src[6], const uint8_t dst[6],
                const uint8_t *body, int len);

// The radio associated a station (backends where hostapd does the joining)
void UdsAssociated(uds_host *u, const uint8_t mac[6]);
// The radio lost a station
void UdsLeft(uds_host *u, const uint8_t mac[6]);

// Application data to a node (or UDS_BROADCAST) on a data channel
void UdsSend(uds_host *u, int node_id, int channel, const void *data, int len);

// Drops a node
void UdsKick(uds_host *u, int node_id);

// Beacon contents
int UdsBeaconBody(uds_host *u, uint8_t *out);     // fixed fields + all elements
int UdsBeaconTags(uds_host *u, uint8_t *out);     // just the Nintendo elements
void UdsSsid(uds_host *u, char out[9]);            // the network's 8-character SSID

// The CCMP key every frame of this network is encrypted with
bool UdsCcmpKey(uds_host *u, uint8_t key[16]);

// The node's MAC, or NULL
const uint8_t *UdsNodeMac(uds_host *u, int node_id);

uint32_t UdsNetworkId(uds_host *u);

#endif
