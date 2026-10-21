#!/bin/bash
# Boot the Frolick dashboard built by build.sh under xbcompat.
#   tools/frolickdash/run.sh [WORK] [xbrun options...]
#   e.g. run.sh /tmp/xbdash --frames 120 --screenshot hub.png --shot-frame 100
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/xbdash}; shift
R="$WORK/frolick/run"
exec python3 "$HERE/../xbrun.py" "$R/hdd/partition2/xboxdash.xbe" --dvd "${DVD:-$R/disc}" \
    --d-path '\Device\CdRom0' --xbe-path xboxdash.xbe --hdd "$R/hdd" "$@"
