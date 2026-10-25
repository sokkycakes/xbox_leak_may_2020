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
| status.txt | the helper, every second | whether the Archive answers, the active download's progress, what the others have on disk, which have finished, which history items' cards were deleted (gone) |
| history.txt | the dashboard | what finished (Download History) |
| play.txt | the dashboard | the Streaming Download to play now (empty when the dashboard starts) |

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

Tabs (built in): Featured (sokky's pick of Xbox games, all Streaming
Downloads: Xbox Essentials discs and MechAssault), Demos, Shareware, Freeware (searches on the
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
or keyboard; their .kzi says FrolickInput=mouse). A SETUP.EXE stub that
only starts the disc's AUTORUN.INF program is passed over for that program
(Diablo's Blizzard installer). After that run the helper finds the installed program in
/data/wincarts/<cid>/pfx and points Exec at it, so the next launch plays.
Checked in the cloud with Wine: Diamond Caves II (Wise) installed silently
and started from its card.

While one of the store's cards runs, the helper also watches its Wine
session (session.go): wincart-run only ends a run after it has seen the
game's processes, so a program that quits at once left Wine's services
holding gamescope on a blank screen. Once nothing but those services has
run for 15 seconds, the helper kills Wine (its services ignore SIGTERM)
and stops gamescope, and wincart-run returns to
the dashboard.

Deleting a store game from a Memory screen (its E:\Games stub) uninstalls
it (uninstall.go): the helper then removes its cart folder, .kzi, Wine
prefix (/data/wincarts/<cid>) and download record, and status.txt says
"gone" for it so the dash drops it from its history and the store offers it
again. The stub holds frolick-size.dat, a sparse file as long as the cart
folder and prefix, so the Memory screens show the game's real size without
it taking that space on E:.

Streaming Download (play.go, stream.go, disc.go): an Xbox disc image on
the Archive (a catalog entry `id <item>/<file>.iso | Xbox game`) is
flagged `stream` in catalog.txt, and its tile is the title image inside
its default.xbe, read with a few range requests. Play on it writes
play.txt; the helper installs the game as E:\Games\<title> (default.iso,
the disc image at its full size and filled in as it comes; default.xbe;
frolick-stream.txt, where it comes from) and runs itself with `-stream` to
fill the image. Once the disc's directory and default.xbe are in, the
dashboard starts it from E:\Games like any installed game; xbcompat gives
a game whose folder holds a default.iso that image as its DVD drive
(src/kernel/misc.c), streamed while default.iso.stream and .map are beside
it (dvd.c). Reads the game makes ahead of the download wait for their
chunk; the rest fills in behind them and after the game until the whole
disc is in and checked (md5, sha1), and then the stream files go: an
ordinary installed game. The Memory screens show it at its full size and
delete it like any game (the helper then stops filling it); one left
unfinished by a reboot is filled again in the background. Logs and stats
are in /data/frolick/stream/<name>.
