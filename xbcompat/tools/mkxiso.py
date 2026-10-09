#!/usr/bin/env python3
"""Make an Xbox disc image (XDVDFS, an "XISO") from a directory.

    tools/mkxiso.py DIR OUT.iso [--xgd1]

The result burns to a DVD-R that xbcompat plays from a USB DVD drive (or
runs directly with --dvd OUT.iso). --xgd1 lays it out like a pressed disc:
the game partition at 0x18300000, after an empty video partition, which is
how a full image of a pressed disc looks.

Each directory is written as a degenerate binary tree (every entry's right
child is the next name in order), which the format allows and every reader
walks the same way.
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
XGD1_BASE = 0x18300000


def entries(path):
    names = sorted(os.listdir(path), key=lambda n: n.upper())
    return [(n, os.path.join(path, n)) for n in names]


def dir_table(items):
    """items: [(name, sector, size, attr)] -> table bytes (entries never cross a sector)."""
    out = bytearray()
    offs = []
    for name, sector, size, attr in items:
        nb = name.encode("ascii")
        ln = (14 + len(nb) + 3) & ~3
        if len(out) // SECTOR != (len(out) + ln - 1) // SECTOR:
            out += b"\xff" * (SECTOR - len(out) % SECTOR)
        offs.append(len(out))
        out += bytes(ln)
    for i, (name, sector, size, attr) in enumerate(items):
        nb = name.encode("ascii")
        right = offs[i + 1] // 4 if i + 1 < len(items) else 0
        e = struct.pack("<HHIIBB", 0, right, sector, size, attr, len(nb)) + nb
        out[offs[i]:offs[i] + len(e)] = e
        pad = ((14 + len(nb) + 3) & ~3) - len(e)
        out[offs[i] + len(e):offs[i] + len(e) + pad] = b"\xff" * pad
    if not items:
        out = bytearray(b"\xff" * SECTOR)
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    base = XGD1_BASE if "--xgd1" in sys.argv[3:] else 0

    # Lay out: volume descriptor at sector 32, then directories, then files.
    next_sector = [33]

    def alloc(size):
        s = next_sector[0]
        next_sector[0] += max(1, (size + SECTOR - 1) // SECTOR)
        return s

    files = []          # (sector, host path)
    tables = []         # (sector, bytes)

    def place_dir(path):
        kids = entries(path)
        # Size the table first (sector numbers don't change its length).
        size = len(dir_table([(n, 0, 0, 0) for n, _ in kids]))
        sector = alloc(size)
        items = []
        for n, p in kids:
            if os.path.isdir(p):
                s, sz = place_dir(p)
                items.append((n, s, sz, 0x10))
            else:
                sz = os.path.getsize(p)
                s = alloc(sz)
                files.append((s, p))
                items.append((n, s, sz, 0x20))
        t = dir_table(items)
        tables.append((sector, t))
        return sector, len(t)

    root_sector, root_size = place_dir(src)
    with open(dst, "wb") as f:
        vd = bytearray(SECTOR)
        vd[0:20] = MAGIC
        vd[20:28] = struct.pack("<II", root_sector, root_size)
        vd[28:36] = struct.pack("<Q", 133000000000000000)
        vd[0x7EC:0x800] = MAGIC
        f.seek(base + 32 * SECTOR)
        f.write(vd)
        for s, t in tables:
            f.seek(base + s * SECTOR)
            f.write(t)
        for s, p in files:
            f.seek(base + s * SECTOR)
            with open(p, "rb") as g:
                while True:
                    b = g.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
        f.truncate(base + next_sector[0] * SECTOR)
    print(f"{dst}: {next_sector[0]} sectors{' (XGD1 layout)' if base else ''}")


if __name__ == "__main__":
    main()
