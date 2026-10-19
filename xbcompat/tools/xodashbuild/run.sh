#!/bin/bash
# Run the Live dashboard built by build.sh under xbcompat, with the emulated
# network card on the NAT backend unless --net says otherwise.
#   tools/xodashbuild/run.sh /tmp/xodash --frames 600
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/xodash}; shift
R="$WORK/run"
exec python3 "$HERE/../xbrun.py" "$R/hdd/partition2/xodash.xbe" --dvd "$R/disc" --d-path '\\Device\\Harddisk0\\Partition2' --xbe-path xodash.xbe --hdd "$R/hdd" --net "${XBCOMPAT_NET:-nat}" "$@"
