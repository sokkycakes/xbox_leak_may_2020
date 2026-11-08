# Sion Link on the RG DS (and other Linux handhelds)

A native Sion Link client: the handheld joins the Sion's DS network exactly
like a DS does, with no emulator and no change on the Sion.

- `sllinux.c` / `sllinux.h`: the `dslib/sllink.h` calls for Linux. It finds
  the Sion by the Nintendo vendor element in its beacon (game ID `SION`), joins
  with the system's `wpa_supplicant` (our own instance, driven over its
  control socket), turns Wi-Fi power save off, then trades Sion Link packets
  as 802.3 frames with a length field on a packet socket. mac80211 then sends
  the payload straight after the 802.11 header, where a DS puts it, and hands
  the Sion's LLC-less broadcasts up the same way, so `sionlinkd --if` sees one
  more DS.
- `slclient.c`: the client app (SDL2 + SDL2_ttf from the system), the native
  counterpart of the DS Sion Link app: link, welcome, texts, A pings, B
  unlinks. DS programs the Sion offers over Download Play are turned down
  (`SL_FILE_REFUSED`); native clients of Sion games will say hello with their
  own app ID instead.
- `ports/Sion Link.sh`: BananaOS Ports entry. Takes the handheld's own Wi-Fi
  down while linked (the RTL8821CS on rtw88 can't be on two networks) and
  brings it back afterwards if it was up. Log: `roms/ports/sionlink/log.txt`.

## Build

    make                                  # native, for testing
    make slclient-headless                # static, no SDL, --headless only
    make CC=aarch64-linux-gnu-gcc \
         PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig \
         PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig   # RG DS

The arm64 build needs Ubuntu's `gcc-aarch64-linux-gnu` and
`libsdl2-dev:arm64 libsdl2-ttf-dev:arm64` (ports.ubuntu.com, multiarch). It
needs glibc 2.34; BananaOS has a newer one, plus SDL2 and SDL2_ttf.

## Tested (2026-11-08, cloud)

- veth pair to `sionlinkd --if`, with `--direct-peer` (no Wi-Fi): link,
  welcome, text, pings, Download Play refused, unlink.
- QEMU with Ubuntu's 6.8 kernel and `mac80211_hwsim radios=2`: our patched
  hostapd 2.11 with `sion/hostapd-sionlink.conf.in` on wlan0 plus unchanged
  `sionlinkd --if wlan0`; `slclient-headless --iface wlan1` with a static
  wpa_supplicant 2.11. Found the Sion by its beacon, associated, welcomed,
  text over the air, 21 pings answered in 0 to 8 ms, clean unlink.

Not yet on the real RG DS and Sion.

## Kit layout (card)

    roms/ports/Sion Link.sh
    roms/ports/sionlink/slclient
    roms/ports/sionlink/font.ttf          (DejaVu Sans Mono)
