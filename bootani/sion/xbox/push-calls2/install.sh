#!/bin/sh
# Linux: puts the xbcompat with all kernel calls, the late-SDK D3D/sound calls
# and the Splinter Cell lighting fixes on the Sion, with its xbsymmap.py, and
# restarts the dashboard. Keeps xbcompat.pre-calls and xbsymmap.py.pre-calls.
# Usage: install.sh [ssh-host]   (default: sion)
cd "$(dirname "$0")" || exit 1
HOST=${1:-sion}
echo "82fa3e422db1acf3e536f5cb55ddcaa1  xbcompat" | md5sum -c - || exit 1
scp -q xbcompat xbsymmap.py "$HOST:/tmp/" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
ssh "$HOST" 'cd /opt/xbcompat/bin && /etc/init.d/S98xbox stop >/dev/null; cp xbcompat xbcompat.pre-calls && cp /opt/xbcompat/tools/xbsymmap.py /opt/xbcompat/tools/xbsymmap.py.pre-calls && mv /tmp/xbcompat xbcompat && chmod 755 xbcompat && mv /tmp/xbsymmap.py /opt/xbcompat/tools/xbsymmap.py && sion persist /opt/xbcompat/bin/xbcompat /opt/xbcompat/bin/xbcompat.pre-calls /opt/xbcompat/tools/xbsymmap.py /opt/xbcompat/tools/xbsymmap.py.pre-calls >/dev/null; /etc/init.d/S98xbox start >/dev/null; sleep 10; md5sum xbcompat; grep -E "fatal" /var/log/xbox.log | tail -n 2; pidof xbcompat >/dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"'
