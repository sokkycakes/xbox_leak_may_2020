#!/bin/bash
# Run the devkit dashboard built by build.sh under xbcompat the way the
# Sion's xbox-session runs it with XBOX_DASH=devkit: the devkit dashboard
# from C:\devkit, xbox-hostinfo keeping E:\DEVKIT\hostinfo.txt current, and
# the stock dashboard (C:\xboxdash.xbe) for one run whenever the devkit
# dashboard asks for it (E:\DEVKIT\run-xboxdash).  Titles either one
# launches come back to the devkit dashboard when they quit.
#
#   tools/devkitdash/run.sh [WORK] [xbcompat options...]
#
# WORK is build.sh's (default /tmp/devkitdash).  The hard disk is
# WORK/run/hdd; its partition2 (C:) gets the devkit files and, when
# DASHWORK names a tools/dashbuild work directory (default /tmp/xbdash),
# the stock dashboard's files.  RUNS=N stops after N dashboard runs
# (default 1, plus the stock dashboard's run when the devkit one asks for it;
# the Sion's session goes on forever).  xbox-bluetooth serves the Settings
# page's Bluetooth controllers through the host's bluetoothctl; BTCTL=FAKE
# uses tests/fake-bluetoothctl instead (state in WORK/run/fakebt.json).
HERE=$(cd "$(dirname "$0")" && pwd)
XBC=$(cd "$HERE/../.." && pwd)
WORK=${1:-/tmp/devkitdash}; shift
DASHWORK=${DASHWORK:-/tmp/xbdash}
HDD="$WORK/run/hdd"
mkdir -p "$HDD"/partition{1,2,3,4,5} "$HDD/partition1/Games" "$WORK/run/disc"
if [ -d "$DASHWORK/run/hdd/partition2" ]; then
    cp -ru "$DASHWORK/run/hdd/partition2/." "$HDD/partition2/"
fi
rm -rf "$HDD/partition2/devkit"
cp -r "$WORK/devkit" "$HDD/partition2/devkit"

HOSTINFO="$XBC/../bootani/buildroot/package/xbcompat/xbox-hostinfo"
"$HOSTINFO" "$HDD/partition1/DEVKIT/hostinfo.txt" 5 $$ &

BLUETOOTH="$XBC/../bootani/buildroot/package/xbcompat/xbox-bluetooth"
if [ "$BTCTL" = FAKE ]; then
    export BTCTL="$HERE/tests/fake-bluetoothctl" FAKEBT_STATE="$WORK/run/fakebt.json"
fi
TMPDIR="$WORK/run" sh "$BLUETOOTH" "$HDD/partition1/DEVKIT/BT" $$ &

runs=0
while :; do
    runs=$((runs + 1))
    if [ -e "$HDD/partition1/DEVKIT/run-xboxdash" ]; then
        rm -f "$HDD/partition1/DEVKIT/run-xboxdash"
        echo "== run $runs: the stock dashboard"
        XBCOMPAT_RESET=1 python3 "$XBC/tools/xbrun.py" "$HDD/partition2/xboxdash.xbe" --dvd "${DVD:-$WORK/run/disc}" \
            --d-path '\Device\CdRom0' --xbe-path xboxdash.xbe --hdd "$HDD" "$@"
    else
        echo "== run $runs: the devkit dashboard"
        python3 "$XBC/tools/xbrun.py" "$HDD/partition2/devkit/devkitdash.xbe" --dvd "${DVD:-$WORK/run/disc}" \
            --d-path '\Device\Harddisk0\Partition2\devkit' --xbe-path devkitdash.xbe --hdd "$HDD" "$@"
    fi
    echo "== exited with $?"
    # The devkit dashboard asked for the stock one, or more runs were asked for.
    [ -e "$HDD/partition1/DEVKIT/run-xboxdash" ] && continue
    [ -n "$RUNS" ] && [ "$runs" -lt "$RUNS" ] && continue
    break
done
