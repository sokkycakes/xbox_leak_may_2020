# Sion Link

DS local wireless between a Nintendo DS (or a 3DS running DS homebrew) and
the Sion. Prototype 1: the DS finds the Sion, links to it, pings it and shows
text the Sion sends.

## How it works

The Sion runs a second, DS-only access point next to its home Wi-Fi. Its
beacon carries Nintendo's vendor element (OUI 00:09:BF, game ID `SION`), so
the DS app sees it in DSWifi's local multiplayer scan like another DS host.
The DS joins with the normal open auth/assoc, then both sides exchange plain
802.11 data frames (DSWifi's "direct" multiplayer transfers). The
CMD/REPLY/ACK cycle that needs microsecond host timing is never used, so the
Sion's Intel card works with stock firmware. Protocol: `common/slproto.h`.

- `ds/` BlockSDS app (`make`, needs `/opt/wonderful` BlockSDS).
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
