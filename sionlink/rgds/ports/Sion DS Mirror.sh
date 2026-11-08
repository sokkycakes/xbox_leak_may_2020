#!/bin/bash
# BananaOS Ports entry: the DS game running on the Sion, on this handheld's two screens
# (the Sion's DS mirror, sionlink/mirror). The handheld runs the same client a real DS
# runs (sion-link-live.nds) in mirrorrun, whose simulated DSpico talks to the Sion's
# mirrorrun (--serve usb,tcp:7790) over the network. Top screen on the top panel, bottom
# screen on the bottom panel, touch on the bottom panel; F quits.
#
# The network: the home Wi-Fi when the handheld is on it and the Sion answers there
# (SION_HOST in sionlink/mirror.conf), else the Sion's own DS network (needs
# `sion-dslink start` on the Sion): the handheld joins it and reaches the Sion at its
# IPv6 link-local address. Home Wi-Fi comes back afterwards if it was up.
D="$(cd "$(dirname "$0")" && pwd)/sionlink"
LOG="$D/mirror-log.txt"
SION_HOST=192.168.1.247
SION_PORT=7790
[ -f "$D/mirror.conf" ] && . "$D/mirror.conf"
exec >> "$LOG" 2>&1
echo "== $(date) Sion DS Mirror"

# run from /tmp: the card's exFAT may not keep the executable bit
for f in mirrorrun slclient; do cp "$D/$f" /tmp/sion-$f && chmod +x /tmp/sion-$f; done

was_up=
pidof wpa_supplicant >/dev/null && was_up=1
joiner=
restore() {
    [ -n "$joiner" ] && kill "$joiner" 2>/dev/null && wait "$joiner" 2>/dev/null
    joiner=
    if [ -n "$was_up" ] && ! pidof wpa_supplicant >/dev/null; then /usr/libexec/banana/wifi up; fi
    # the bottom panel's backlight back as run-port left it
    [ -n "$BL" ] && echo 4 > "$BL/bl_power" 2>/dev/null && echo "$BL" > /run/banana/panel-off
}
trap 'restore; exit 0' TERM INT HUP

target=
if [ -n "$was_up" ] && timeout 3 bash -c "exec 3<>/dev/tcp/$SION_HOST/$SION_PORT" 2>/dev/null; then
    # (the Sion takes that test connection for a viewer and waits again once it closes)
    sleep 0.5
    target="tcp:$SION_HOST:$SION_PORT"
    echo "home Wi-Fi: the Sion answers at $SION_HOST"
else
    echo "the Sion isn't on the home Wi-Fi here; joining its DS network"
    modprobe rtw88_8821cs 2>/dev/null
    /usr/libexec/banana/wifi down
    ip link set wlan0 up 2>/dev/null
    : > /tmp/sion-join.log
    SL_NAME="${SL_NAME:-RG DS}" /tmp/sion-slclient --join-only > /tmp/sion-join.log 2>&1 &
    joiner=$!
    for i in $(seq 1 120); do
        grep -q '^joined ' /tmp/sion-join.log && break
        kill -0 "$joiner" 2>/dev/null || break
        sleep 0.5
    done
    cat /tmp/sion-join.log
    b=$(sed -n 's/^joined //p' /tmp/sion-join.log)
    [ -n "$b" ] || { echo "couldn't join the Sion's DS network"; restore; exit 1; }
    # the Sion's link-local address, from its MAC (EUI-64)
    set -- $(echo "$b" | tr ':' ' ')
    ll="fe80::$(printf %x $((0x$1 ^ 2)))$2:$3ff:fe$4:$5$6"
    target="tcp:[$ll%wlan0]:$SION_PORT"
    echo "the Sion at $ll"
fi

# both panels lit: run-port darkened the bottom one
BL=$(cat /run/banana/panel-off 2>/dev/null)
[ -n "$BL" ] && echo 0 > "$BL/bl_power" && rm -f /run/banana/panel-off

# the RG DS pad by where its buttons sit (SDL's a = the bottom button); F is the Guide (quits)
export SDL_GAMECONTROLLERCONFIG="190000004b4800000111000000010000,RetroGame Joypad,a:b0,b:b1,x:b3,y:b2,back:b8,start:b9,guide:b10,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b11,rightstick:b12,dpup:b13,dpdown:b14,dpleft:b15,dpright:b16,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,"
export SDL_VIDEODRIVER=evdev SDL_BANANA_PANELS=stack SDL_BANANA_TOP=${BANANA_TOP:-DSI-2}
echo "mirror: $target"
/tmp/sion-mirrorrun "$D/sion-link-live.nds" 0 --dspico "$target" --realtime 1 --jit 1 --view sdl
echo "mirror exited with $?"
restore
