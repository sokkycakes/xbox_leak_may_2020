#!/usr/bin/env python3
"""Replace or add plain files (XAP scripts) in a dashboard XIP archive.

    xipedit.py in.xip out.xip name=hostfile [name=hostfile ...]

Layout (private/ui/xapp/xip.h): a header, FILEDATA[files], FILENAME[names]
sorted for bsearch with _stricmp, the name strings, then the data.  The
dashboard reads plain files and textures back to back in FILEDATA order, so
the data is rewritten contiguously; mesh references (type 4) have none.
"""
import struct, sys

src, dst, edits = sys.argv[1], sys.argv[2], dict(a.split("=", 1) for a in sys.argv[3:])
d = open(src, "rb").read()
magic, start, nfiles, nnames, size = struct.unpack_from("<IIHHI", d, 0)
assert magic == 0x30504958, "not a XIP"
files = [list(struct.unpack_from("<IIII", d, 16 + 16 * i)) for i in range(nfiles)]
nb = 16 + 16 * nfiles + 4 * nnames
names = {}
for i in range(nnames):
    idx, off = struct.unpack_from("<HH", d, 16 + 16 * nfiles + 4 * i)
    names[d[nb + off:d.index(b"\0", nb + off)].decode("latin-1")] = idx

data = [None if t == 4 else d[start + o:start + o + s] for o, s, t, ts in files]
for name, host in edits.items():
    body = open(host, "rb").read()
    hit = [n for n in names if n.lower() == name.lower()]
    if hit:
        data[names[hit[0]]] = body
    else:
        files.append([0, 0, 0, 0])
        data.append(body)
        names[name] = len(files) - 1

blob = b""
for f, b in zip(files, data):
    if b is None:
        continue
    f[0], f[1] = len(blob), len(b)
    blob += b

order = sorted(names, key=lambda n: n.lower())
strs, offs = b"", {}
for n in order:
    offs[n] = len(strs)
    strs += n.encode("latin-1") + b"\0"
start = 16 + 16 * len(files) + 4 * len(order) + len(strs)
out = struct.pack("<IIHHI", magic, start, len(files), len(order), len(blob))
out += b"".join(struct.pack("<IIII", *f) for f in files)
out += b"".join(struct.pack("<HH", names[n], offs[n]) for n in order)
open(dst, "wb").write(out + strs + blob)
