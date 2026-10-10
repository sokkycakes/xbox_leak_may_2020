#!/usr/bin/env python3
"""Signatures of the leak's XDK 4400 libraries for the Pi image, made at build
time (mksigs needs the leak's .lib files, which the Pi hasn't). Every
combination, as tools/sion/bundle.sh makes for the Sion: the XBLA disc's
CDX key installer (CDXU/reboot.xbe) links XAPILIB alone, and without a
signature file for that its map can't be made on the Pi, so installing a
game's key failed. Written to $XDG_CACHE_HOME/xbcompat."""
import itertools
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import xbrun

for suffix in ("", "d"):
    names = [n + suffix for n in ("d3d8", "dsound", "xapilib")]
    for k in (1, 2, 3):
        for libs in itertools.combinations(names, k):
            xbrun.signatures(list(libs))
