#!/bin/sh
# Linux PC: puts Sion Link on the Sion and starts its DS network.
# Pass the Sion as the first argument if it isn't root@192.168.1.247.
cd "$(dirname "$0")" || exit 1
HOST=${1:-root@192.168.1.247}
OPT="-o StrictHostKeyChecking=accept-new"
echo "Sion Link kit 12 (2026-10-16)"
echo "Copying to the Sion ($HOST)..."
ssh $OPT "$HOST" 'mkdir -p /tmp/sionlink' &&
scp -q $OPT files/* "$HOST:/tmp/sionlink/" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
ssh $OPT "$HOST" 'sh /tmp/sionlink/install-sionlink.sh'
