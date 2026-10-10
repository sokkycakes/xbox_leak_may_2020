#!/usr/bin/env python3
"""Convert an Xbox PE executable (the linker output in obj/i386/*.exe) into an XBE.

This is a small re-implementation of the parts of the leak's imagebld
(private/sdktools/imagebld.new/imagebld.cpp) that matter for running the image:
the headers are laid out at XBEIMAGE_STANDARD_BASE_ADDRESS (0x10000), the PE
image is relocated to start on the page after the headers, and the xboxkrnl
import address table becomes the XBE kernel thunk table.  Nothing is signed;
the entry point and kernel thunk pointers are scrambled with the debug keys,
matching what a devkit build of imagebld produces.
"""
import argparse
import struct
import sys

XBE_BASE = 0x10000
PAGE = 0x1000
DEBUG_EP_KEY = 0x94859D4B
DEBUG_KT_KEY = 0xEFB1F152

IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_WRITE = 0x80000000

XBE_SEC_WRITEABLE = 0x1
XBE_SEC_PRELOAD = 0x2
XBE_SEC_EXECUTABLE = 0x4

XBE_HEADER_SIZE = 0x178
XBE_CERT_SIZE = 0x1D0
XBE_SECTION_SIZE = 56
XBE_LIBVER_SIZE = 16


def align(x, a):
    return (x + a - 1) & ~(a - 1)


