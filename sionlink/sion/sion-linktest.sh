#!/bin/sh
# Linux PC: runs Link Test on the Sion and shows its log here. Ctrl+C ends it
# and the Sion's dashboard comes back. Add --headless to leave the screen alone.
# Pass the Sion as the first argument if it isn't root@192.168.1.247.
HOST=root@192.168.1.247
case "$1" in *@*|sion) HOST=$1; shift ;; esac
exec ssh -t -o StrictHostKeyChecking=accept-new "$HOST" sion-dslink linktest "$@"
