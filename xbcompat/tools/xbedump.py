#!/usr/bin/env python3
"""Dump the header, sections, kernel imports and library versions of an XBE."""
import struct, sys

EP_KEYS = {"retail": 0xA8FC57AB, "debug": 0x94859D4B, "chihiro": 0x40B5C16E, "none": 0}
KT_KEYS = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152, "chihiro": 0x2290059D, "none": 0}

def main(path):
    d = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    assert d[:4] == b"XBEH", "not an XBE"
    base = u32(0x104)
    size_img = u32(0x10C)
    va2off = lambda va: va - base  # headers are mapped at base in file order
    nsec, sechdr = u32(0x11C), u32(0x120)
    secs = []
    for i in range(nsec):
        o = va2off(sechdr) + i * 56
        fl, va, vs, raw, rs, nm = struct.unpack_from("<6I", d, o)
        name = d[va2off(nm):d.index(b"\0", va2off(nm))].decode()
        secs.append((name, fl, va, vs, raw, rs))
    def inimg(a): return base <= a < base + size_img
    ep_raw, kt_raw = u32(0x128), u32(0x158)
    kind = next(k for k in EP_KEYS if inimg(ep_raw ^ EP_KEYS[k]))
    ep, kt = ep_raw ^ EP_KEYS[kind], kt_raw ^ KT_KEYS[kind]
    print(f"base {base:#x} image size {size_img:#x} kind {kind} entry {ep:#x} kthunk {kt:#x}")
    print(f"init flags {u32(0x124):#x} stack {u32(0x130):#x} TLS dir {u32(0x12C):#x}")
    def file_off(va):
        for n, fl, sva, vs, raw, rs in secs:
            if sva <= va < sva + rs: return raw + va - sva
        return va2off(va)
    for n, fl, va, vs, raw, rs in secs:
        print(f"  section {n:10s} flags {fl:#04x} va {va:#010x} vsize {vs:#08x} raw {raw:#08x} rsize {rs:#08x}")
    nlib, libs = u32(0x160), u32(0x164)
    for i in range(nlib):
        o = va2off(libs) + i * 16
        nm = d[o:o+8].rstrip(b"\0").decode()
        mj, mn, bd, q = struct.unpack_from("<4H", d, o + 8)
        print(f"  lib {nm:8s} {mj}.{mn}.{bd}.{q & 0x1fff}")
    o = file_off(kt)
    ords = []
    while True:
        t = u32(o); o += 4
        if t == 0: break
        ords.append(t & 0x7fffffff)
    print(f"kernel imports ({len(ords)}): {' '.join(map(str, ords))}")

if __name__ == "__main__":
    main(sys.argv[1])
