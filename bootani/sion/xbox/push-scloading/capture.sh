#!/bin/sh
# Linux: record what xbcompat on the Sion does while something happens on
# screen (the fade, a loading screen). For SECONDS (default 20) it asks for a
# full trace of one frame every 2 seconds, then saves everything the log got
# meanwhile to capture-NAME.log here. Start it, then do the thing on the Sion.
# Usage: [EVERY=seconds] capture.sh NAME [SECONDS] [ssh-host]
cd "$(dirname "$0")" || exit 1
NAME=${1:?usage: capture.sh NAME [SECONDS] [host]}; SECS=${2:-20}; HOST=${3:-sion}
START=$(ssh "$HOST" 'wc -l < /var/log/xbox.log') || { echo 'Is the Sion on and on the network?'; exit 1; }
echo "capturing $NAME for $SECS seconds"
ssh "$HOST" "EVERY=${EVERY:-2}; end=\$((\$(date +%s) + $SECS)); while [ \$(date +%s) -lt \$end ]; do touch /tmp/xbcompat-trace-frame; sleep \$EVERY; done"
ssh "$HOST" "tail -n +$((START + 1)) /var/log/xbox.log" > "capture-$NAME.log"
echo "saved capture-$NAME.log ($(wc -l < "capture-$NAME.log") lines)"
grep -E "not found|can't open|fatal" "capture-$NAME.log" | sort | uniq -c | sort -rn | head -n 20
