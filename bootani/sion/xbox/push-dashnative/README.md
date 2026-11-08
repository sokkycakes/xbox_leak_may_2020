# push-dashnative

Puts the Xbox dashboard (and Frolick, which is built from the same source) back on its native view.

Since 2026-10-14 the Sion ran with `XBCOMPAT_DASH_VIEW=4:3` in /etc/default/xbox: the dash built its 3D view as 4:3 (640/480) and that picture was stretched over the 720-wide screen, so everything looked a little wide. Without it the dash uses the aspect it was made with (720/480 over its 640x480 picture), as on a real Xbox. Games never saw this setting.

Files: sion-install.sh only. Nothing else is replaced.
Install: `sh sion-install.sh` as root on the Sion. `NOW=1` restarts the Xbox session right away when no game is running; otherwise the view changes at the next dashboard start.
Undo: `REVERT=1 sh sion-install.sh` (or /etc/default/xbox.pre-dashnative).
