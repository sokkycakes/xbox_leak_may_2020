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

The kazeta-guide binary in this kit is built in the cloud with build.sh. The
first one installed on the Sion (2026-10-21 morning, md5 0939fa7d) was built
on the PC.

Sound and display (2026-10-21 evening, after sokky's first play):
- The Guide's sounds go to ALSA directly (PCM wincart_ui, libasound opened by
  kazeta-guide), not through SDL, which could land on the title's "default"
  path and be turned down with it.
- The title's volume (slider and ducking) is set in decibels: Wincart's
  softvol steps are linear in dB, so the old percentages put the Guide's
  30% duck at -36 dB, which sounded like the sound stopping.
- dmix's buffer is 1920 frames (40 ms, WINCART_PERIOD/WINCART_BUFFER to
  change it); unset, it was large enough to delay the game's sound.
- The Guide's overlay window is mapped only while the Guide is on screen
  (KAZETA_GUIDE_MAPPED=1 keeps the old behaviour), so gamescope composites
  nothing over the game the rest of the time.
- Test hooks: KAZETA_GUIDE_BIN (a Guide host under /opt/xbcompat or the
  cart's state folder), WINCART_GAME_ENV and WINCART_GUIDE_ENV (extra
  environment for gamescope/Wine and the Guide), KAZETA_GUIDE_SCRIPT
  ("SECONDS:MASK,..." Guide buttons, for unattended tests).

`QUICK=1 install.sh` installs only wincart-run, wincart-av, kazeta-guide and
the overrides.

Display mode and Proton builds (2026-10-21 evening):
- `drmkeep/drmkeep.so` (installed as /opt/xbcompat/tools/drmkeep.so, preloaded
  into gamescope only) hides the TV's other 720x480 timings, so gamescope
  takes over in xbox-av's 59.94 Hz mode instead of the 60.00 Hz one it would
  pick (a full modeset: colour bars on the HDMI-to-AV box). It also keeps the
  connector's "content type" at No Data (gamescope's Game changed the AVI
  infoframe, which i915 can't fastset), skips gamescope's commit that turns
  the display off at exit, and leaves a holder process ("drmkeep-hold") with
  gamescope's DRM file open so its last frame stays up until xbox-av takes
  over; wincart-run stops the holder after that. WINCART_DRMKEEP=0 turns it
  off (DRMKEEP_HOLD=0 only the exit part); WINCART_DRMKEEP_SO tests another
  build.
- `WINCART_PROTON=DIR` runs a Proton build's Wine (e.g. GE-Proton11-11,
  unpacked under /data/proton/) in its own prefix, pfx-<build>, copying the
  build's vkd3d DLLs into it as proton's script does. GE-Proton11-11 runs
  Silent Hill 4 this way; the card's GE-Proton10-32 does not.

Guide redesign (2026-10-21 21:00): kazeta-guide is now built from the
redesigned Guide (claude/project-thread-xujlbp c5668dc3: Game/Settings tabs,
Now Playing; scenes in push-guide guide.tar.gz bc0b4120) with
`XB=<that tree>/xbcompat kazeta-guide/build.sh`. It needs those scenes in
/opt/xbcompat/bin/guide, so install it together with or after the Guide kit.
The game box shows no picture for a Windows cart yet (xbe_file_path is a weak
hook the host doesn't provide).
