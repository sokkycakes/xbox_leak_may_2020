#!/usr/bin/env python3
"""Render a texture dumped by XBCOMPAT_DUMP_TEXTURES (texN_WxH_fmt.bin) as a PNG."""
import re, struct, sys, zlib
src, dst = sys.argv[1], sys.argv[2]
w, h, fmt = re.search(r"_(\d+)x(\d+)_([0-9a-f]+)\.bin$", src).groups()
w, h, fmt = int(w), int(h), int(fmt, 16)
data = open(src, "rb").read()
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        i = y * w + x
        if fmt in (0x06, 0x07, 0x12, 0x1E):
            b, g, r, a = data[i*4:i*4+4]
            if fmt in (0x07, 0x1E): a = 255
        elif fmt in (0x04, 0x1D):
            v = struct.unpack_from("<H", data, i*2)[0]
            a, r, g, b = [((v >> s) & 15) * 17 for s in (12, 8, 4, 0)]
        elif fmt in (0x05, 0x11):
            v = struct.unpack_from("<H", data, i*2)[0]
            r, g, b, a = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3, 255
        elif fmt in (0x02, 0x03, 0x10):
            v = struct.unpack_from("<H", data, i*2)[0]
            a, r, g, b = (255 if v >> 15 else 0), ((v >> 10) & 31) << 3, ((v >> 5) & 31) << 3, (v & 31) << 3
            if fmt == 0x03: a = 255
        elif fmt in (0x00, 0x13):
            r = g = b = data[i]; a = 255
        elif fmt in (0x19, 0x1F):
            r = g = b = 255; a = data[i]
        elif fmt in (0x1A, 0x20):
            r = g = b = data[i*2]; a = data[i*2+1]
        else:
            r = g = b = a = 128
        row += bytes((r, g, b, a))
    rows.append(bytes(row))
raw = b"".join(rows)
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open(dst, "wb").write(png)
