#!/bin/sh
# Linux: puts the xbcompat that draws Splinter Cell's models again on the Sion
# (two internal D3D functions go back to the game's own code) and restarts the
# dashboard. Keeps xbcompat.pre-scmodels. Usage: install.sh [ssh-host]
cd "$(dirname "$0")" || exit 1
HOST=${1:-sion}
echo "47e4d1f228274085194f62358ffc4526  xbcompat" | md5sum -c - || exit 1
scp -q xbcompat "$HOST:/tmp/xbcompat.new" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
ssh "$HOST" 'cd /opt/xbcompat/bin && /etc/init.d/S98xbox stop >/dev/null; cp xbcompat xbcompat.pre-scmodels && mv /tmp/xbcompat.new xbcompat && chmod 755 xbcompat && sion persist /opt/xbcompat/bin/xbcompat /opt/xbcompat/bin/xbcompat.pre-scmodels >/dev/null; /etc/init.d/S98xbox start >/dev/null; sleep 10; md5sum xbcompat; grep -E "fatal" /var/log/xbox.log | tail -n 2; pidof xbcompat >/dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"'
