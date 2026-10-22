# frolick-store

Frolick's storefront on the Sion, from the Internet Archive. Frolick runs as
an Xbox title under xbcompat and has no TLS, so this helper does the
archive.org work on the Linux side and talks to the dashboard through files
on its hard disk, E:\FROLICK\STORE (/data/xbox/hdd/partition1/FROLICK/STORE):

| file | written by | what it holds |
|---|---|---|
| catalog.txt | the helper | Spotlight's tabs and items: advancedsearch.php per tab, /metadata/{id} per item (title, description, the game's files and their size) |
| img/*.png | the helper | each item's tile (/services/img/{id}, made a 96x72 PNG: the dashboard decodes PNG) |
| queue.txt | the dashboard (nodes/Downloads.cpp) | the download queue, the active download first, paused or not |
| status.txt | the helper, every second | whether the Archive answers, the active download's progress, what the others have on disk, which have finished |
| history.txt | the dashboard | what finished (Download History) |

The dashboard side is nodes/Store.cpp (the catalog, `theStore`) and
nodes/Downloads.cpp (the queue, `theDownloads`). Spotlight's tab row is laid
out at build time (genpage.py `SPOTLIGHT`), so the helper's tabs are matched
by number: keep the two lists in step.

Downloads go to /data/frolick/downloads/<identifier>/, fetched from
/download/{id}/{file} (redirects followed, resumed with Range from
`<file>.part`). Which files: an item's disc images (every disc; an .iso set
over its .mds/.ccd copies), else its installers (a GOG setup .exe and its
.bin parts), else its largest archive; never metadata, torrents, thumbnails,
scans, covers or manuals. A finished item gets a frolick-item.txt (title,
kind, files, source, the Archive's credit). Removing an item from the queue
deletes what it had; finished items are kept.

Tabs (built in, Windows games first): Featured (a hand-picked list of free,
shareware and demo releases), Demos, Shareware, Freeware (searches on the
items' subjects, most downloaded first). /data/frolick/store.conf replaces
them:

    tab Featured | Windows game
    id oni-ae
    id DiabloSharewareWindowsBlizzardEntertainment1997
    tab Demos | Demo
    query mediatype:software AND subject:(windows) AND subject:(game) AND subject:(demo)
    rows 24

`build.sh` builds it (Go, static, the Mozilla roots built in since the Sion
has no CA store) and copies it with `sion/S97frolick-store` and
`sion/install-store.sh` into both kits in dist/. `frolick-store -once
-store DIR` builds a catalog into DIR and exits (how the cloud renders were
made).

A finished download becomes a Windows game card (cart.go): a Kazeta cart in
/data/frolick/carts (<cid>.kzi, Runtime=windows, and its folder: zips
unpacked, disc images extracted, Joliet names kept) plus a stub title in
E:\Games\<cid> (default.xbe, .wincart naming the .kzi, icon.png). The
library and the Xbox dashboard list it with the installed games; launching
it, xbcompat sees the .wincart and runs wincart-run on the cart (Wine in
Conty, with the Guide), as for the game card's Windows carts. Exec is the
game's own program when the files have one (frolick-run.bat starts it from
its own folder); a disc or download that only has an installer gets
frolick-install.bat, which runs it once (silently for Inno Setup, NSIS, Wise
and MSI; InstallShield and others show their own windows and need a mouse
or keyboard). After that run the helper finds the installed program in
/data/wincarts/<cid>/pfx and points Exec at it, so the next launch plays.
Checked in the cloud with Wine: Diamond Caves II (Wise) installed silently
and started from its card.
