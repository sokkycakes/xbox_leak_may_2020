#!/bin/sh
# Linux: puts the Frolick dashboard on the Sion, or takes it off again.
#   sh sion-frolick.sh           install it (the Xbox dashboard is kept as *.pre-frolick)
#   sh sion-frolick.sh revert    put the Xbox dashboard back
# Connects to root@192.168.1.247, the Sion on Wi-Fi (password "sion").
# Pass another host as the second argument.
cd "$(dirname "$0")" || exit 1
HOST=${2:-root@192.168.1.247}
OPT="-o StrictHostKeyChecking=accept-new"
if [ "$1" = revert ]; then
	scp $OPT revert-frolick.sh "$HOST:/tmp/" && ssh $OPT "$HOST" 'sh /tmp/revert-frolick.sh; rm -f /tmp/revert-frolick.sh'
	exit
fi
echo "Copying to the Sion ($HOST)..."
scp $OPT xboxdash.xbe xboxdash.map default.xip *.wav install-frolick.sh frolick-store S97frolick-store install-store.sh "$HOST:/tmp/" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
ssh $OPT "$HOST" 'sh /tmp/install-frolick.sh; rm -f /tmp/install-frolick.sh'
