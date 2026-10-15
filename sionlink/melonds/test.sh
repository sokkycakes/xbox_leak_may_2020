#!/bin/sh
# Emulator test loop: a software Sion (sionlinkd --melon) and melonDS with the
# AirMP bridge exchange real DS radio frames over UDP on localhost.
#   1. build melonDS at BASE with melonds-airmp.patch applied
#   2. sh test.sh /path/to/melonDS ../ds/sionlink.nds
# Type a line into this terminal to send it to the DS. Map DS keys in
# melonDS's Input settings (or melonDS.toml) first.
MELON=${1:?melonDS binary}; NDS=${2:?nds file}
here=$(cd "$(dirname "$0")" && pwd)
MELONDS_AIR=127.0.0.1:7071 "$MELON" "$NDS" &
exec "$here/../host/sionlinkd" --melon 7071 -v
