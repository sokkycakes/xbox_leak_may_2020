# push-wincart

xbcompat with Windows game cards (xbcompat/src/kernel/wincart.c), built on the
Live + Guide build. A Kazeta cart whose Runtime is `windows-*` appears in the
dashboard and in Frolick as a title named after the cart (a stub `default.xbe`
in a shim of the card under /tmp/xbcompat-card-<uid>/; title ID = `SN` +
CRC-16 of the cart's Id, which is where its saves go under UDATA). Launching
it runs `/opt/xbcompat/tools/wincart-run <kzi> --title-id <id>` on the Sion
(Wine from Conty under gamescope, with the kazeta-guide overlay) in place of
the title process, so quitting returns to the dashboard as for any game.

`install.sh` from the PC (see its header). `XBCOMPAT_WINCART_RUN` names another
runner (cloud tests). Undo: /opt/xbcompat/bin/xbcompat.pre-wincart.

## The Sion side

What `install.sh` puts on the Sion besides xbcompat (all kept across reboots
with `sion persist`; the game card is never written):

| kit file | on the Sion | what it is |
|---|---|---|
| `wincart-run` | /opt/xbcompat/tools/wincart-run | runs a Windows cart: Conty (/data/conty_lite.sh, mounted at /mnt/conty on demand), Conty's Wine under gamescope on the TV, kazeta-guide over it, state in /data/wincarts/&lt;Id&gt;/ |
| `wincart-av` | /opt/xbcompat/tools/wincart-av | takes the display from xbox-av for the run ('T') and gives it back black ('S') |
| `kazeta-guide/kazeta-guide` | /opt/xbcompat/bin/kazeta-guide | the Guide over a Windows game (64-bit, runs in Conty): xbcompat's guide.cpp + XUI runtime + `host.cpp`; owns the real pads (SDL over libusb, /opt/xbcompat/share/gamecontrollerdb.txt) and gives Wine a uinput Xbox 360 pad; Y (Xbox Dashboard) ends the run |
| `overrides/<Id>/` | /opt/xbcompat/share/wincart/overrides/&lt;Id&gt;/ | per-cart defaults, copied into /data/wincarts/&lt;Id&gt;/ on the first run: files used in place of the cart's (Silent Hill 4: `input.ini`, laid out for an Xbox pad: Start skips/pauses, right stick is the camera) and `save-dirs` (the cart's save folders) |
| `boot/sion-extra.conf` | /etc/modules-load.d/sion-extra.conf, plus `extra/*.ko:` lines in /lib/modules/6.18.7/modules.dep | loads fuse, erofs, squashfs and uinput at boot through S11modules (an init.d script added by the overlay runs too late for rcS, and the Sion has no depmod) |

`kazeta-guide/build.sh` rebuilds the Guide host from this checkout
(`guide-overlay-alpha.patch` is its one change to guide.cpp: normal blending
keeps coverage in the overlay window's alpha, or gamescope shows the game
through the Guide). `asoundrc.example` is the ALSA setup wincart-run writes
in the chroot.

Saves: a cart's save folders (`save-dirs`, relative to the cart folder; for
Silent Hill 4 `save`, where GOG's build writes `sh4.sav`) live on the Xbox
hard disk as UDATA/&lt;title id&gt;/000000000001/ (with TitleMeta.xbx and
SaveMeta.xbx), so the dashboard's Memory screen lists them. The game sees its
cart folder as /data/wincarts/&lt;Id&gt;/game/: links to the card's files, the
overrides, and the save folders.

Known gap: the FMVs (GOG's dshow.dll) play without sound: their waveOutOpen
fails under Conty's Wine.

The kazeta-guide binary in this kit was rebuilt in the cloud with build.sh
(md5 17e84415); the one installed on the Sion on 2026-10-21 (md5 0939fa7d) is
the same code built on the PC, differing only in its build-id.
