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
- Frolick sees the hard disk's real size and free space on its Memory
  screens: an XBE named "Frolick", or any XBE run from Games/Frolick (the
  Sion's Frolick is an older build still named "Xbox Dashboard"); games
  and the stock dashboard still see a volume of at most 8 GB
  (src/kernel/io.c). As on a console, the cache drives (X:, Y:, Z:) are
  hidden: Frolick's total is the disk less a 2.25 GB reservation for them,
  and what they (and xbcompat's own cache beside the hard disk) hold
  doesn't count as used, never showing more free than there really is.

Replaces 274fef3a's build (md5 742ff4a0), which counted the cache drives
in Frolick's used space.

The Guide is unchanged, so kazeta-guide needs no rebuild.

Install (nothing running: no game, no test): check the live binary is
still 742ff4a0... (274fef3a), keep it as
/opt/xbcompat/bin/xbcompat.pre-memcache-<date>, copy this one over it
(`chmod 755`), then restart the dashboard. Check with
`sha256sum -c SHA256SUMS` first. Nothing else changes.

Frolick's side (the Spotlight item, its Play, the store helper that streams
and installs streamed games in E:\Games) comes in the sion and game kits.

Undo: put the .pre-memcache copy back.
