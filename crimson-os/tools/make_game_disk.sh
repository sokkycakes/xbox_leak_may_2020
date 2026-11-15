#!/bin/sh
# Packs a Windows app into a game disk for the Era Game OS, like a game package.
#   make_game_disk.sh <app folder> <exe inside it> <out.img> [size MB]
# The disk gets \Crimson\launch-game.cmd, which the Game OS runs at boot.
# QEMU:    -drive file=out.img,format=raw,if=ide
# Hyper-V: qemu-img convert -O vhdx out.img game.vhdx, then pass -GameDisk game.vhdx
set -e
[ $# -ge 3 ] || { echo "usage: $0 <app folder> <exe> <out.img> [size MB]"; exit 1; }
src=$1 exe=$2 out=$3
need=$(du -sm "$src" | cut -f1)
size=${4:-$((need + 64))}
tmp=$(mktemp -d)
mkdir -p "$tmp/Crimson" "$tmp/App"
cp -r "$src"/. "$tmp/App/"
printf '@echo off\r\ncd /d %%~d0\\App\r\nstart "" "%%~d0\\App\\%s"\r\n' "$exe" > "$tmp/Crimson/launch-game.cmd"
rm -f "$out"
truncate -s ${size}M "$out"
echo 'start=2048, type=0c' | sfdisk -q "$out"
mformat -i "$out@@1M" -F -v GAME ::
mcopy -s -i "$out@@1M" "$tmp/Crimson" "$tmp/App" ::/
rm -rf "$tmp"
printf "wrote %s (%s MB): App/%s\n" "$out" "$size" "$exe"
