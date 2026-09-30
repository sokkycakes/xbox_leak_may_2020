#!/usr/bin/env python3
"""Convert the 32-bit BMP screenshots xbcompat writes into PNG (stdlib only)."""
import struct
import sys
import zlib


def bmp2png(src, dst):
    d = open(src, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0] // 8
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        r = d[off + sy * stride: off + sy * stride + w * bpp]
        px = bytearray(w * 3)
        px[0::3], px[1::3], px[2::3] = r[2::bpp], r[1::bpp], r[0::bpp]
        rows.append(b"\0" + bytes(px))

    def chunk(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, abs(h), 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    open(dst, "wb").write(png)


if __name__ == "__main__":
    bmp2png(sys.argv[1], sys.argv[2])
