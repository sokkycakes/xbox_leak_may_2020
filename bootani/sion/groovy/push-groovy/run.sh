#!/bin/sh
# Mirror the Sion's screen to a MiSTer running the Groovy core, as 480i.
# Nothing is installed and nothing restarts: it runs from wherever this kit
# sits and stops with Ctrl+C. Run as root on the Sion (needed to read the
# active framebuffer).
#
#   sh run.sh <mister-ip>            mirror the screen (LZ4, RGB888, 60 fps)
#   sh run.sh <mister-ip> --test     colour bars only, checks the link
#   sh run.sh <mister-ip> --rgb565   half the bandwidth, for Wi-Fi
set -e
D=$(cd "$(dirname "$0")" && pwd)
cd "$D"
md5sum -c MD5SUMS >/dev/null
IP=$1; shift || true
[ -n "$IP" ] || { echo "usage: sh run.sh <mister-ip> [--test|--rgb565]"; exit 2; }
chmod 755 ./sion-groovy
exec ./sion-groovy --host "$IP" -v "$@"
