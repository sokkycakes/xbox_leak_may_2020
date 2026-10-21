#!/bin/sh
# Run on the Sion from this folder. Replaces sion-cards (game card mounts)
# with the version that unmounts a card pulled from a card reader, and
# restarts it. The old copy is kept as sion-cards.old for rollback.
set -e
cd "$(dirname "$0")"
for d in /data/overlay/usr/sbin /usr/sbin; do
	mkdir -p "$d"
	[ -f "$d/sion-cards" ] && [ ! -f "$d/sion-cards.old" ] && cp "$d/sion-cards" "$d/sion-cards.old"
	cp sion-cards "$d/sion-cards.new" && chmod 755 "$d/sion-cards.new" && mv "$d/sion-cards.new" "$d/sion-cards"
done
/etc/init.d/S35cards stop
sleep 1
/etc/init.d/S35cards start
sleep 3
echo "--- mounts under /media:"; grep ' /media/' /proc/mounts || echo "(none)"
md5sum /usr/sbin/sion-cards
