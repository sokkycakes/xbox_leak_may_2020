#!/usr/bin/env python3
"""Generate byte signatures for every function in MS COFF static libraries.

Usage:
    mksigs.py LIB [LIB...] -o OUT.json [--min-size N]

Xbox titles link Microsoft's libraries (d3d8, dsound, xapilib, ...)
statically, so the only way to find e.g. D3DDevice_SetRenderState_Simple in
a game image is by pattern matching.  Since we have the exact prebuilt .lib
files, the pattern is just the function's object-file bytes with every byte
patched by a relocation turned into a wildcard.

Output JSON:
    {"lib": "d3d8.lib" (or "a.lib,b.lib"),
     "functions": [
        {"name":   decorated symbol name,
         "object": archive member (object file) it came from,
         "lib":    library file,
         "size":   number of bytes,
         "bytes":  hex string (wildcarded bytes are written as 00),
         "mask":   string, one char per byte: 'x' = must match, '?' = wildcard,
         "relocs": [{"off": offset inside function, "type": reloc type name,
                     "sym": target symbol name, "addend": value stored in the
                     object at the patch site (displacement from sym),
                     "func": target is a function (COFF type 0x20),
                     "local": target is a static / section symbol}]}]}

Only standard library modules are used.
"""
import argparse
import bisect
import json
import os
import struct
import sys

# --- COFF constants --------------------------------------------------------

IMAGE_FILE_MACHINE_I386 = 0x14C
IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_LNK_COMDAT = 0x00001000

IMAGE_SYM_CLASS_EXTERNAL = 2
IMAGE_SYM_CLASS_STATIC = 3
IMAGE_SYM_DTYPE_FUNCTION = 0x20

# i386 relocation types -> (name, number of patched bytes)
RELOC_I386 = {
    0x00: ("ABSOLUTE", 0),
    0x01: ("DIR16", 2),
    0x02: ("REL16", 2),
    0x06: ("DIR32", 4),
    0x07: ("DIR32NB", 4),
    0x09: ("SEG12", 2),
    0x0A: ("SECTION", 2),
    0x0B: ("SECREL", 4),
    0x0C: ("TOKEN", 4),
    0x0D: ("SECREL7", 1),
    0x14: ("REL32", 4),
}


# --- archive reading ---------------------------------------------------------

def iter_archive(data):
    """Yield (member_name, member_bytes) for each regular member of an
    `!<arch>` file, skipping the linker members and the longnames table."""
    if data[:8] != b"!<arch>\n":
        raise ValueError("not a COFF archive")
    pos = 8
    longnames = b""
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        raw_name = hdr[0:16].decode("latin-1").rstrip()
        size = int(hdr[48:58].decode("ascii").strip())
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)          # members are 2-byte aligned
        if raw_name == "/":                    # 1st / 2nd linker member
            continue
        if raw_name == "//":                   # longnames member
            longnames = body
            continue
        if raw_name.startswith("/") and raw_name[1:].isdigit():
            off = int(raw_name[1:])
            end = longnames.find(b"\0", off)
            name = longnames[off:end if end >= 0 else None].decode("latin-1")
        else:
            name = raw_name.rstrip("/")
        yield name, body


# --- COFF object parsing -----------------------------------------------------

def cstr(buf, off):
    end = buf.find(b"\0", off)
    return buf[off:end if end >= 0 else None].decode("latin-1")


class CoffObject:
    """Minimal i386 COFF object parser: sections, relocations, symbols."""

    def __init__(self, data):
        self.data = data
        (self.machine, nsec, _ts, symptr, nsym, optsz,
         _chars) = struct.unpack_from("<HHIIIHH", data, 0)
        if self.machine != IMAGE_FILE_MACHINE_I386:
            raise ValueError("machine %#x" % self.machine)
        strtab_off = symptr + nsym * 18
        self.strtab = data[strtab_off:] if symptr else b""

        # Symbol table; index i -> dict (aux records occupy indices too).
        self.symbols = {}
        i = 0
        while i < nsym:
            o = symptr + i * 18
            raw = data[o:o + 8]
            value, secnum, typ, sclass, naux = struct.unpack_from(
                "<IhHBB", data, o + 8)
            self.symbols[i] = dict(name=self._symname(raw), value=value,
                                   sec=secnum, type=typ, sclass=sclass,
                                   naux=naux)
            i += 1 + naux

        self.sections = []
        for s in range(nsec):
            o = 20 + optsz + s * 40
            (name, vsize, va, rawsz, rawptr, relptr, _lnptr, nrel, _nln,
             chars) = struct.unpack_from("<8sIIIIIIHHI", data, o)
            name = name.rstrip(b"\0").decode("latin-1")
            if name.startswith("/") and name[1:].isdigit():   # long name
                name = cstr(self.strtab, int(name[1:]))
            relocs = []
            for r in range(nrel):
                roff, symidx, rtype = struct.unpack_from(
                    "<IIH", data, relptr + r * 10)
                relocs.append((roff, symidx, rtype))
            self.sections.append(dict(
                name=name, chars=chars, size=rawsz,
                raw=data[rawptr:rawptr + rawsz] if rawptr else b"",
                relocs=relocs))

    def _symname(self, raw8):
        if raw8[:4] == b"\0\0\0\0":            # long name: string table offset
            return cstr(self.strtab, struct.unpack_from("<I", raw8, 4)[0])
        return raw8.rstrip(b"\0").decode("latin-1")


