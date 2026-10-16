#!/usr/bin/env python3
"""Pull the demos out of a DS Download Station card image.

The kiosk card keeps its multiboot client in /mb/ds_demo_client.srl and each
demo in /ds_demo/ as an "LZOn" file: the magic, a 4-byte tag, the unpacked
and packed sizes (both big-endian), then one raw LZO1X stream. Unpacked, every
demo is an ordinary .nds image that `sion-dslink send` can boot.

usage: dlstation.py CARD.nds OUTDIR      (needs python-lzo)
"""
import os, struct, sys
import lzo

def files(d):
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]
    fnt, fat = u32(0x40), u32(0x48)
    def walk(did, path):
        sub, first, _ = struct.unpack_from('<IHH', d, fnt + (did & 0xfff) * 8)
        p, fid = fnt + sub, first
        while d[p]:
            t = d[p]; n = d[p+1:p+1+(t & 0x7f)].decode('latin1'); p += 1 + (t & 0x7f)
            if t & 0x80:
                yield from walk(struct.unpack_from('<H', d, p)[0], path + n + '/'); p += 2
            else:
                s, e = struct.unpack_from('<II', d, fat + fid * 8)
                yield path + n, d[s:e]; fid += 1
    yield from walk(0xf000, '/')

def main(card, out):
    d = open(card, 'rb').read()
    os.makedirs(out, exist_ok=True)
    for path, data in files(d):
        name = path.rsplit('/', 1)[1]
        if data[:4] == b'LZOn':
            usz, csz = struct.unpack_from('>II', data, 8)
            data = lzo.decompress(data[16:16+csz], False, usz)
            if data[0xc0:0xc4] == b'\x24\xff\xae\x51':
                name += '.nds'
        elif not path.startswith(('/mb/', '/ds_demo/')):
            continue
        open(os.path.join(out, name), 'wb').write(data)
        print(f'{path:32s} -> {name:24s} {len(data):9d}  {data[0:12].rstrip(bytes(1)).decode("latin1")}')

if __name__ == '__main__':
    if len(sys.argv) != 3: sys.exit(__doc__)
    main(*sys.argv[1:])
