#!/usr/bin/env python3
"""Rasterise a Figma icon SVG (one path of M/L/H/V/C/Z, absolute, as the
Material Symbols exports are) to a PNG at its own size: the fill colour and
the group opacity come from the file, holes by even-odd (each subpath XORs
into the mask), 8x supersampled.

    svgicon.py icon.svg icon.png
"""
import re, sys
from PIL import Image, ImageDraw, ImageChops

SS = 8


def subpaths(d):
    toks = re.findall(r"[MLHVCZ]|-?[\d.]+(?:e-?\d+)?", d)
    i, cur, start, out, path = 0, (0.0, 0.0), (0.0, 0.0), [], []
    cmd = None
    while i < len(toks):
        t = toks[i]
        if t in "MLHVCZ":
            cmd = t; i += 1
            if cmd == "Z":
                if path: out.append(path)
                path = []; cur = start
            continue
        if cmd == "M":
            cur = start = (float(toks[i]), float(toks[i + 1])); i += 2
            path = [cur]; cmd = "L"
        elif cmd == "L":
            cur = (float(toks[i]), float(toks[i + 1])); i += 2; path.append(cur)
        elif cmd == "H":
            cur = (float(toks[i]), cur[1]); i += 1; path.append(cur)
        elif cmd == "V":
            cur = (cur[0], float(toks[i])); i += 1; path.append(cur)
        elif cmd == "C":
            p1 = (float(toks[i]), float(toks[i + 1])); p2 = (float(toks[i + 2]), float(toks[i + 3]))
            p3 = (float(toks[i + 4]), float(toks[i + 5])); i += 6
            p0 = cur
            for k in range(1, 17):
                u = k / 16.0; v = 1 - u
                path.append((v * v * v * p0[0] + 3 * v * v * u * p1[0] + 3 * v * u * u * p2[0] + u * u * u * p3[0],
                             v * v * v * p0[1] + 3 * v * v * u * p1[1] + 3 * v * u * u * p2[1] + u * u * u * p3[1]))
            cur = p3
    if path: out.append(path)
    return out


def main(src, dst):
    s = open(src).read()
    w = int(float(re.search(r'width="([\d.]+)"', s).group(1)))
    h = int(float(re.search(r'height="([\d.]+)"', s).group(1)))
    fill = re.search(r'<path[^>]*fill="(#[0-9A-Fa-f]{6})"', s).group(1)
    op = re.search(r'opacity="([\d.]+)"', s)
    op = float(op.group(1)) if op else 1.0
    d = re.search(r'<path[^>]* d="([^"]*)"', s).group(1)
    mask = Image.new("L", (w * SS, h * SS), 0)
    for sp in subpaths(d):
        m = Image.new("L", mask.size, 0)
        ImageDraw.Draw(m).polygon([(x * SS, y * SS) for x, y in sp], fill=255)
        mask = ImageChops.logical_xor(mask.convert("1"), m.convert("1")).convert("L")
    mask = mask.resize((w, h), Image.LANCZOS).point(lambda a: int(a * op))
    r, g, b = int(fill[1:3], 16), int(fill[3:5], 16), int(fill[5:7], 16)
    out = Image.new("RGBA", (w, h), (r, g, b, 0))
    out.putalpha(mask)
    out.save(dst)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
