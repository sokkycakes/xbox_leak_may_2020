#!/usr/bin/env python3
"""mkbg.py IN OUT.bmp: any picture PIL reads -> the devkit dashboard's
640x480 24-bit background.  A 4:3 picture is scaled; another shape is
cropped to 4:3 about its centre first."""
import sys
from PIL import Image

src, dst = sys.argv[1:3]
im = Image.open(src).convert("RGB")
w, h = im.size
if w * 3 != h * 4:
    if w * 3 > h * 4:
        nw = h * 4 // 3
        im = im.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
    else:
        nh = w * 3 // 4
        im = im.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))
im.resize((640, 480), Image.LANCZOS).save(dst, "BMP")
