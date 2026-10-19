#!/bin/sh
# Linux: Splinter Cell missing-models bisect. Puts build a, b or c on the Sion
# and restarts the dashboard. The first run keeps the live build as
# xbcompat.pre-scbisect (never overwritten later).
#   a = everything, with SelectVertexShader back to the handle's own program (likely fix)
#   b = the first round only (Splinter Cell lighting push, a362b510)
#   c = build a without the scissor / model view / push buffer round (b25ff1ce)
# Usage: try.sh a|b|c [ssh-host]
cd "$(dirname "$0")" || exit 1
V=$1; HOST=${2:-sion}
case "$V" in a|b|c) ;; *) echo "usage: try.sh a|b|c [host]"; exit 1;; esac
grep " xbcompat.$V\$" MD5SUMS | md5sum -c - || exit 1
scp -q "xbcompat.$V" "$HOST:/tmp/xbcompat.new" || { echo 'Copy failed: is the Sion on and on the network?'; exit 1; }
ssh "$HOST" 'cd /opt/xbcompat/bin && /etc/init.d/S98xbox stop >/dev/null; [ -f xbcompat.pre-scbisect ] || cp xbcompat xbcompat.pre-scbisect; mv /tmp/xbcompat.new xbcompat && chmod 755 xbcompat && sion persist /opt/xbcompat/bin/xbcompat /opt/xbcompat/bin/xbcompat.pre-scbisect >/dev/null; /etc/init.d/S98xbox start >/dev/null; sleep 10; md5sum xbcompat; pidof xbcompat >/dev/null && echo "the Xbox dashboard is running" || echo "the dashboard is not running"'
