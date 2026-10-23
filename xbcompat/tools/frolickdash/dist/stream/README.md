# xbcompat with Streaming Download: Sion kit

The Sion's installed combined xbcompat (push-liveguide, gudfcx ff23b324:
Windows game cards, Live, the Guide, discs and the USB drive, the game
fixes) plus play as you download, to replace /opt/xbcompat/bin/xbcompat.
Built from ff23b324 with only src/kernel/dvd.c, io.c and misc.c changed
(frolick-xap-dash branch); the same tree without them builds to md5
8c1df1869eee9886fa3f7113afdcb85c, the Sion's live binary.

| file | what it is |
|---|---|
| xbcompat | the combined build plus streamed images (i386, stripped; interpreter /opt/xbcompat/lib/ld-linux.so.2) |

What it adds:
- an image beside a game.iso.stream and game.iso.map file is streamed:
  reads of chunks not there yet ask frolick-store for them on its socket
  and wait (src/kernel/dvd.c);
- a game installed in E:\Games as its disc image (default.iso beside its
  default.xbe; Frolick's Streaming Downloads) runs with that image as its
  DVD drive for that launch (src/kernel/misc.c). The USB drive is left
  alone. Every other game launches as before.

Replaces bfce8545's build (md5 860fd49d), which put a streamed disc in the
tray through /data/xbox/hdd/.dvdimage; that is gone.

The Guide is unchanged, so kazeta-guide needs no rebuild.

Install (nothing running: no game, no test): check the live binary is
still 860fd49d... (bfce8545), keep it as
/opt/xbcompat/bin/xbcompat.pre-gamestream-<date>, copy this one over it
(`chmod 755`) together with the store service from the game or sion kit,
then restart the dashboard. Check with `sha256sum -c SHA256SUMS` first.
The new store service moves a disc streamed under bfce8545
(/data/frolick/stream/<name>/disc.iso) into E:\Games\<title> without
fetching it again.

Frolick's side (the Spotlight item, its Play, the store helper that streams
and installs streamed games in E:\Games) comes in the sion and game kits.

Undo: put the .pre-gamestream copy back (streamed games in E:\Games then
launch without their disc), or xbcompat.pre-stream-20261023 for no
streaming at all.
