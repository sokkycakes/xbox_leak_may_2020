# Sion Link

Local wireless between a Nintendo DS or Nintendo 3DS and the Sion.

## How it works (DS)

The Sion runs a second, DS-only access point next to its home Wi-Fi. Its
beacon carries Nintendo's vendor element (OUI 00:09:BF, game ID `SION`), so
the DS app sees it in DSWifi's local multiplayer scan like another DS host.
Both sides exchange plain 802.11 data frames (DSWifi's "direct" multiplayer
transfers). The CMD/REPLY/ACK cycle that needs microsecond host timing is
never used, so the Sion's Intel card works with stock firmware. Protocol:
`common/slproto.h`.

A real DS hears the Intel's broadcasts but none of its one-to-one frames
(tested 2026-10-15: beacons arrive, auth replies never do, at any range or
rate). So nothing from the Sion is one-to-one:

- The DS joins without waiting for auth and assoc replies
  (`ds/dswifi/sionlink.patch` on DSWifi, built by `ds/dswifi/build.sh`).
- hostapd accepts it without hearing it acknowledge them
  (`sion/hostapd-2.11-sionlink.patch`, `ignore_unacked_join=1`; build with
  `sion/build-hostapd.sh`).
- Every Sion Link packet from the Sion is a broadcast that names its DS.
  Broadcasts aren't retried by the radio, so the DS says hello every 3 s
  and the Sion answers with a fresh WELCOME.

`sionlinkd --melon PORT --deaf` plays the same Intel in the emulator: it
never answers auth or assoc.

- `ds/` BlockSDS app with its own ARM7 and DSWifi build (`make`, needs
  `/opt/wonderful` BlockSDS and git).
- `host/sionlinkd.c` the Sion side. `--if IFACE` for real hardware (hostapd
  runs the AP; DS frames arrive as 802.3 frames with a length field on a
  packet socket), `--melon PORT` for emulator tests (a software AP).
- `sion/` `sion-dslink` control script, hostapd template, install and push
  scripts, `make-kit.sh` to assemble the push kit.
- `melonds/` AirMP patch for melonDS: with `MELONDS_AIR=host:port` the
  emulated DS radio trades raw frames with `sionlinkd --melon` over UDP.

## On the Sion

    sion-dslink check                  what the card supports (changes nothing)
    sion-dslink start [CH]             DS network on the home Wi-Fi's channel
    sion-dslink 3ds [CH] [--dlp CIA]   3DS UDS network (with Download Play)
    sion-dslink dlp-start              start 3DS Download Play distribution early
    sion-dslink say TEXT               show TEXT on every linked DS/3DS
    sion-dslink status|stop

`start` refuses (unless `--force`) when the card can't run an access point
beside the home Wi-Fi, so SSH over Wi-Fi isn't dropped.

## 3DS UDS and Download Play

A 3DS uses Nintendo's UDS local wireless (`host/uds.h`, `host/uds.c`,
`host/radio3ds.c`, `ctr/source/sllink3ds.c`): an open 802.11 AP whose beacons
carry Nintendo's encrypted UDS vendor IEs (OUI `00:1F:32`), followed by a
4-frame EAPoL-Start (`0x888E`) handshake that derives a per-client CCMP key
from the network's passphrase (`"Sion Link"`, or the Download Play key).

`sionlinkd --if3ds IFACE` spawns and drives `hostapd` over its control socket:

- `sion/hostapd-2.11-sionlink.patch` adds `force_privacy=1` (sets the Privacy
  capability bit on an open AP without configuring WEP), leaves the kernel's
  `802.1X` control port off when `WPA_KEY_MGMT_NONE` so incoming `0x888E`
  handshake frames reach `sionlinkd`'s packet socket instead of being eaten by
  `mac80211`, and sets the kernel's `ASSOC` station flag before emitting
  `AP-STA-CONNECTED` so `NL80211_CMD_NEW_KEY` succeeds for the pairwise CCMP
  key.
- With `--dlp FILE.cia` (`host/dlp.c`), `sionlinkd` first hosts the 3DS
  Download Play network (`local_wlan_id` `0x00000121`), sends the child CIA in
  paced 1440-byte broadcast fragments (`FRAGS_PER_TICK` per 2 ms tick), answers
  retry block maps, hands the 3DS the `"Sion Link"` wireless reboot passphrase,
  and switches the AP to the Sion Link UDS network (`0x53494F4E`) so the booted
  CIA joins automatically. A 3DS needs custom firmware (Luma3DS) so Download
  Play accepts a development-signed child CIA.
