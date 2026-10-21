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
