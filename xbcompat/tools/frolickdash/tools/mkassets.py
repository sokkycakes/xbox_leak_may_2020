#!/usr/bin/env python3
"""Pack the Frolick dashboard's data into a XIP archive.

    mkassets.py out.xip srcdir [srcdir ...]

Every file under the source directories goes in under its relative path
(backslashes, as the dashboard looks names up: y:\\pads\\library.xui).
PNG images are converted to .xtx, the raw texture the XuiPanel node
reads: "XTX1", uint32 width, uint32 height, then width*height*4 bytes of
B,G,R,A (not premultiplied).  Later directories override earlier ones.

Layout (private/ui/xapp/xip.h): a header, FILEDATA[files], FILENAME[names]
sorted for bsearch with _stricmp, the name strings, then the data.  All
entries are plain files (type 0).
"""
import os, struct, sys

XIP_MAGIC = 0x30504958


def xtx(path):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    return b"XTX1" + struct.pack("<II", w, h) + im.tobytes("raw", "BGRA")


def collect(dirs):
    files = {}
    for d in dirs:
        for root, _, names in os.walk(d):
            for n in sorted(names):
                p = os.path.join(root, n)
                rel = os.path.relpath(p, d).replace("/", "\\")
                if n.lower().endswith(".png"):
                    files[rel[:-4] + ".xtx"] = xtx(p)
                else:
                    files[rel] = open(p, "rb").read()
    return files


def write_xip(out, files):
    order = sorted(files, key=lambda n: n.lower())
    index = {n: i for i, n in enumerate(order)}
    blob, fd = b"", []
    for n in order:
        b = files[n]
        fd.append(struct.pack("<IIII", len(blob), len(b), 0, 0))
        blob += b
    strs, offs = b"", {}
    for n in order:
        offs[n] = len(strs)
        strs += n.encode("latin-1") + b"\0"
    start = 16 + 16 * len(order) + 4 * len(order) + len(strs)
    hdr = struct.pack("<IIHHI", XIP_MAGIC, start, len(order), len(order), len(blob))
    names = b"".join(struct.pack("<HH", index[n], offs[n]) for n in order)
    open(out, "wb").write(hdr + b"".join(fd) + names + strs + blob)


if __name__ == "__main__":
    files = collect(sys.argv[2:])
    write_xip(sys.argv[1], files)
    print("%s: %d files, %d bytes" % (sys.argv[1], len(files), sum(map(len, files.values()))))
