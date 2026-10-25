# Sion Cards: a 3DS "Download Play" launcher for Sion Link (brainstorm, 2026-10-16)

Nothing here is built. This is the idea as talked through in the "Sion Download Play app" thread.

## The idea
One homebrew app, **Sion Cards**, installed once on each Luma 3DS (a CIA with its own Home Menu icon).
When the Sion is in range it lists what the Sion offers; you pick one, it downloads, and the app
keeps it as a "card" on the SD card. The last few cards stay there, so you can run them again later
with no Sion around, like an e-Reader's saved cards.

## What it gets around
| Stock Download Play limit | Sion Cards |
|---|---|
| One child at a time, wiped on exit | Keeps the last N (default 5) on SD, oldest evicted |
| No Home Menu entry for the child | The launcher is the entry; each card shows its own icon and name inside it |
| System update / region check before joining | Our own protocol, no update list, no region field |
| Needs 3DS UDS console keys (the blocked part) | No UDS at all: plain Wi-Fi + UDP/TCP to sionlinkd, so no keys |
| Child size cap, RSA-signed children | Any size the SD fits; we decide what is trusted |

## How it talks to the Sion
- **Preferred (2026-10-16): UDS with our own protocol on top.** The 3DS app calls UDS through libctru with its own
  wlancommID; the 3DS's system module does the CCMP crypto in hardware, so the app needs no keys. sionlinkd's
  existing UDS host (radio3ds.c, beacon/CCMP code checked against the real-host captures) is the other end, using
  the key already on the Sion. Download Play's layer (one child, update list, region) is dropped, not copied.
  A device without the key can't join, so it's not fully open.
- Fallback:  Sion's own Wi-Fi network, joined with nwm::INF ConnectToAP (no System Settings setup, untested);
  fallback is a saved connection slot.
- sionlinkd gets a small "card server": a catalogue (title, icon, size, version, hash) and a
  download endpoint. The same server can push updates to cards you already have.
- Multiplayer cards (FFCC GBA client etc.) reuse the planned UDP Sion Link backend (ctr/slnet3ds.c).

## What a card is
`sdmc:/sionlink/cards/<id>/` holding `card.json`, `icon.smdh`, and one payload:
1. **3DSX homebrew** (first choice): launched straight from the app via Luma's homebrew loader,
   nothing installed, nothing to clean up.
2. **CIA**: installed through AM when picked, uninstalled when evicted. Shows up on the Home Menu
   while it is saved.
3. **Data card**: small add-on for an app (levels, saves, a GBA ROM for the FFCC client), the
   closest thing to a real e-Reader card.
4. Later: DS .nds through TWiLight Menu++.

Cards can be signed by the Sion (ed25519) so the app only runs things the Sion made. Optional.

## Open questions
- How many slots, and should some be pinnable so they never get evicted?
- 3DSX first, or CIA first?
- Does soc:U work after ConnectToAP? (first thing to test on hardware)
