#!/bin/bash
# BananaOS Ports entry: Sion Link, the DS app as a native program (rgds/linkapp), on both
# panels. While no DS game is mirrored the handheld is a pad for the Sion (Controller); a DS
# title started on the Sion takes the panels (Mirror) and gives them back when it ends (F
# leaves it, A goes back); on the Sion's own DS network the Sion's programs come over
# Download Play and start in the DS core. Home Wi-Fi first when the Sion answers there
# (SION_HOST in sionlink/mirror.conf); Y switches to the Sion's DS network (needs
# `sion-dslink start` on the Sion) and back. F quits; home Wi-Fi comes back if it was up.
D="$(cd "$(dirname "$0")" && pwd)/sionlink"
LOG="$D/log.txt"
SION_HOST=192.168.1.247
[ -f "$D/mirror.conf" ] && . "$D/mirror.conf"
exec >> "$LOG" 2>&1
echo "== $(date) Sion Link"

# run from /tmp: the card's exFAT may not keep the executable bit
for f in sionlink-app mirrorrun; do cp "$D/$f" /tmp/sion-$f && chmod +x /tmp/sion-$f; done

was_up=
pidof wpa_supplicant >/dev/null && was_up=1
restore() {
    if [ -n "$was_up" ] && ! pidof wpa_supplicant >/dev/null; then /usr/libexec/banana/wifi up; fi
    # the bottom panel's backlight back as run-port left it
    [ -n "$BL" ] && echo 4 > "$BL/bl_power" 2>/dev/null && echo "$BL" > /run/banana/panel-off
}
trap 'restore; exit 0' TERM INT HUP

# both panels lit: run-port darkened the bottom one
BL=$(cat /run/banana/panel-off 2>/dev/null)
[ -n "$BL" ] && echo 0 > "$BL/bl_power" && rm -f /run/banana/panel-off

# the RG DS pad by where its buttons sit (SDL's a = the bottom button); F is the Guide (quits)
export SDL_GAMECONTROLLERCONFIG="190000004b4800000111000000010000,RetroGame Joypad,a:b0,b:b1,x:b3,y:b2,back:b8,start:b9,guide:b10,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,leftstick:b11,rightstick:b12,dpup:b13,dpdown:b14,dpleft:b15,dpright:b16,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,"
export SDL_VIDEODRIVER=evdev SDL_BANANA_PANELS=stack SDL_BANANA_TOP=${BANANA_TOP:-DSI-2}
export SION_HOST SL_NAME="${SL_NAME:-RG DS}"
/tmp/sion-sionlink-app --mirrorrun /tmp/sion-mirrorrun --nds "$D/sion-link-live.nds"
echo "Sion Link exited with $?"
restore
