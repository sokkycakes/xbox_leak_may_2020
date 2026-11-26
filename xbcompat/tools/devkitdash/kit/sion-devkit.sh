#!/bin/sh
# Linux, macOS or WSL: puts the devkit dashboard on the Sion, or takes it off.
#   sh sion-devkit.sh              install it and boot into it (the Xbox dashboard stays)
#   sh sion-devkit.sh revert       boot the Xbox dashboard again (files stay)
#   sh sion-devkit.sh revert all   ... and remove the devkit dashboard
# Connects to root@192.168.1.247, the Sion on Wi-Fi (password "sion");
# HOST=root@other.address sh sion-devkit.sh to reach another.
cd "$(dirname "$0")" || exit 1
HOST=${HOST:-root@192.168.1.247}
OPT="-o StrictHostKeyChecking=accept-new"
if [ "$1" = revert ]; then
	scp $OPT revert-devkit.sh "$HOST:/tmp/" && ssh $OPT "$HOST" "sh /tmp/revert-devkit.sh $2; rm -f /tmp/revert-devkit.sh"
	exit
fi
sha256sum -c SHA256SUMS > /dev/null 2>&1 || shasum -a 256 -c SHA256SUMS > /dev/null || { echo 'the kit is damaged'; exit 1; }
echo "Copying to the Sion ($HOST)..."
ssh $OPT "$HOST" 'rm -rf /tmp/devkit && mkdir -p /tmp/devkit/media' &&
scp $OPT -q devkitdash.xbe devkitdash.map xbox-hostinfo xbox-bluetooth xbox-session xbox-session.patch xbox-session.base.sha256 xbox-session.oldimage \
	install-devkit.sh SHA256SUMS "$HOST:/tmp/devkit/" &&
scp $OPT -q media/* "$HOST:/tmp/devkit/media/" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
ssh $OPT "$HOST" 'sh /tmp/devkit/install-devkit.sh; rm -rf /tmp/devkit'
