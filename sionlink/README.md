# Sion Link

DS local wireless between a Nintendo DS (or a 3DS running DS homebrew) and
the Sion. Prototype 1: the DS finds the Sion, links to it, pings it and shows
text the Sion sends.

## How it works

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

    sion-dslink check        what the card supports (changes nothing)
    sion-dslink start [CH]   DS network on the home Wi-Fi's channel
    sion-dslink say TEXT     show TEXT on every linked DS
    sion-dslink status|stop

`start` refuses (unless `--force`) when the card can't run an access point
beside the home Wi-Fi, so SSH over Wi-Fi isn't dropped.

## The Intel 7265 rate bug

Linux 6.18's iwlmvm sends every frame that uses a host-chosen rate (management
frames, broadcasts) at the wrong legacy rate on firmware with TX_CMD version 8
or older, such as the Sion's 7265D-29: 1 Mbit/s goes out at 5.5 Mbit/s. Beacons
use another path and are fine. Phones cope; a DS only does 1 and 2 Mbit/s, so
it heard the Sion's beacons and nothing else. `sion/ratefix/iwl_ratefix.ko`
kprobes `iwl_trans_tx()` and puts the intended rate back; `sion-dslink start`
loads it. Build it against a linux-6.18.7 tree configured like the Sion
(`KDIR=... make -C sion/ratefix KBUILD_MODPOST_WARN=1`).
