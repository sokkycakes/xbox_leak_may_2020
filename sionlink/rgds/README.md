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

# Sion Link app (`linkapp/`, `ports/Sion Link.sh`)

The DS app (`mirror/ds`, `sion-link-live.nds`) as a native program for the RG DS, with the
same screens: `mirror/ds/source/nitroui.c` and its baked art built on a small libnds shim
(`linkapp/shim/nds.h`), the two 256x192 screens scaled onto the two panels. Like the DS app it
does its three things by itself:

- **Controller**: while no DS game is mirrored, the RG DS's buttons and touch are a pad for
  the Sion. Reports (`mirror_input`) go over TCP 7791 to a second `dspad` on the Sion
  (`--link tcp:7791 --sock /run/sionlink-pad-net.sock`, started by `S97sionlink-pad`), which
  makes its own Xbox 360 pad, so a DS on the DSpico and the RG DS can both play. Its pad
  bundles give the lines on the lower screen.
- **Mirror**: once a DS title runs on the Sion (its mirrorrun answers on TCP 7790), the app
  hands that connection to the DS core (`mirrorrun ... --dspico fd:N --view sdl`, with
  `MIRROR_QUIT_ON_CLOSE`), which has the panels until the title ends (exit code 3: home, and
  the pad again) or F (home; A goes back to the game). The picture still comes through the DS
  app run in melonDS; drawing the bundles natively is a later step.
- **Download Play**: on the Sion's DS network (`sllinux.c`, as a DS joins it), programs the
  Sion sends are taken in (the DS app's acks, chunk map and all), made back into a .nds and
  started in the DS core. Sion games whose DS side keeps talking to the Sion after the
  download need the core's radio bridged to the link (not yet).

The Sion: home Wi-Fi when its ssh port answers at `SION_HOST` (`sionlink/mirror.conf`), else
the Sion's DS network; Y switches between them (the DS network needs `sion-dslink start` on
the Sion, and takes the handheld's own Wi-Fi down meanwhile). F quits.

Sion side: `mirror/kit/sion/install-pad.sh` (the network pad and the mirror's network link).

Build: `make -C linkapp` (native), or with the arm64 `CC`/pkg-config as above. Cloud tests
2026-11-08: the pad against `dspad --link tcp:7791 --dry 1` (buttons arrive, the pad lines
show); the mirror hand-off against a Sion-side mirrorrun with the MKDS demo (1040 frames shown,
the title's end brings the home screen and the pad back); Download Play over a veth pair to
`sionlinkd --if` (`--direct-peer`): `mirrorplay.nds` 195548 bytes in 1.8 s, rebuilt .nds
boots the same as the original; the arm64 build under qemu-user. Not yet on the RG DS.
