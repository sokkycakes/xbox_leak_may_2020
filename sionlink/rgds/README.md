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

# DS mirror on the RG DS (`ports/Sion DS Mirror.sh`)

The DS game the Sion runs (sionlink/mirror) on the RG DS's two screens. The RG DS runs
the same client a real DS runs (`mirror/kit/sion-link-live.nds`) in `mirrorrun` built
for arm64 with `--view sdl` (harness/viewout.cpp): BananaOS's SDL draws one window across
both panels (`SDL_VIDEODRIVER=evdev SDL_BANANA_PANELS=stack`), each DS screen fills a
640x480 panel, the bottom panel's touch is the DS's, F quits. The client's simulated
DSpico (`--dspico tcp:HOST:PORT`) carries the bundle stream from the Sion's mirrorrun,
which now serves `--serve usb,tcp:7790` (a DS on the DSpico or a viewer, whichever comes).

Network: the home Wi-Fi when the RG DS is on it and the Sion answers at `SION_HOST`
(`sionlink/mirror.conf`), else the Sion's DS network: `slclient --join-only` joins it and
the viewer reaches the Sion at its IPv6 link-local address (EUI-64 from the BSSID).

arm64 build: melonDS at `mirror/melonds/BASE` + `mirror-tap.patch`, cmake with an
aarch64 toolchain file (`-DENABLE_GDBSTUB=OFF -DENABLE_OGLRENDERER=OFF`, JIT on) into
`build-arm64`, then
`CXX="aarch64-linux-gnu-g++ -static-libstdc++ -static-libgcc" ARCH=ARM64 B=build-arm64 VIEW=1 sh build.sh`
with the arm64 pkg-config path. (melonDS must be built with the GDB stub off: the harness
is built without it, and a mismatch crashes in FirmwareMem::Reset.)

Tested 2026-11-08 in the cloud with the Mario Kart DS demo: over local TCP 2642 of 2700
frames shown (menus and a race); through a proxy capped at 180 KB/s (the DS network's
speed) 2458 of 2700, race fine; the arm64 build under qemu-user keeps up; and in QEMU with
mac80211_hwsim: join the patched hostapd DS network, then the Sion kit's static
mirrorrun over IPv6 link-local (569 frames shown under emulation). TCP note: a 4 KB
receive buffer on the viewer starved the stream on delayed acks; only the Sion's send
buffer stays small (16 KB). Not yet on the real RG DS. No game sound on the RG DS yet.

The Sion's DSpico is always plugged in, so it used to win `usb,tcp:7790` at once and the port
closed (first real try, 2026-11-08). Now a viewer over the network takes the game over from
the DS on the DSpico whenever it connects, and the DS gets it back when the viewer quits; the
port script checks the Sion's ssh port to tell whether it's on the home Wi-Fi.
