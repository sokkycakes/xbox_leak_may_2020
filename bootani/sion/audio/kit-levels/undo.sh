#!/bin/sh
# Puts back the router and Guide sounds from before push-levels.
B=/usr/bin/sion-audiod
[ -e $B.pre-levels ] || { echo "no $B.pre-levels"; exit 1; }
cp $B.pre-levels $B.new && mv $B.new $B
persist=$B
for f in $(find /opt /usr -name '*.wav.pre-levels' 2>/dev/null); do
	cp "$f" "${f%.pre-levels}" && persist="$persist ${f%.pre-levels}"
done
sion persist $persist > /dev/null
killall sion-audiod
sleep 4
pidof sion-audiod > /dev/null && echo "old sion-audiod running" || echo "sion-audiod NOT running"
