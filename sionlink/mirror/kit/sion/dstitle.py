#!/usr/bin/env python3
# dstitle.py: a DS cart (a Kazeta .kzi with Runtime=nds) as an Xbox title on the Sion's
# hard disk, so both dashboards list it and launch it like any game.
#
#   dstitle.py CART.kzi --games E:/Games [--udata E:/UDATA]
#
# --games: E:\Games\<Id>\ gets a stub default.xbe and a .wincart naming the cart. The
#   stub is the one xbcompat's wincart.c makes for a Windows cart on the game card: a
#   certificate with the cart's Name and a title ID made from its Id ('SN' + CRC-16), and
#   the game's icon (from the ROM's banner) as its title image. Nothing in it runs: the
#   xbcompat started for it finds the .wincart and execs wincart-run, which hands a DS
#   cart to dscart-run.
# --udata: UDATA/<title id>/ gets TitleMeta.xbx and TitleImage.xbx, and the save folder
#   000000000001/ its SaveMeta.xbx, where they're missing (dscart-run, on each launch).
# Prints the title ID. Python 3 with nothing but its standard library: on the Sion it runs
# under xbcompat's own Python (/opt/xbcompat/bin/python3).
import os
import struct
import sys

SAVE_SLOT = "000000000001"


def kzi_read(path):
    keys = {}
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace").lstrip("\ufeff")
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in "#;" or "=" not in line:
            continue
        k, v = line.split("=", 1)
        keys[k.strip().lower()] = v.strip()
    return keys


def title_id(cart_id):
    # wincart_title_id: 'SN' (Sion) + CRC-16/CCITT of the Id, case-insensitive
    crc = 0xFFFF
    for b in cart_id.encode("utf-8"):
        crc ^= (b | 0x20) << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
        crc &= 0xFFFF
    return 0x534E0000 | crc


