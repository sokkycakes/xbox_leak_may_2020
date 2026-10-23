# xbcompat with Streaming Download: Sion kit

The Sion's installed combined xbcompat (push-liveguide, gudfcx ff23b324:
Windows game cards, Live, the Guide, discs and the USB drive, the game
fixes) plus play as you download, to replace /opt/xbcompat/bin/xbcompat.
Built from ff23b324 with only src/kernel/dvd.c, io.c and misc.c changed
(xbcompat-gudfcx.patch here: apply it to gudfcx for any later combined build); the same tree without them builds to md5
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
  alone. Every other game launches as before;
- Frolick's dashboard sees the hard disk's real size and free space (its
  Memory screens); games and the stock dashboard still see a volume of
  at most 8 GB (src/kernel/io.c).

Replaces 8984cdff's build (md5 1ca4a892), which had Frolick's Memory
screens at 8 GB free of 8 GB on the internal SSD.

The Guide is unchanged, so kazeta-guide needs no rebuild.

Install (nothing running: no game, no test): check the live binary is
still 1ca4a892... (8984cdff), keep it as
/opt/xbcompat/bin/xbcompat.pre-memsize-<date>, copy this one over it
(`chmod 755`), then restart the dashboard. Check with
`sha256sum -c SHA256SUMS` first. Nothing else changes.

Frolick's side (the Spotlight item, its Play, the store helper that streams
and installs streamed games in E:\Games) comes in the sion and game kits.

Undo: put the .pre-memsize copy back.
