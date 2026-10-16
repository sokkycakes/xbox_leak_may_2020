// Sion Link on a 3DS: the 3DS's local wireless (UDS) network the Sion hosts
// for 3DS clients, shared by the 3DS programs and the Sion host.
//
// A 3DS client finds the network by its local-WLAN communication ID, joins
// it with the passphrase as a UDS client, and trades Sion Link packets
// (slproto.h) with the host as UDS data frames on SL_UDS_CHANNEL:
//   3DS -> Sion: [sl_hdr] [body], to the host (network node 1)
//   Sion -> 3DS: [sl_hdr] [body], to that 3DS's node, or broadcast
// Unlike a DS, a 3DS hears the Sion's one-to-one frames, so nothing names
// its receiver inside the payload.

#ifndef SLUDS_H
#define SLUDS_H

#define SL_UDS_COMM_ID    0x534C4E4Bu // "SLNK": local-WLAN communication ID
#define SL_UDS_ID8        0x00
#define SL_UDS_PASSPHRASE "Sion Link"  // sent without its NUL
#define SL_UDS_CHANNEL    1           // UDS data channel
#define SL_UDS_MAX_NODES  8           // the host plus up to 7 3DSes

// A Sion serving 3DS Download Play hosts Download Play's network instead
// (host/dlp.h), and 3DS programs join that one, with its passphrase
#define SL_DLP_COMM_ID    0x00002810u
#define SL_DLP_ID8        0x55
#define SL_DLP_PASSPHRASE "0km@tsa$uhmy1a0sa" // sent with its NUL

#endif
