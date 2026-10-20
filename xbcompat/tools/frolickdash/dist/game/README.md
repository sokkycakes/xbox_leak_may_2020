# Frolick as a game, Sion kit

The Frolick dashboard (`tools/frolickdash`, the commit it ships in) packaged
as an installed title: the Xbox dashboard stays as it is, lists Frolick under
Games and launches it like any game in E:\Games.  L+R+Back+Start, or a tap of
the power button, returns to the dashboard.

| File | Goes to | Note |
|---|---|---|
| default.xbe | E:\Games\Frolick\default.xbe | title ID 46524C4B "Frolick"; loads its files from D: (beside itself) |
| default.xip | E:\Games\Frolick\default.xip | the scene, art and XUI panels |
| launch.wav | the launch sound, played while the Xbox logo holds before a game starts | |
| b0d4c591...-d3d8+dsound+xapilib-9e55a416902bf3c8.map | the HLE map cache (/data/xbox/cache/xbcompat) | optional: xbrun makes its own on the first launch otherwise |
| install-game.sh | runs on the Sion | makes the folder, copies the engine's fonts (Xbox.xtf, XBox Book.xtf, Fonts/tahoma.ttf) from /opt/xbcompat/dash beside the XBE |
| sion-game.sh | runs on the PC | scp + ssh wrapper: `sh sion-game.sh` installs, `sh sion-game.sh remove` deletes the folder |

E:\Games\Frolick on the Sion is /data/xbox/hdd/partition1/Games/Frolick.
Removing Frolick is deleting that folder.  The same XBE run from the
dashboard partition (the `dist/sion` kit) finds its files on Y: instead.

Checked in the cloud: the real dashboard lists "Frolick" in Games with its
ID, Play Game launches it (XLaunchNewImage with D: = the game folder) and
the hub renders.

Check the files with `sha256sum -c SHA256SUMS`.
