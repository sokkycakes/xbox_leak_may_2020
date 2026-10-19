#!/usr/bin/env python3
"""Button glyphs for the panels.

    mkglyphs.py outdir [art360dir]

With art360dir (the A/B/X/Y-Button.png of the Xbox 360 Blades dashboard's
shared resources, xam.xex's shrdres package, 32x32 orbs), writes
glyph_a.png, glyph_a_button.png, glyph_b.png, glyph_x.png and glyph_y.png
(100x100, like Frolick's) from those orbs with the button's letter over
them, as the Blades legend draws them, and glyph_start.png from the grey
orb with the Start triangle.  Without it, stand-ins: a coloured
disc with the letter.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

COLOURS = {"a": (0x7f, 0xc3, 0x3a), "b": (0xe3, 0x4b, 0x3a), "x": (0x3a, 0x8e, 0xe3), "y": (0xe3, 0xc0, 0x3a)}


def font(size):
    try:
        return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", size)
    except OSError:
        return ImageFont.load_default()


def letter(im, t, size):
    d = ImageDraw.Draw(im)
    f = font(size)
    box = d.textbbox((0, 0), t, font=f)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((im.width - w) / 2 - box[0], (im.height - h) / 2 - box[1] - 2), t, font=f, fill=(255, 255, 255, 255))


def glyph(l, path, rim=False):
    im = Image.new("RGBA", (100, 100), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = COLOURS[l]
    d.ellipse((4, 4, 95, 95), fill=(0, 0, 0, 160))
    d.ellipse((6, 6, 93, 93), fill=c + (255,))
    d.ellipse((14, 10, 86, 60), fill=tuple(min(255, v + 70) for v in c) + (90,))
    if rim:
        d.ellipse((2, 2, 97, 97), outline=(255, 255, 255, 220), width=4)
    letter(im, l.upper(), 54)
    im.save(path)


def glyph360(l, src, path, rim=False):
    orb = Image.open(src).convert("RGBA")
    im = Image.new("RGBA", (100, 100), (0, 0, 0, 0))
    im.paste(orb.resize((100, 100), Image.LANCZOS), (0, 0))
    if rim:
        ImageDraw.Draw(im).ellipse((2, 2, 97, 97), outline=(255, 255, 255, 220), width=4)
    letter(im, l.upper(), 50)
    im.save(path)


def start(path):
    im = Image.new("RGBA", (100, 100), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((4, 4, 95, 95), fill=(0, 0, 0, 160))
    d.ellipse((6, 6, 93, 93), fill=(0x60, 0x60, 0x60, 255))
    d.polygon([(38, 30), (38, 70), (70, 50)], fill=(255, 255, 255, 255))
    im.save(path)


def start360(src, path):
    """The Start button: the Blades legend draws it as the grey orb with a
    white triangle (the packages carry no Start image, only the orb)."""
    orb = Image.open(src).convert("RGBA")
    im = Image.new("RGBA", (100, 100), (0, 0, 0, 0))
    im.paste(orb.resize((100, 100), Image.LANCZOS), (0, 0))
    ImageDraw.Draw(im).polygon([(38, 30), (38, 70), (70, 50)], fill=(255, 255, 255, 255))
    im.save(path)


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
art = sys.argv[2] if len(sys.argv) > 2 else None
# Frolick's names: glyph_a.png is the Start button (its file is Xelu's
# 360_Start.png), glyph_a_button.png the A button.
for l in "bxy":
    if art:
        glyph360(l, os.path.join(art, "%s-Button.png" % l.upper()), os.path.join(out, "glyph_%s.png" % l))
    else:
        glyph(l, os.path.join(out, "glyph_%s.png" % l))
if art:
    glyph360("a", os.path.join(art, "A-Button.png"), os.path.join(out, "glyph_a_button.png"))
    start360(os.path.join(art, "disabled-Button.png"), os.path.join(out, "glyph_a.png"))
    print("wrote Xbox 360 glyphs to", out)
else:
    glyph("a", os.path.join(out, "glyph_a_button.png"))
    start(os.path.join(out, "glyph_a.png"))
    print("wrote stand-in glyphs to", out)