def banner_icon(rom):
    """The ROM banner's 32x32 icon as rows of (r, g, b, a), or None."""
    with open(rom, "rb") as f:
        f.seek(0x68)
        (off,) = struct.unpack("<I", f.read(4))
        if not off:
            return None
        f.seek(off)
        bn = f.read(0x240)
    if len(bn) < 0x240:
        return None
    pal = []
    for i in range(16):
        (c,) = struct.unpack_from("<H", bn, 0x220 + i * 2)
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        pal.append((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 0 if i == 0 else 255))
    px = [[None] * 32 for _ in range(32)]
    # 4x4 tiles of 8x8, 4 bits a pixel, low nibble first
    for t in range(16):
        tx, ty = t % 4, t // 4
        for y in range(8):
            for x in range(8):
                byte = bn[0x20 + t * 32 + y * 4 + x // 2]
                px[ty * 8 + y][tx * 8 + x] = pal[(byte >> 4) if x & 1 else (byte & 15)]
    return px


TI_SIDE, TI_HDR = 128, 0x800


def title_image(icon):
    """An XPR0 texture, 128x128 swizzled A8R8G8B8, as the dashboards read an XBE's
    $$XTIMAGE (and UDATA's TitleImage.xbx): the icon scaled up 4 times."""
    size = TI_HDR + TI_SIDE * TI_SIDE * 4
    x = bytearray(size)
    struct.pack_into("<9I", x, 0, 0x30525058, size, TI_HDR, 0x00040001, 0, 0, 0x07710629, 0, 0xFFFFFFFF)
    x[36:TI_HDR] = b"\xAD" * (TI_HDR - 36)
    for y in range(TI_SIDE):
        for xx in range(TI_SIDE):
            r, g, b, a = icon[y * 32 // TI_SIDE][xx * 32 // TI_SIDE]
            at = 0
            for bit in range(7):
                at |= ((xx >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
            o = TI_HDR + at * 4
            if a:
                x[o:o + 3] = bytes((b, g, r))
            x[o + 3] = a
    return bytes(x)


XBE_BASE = 0x10000
HEADER = 0x178        # XBE_HEADER
CERT = 0xB0           # XBE_CERTIFICATE
SECTION = 0x38        # XBE_SECTION


def stub_xbe(name, tid, stamp, ti):
    """wincart.c's write_stub_xbe: headers, a certificate, one empty section and, given a
    title image, its $$XTIMAGE section."""
    HDR, TEXT_VA = 0x1000, XBE_BASE + 0x1000
    tisize = len(ti) if ti else 0
    tiraw = (tisize + 0xFFF) & ~0xFFF
    size = HDR + 0x1000 + tiraw
    img = bytearray(size)
    cert = XBE_BASE + HEADER
    secs = cert + CERT
    names = secs + 2 * SECTION   # ".text", "$$XTIMAGE", then an empty string
    struct.pack_into("<I", img, 0, 0x48454258)   # XBEH
    struct.pack_into("<10I", img, 0x104,
                     XBE_BASE, HDR, size, HEADER, stamp, cert, 2 if ti else 1, secs,
                     0, (TEXT_VA + 0x10) ^ 0xA8FC57AB)   # init flags; entry point, retail key
    struct.pack_into("<5I", img, 0x130, 0x10000, 0x100000, 0x1000, 0x10000, size)   # stack, heap, NT base and size
    struct.pack_into("<3I", img, 0x14C, names + 32, names + 32, names + 32)   # debug path names
    struct.pack_into("<I", img, 0x158, TEXT_VA ^ 0x5B6D40B6)   # kernel thunks, retail key
    struct.pack_into("<2I", img, 0x160, 0, names + 32)   # no library versions
    c = cert - XBE_BASE
    struct.pack_into("<3I", img, c, CERT, stamp, tid)
    units = name.encode("utf-16-le")[:39 * 2]
    img[c + 12:c + 12 + len(units)] = units
    struct.pack_into("<4I", img, c + 12 + 80 + 64, 0xFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)   # media, region, ratings, disk
    struct.pack_into("<I", img, c + 12 + 80 + 64 + 16, 1)   # version
    s = secs - XBE_BASE
    struct.pack_into("<6I", img, s, 0x2 | 0x1 | 0x4, TEXT_VA, 0x1000, HDR, 0x1000, names)   # preload, writable, executable
    img[names - XBE_BASE:names - XBE_BASE + 6] = b".text\0"
    img[HDR + 0x10] = 0xC3   # ret, never reached
    if ti:
        struct.pack_into("<6I", img, s + SECTION, 0, TEXT_VA + 0x1000, tisize, HDR + 0x1000, tisize, names + 16)
        img[names + 16 - XBE_BASE:names + 16 - XBE_BASE + 10] = b"$$XTIMAGE\0"
        img[HDR + 0x1000:HDR + 0x1000 + tisize] = ti
    return bytes(img)


def write(path, data, keep=False):
    """data to path (through a temp file); keep: only if path isn't there yet"""
    if keep and os.path.exists(path):
        return
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.rename(tmp, path)


def xbx(text):
    # UTF-16LE with a BOM and a CRLF, as the dashboard's .xbx files are
    return ("\ufeff" + text + "\r\n").encode("utf-16-le")


def main():
    args = sys.argv[1:]
    if not args or args[0].startswith("-"):
        sys.exit("usage: dstitle.py CART.kzi [--games DIR] [--udata DIR]")
    kzi = os.path.abspath(args[0])
    games = udata = None
    i = 1
    while i < len(args):
        if args[i] == "--games" and i + 1 < len(args):
            games = args[i + 1]
        elif args[i] == "--udata" and i + 1 < len(args):
            udata = args[i + 1]
        else:
            sys.exit("dstitle.py: unknown option " + args[i])
        i += 2
    k = kzi_read(kzi)
    base = os.path.splitext(os.path.basename(kzi))[0]
    cart_id = k.get("id") or base
    name = k.get("name") or cart_id
    if not k.get("runtime", "").lower().startswith("nds"):
        sys.exit("dstitle.py: %s isn't a DS cart (Runtime=%s)" % (kzi, k.get("runtime", "")))
    # Kazeta: the cart's files are in the folder named like its .kzi
    cartdir = os.path.join(os.path.dirname(kzi), base)
    if not os.path.isdir(cartdir):
        cartdir = os.path.dirname(kzi)
    rom = os.path.join(cartdir, k.get("exec", ""))
    tid = title_id(cart_id)
    icon = banner_icon(rom) if os.path.isfile(rom) else None
    ti = title_image(icon) if icon else None
    stamp = int(max(os.path.getmtime(kzi), os.path.getmtime(rom) if os.path.isfile(rom) else 0))

    if games:
        # the folder is the Id (the dashboards show the certificate's name); no spaces on
        # the way to the launcher and xbrun's map
        d = os.path.join(games, cart_id[:42])
        os.makedirs(d, exist_ok=True)
        write(os.path.join(d, "default.xbe"), stub_xbe(name, tid, stamp, ti))
        write(os.path.join(d, ".wincart"), ("kzi=%s\ntitle=%08x\nname=%s\nid=%s\nruntime=%s\nexec=%s\n" % (
            kzi, tid, name, cart_id, k.get("runtime", ""), k.get("exec", ""))).encode("utf-8"))
    if udata:
        t = os.path.join(udata, "%08x" % tid)   # lower case, as wincart-run makes them
        os.makedirs(os.path.join(t, SAVE_SLOT), exist_ok=True)
        write(os.path.join(t, "TitleMeta.xbx"), xbx("TitleName=" + name), keep=True)
        if ti:
            write(os.path.join(t, "TitleImage.xbx"), ti, keep=True)
        write(os.path.join(t, SAVE_SLOT, "SaveMeta.xbx"), xbx("Name=" + name), keep=True)
    print("%08X" % tid)


if __name__ == "__main__":
    main()
