# Devkit dashboard, Sion kit

The devkit dashboard (`xbcompat/tools/devkitdash`, the commit it ships in),
packaged to boot on the Sion in place of the Xbox dashboard. The Xbox
dashboard is not replaced or deleted: it stays at `C:\xboxdash.xbe`, the
devkit dashboard lists it first, and A opens it.

## Install

From a Linux, macOS or WSL shell on the same network as the Sion:

    sh sion-devkit.sh                 # install and restart into it
    sh sion-devkit.sh revert          # boot the Xbox dashboard again (files stay)
    sh sion-devkit.sh revert all      # ...and remove the devkit dashboard

It connects to `root@192.168.1.247` (password `sion`); set `HOST=root@...`
for another address.

## What it changes on the Sion

| File | Goes to | Note |
|---|---|---|
| devkitdash.xbe | /opt/xbcompat/dash/devkit/ (C:\devkit) | the dashboard, title ID FFFE0000 |
| devkitdash.map | /opt/xbcompat/dash/devkit/ | its HLE symbol map (`xbrun.py --map-only`) |
| media/background.bmp | /opt/xbcompat/dash/devkit/media/ | the background; any 24 or 32-bit BMP works, scaled to 640x480 |
| media/font.ttf, fontb.ttf | /opt/xbcompat/dash/devkit/media/ | Liberation Sans and Sans Bold (metric-compatible with Arial), drawn with XFONT; any TrueType font works |
| xbox-hostinfo | /usr/libexec/ | writes the name and addresses to E:\DEVKIT\hostinfo.txt every 5 s |
| xbox-bluetooth | /usr/libexec/ | pairs Bluetooth controllers for the Settings page through bluetoothctl (E:\DEVKIT\BT) |
| xbox-session | /usr/libexec/ | the session, taught `XBOX_DASH=devkit`; the image's copy is kept as `xbox-session.pre-devkit`. The installer applies `xbox-session.patch` to the image's own session, and only uses the whole file when that session is the one the patch was made from (`xbox-session.base.sha256`). A session patched by an earlier kit is patched again from `xbox-session.pre-devkit` |
| xbox-session.oldimage | /usr/libexec/xbox-session | on images from before the map cache (October 2026), which the patch doesn't fit: that image's session with the devkit dashboard added. The Xbox dashboard has no reset combination on those images; a game started from it comes back to the devkit dashboard when it quits, or `sion restart xbox` |
| (one line) | /etc/default/xbox | `XBOX_DASH=devkit` |

Everything is kept across reboots with `sion persist`. Without
`XBOX_DASH=devkit` the new session behaves exactly as before.

## Using it

- Up/down (d-pad or left stick) moves, the triggers page.
- **A** launches the title with a developer command line: launch data of type
  `LDT_FROM_DEBUGGER_CMDLINE`, `szCmdLine` = `-dev`, the way a devkit's
  debugger starts a title. A title reads it with `XGetLaunchInfo`.
- **X** launches it plainly, as the retail dashboard would.
- **Y** hides or shows the list (remembered).
- **Start** opens Settings, where Bluetooth controllers are paired: Y
  searches, A pairs the selected controller (put it in pairing mode first),
  connects or disconnects it, X twice forgets it, B goes back. Paired
  controllers reconnect by themselves when switched on. This needs an image
  with BlueZ (built from this commit on); on older images the page says
  "This system image has no Bluetooth support".
- On **Xbox Dashboard**, A runs the stock dashboard the way the session
  always runs it. L + R + Back + Start (or a tap of the power button) comes
  back to the devkit dashboard, as does quitting any game.

The list is `C:\xboxdash.xbe`, the disc in the drive, then
`E:\Games\*\default.xbe`, `E:\Apps\*\default.xbe` and every `.xbe` in
`E:\DEVKIT\*\`, by name. It is rescanned every two seconds.
`E:\DEVKIT\devkit.ini` holds the settings: `cmdline=` (what A passes),
`launcher=0/1` and the last title launched, which is selected at start.

On the Sion, E: is `/data/xbox/hdd/partition1`.

The fonts are Liberation Sans, made to match Arial's letter widths (SIL Open
Font Licence, free to redistribute; Arial itself can't be shipped). To use
Arial itself, copy arial.ttf and arialbd.ttf over media/font.ttf and
media/fontb.ttf.

Check the files with `sha256sum -c SHA256SUMS`.
