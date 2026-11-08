#!/bin/bash
# BananaOS Ports entry: links this handheld to the Sion like a DS does
# (sionlink/rgds on the Sion Link branch). Put this file and the sionlink
# folder in roms/ports on the card. The RTL8821CS can be on one network at a
# time, so the handheld's own Wi-Fi goes down while linked and comes back
# after, if it was up.
D="$(cd "$(dirname "$0")" && pwd)/sionlink"
LOG="$D/log.txt"
was_up=
pidof wpa_supplicant >/dev/null && was_up=1
{
    echo "== $(date) Sion Link, Wi-Fi was ${was_up:+up}${was_up:-down}"
    modprobe rtw88_8821cs 2>/dev/null
    /usr/libexec/banana/wifi down
    ip link set wlan0 up 2>/dev/null
} >> "$LOG" 2>&1
restore() {
    [ -n "$was_up" ] && /usr/libexec/banana/wifi up >> "$LOG" 2>&1
}
trap 'restore; exit 0' TERM INT HUP
# run from /tmp: the card's exFAT may not keep the executable bit
cp "$D/slclient" /tmp/sion-slclient && chmod +x /tmp/sion-slclient
SL_NAME="${SL_NAME:-RG DS}" /tmp/sion-slclient --font "$D/font.ttf" >> "$LOG" 2>&1
restore
