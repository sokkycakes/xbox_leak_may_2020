# xbcompat with Streaming Download: Sion kit

The Sion's installed combined xbcompat (push-liveguide, gudfcx ff23b324:
Windows game cards, Live, the Guide, discs and the USB drive, the game
fixes) plus play as you download, to replace /opt/xbcompat/bin/xbcompat.
Built from ff23b324 with only src/kernel/dvd.c and src/kernel/io.c changed
(frolick-xap-dash branch); the same tree without them builds to md5
8c1df1869eee9886fa3f7113afdcb85c, the Sion's live binary.

| file | what it is |
|---|---|
| xbcompat | the combined build plus streamed images (i386, stripped; interpreter /opt/xbcompat/lib/ld-linux.so.2) |

What it adds:
- an image beside a game.iso.stream and game.iso.map file is streamed:
  reads of chunks not there yet ask frolick-store for them on its socket
  and wait (src/kernel/dvd.c);
- while /data/xbox/hdd/.dvdimage names an image, that image is in the DVD
  tray, over the USB drive. frolick-store writes it when a Streaming
  Download is ready to play and removes it when the dashboard comes back,
  so the dashboard starts the game the way it starts a disc, in its own
  process (no stepping aside). Without the file nothing changes.

The Guide is unchanged, so kazeta-guide needs no rebuild.

Install (nothing running: no game, no test): keep the live binary as
/opt/xbcompat/bin/xbcompat.pre-stream-<date>, check its md5 is still
8c1df186..., copy this one over it (`chmod 755`), then restart the
dashboard. Check with `sha256sum -c SHA256SUMS` first.

Frolick's side (the Spotlight item, its Play, the store helper that streams
and fills the tray) comes in the sion and game kits.

Undo: put the .pre-stream copy back.