class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not a PE image")
        nt = struct.unpack_from("<I", data, 0x3C)[0]
        if data[nt:nt + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        nsec, = struct.unpack_from("<H", data, nt + 6)
        opt_size, = struct.unpack_from("<H", data, nt + 20)
        opt = nt + 24
        self.entry_rva, = struct.unpack_from("<I", data, opt + 16)
        self.image_base, = struct.unpack_from("<I", data, opt + 28)
        self.section_alignment, = struct.unpack_from("<I", data, opt + 32)
        self.size_of_image, = struct.unpack_from("<I", data, opt + 56)
        (self.stack_reserve, self.stack_commit,
         self.heap_reserve, self.heap_commit) = struct.unpack_from("<4I", data, opt + 72)
        self.subsystem, = struct.unpack_from("<H", data, opt + 68)
        self.timestamp, = struct.unpack_from("<I", data, nt + 8)
        self.checksum, = struct.unpack_from("<I", data, opt + 64)
        ndirs, = struct.unpack_from("<I", data, opt + 92)
        self.dirs = [struct.unpack_from("<2I", data, opt + 96 + 8 * i) for i in range(ndirs)]
        self.sections = []
        sh = opt + opt_size
        for i in range(nsec):
            name = data[sh:sh + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rsize, rptr = struct.unpack_from("<4I", data, sh + 8)
            chars, = struct.unpack_from("<I", data, sh + 36)
            self.sections.append(dict(name=name, vsize=vsize, rva=va, rsize=rsize,
                                      rptr=rptr, chars=chars))
            sh += 40
        # Map the image into a flat buffer indexed by RVA.
        self.image = bytearray(self.size_of_image)
        for s in self.sections:
            n = min(s["rsize"], s["vsize"])
            self.image[s["rva"]:s["rva"] + n] = data[s["rptr"]:s["rptr"] + n]

    def dir(self, i):
        return self.dirs[i] if i < len(self.dirs) else (0, 0)

    def u32(self, rva):
        return struct.unpack_from("<I", self.image, rva)[0]

    def cstr(self, rva):
        end = self.image.index(b"\0", rva)
        return self.image[rva:end].decode("latin-1")

    def section(self, name):
        return next((s for s in self.sections if s["name"].lower() == name.lower()), None)


def relocate(pe, delta):
    rva, size = pe.dir(5)
    if size == 0:
        raise ValueError("image has no base relocations; imagebld requires them")
    end = rva + size
    while rva < end:
        page, block = struct.unpack_from("<2I", pe.image, rva)
        if block == 0:
            break
        for i in range((block - 8) // 2):
            entry, = struct.unpack_from("<H", pe.image, rva + 8 + 2 * i)
            typ, off = entry >> 12, entry & 0xFFF
            if typ == 3:  # IMAGE_REL_BASED_HIGHLOW
                a = page + off
                v, = struct.unpack_from("<I", pe.image, a)
                struct.pack_into("<I", pe.image, a, (v + delta) & 0xFFFFFFFF)
            elif typ != 0:
                raise ValueError(f"unsupported relocation type {typ}")
        rva += block


def kernel_thunk_rva(pe):
    rva, size = pe.dir(1)
    other = []
    kthunk = 0
    while size >= 20:
        oft, _, _, name, ft = struct.unpack_from("<5I", pe.image, rva)
        if oft == 0 and name == 0:
            break
        dll = pe.cstr(name)
        if dll.lower() == "xboxkrnl.exe":
            kthunk = ft
        else:
            other.append(dll)
        rva += 20
        size -= 20
    if other:
        raise ValueError(f"non-kernel imports are not supported: {other}")
    return kthunk


def build(pe, title_id, title_name, debug_path):
    # Keep every section up to the last non-discardable one, except .reloc
    # (imagebld trims discardable sections from the end the same way).
    last = max(i for i, s in enumerate(pe.sections)
               if not s["chars"] & IMAGE_SCN_MEM_DISCARDABLE)
    sections = [s for s in pe.sections[:last + 1] if s["name"] != ".reloc"]
    first_rva = sections[0]["rva"]

    # Library versions come from the .XBLD section the XDK libraries emit.
    libvers = []
    xbld = pe.section(".XBLD")
    if xbld:
        raw = pe.image[xbld["rva"]:xbld["rva"] + xbld["vsize"]]
        for i in range(0, len(raw) - XBE_LIBVER_SIZE + 1, XBE_LIBVER_SIZE):
            entry = bytes(raw[i:i + XBE_LIBVER_SIZE])
            if struct.unpack_from("<I", entry)[0] != 0:
                libvers.append(entry)

    # The .tls section is discardable, so like imagebld we copy any non-zero
    # __declspec(thread) initializers into the headers and zero-fill the rest.
    tls_rva, tls_size = pe.dir(9)
    tls_raw = b""
    if tls_size:
        start, end = struct.unpack_from("<2I", pe.image, tls_rva)
        raw = bytes(pe.image[start - pe.image_base:end - pe.image_base])
        tls_raw = raw.rstrip(b"\0")
        struct.pack_into("<I", pe.image, tls_rva + 16, len(raw) - len(tls_raw))

    names = b"".join(s["name"].encode() + b"\0" for s in sections)
    uname = title_name.encode("utf-16-le")
    dbg_file = debug_path.replace("/", "\\").split("\\")[-1]
    dbg = dbg_file.encode("utf-16-le") + b"\0\0" + debug_path.encode() + b"\0"

    # Lay out the headers in imagebld's order, DWORD aligned.
    layout = {}
    off = XBE_HEADER_SIZE
    for key, size in (("cert", XBE_CERT_SIZE),
                      ("sections", XBE_SECTION_SIZE * len(sections)),
                      ("names", len(names)),
                      ("refcounts", 2 * (2 * len(sections) + 1)),
                      ("libvers", XBE_LIBVER_SIZE * len(libvers)),
                      ("debug", len(dbg)),
                      ("tls", len(tls_raw))):
        off = align(off, 4)
        layout[key] = off
        off += size
    size_of_headers = align(off, 4)
    new_base = XBE_BASE + align(size_of_headers, PAGE) - first_rva
    relocate(pe, new_base - pe.image_base)

    last_s = sections[-1]
    exec_size = align(last_s["rva"] + last_s["vsize"] - first_rva, pe.section_alignment)
    size_of_image = align(size_of_headers, PAGE) + exec_size

    hdr = bytearray(size_of_headers)
    va = lambda o: XBE_BASE + o

    # Certificate.
    c = layout["cert"]
    struct.pack_into("<3I", hdr, c, XBE_CERT_SIZE, pe.timestamp, title_id)
    hdr[c + 12:c + 12 + min(len(uname), 78)] = uname[:78]
    struct.pack_into("<5I", hdr, c + 0x9C, 0x800000FF, 0x80000007, 0xFFFFFFFF, 0, 0)

    # Section headers, shared page reference counters and names.
    name_off = 0
    ref_off = layout["refcounts"]
    for i, s in enumerate(sections):
        flags = XBE_SEC_PRELOAD
        if s["chars"] & IMAGE_SCN_MEM_WRITE:
            flags |= XBE_SEC_WRITEABLE
        if s["chars"] & IMAGE_SCN_MEM_EXECUTE:
            flags |= XBE_SEC_EXECUTABLE
        struct.pack_into("<9I", hdr, layout["sections"] + i * XBE_SECTION_SIZE,
                         flags, new_base + s["rva"], s["vsize"],
                         0, min(s["rsize"], s["vsize"]),  # raw pointer patched below
                         va(layout["names"] + name_off), 0,
                         va(ref_off + 4 * i), va(ref_off + 4 * i + 2))
        name_off += len(s["name"]) + 1
    hdr[layout["names"]:layout["names"] + len(names)] = names

    for i, lv in enumerate(libvers):
        o = layout["libvers"] + i * XBE_LIBVER_SIZE
        hdr[o:o + XBE_LIBVER_SIZE] = lv
    hdr[layout["debug"]:layout["debug"] + len(dbg)] = dbg

    def libver_va(prefix):
        for i, lv in enumerate(libvers):
            if lv[:8].rstrip(b"\0").startswith(prefix):
                return va(layout["libvers"] + i * XBE_LIBVER_SIZE)
        return 0

    hdr[layout["tls"]:layout["tls"] + len(tls_raw)] = tls_raw
    if tls_size:
        struct.pack_into("<2I", pe.image, tls_rva,
                         va(layout["tls"]), va(layout["tls"] + len(tls_raw)))

    kthunk = kernel_thunk_rva(pe)
    struct.pack_into("<I", hdr, 0, 0x48454258)  # "XBEH"
    struct.pack_into("<10I", hdr, 0x104,
                     XBE_BASE, size_of_headers, size_of_image, XBE_HEADER_SIZE,
                     pe.timestamp, va(layout["cert"]), len(sections),
                     va(layout["sections"]), 0,  # init flags
                     (new_base + pe.entry_rva) ^ DEBUG_EP_KEY)
    struct.pack_into("<9I", hdr, 0x12C,
                     new_base + tls_rva if tls_size else 0,
                     pe.stack_commit, pe.heap_reserve, pe.heap_commit,
                     new_base, pe.size_of_image, pe.checksum, pe.timestamp,
                     va(layout["debug"] + len(dbg_file) * 2 + 2))
    struct.pack_into("<9I", hdr, 0x150,
                     va(layout["debug"] + len(dbg_file) * 2 + 2 +
                        len(debug_path) - len(dbg_file)),
                     va(layout["debug"]),
                     (new_base + kthunk) ^ DEBUG_KT_KEY if kthunk else 0,
                     0, len(libvers), va(layout["libvers"]) if libvers else 0,
                     libver_va(b"XBOXKRNL"), libver_va(b"XAPILIB"), 0)

    # File layout: headers, then each section's raw data on a page boundary.
    out = bytearray(align(size_of_headers, PAGE))
    for i, s in enumerate(sections):
        raw = pe.image[s["rva"]:s["rva"] + min(s["rsize"], s["vsize"])]
        struct.pack_into("<I", hdr, layout["sections"] + i * XBE_SECTION_SIZE + 12, len(out))
        out += raw
        out += bytes(align(len(out), PAGE) - len(out))
    out[:size_of_headers] = hdr
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--title-id", type=lambda x: int(x, 0), default=0xFFFF0000)
    ap.add_argument("--title-name", default=None)
    args = ap.parse_args()
    pe = PE(open(args.input, "rb").read())
    if pe.subsystem != 14:
        print("warning: PE subsystem is not XBOX (14)", file=sys.stderr)
    name = args.title_name or args.input.replace("\\", "/").split("/")[-1].rsplit(".", 1)[0]
    xbe = build(pe, args.title_id, name, "D:\\" + name + ".exe")
    open(args.output, "wb").write(xbe)


if __name__ == "__main__":
    main()