- The 3DS hardware AES keys live in `/data/sionlink/3ds-keys.txt` on the Sion
  (never in the repository; format in `host/uds.h`: `beacon_key`,
  `uds_data_key`, `dlp_key`, `dlp_iv`).
- `ctr/hwsim/fake3ds.c` tests both the UDS handshake and the full Download Play
  CIA download against `sionlinkd --if3ds` over Linux `mac80211_hwsim`.
- In Azahar (or on a 3DS without Download Play), `ctr/source/sllink3ds.c` falls
  back to UDP port 9334 when UDS finds no Sion (`SIONLINK_UDP=1` on `sionlinkd`).

## The Intel 7265 rate bug

Linux 6.18's iwlmvm sends every frame that uses a host-chosen rate (management
frames, broadcasts) at the wrong legacy rate on firmware with TX_CMD version 8
or older, such as the Sion's 7265D-29: 1 Mbit/s goes out at 5.5 Mbit/s. Beacons
use another path and are fine. Phones cope; a DS only does 1 and 2 Mbit/s, so
it heard the Sion's beacons and nothing else. `sion/ratefix/iwl_ratefix.ko`
kprobes `iwl_trans_tx()` and puts the intended rate back; `sion-dslink start`
loads it. Build it against a linux-6.18.7 tree configured like the Sion
(`KDIR=... make -C sion/ratefix KBUILD_MODPOST_WARN=1`).

## Download Play

`sion-dslink send FILE.nds` (or `ssh sion 'sion-dslink send -' < FILE.nds`)
sends a program to every linked DS, which keeps it in RAM and boots it.
Chunks go out as broadcasts in a sliding window; the DS acknowledges the
first chunk it still lacks plus a bitmap of the 32 after it, and the Sion
resends only what's missing (`SIONLINK_DROP=N` drops N% on purpose, for
testing). The Sion sends only what's needed to boot, as a packed image: header, ARM9
binary, ARM7 binary (see `common/slboot.h`). Booting is in
`ds/source/boot.c` and `ds/arm7/source/boot7.c`, handshaking over the IPC sync
register. The ARM9 binary may load anywhere in main RAM below the staging area;
the ARM7 binary either in main RAM (BlockSDS/devkitPro default, 0x02380000) or
in ARM7 RAM (0x037F8000, common in retail programs), in which case it travels
through VRAM C+D and a small stub there copies it. Up to about 3.6 MB of code
fits. Programs that read files from their own card (NitroFS, retail file
systems) won't find them. `demo/` builds `sion-hello.nds` to try it with.

## Games on the Sion with DS players

A game on the Sion reaches its DS players through `sionlinkd`'s game socket
(`common/slgame.h`, `/run/sionlink/game.sock`). It registers with an app ID
and a DS client .nds; every DS that links with the Sion Link app is sent that
client, boots it, links again saying hello with the game's app ID, and from
then on `SL_GAME` packets pass between it and the game.

`linktest/` is the first such game. `sion-dslink linktest` puts it on the
Sion's screen (the dashboard steps aside until Ctrl+C); each DS moves a square
with the D-pad or touch screen. Every frame the DS sends its input and the
Sion sends one snapshot of the field to all DSes, and both ends count what
arrives: frames per second, lost frames, the longest gap between two, and
the round trip (`linktest/ltproto.h`). The Sion's screen and log show both
directions for every player; the DS's top screen shows the same from its
side. The Sion draws with KMS dumb buffers at 720x480, so it needs no SDL or
display server; `--headless` runs it without a screen.

## Download Station demos

A DS Download Station card keeps its kiosk demos in `/ds_demo/` as LZO1X
files ("LZOn" header). `tools/dlstation.py CARD.nds OUTDIR` unpacks them into
plain .nds images, and each one boots with `sion-dslink send`. The card's own
multiboot, `/mb/ds_demo_client.srl`, is only the kiosk's download client: it
expects the kiosk server to hand it a demo, so on its own it just waits.
