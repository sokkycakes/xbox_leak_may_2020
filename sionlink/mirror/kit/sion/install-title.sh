#!/bin/sh
#
# Installs a DS game on the Sion as an Xbox title: it shows up in the Xbox dashboard's
# Games and in Frolick like any game, and launching it runs it in the DS runtime
# (dscart-run: mirrorrun on the TV, the Sion's pads, a DS over Sion Link, the game's
# menu on a 1.5 s Back hold). Run on the Sion, as root, from the kit folder:
#
#   sh install-title.sh GAME.nds ["Name"] [id] [GAMECODE]
#   sh install-title.sh /tmp/sionlink/mkds.nds          (the defaults: the Mario Kart DS demo)
#   sh install-title.sh --remove [id]                   takes the title and the cart off again
#                                                       (the saves stay in UDATA)
#
# What goes where (kept across reboots):
#   /opt/sionlink/            mirrorrun, mirrorrun-gl, dscart-run, dstitle.py (sion persist)
#   /data/sionlink/carts/     ID.kzi (Runtime=nds) and ID/GAME.nds, a copy of the game
#   E:\Games\<id>\            the title: a stub default.xbe and a .wincart naming the cart
#                             (/data/xbox/hdd/partition1/Games)
#   E:\UDATA\<title id>\      its title image and names; 000000000001\ the save, the
#                             quick save and the screen layout (a save beside GAME.nds is
#                             brought over the first time)
#   wincart-run               one line, after a copy of it as wincart-run.pre-ds: DS carts
#                             go to dscart-run (xbcompat hands every stub title to
#                             wincart-run)
# The dashboard restarts (when it's all that runs) so it lists the new title. The game
# never leaves the Sion.
K=$(cd "$(dirname "$0")" && pwd)
B=/opt/sionlink
D=/data/sionlink
X=/opt/xbcompat
W=$X/tools/wincart-run
E=/data/xbox/hdd/partition1

# nothing playing: the dashboard is xbcompat running xboxdash.xbe (a game it starts,
# Frolick too, replaces it), Windows games run under gamescope
playing=
for p in $(pidof xbcompat); do
	tr '\0' ' ' < "/proc/$p/cmdline" | grep -q xboxdash.xbe || playing="a game"
done
pidof gamescope kazeta-guide > /dev/null && playing="a Windows game"
pidof mirrorrun mirrorrun-gl > /dev/null && playing="the DS mirror"
if [ -n "$playing" ]; then
	echo "$playing is running: not installing (go back to the dashboard first)"
	exit 1
fi

py() { /usr/bin/env LD_LIBRARY_PATH=$X/lib PYTHONHOME=$X PYTHONDONTWRITEBYTECODE=1 $X/bin/python3 "$@"; }

restart_dash() {
	pidof xbcompat > /dev/null || { echo "the dashboard isn't running: it lists the change when it next starts"; return 0; }
	/etc/init.d/S98xbox stop > /dev/null 2>&1
	/etc/init.d/S98xbox start > /dev/null 2>&1
	sleep 10
	pidof xbcompat > /dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"
}

if [ "$1" = --remove ]; then
	id=${2:-mario-kart-ds-demo}
	kzi=$D/carts/$id.kzi
	[ -f "$kzi" ] || { echo "no $kzi"; exit 1; }
	for w in "$E"/Games/*/.wincart; do
		grep -qx "kzi=$kzi" "$w" 2>/dev/null && rm -rf "${w%/.wincart}" && echo "removed ${w%/.wincart}"
	done
	rm -rf "$kzi" "${kzi%.kzi}"
	echo "removed the cart $id (its saves stay in $E/UDATA)"
	restart_dash
	exit 0
fi

ROM=$1
NAME=${2:-Mario Kart DS Demo}
ID=${3:-mario-kart-ds-demo}
CODE=${4-AMKE}
[ -n "$ROM" ] && [ -r "$ROM" ] || { echo "usage: sh install-title.sh GAME.nds [\"Name\"] [id] [GAMECODE] | --remove [id]"; exit 1; }
case $ID in *[!a-z0-9-]*|'') echo "the id is lower case letters, digits and -"; exit 1 ;; esac
[ -x $X/bin/python3 ] || { echo "no $X/bin/python3"; exit 1; }
[ -f $W ] || { echo "no $W: this xbcompat has no Windows-cart support, which DS titles use"; exit 1; }
for f in mirrorrun mirrorrun-gl dscart-run dstitle.py; do
	[ -f "$K/$f" ] || { echo "the kit has no $f"; exit 1; }
done

# the runtime
mkdir -p $B
for f in mirrorrun mirrorrun-gl dscart-run dstitle.py; do
	cp "$K/$f" $B/$f.new && chmod 755 $B/$f.new && mv $B/$f.new $B/$f
done
sion persist $B > /dev/null

# the cart: a Kazeta cart on the Sion's own disk
game=${ROM##*/}
cart=$D/carts/$ID
mkdir -p "$cart" $D/logs
if ! cmp -s "$ROM" "$cart/$game"; then
	cp "$ROM" "$cart/$game.new" && mv "$cart/$game.new" "$cart/$game" || { echo "couldn't copy the game"; exit 1; }
fi
{
	echo "Name=$NAME"
	echo "Id=$ID"
	echo "Runtime=nds"
	echo "Exec=$game"
	[ -n "$CODE" ] && echo "Gamecode=$CODE"
} > "$D/carts/$ID.kzi"

# the title and its UDATA
tid=$(py $B/dstitle.py "$D/carts/$ID.kzi" --games "$E/Games" --udata "$E/UDATA" | tail -n1)
[ -n "$tid" ] || { echo "dstitle.py failed"; exit 1; }
tid=$(echo "$tid" | tr 'A-F' 'a-f')
slot=$E/UDATA/$tid/000000000001
# a save made by the mirror before (tv.sh keeps it beside the game) comes along once
for ext in sav state; do
	src=${ROM%.*}.$ext
	[ -f "$src" ] && [ ! -e "$slot/${game%.*}.$ext" ] && cp "$src" "$slot/${game%.*}.$ext" && echo "brought over $src"
done

# wincart-run hands DS carts to dscart-run
if ! grep -q dscart-run $W; then
	cp $W $W.pre-ds
	sed -i '/^runtime=\$(key Runtime)$/a\
case $runtime in nds*) exec sh /opt/sionlink/dscart-run "$kzi" --title-id "$tid" ;; esac   # DS carts: Sion Link' $W
	grep -q dscart-run $W || { cp $W.pre-ds $W; echo "couldn't add DS carts to $W (left as it was)"; exit 1; }
	sion persist $W $W.pre-ds > /dev/null
	echo "wincart-run hands DS carts to dscart-run (the one before: $W.pre-ds)"
fi

echo "installed: $NAME, title $tid, E:\\Games\\$ID"
md5sum $B/mirrorrun $B/mirrorrun-gl $B/dscart-run $B/dstitle.py "$cart/$game" $W
restart_dash
