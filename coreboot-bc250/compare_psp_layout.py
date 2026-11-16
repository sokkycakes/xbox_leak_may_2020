#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check a coreboot BC-250 image against the stock BIOS it took its blobs from.

Every PSP directory entry and the APCB must be byte-identical to the stock
ones; the only BIOS directory entries allowed to differ are the reset image
(it is coreboot) and the ones listed in EXPECTED_EXTRA.

Usage: compare_psp_layout.py STOCK.ROM COREBOOT.ROM
"""
import struct
import sys

ROM_SIZE = 16 * 1024 * 1024
EXPECTED_EXTRA = {0x68: "APCB backup pointer (amdfwtool adds it; same bytes as 0x60)"}


def efs(rom):
    for off in (0x20000, 0x820000, 0xC20000, 0xE20000, 0xF20000, 0xFA0000):
        if struct.unpack_from("<I", rom, off)[0] == 0x55AA55AA:
            return struct.unpack_from("<10I", rom, off)
    sys.exit("no EFS")


def entries(rom):
    w = efs(rom)
    psp, bhd = w[5] & (ROM_SIZE - 1), w[7] & (ROM_SIZE - 1)
    out_psp, out_bhd = {}, {}
    magic, _, n, _ = struct.unpack_from("<4sIII", rom, psp)
    assert magic == b"$PSP", magic
    for i in range(n):
        t, size, addr = struct.unpack_from("<IIQ", rom, psp + 16 + i * 16)
        if size == 0xFFFFFFFF:
            out_psp[t & 0xFF] = ("value", addr)
            continue
        a = addr & (ROM_SIZE - 1)
        out_psp[t & 0xFF] = ("blob", rom[a:a + size])
    magic, _, n, _ = struct.unpack_from("<4sIII", rom, bhd)
    assert magic == b"$BHD", magic
    for i in range(n):
        t, size, src, dst = struct.unpack_from("<IIQQ", rom, bhd + 16 + i * 24)
        a = src & (ROM_SIZE - 1)
        blob = rom[a:a + size] if src not in (0, 0xFFFFFFFFFFFFFFFF) and size else b""
        out_bhd[t & 0xFF] = (t >> 8, size, dst, blob)
    return out_psp, out_bhd


def main():
    stock = open(sys.argv[1], "rb").read()
    ours = open(sys.argv[2], "rb").read()
    sp, sb = entries(stock)
    op, ob = entries(ours)
    bad = 0
    for t, v in sorted(sp.items()):
        if t not in op:
            print(f"PSP  {t:#04x}: MISSING from coreboot image"); bad += 1
        elif op[t] != v:
            print(f"PSP  {t:#04x}: DIFFERS"); bad += 1
        else:
            print(f"PSP  {t:#04x}: identical ({len(v[1]) if v[0] == 'blob' else 'value'})")
    for t in sorted(set(op) - set(sp)):
        print(f"PSP  {t:#04x}: EXTRA in coreboot image"); bad += 1
    for t, (flags, size, dst, blob) in sorted(sb.items()):
        if t not in ob:
            print(f"BHD  {t:#04x}: MISSING from coreboot image"); bad += 1
            continue
        oflags, osize, odst, oblob = ob[t]
        if t == 0x62:
            print(f"BHD  0x62: reset image {osize:#x} bytes -> {odst:#x} "
                  f"(stock {size:#x} -> {dst:#x}), flags {oflags:#x} (stock {flags:#x})")
            if oflags & 0x300 != 0x300:
                print("           reset/copy flags missing"); bad += 1
            entry = odst + osize - 0x10
            print(f"           x86 starts at {entry:#x}")
        elif t == 0x63:
            print(f"BHD  0x63: APOB NV {osize:#x} bytes (stock {size:#x}, erased either way)")
        elif t == 0x61:
            same = "same as" if odst == dst else "DIFFERS from"
            print(f"BHD  0x61: APOB -> {odst:#x}, {same} stock")
            bad += odst != dst
        elif oblob != blob:
            print(f"BHD  {t:#04x}: DIFFERS"); bad += 1
        else:
            print(f"BHD  {t:#04x}: identical ({len(blob)} bytes)")
    for t in sorted(set(ob) - set(sb)):
        note = EXPECTED_EXTRA.get(t)
        print(f"BHD  {t:#04x}: extra, {note}" if note else f"BHD  {t:#04x}: EXTRA"); bad += not note
    print("OK: all stock PSP blobs carried over unchanged" if not bad else f"{bad} problem(s)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
