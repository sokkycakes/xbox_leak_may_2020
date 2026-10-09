#!/bin/bash
# Boot the dashboard built by build.sh under xbcompat.  D: is the DVD drive
# (WORK/run/disc, or DVD=dir); an empty directory is an empty tray.
#   DVD=/path/to/extracted/disc tools/dashbuild/run.sh /tmp/xbdash --frames 600
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-/tmp/xbdash}; shift
R="$WORK/run"
exec python3 "$HERE/../xbrun.py" "$R/hdd/partition2/xboxdash.xbe" --dvd "${DVD:-$R/disc}" \
    --d-path '\Device\CdRom0' --xbe-path xboxdash.xbe --hdd "$R/hdd" "$@"
