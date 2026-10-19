#!/usr/bin/env python3
"""The hub's sprite art, rendered from main.tscn's descriptions.

    mkart.py outdir [labelfont.ttf]

Writes, for the Sprite nodes genhub.py places:
  label_catalog.png, label_library.png, label_settings.png: the pad names
    as Godot's Label3D draws them: Trebuchet MS (art/fonts/trebuc.ttf, the
    one Frolick ships) at 32 px, natural width, +6 px glyph spacing and a
    3 px outline (0.75 x Label3D's outline_size 12, as Frolick's label bake has it), in the text and outline colours of each label (rendered
    at 2x: pixelSize is 0.002 * 0.552553 / 2).  main.tscn's font_stretch 50
    and font_weight 999 only pick a face; Trebuchet has neither, so the
    text is drawn as the Regular face is.
  circle.png: the ring the three stacked Sprite3D emblems use, in place of
    frolick_assets/textures/Circle.png (240x240, white, tinted per pad).
  glyph_a.png: a fallback A button, only when the directory lacks one.

Frolick's own Circle.png and label_*.png replace these when they arrive;
drop them in the same directory before mkassets.py runs.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

S = 2  # supersampling
FONT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "art", "fonts", "trebuc.ttf")
LABELS = {  # text, text colour, outline colour (main.tscn Label3D modulate / outline_modulate)
    "library": ((0.31, 0.31, 0.31), (0.839844, 0.839844, 0.839844)),
    "settings": ((0.7, 0.693, 0.616), (0.31, 0.31, 0.31)),
    "catalog": ((0.7, 0.693, 0.616), (0.31, 0.31, 0.31)),
}


def rgb(c, a=255):
    return tuple(int(v * 255 + 0.5) for v in c) + (a,)


def label(text, colour, outline, path, fontfile=FONT):
    # As Frolick's own label bake (frolick_assets/label_mesh.py) draws them:
    # the outline is an outward stroke of 0.75 x Label3D's default
    # outline_size 12, i.e. 0.094 em (3 px at 32 px), round joins, and the
    # glyph spacing of 6 is at its 37 px logical size, 0.162 em.
    size = 32 * S
    spacing, stroke = int(round(0.162 * size)), int(round(0.09375 * size))
    font = ImageFont.truetype(fontfile, size)
    widths = [font.getlength(ch) for ch in text]
    w = int(sum(widths) + spacing * (len(text) - 1) + stroke * 2 + 8)
    h = int(size * 1.4 + stroke * 2)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # The outline goes behind every glyph, as Label3D draws it: all the
    # outlines first, then all the fills, or each glyph's outline would eat
    # into its neighbours' fills and the letters would look condensed.
    for pass_fill, pass_stroke in ((rgb(outline), stroke), (rgb(colour), 0)):
        x = stroke + 4
        y = stroke
        for ch, cw in zip(text, widths):
            d.text((x, y), ch, font=font, fill=pass_fill, stroke_width=pass_stroke, stroke_fill=rgb(outline))
            x += cw + spacing
    # trim to the ink, keeping it centred on the text (Label3D centres its box)
    box = im.getbbox()
    im = im.crop(box)
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
    fontfile = sys.argv[2] if len(sys.argv) > 2 else FONT
    os.makedirs(out, exist_ok=True)
    for name, (colour, outline) in LABELS.items():
        print(name, label(name, colour, outline, os.path.join(out, "label_%s.png" % name), fontfile))
    circle(os.path.join(out, "circle.png"))
    print("wrote hub art to", out)
