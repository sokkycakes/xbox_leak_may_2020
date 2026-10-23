# Frolick dashboard, Sion kit

The Frolick dashboard built on the Xbox dashboard engine (`tools/frolickdash`,
the commit it ships in), packaged to run on the Sion in place of the Xbox dashboard.

| File | Goes to | Note |
|---|---|---|
| xboxdash.xbe | /opt/xbcompat/dash/xboxdash.xbe | the Frolick dashboard |
| xboxdash.map | /opt/xbcompat/dash/xboxdash.map | its HLE symbol map (`tools/xbrun.py --map-only`, findsigs over d3d8, dsound, xapilib; independent of the xbcompat binary) |
| default.xip | /opt/xbcompat/dash/default.xip | the scene, art and XUI panels |
| launch.wav | the launch sound, played while the Xbox logo holds before a game starts | |
| woosh.wav, blades_*.wav | the UI sounds: Frolick's woosh pad to pad, the 360 Blades' focus / select / back / open / close for the rest | loose files beside the XBE |
| menu_music.wav | the menu ambience, looped under the menus (Xbox ADPCM, 34 MB, streamed from the file) | |
| frolick-store | /opt/frolick/bin/frolick-store | the storefront helper (`store/`, static x86-64): the Internet Archive catalog for Spotlight and the downloads behind the Downloads page, through files in E:\FROLICK\STORE; downloads land in /data/frolick/downloads |
| S97frolick-store | /etc/init.d/S97frolick-store | starts the helper at boot; log /var/log/frolick-store.log |
| install-store.sh | runs on the Sion | installs the two above (sion persist) and starts the helper; the kit's installer runs it |
| install-frolick.sh | runs on the Sion | keeps the replaced files as `*.pre-frolick`, restarts S98xbox |
| revert-frolick.sh | runs on the Sion | puts the `*.pre-frolick` copies back |
| sion-frolick.sh | runs on the PC | scp + ssh wrapper: `sh sion-frolick.sh` installs, `sh sion-frolick.sh revert` reverts |

Only those three files in /opt/xbcompat/dash change. xbcompat itself, the other
.xip files, Audio/ and Fonts/ stay. The XBE expects the Xbox dashboard's own
fonts beside it, which the Sion has (Xbox.xtf, XBox Book.xtf, Fonts/tahoma.ttf).
If the picture is narrower than the cloud screenshots, the Sion's
`XBCOMPAT_DASH_VIEW=4:3` in /etc/default/xbox is the likely cause.

Check the files with `sha256sum -c SHA256SUMS`.

Streaming Download (Play on Half-Life 2 in Spotlight) needs the store service from this kit and the xbcompat in dist/stream installed in /opt/xbcompat.
