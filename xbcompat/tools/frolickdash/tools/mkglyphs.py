#!/usr/bin/env python3
"""Stand-in button glyphs for a build without Frolick's own art.

    mkglyphs.py outdir

Writes glyph_a.png, glyph_a_button.png, glyph_b.png, glyph_x.png and
glyph_y.png (100x100, like Frolick's): a coloured disc with the button's
letter.  Frolick's real glyphs replace them when they are available (set
GLYPHS to their directory for build.sh).
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

COLOURS = {"a": (0x7f, 0xc3, 0x3a), "b": (0xe3, 0x4b, 0x3a), "x": (0x3a, 0x8e, 0xe3), "y": (0xe3, 0xc0, 0x3a)}


def glyph(letter, path, rim=False):
    im = Image.new("RGBA", (100, 100), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = COLOURS[letter]
    d.ellipse((4, 4, 95, 95), fill=(0, 0, 0, 160))
    d.ellipse((6, 6, 93, 93), fill=c + (255,))
    d.ellipse((14, 10, 86, 60), fill=tuple(min(255, v + 70) for v in c) + (90,))
    if rim:
        d.ellipse((2, 2, 97, 97), outline=(255, 255, 255, 220), width=4)
    try:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 54)
    except OSError:
        f = ImageFont.load_default()
    t = letter.upper()
    box = d.textbbox((0, 0), t, font=f)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((100 - w) / 2 - box[0], (100 - h) / 2 - box[1] - 2), t, font=f, fill=(255, 255, 255, 255))
    im.save(path)


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
for letter in "abxy":
    glyph(letter, os.path.join(out, "glyph_%s.png" % letter))
glyph("a", os.path.join(out, "glyph_a_button.png"), rim=True)
print("wrote stand-in glyphs to", out)
