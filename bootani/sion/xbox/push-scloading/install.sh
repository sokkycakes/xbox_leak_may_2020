#!/bin/sh
# Linux: Splinter Cell loading-screen build. Puts this xbcompat on the Sion and
# restarts the dashboard. The first run keeps the live build as
# xbcompat.pre-scloading (never overwritten later).
# Usage: install.sh [ssh-host]
cd "$(dirname "$0")" || exit 1
HOST=${1:-sion}
md5sum -c MD5SUMS || exit 1
scp -q xbcompat "$HOST:/tmp/xbcompat.new" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
ssh "$HOST" 'cd /opt/xbcompat/bin && /etc/init.d/S98xbox stop >/dev/null; [ -f xbcompat.pre-scloading ] || cp xbcompat xbcompat.pre-scloading; mv /tmp/xbcompat.new xbcompat && chmod 755 xbcompat && sion persist /opt/xbcompat/bin/xbcompat /opt/xbcompat/bin/xbcompat.pre-scloading >/dev/null; /etc/init.d/S98xbox start >/dev/null; sleep 10; md5sum xbcompat; pidof xbcompat >/dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"'
