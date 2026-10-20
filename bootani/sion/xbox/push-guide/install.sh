#!/bin/sh
# Linux: the Guide (GFWL look) over every game. Puts this xbcompat and the
# Guide's assets (guide/, built by xbcompat/tools/guide/mkguide.py; not in
# git, they come with the kit from the project files) on the Sion and
# restarts the dashboard. The first run keeps the live build as
# xbcompat.pre-guide (never overwritten later).
# Open the Guide in a game: hold Back + Start for half a second.
# Usage: install.sh [ssh-host]
cd "$(dirname "$0")" || exit 1
HOST=${1:-sion}
md5sum -c MD5SUMS || exit 1
[ -f guide/hudbkgnd.xui ] || { echo 'guide/ is missing: unpack the kit from the project files here'; exit 1; }
scp -q xbcompat "$HOST:/tmp/xbcompat.new" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
tar cf - guide | ssh "$HOST" 'rm -rf /tmp/guide.new && mkdir /tmp/guide.new && tar xf - -C /tmp/guide.new' || exit 1
ssh "$HOST" 'cd /opt/xbcompat/bin && /etc/init.d/S98xbox stop >/dev/null; [ -f xbcompat.pre-guide ] || cp xbcompat xbcompat.pre-guide; mv /tmp/xbcompat.new xbcompat && chmod 755 xbcompat && rm -rf guide && mv /tmp/guide.new/guide guide && rmdir /tmp/guide.new && sion persist /opt/xbcompat/bin/xbcompat /opt/xbcompat/bin/xbcompat.pre-guide /opt/xbcompat/bin/guide >/dev/null; /etc/init.d/S98xbox start >/dev/null; sleep 10; md5sum xbcompat; ls guide | wc -l; pidof xbcompat >/dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"'
