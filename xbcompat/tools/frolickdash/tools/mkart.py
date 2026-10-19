#!/usr/bin/env python3
"""The hub's sprite art, rendered from main.tscn's descriptions.

    mkart.py outdir

Writes, for the Sprite nodes genhub.py places:
  label_catalog.png, label_library.png, label_settings.png: the pad names
    as Godot's Label3D draws them, 32 px heavy condensed type with +6 px
    glyph spacing and a 12 px outline, in the text and outline colours of
    each label (rendered at 2x: pixelSize is 0.002 * 0.552553 / 2).
  circle.png: the ring the three stacked Sprite3D emblems use, in place of
    frolick_assets/textures/Circle.png (240x240, white, tinted per pad).
  glyph_a.png: a fallback A button, only when the directory lacks one.

Frolick's own Circle.png and label_*.png replace these when they arrive;
drop them in the same directory before mkassets.py runs.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

S = 2  # supersampling
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
LABELS = {  # text, text colour, outline colour (main.tscn Label3D modulate / outline_modulate)
    "library": ((0.31, 0.31, 0.31), (0.839844, 0.839844, 0.839844)),
    "settings": ((0.7, 0.693, 0.616), (0.31, 0.31, 0.31)),
    "catalog": ((0.7, 0.693, 0.616), (0.31, 0.31, 0.31)),
}


def rgb(c, a=255):
    return tuple(int(v * 255 + 0.5) for v in c) + (a,)


def label(text, colour, outline, path):
    size, spacing, stroke = 32 * S, 6 * S, 12 * S
    font = ImageFont.truetype(FONT, size)
    # Trebuchet MS at font_stretch 50 is a condensed face; DejaVu Sans Bold
    # is drawn and then narrowed to match its proportions.
    condense = 0.78
    widths = [font.getlength(ch) for ch in text]
    w = int(sum(widths) + spacing * (len(text) - 1) + stroke * 2 + 8)
    h = int(size * 1.4 + stroke * 2)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x = stroke + 4
    y = stroke
    for ch, cw in zip(text, widths):
        d.text((x, y), ch, font=font, fill=rgb(colour), stroke_width=stroke, stroke_fill=rgb(outline))
        x += cw + spacing
    # trim to the ink, keeping it centred on the text (Label3D centres its box)
    box = im.getbbox()
    im = im.crop(box)
    im = im.resize((max(1, int(im.width * condense)), im.height), Image.LANCZOS)
    im.save(path)
    return im.size


def circle(path, size=240):
    im = Image.new("RGBA", (size * S, size * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = 6 * S
    d.ellipse((pad, pad, size * S - pad, size * S - pad), outline=(255, 255, 255, 255), width=30 * S)
    im = im.resize((size, size), Image.LANCZOS)
    im.save(path)


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, (colour, outline) in LABELS.items():
        print(name, label(name, colour, outline, os.path.join(out, "label_%s.png" % name)))
    circle(os.path.join(out, "circle.png"))
    print("wrote hub art to", out)
