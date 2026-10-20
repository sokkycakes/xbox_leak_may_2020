#!/bin/sh
# Linux: puts Frolick on the Sion as a game (E:\Games\Frolick), or removes it.
#   sh sion-game.sh           install
#   sh sion-game.sh remove    delete E:\Games\Frolick again
# Connects to root@192.168.1.247, the Sion on Wi-Fi (password "sion").
# Pass another host as the second argument.
cd "$(dirname "$0")" || exit 1
HOST=${2:-root@192.168.1.247}
OPT="-o StrictHostKeyChecking=accept-new"
if [ "$1" = remove ]; then
	ssh $OPT "$HOST" 'rm -r /data/xbox/hdd/partition1/Games/Frolick && echo removed'
	exit
fi
echo "Copying to the Sion ($HOST)..."
scp $OPT default.xbe default.xip launch.wav *.map install-game.sh "$HOST:/tmp/" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
ssh $OPT "$HOST" 'sh /tmp/install-game.sh; rm -f /tmp/install-game.sh'