def is_function_symbol(sym):
    """A symbol that marks the start of a function inside a code section."""
    if sym["sec"] <= 0:
        return False
    if sym["sclass"] == IMAGE_SYM_CLASS_EXTERNAL:
        # C/C++ functions have type 0x20; hand-written asm publics have 0.
        return True
    if sym["sclass"] == IMAGE_SYM_CLASS_STATIC:
        # Excludes the section symbols (type 0, with an aux record).
        return sym["type"] == IMAGE_SYM_DTYPE_FUNCTION
    return False


def extract_functions(obj, objname, libname):
    """Return the signature records for every function in `obj`."""
    # Group function symbols by (1-based) section number.
    by_sec = {}
    for sym in obj.symbols.values():
        if is_function_symbol(sym):
            by_sec.setdefault(sym["sec"], []).append(sym)

    out = []
    for secnum, syms in by_sec.items():
        sec = obj.sections[secnum - 1]
        if not sec["chars"] & IMAGE_SCN_CNT_CODE or not sec["raw"]:
            continue
        raw = sec["raw"]
        # Section-wide wildcard map and reloc list.
        wild = bytearray(len(raw))
        rels = []
        for roff, symidx, rtype in sec["relocs"]:
            tname, width = RELOC_I386.get(rtype, ("TYPE_%#x" % rtype, 4))
            if width == 0:
                continue
            for k in range(roff, min(roff + width, len(raw))):
                wild[k] = 1
            tsym = obj.symbols.get(symidx, {})
            addend = int.from_bytes(raw[roff:roff + width], "little",
                                    signed=(tname in ("REL32", "REL16")))
            rels.append(dict(
                off=roff, type=tname, sym=tsym.get("name", "?%d" % symidx),
                addend=addend,
                func=tsym.get("type") == IMAGE_SYM_DTYPE_FUNCTION,
                local=tsym.get("sclass") != IMAGE_SYM_CLASS_EXTERNAL))

        # Sort by offset; aliases at the same offset share one span.
        starts = sorted({s["value"] for s in syms})
        for sym in sorted(syms, key=lambda s: s["value"]):
            begin = sym["value"]
            i = bisect.bisect_right(starts, begin)
            end = starts[i] if i < len(starts) else len(raw)
            if end <= begin:
                continue
            body = bytearray(raw[begin:end])
            mask = []
            for k in range(end - begin):
                if wild[begin + k]:
                    body[k] = 0
                    mask.append("?")
                else:
                    mask.append("x")
            out.append(dict(
                name=sym["name"], object=objname, lib=libname,
                size=end - begin, bytes=body.hex(), mask="".join(mask),
                local=sym["sclass"] != IMAGE_SYM_CLASS_EXTERNAL,
                relocs=[dict(r, off=r["off"] - begin) for r in rels
                        if begin <= r["off"] < end]))
    return out


def process_lib(path, stats):
    libname = os.path.basename(path)
    data = open(path, "rb").read()
    funcs = []
    for member, body in iter_archive(data):
        if len(body) < 20:
            continue
        sig1, sig2 = struct.unpack_from("<HH", body, 0)
        if sig1 == 0 and sig2 == 0xFFFF:       # short import / anon object
            stats["skipped"] += 1
            continue
        try:
            obj = CoffObject(body)
        except (ValueError, struct.error) as exc:
            stats["skipped"] += 1
            print("warning: %s(%s): %s" % (libname, member, exc),
                  file=sys.stderr)
            continue
        stats["objects"] += 1
        funcs.extend(extract_functions(obj, member, libname))
    return funcs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("libs", nargs="+", metavar="LIB")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--min-size", type=int, default=1,
                    help="drop functions smaller than N bytes (default 1)")
    args = ap.parse_args()

    stats = dict(objects=0, skipped=0)
    funcs, seen = [], set()
    for lib in args.libs:
        for f in process_lib(lib, stats):
            if f["size"] < args.min_size:
                continue
            # The same COMDAT (inline helpers, templates) appears in many
            # objects; keep one copy of each distinct body per name.
            key = (f["name"], f["bytes"], f["mask"])
            if key in seen:
                continue
            seen.add(key)
            funcs.append(f)

    out = dict(lib=",".join(os.path.basename(l) for l in args.libs),
               functions=funcs)
    with open(args.output, "w") as fp:
        json.dump(out, fp, indent=0)
    print("%d objects (%d skipped), %d function signatures -> %s" % (
        stats["objects"], stats["skipped"], len(funcs), args.output),
        file=sys.stderr)


if __name__ == "__main__":
    main()
