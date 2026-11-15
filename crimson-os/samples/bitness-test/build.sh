#!/bin/sh
# Builds bitness32.exe and bitness64.exe (needs gcc-mingw-w64-i686 and -x86-64),
# then bitness-disk.img, a game disk whose launcher starts both.
set -e
cd "$(dirname "$0")"
mkdir -p out/app
for b in 32 64; do
  [ $b = 32 ] && cc=i686-w64-mingw32 || cc=x86_64-w64-mingw32
  $cc-windres bitness.rc -O coff -o out/res$b.o
  $cc-gcc -O2 -mwindows -o out/app/bitness$b.exe bitness.c out/res$b.o -lcomctl32 -lopengl32
done
../../tools/make_game_disk.sh out/app bitness32.exe out/bitness-disk.img 64
printf '@echo off\r\ncd /d %%~d0\\App\r\nstart "" "%%~d0\\App\\bitness32.exe"\r\nstart "" "%%~d0\\App\\bitness64.exe"\r\n' > out/launch-game.cmd
mcopy -o -i out/bitness-disk.img@@1M out/launch-game.cmd ::/Crimson/launch-game.cmd
echo "After a run, read the answers with: mtype -i out/bitness-disk.img@@1M ::/App/result-32.txt"
