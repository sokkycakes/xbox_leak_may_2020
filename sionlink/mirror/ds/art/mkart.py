#!/usr/bin/env python3
# Bakes the DS app's own look into source/nitroart.h:
#   top.png, bottom.png   sokky's "DS UI" Figma frames (NitroLink), 256x192: the top one as
#                         exported, the bottom one with its placeholder text taken out (the
#                         app writes its own there). Dithered to the DS's 15-bit colour
#                         (ordered, 4x4: rounding alone banded the gradient and tinted it),
#                         packed as the live link packs bundles (LZ4 blocks, common/mirror_lz.h).
#   the font              Tahoma at its own 10 pixels, from akmenu-next's tahoma.bdf (the
#                         tahoma.pcf in frolick_assets is the same font, put through bdftopcf):
#                         Latin-1, Latin Extended-A and the punctuation the menus use.
#
#   python3 mkart.py PATH/TO/tahoma.bdf     (needs Pillow)
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "source", "nitroart.h")

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def rgb555(path):
    im = Image.open(path).convert("RGB")
    assert im.size == (256, 192), path
    px = im.load()
    out = bytearray()
    for y in range(192):
        for x in range(256):
            t = (BAYER[y & 3][x & 3] + 0.5) / 16
            r, g, b = (min(31, int(c * 31 / 255 + t)) for c in px[x, y])
            out += struct.pack("<H", r | g << 5 | b << 10 | 0x8000)   # bit 15: shown (16-bit bitmap BG)
    return bytes(out)


def lz4_pack(src):
    """A greedy LZ4 block, as harness/lzpack.h makes them"""
    n = len(src)
    out = bytearray()
    table = {}
    anchor = i = 0
    limit = n - 12 if n > 12 else 0

    def length(v):
        while v >= 255:
            out.append(255)
            v -= 255
        out.append(v)

    def seq(lit, off, match):
        ml = match - 4 if match else 0
        out.append((min(len(lit), 15) << 4) | min(ml, 15))
        if len(lit) >= 15:
            length(len(lit) - 15)
        out.extend(lit)
        if match:
            out.extend(struct.pack("<H", off))
            if ml >= 15:
                length(ml - 15)

    while i < limit:
        key = src[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= 0xFFFF:
            m = 4
            while i + m < n - 5 and src[cand + m] == src[i + m]:
                m += 1
            seq(src[anchor:i], i - cand, m)
            for k in range(i + 1, min(i + m, limit)):
                table[src[k:k + 4]] = k
            i += m
            anchor = i
        else:
            i += 1
    seq(src[anchor:], 0, 0)
    return bytes(out)


def lz4_unpack(s, size):
    d = bytearray()
    i = 0
    while i < len(s):
        tok = s[i]
        i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = s[i]
                i += 1
                lit += b
                if b != 255:
                    break
        d += s[i:i + lit]
        i += lit
        if i >= len(s):
            break
        off = s[i] | s[i + 1] << 8
        i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = s[i]
                i += 1
                ml += b
                if b != 255:
                    break
        for _ in range(ml + 4):
            d.append(d[-off])
    assert len(d) == size
    return bytes(d)


WANT = (list(range(0x20, 0x7F)) + list(range(0xA0, 0x180)) + list(range(0x2010, 0x2027)) +
        [0x2030, 0x2039, 0x203A, 0x20AC, 0x2122, 0x2190, 0x2191, 0x2192, 0x2193])


def bdf_glyphs(path):
    glyphs, ascent, cur = {}, None, None
    for line in open(path, encoding="latin-1"):
        w = line.split()
        if not w:
            continue
        if w[0] == "FONT_ASCENT":
            ascent = int(w[1])
        elif w[0] == "STARTCHAR":
            cur = {"rows": []}
        elif w[0] == "ENCODING":
            cur["code"] = int(w[1])
        elif w[0] == "DWIDTH":
            cur["adv"] = int(w[1])
        elif w[0] == "BBX":
            cur["w"], cur["h"], cur["x"], cur["y"] = map(int, w[1:5])
        elif w[0] == "BITMAP":
            cur["bitmap"] = True
        elif w[0] == "ENDCHAR":
            del cur["bitmap"]
            glyphs[cur["code"]] = cur
            cur = None
        elif cur is not None and cur.get("bitmap"):
            cur["rows"].append(bytes.fromhex(w[0])[:(cur["w"] + 7) // 8])
    return glyphs, ascent


def carray(name, data):
    lines = ["static const u8 %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: mkart.py tahoma.bdf")
    out = ["// Made by art/mkart.py: the NitroLink look (sokky's Figma) and Tahoma 10 px. Don't edit.",
           "#ifndef NITROART_H", "#define NITROART_H", ""]
    for name, f in (("artTop", "top.png"), ("artBottom", "bottom.png")):
        raw = rgb555(os.path.join(HERE, f))
        packed = lz4_pack(raw)
        assert lz4_unpack(packed, len(raw)) == raw
        out.append("// %s: 256x192, 15-bit, %d bytes packed" % (f, len(packed)))
        out.append(carray(name + "Lz", packed))
        out.append("")
    glyphs, ascent = bdf_glyphs(sys.argv[1])
    bits = bytearray()
    table = []
    for code in WANT:
        g = glyphs.get(code)
        if not g:
            continue
        at = len(bits)
        for row in g["rows"]:
            bits += row
        # top of the glyph's box from the top of the line, the line ASCENT above the baseline
        table.append((code, g["x"], ascent - (g["y"] + g["h"]), g["w"], g["h"], g["adv"], at))
    out.append("#define FONT_ASCENT %d   // the baseline, from the top of a line" % ascent)
    out.append("#define FONT_LINE 12")
    out.append("typedef struct { u16 code; s8 x, y; u8 w, h, adv; u16 bits; } font_glyph;   // bits: rows, MSB first, whole bytes")
    out.append("static const font_glyph fontGlyphs[%d] = {" % len(table))
    for t in table:
        out.append("    {0x%04x, %d, %d, %d, %d, %d, %d}," % t)
    out.append("};")
    out.append(carray("fontBits", bits))
    out.append("")
    out.append("#endif")
    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print("%s: %d glyphs, art %s" % (OUT, len(table), os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
