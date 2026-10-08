#!/usr/bin/env python3
"""Locate statically linked library functions in an Xbox PE or XBE image.

Usage:
    findsigs.py SIGS.json IMAGE [--names REGEX] [--json] [--min-fixed N]

SIGS.json comes from mksigs.py.  Every signature is searched in the
executable sections of IMAGE (a PE .exe or an .xbe).  A signature matches
when every non-wildcard byte is equal; to keep this fast each signature is
first located by its longest run of fixed bytes (bytes.find), then verified.
Signatures with fewer than --min-fixed (default 8) fixed bytes (not counting
trailing nop/int3 padding) are skipped.

For every uniquely matched function, the relocations recorded in the
signature are replayed backwards to recover the addresses of the symbols the
function references:
    DIR32:  S = [site] - addend
    REL32:  S = [site] + site + 4 - addend   (following one `jmp rel32` if the
            target is an incremental-link thunk)
Values from different functions referring to the same symbol are
cross-checked (majority wins; disagreements are reported as conflicts).
The recovered symbols are then used to settle ambiguous signatures: a
candidate address that is itself a known call target wins ("reference"),
otherwise the only candidate whose own references agree with the known
symbols wins ("relocations").  This repeats until nothing changes.  Code
symbols reached through references but never matched by signature are
listed separately.
"""
import argparse
import json
import re
import struct
import sys


# --- image loading -----------------------------------------------------------

class Section:
    def __init__(self, name, va, data, executable):
        self.name, self.va, self.data, self.executable = (
            name, va, data, executable)


class Image:
    """A loaded PE or XBE: a list of Sections addressed by virtual address."""

    def __init__(self, path):
        self.path = path
        raw = open(path, "rb").read()
        if raw[:4] == b"XBEH":
            self.kind = "xbe"
            self._load_xbe(raw)
        elif raw[:2] == b"MZ":
            self.kind = "pe"
            self._load_pe(raw)
        else:
            raise ValueError("%s: neither PE nor XBE" % path)

    def _load_pe(self, d):
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("bad PE signature")
        nsec, optsz = struct.unpack_from("<H", d, pe + 6)[0], \
            struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<I", d, opt + 28)[0]   # ImageBase
        self.sections = []
        for i in range(nsec):
            o = opt + optsz + i * 40
            (name, vsize, va, rawsz, rawptr) = struct.unpack_from(
                "<8sIIII", d, o)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            size = min(rawsz, vsize) if vsize else rawsz
            self.sections.append(Section(
                name.rstrip(b"\0").decode("latin-1"), self.base + va,
                d[rawptr:rawptr + size],
                bool(chars & (0x20 | 0x20000000))))    # CNT_CODE|MEM_EXECUTE

    def _load_xbe(self, d):
        # Offsets from XBEIMAGE_HEADER (private/inc/xbeimage.h); the headers
        # are mapped at BaseAddress in file order.
        u32 = lambda o: struct.unpack_from("<I", d, o)[0]
        self.base = u32(0x104)
        nsec, sechdr = u32(0x11C), u32(0x120)
        self.sections = []
        for i in range(nsec):
            o = sechdr - self.base + i * 56
            flags, va, _vsize, rawptr, rawsz, nameptr = struct.unpack_from(
                "<6I", d, o)
            n = nameptr - self.base
            name = d[n:d.index(b"\0", n)].decode("latin-1")
            self.sections.append(Section(
                name, va, d[rawptr:rawptr + rawsz],
                bool(flags & 0x4)))                  # SECTION_EXECUTABLE

    def read(self, va, n):
        """Return n bytes at va, or None if not backed by file data."""
        for s in self.sections:
            if s.va <= va and va + n <= s.va + len(s.data):
                return s.data[va - s.va:va - s.va + n]
        return None

    def u32(self, va):
        b = self.read(va, 4)
        return None if b is None else struct.unpack("<I", b)[0]


# --- signatures --------------------------------------------------------------

class Signature:
    def __init__(self, rec):
        self.name = rec["name"]
        self.rec = rec
        body = bytes.fromhex(rec["bytes"])
        mask = rec["mask"]
        self.size = len(body)
        # Runs of fixed bytes: list of (offset, bytes).
        self.runs = [(m.start(), body[m.start():m.end()])
                     for m in re.finditer(r"x+", mask)]
        # Fixed bytes that carry information: alignment padding (nop/int3)
        # at the end of the body is excluded, otherwise every `jmp rel32`
        # stub followed by 11 nops would count as a 12-byte signature.
        tail = len(body)
        while tail and mask[tail - 1] == "x" and body[tail - 1] in (0x90,
                                                                     0xCC):
            tail -= 1
        self.fixed = mask.count("x", 0, tail)
        self.anchor_off, self.anchor = max(
            self.runs, key=lambda r: len(r[1]), default=(0, b""))

    def search(self, section):
        """Yield every VA in `section` where this signature matches."""
        data, anchor = section.data, self.anchor
        pos = data.find(anchor)
        while pos >= 0:
            start = pos - self.anchor_off
            if start >= 0 and start + self.size <= len(data) and all(
                    data[start + o:start + o + len(b)] == b
                    for o, b in self.runs):
                yield section.va + start
            pos = data.find(anchor, pos + 1)


# --- reference resolution ----------------------------------------------------

def reloc_targets(img, sig, va):
    """Yield (sym, addr, is_code) for each external symbol referenced by the
    function `sig` matched at `va`, recovered from the linked image."""
    for r in sig.rec["relocs"]:
        if r["local"]:
            continue          # section-relative / static: not a global name
        site = va + r["off"]
        val = img.u32(site)
        if val is None:
            continue
        if r["type"] == "DIR32":
            addr = (val - r["addend"]) & 0xFFFFFFFF
        elif r["type"] == "REL32":
            if val & 0x80000000:
                val -= 1 << 32
            addr = (site + 4 + val - r["addend"]) & 0xFFFFFFFF
            # Debug builds call through incremental-linking thunks: a table
            # of back-to-back `jmp rel32`.  A lone jmp is a real function.
            op = img.read(addr - 5, 15)
            if op and op[5] == 0xE9 and (op[0] == 0xE9 or op[10] == 0xE9):
                addr = (addr + 5 + struct.unpack_from("<i", op, 6)[0]) \
                    & 0xFFFFFFFF
        else:
            continue
        yield r["sym"], addr, bool(r["func"] or r["type"] == "REL32")


def collect_refs(img, placed):
    """placed: {name: (va, Signature)}.  Returns (known, is_code, conflicts):
    known = {sym: addr} by majority vote over all referrers, and conflicts =
    {sym: {addr: [referrers]}} for symbols whose referrers disagree."""
    votes, is_code = {}, {}
    for name, (va, sig) in placed.items():
        for sym, addr, code in reloc_targets(img, sig, va):
            votes.setdefault(sym, {}).setdefault(addr, []).append(name)
            is_code[sym] = code
    known, conflicts = {}, {}
    for sym, addrs in votes.items():
        best = max(addrs, key=lambda a: len(addrs[a]))
        known[sym] = best
        if len(addrs) > 1:
            conflicts[sym] = {"%#x" % a: w for a, w in addrs.items()}
    return known, is_code, conflicts


def reject_contradicted(img, placed, matches):
    """Several names placed at one address are either bodies folded by
    /OPT:ICF or same-shaped stubs (a thunk per method that differs only in
    the function it calls) of which the linker kept one.  Drop each name
    whose code references contradict the placement of the function they
    point at: it calls an address another function was placed at (and its
    callee's own signature does not match there), or the function it names
    lives elsewhere.  Removes the names from `placed`
    and returns {name: va}."""
    rejected = {}
    while True:
        at = {}
        for n, (va, _) in placed.items():
            at.setdefault(va, set()).add(n)
        bad = set()
        for va, names in at.items():
            if len(names) < 2:
                continue
            for n in names:
                for sym, addr, code in reloc_targets(img, placed[n][1], va):
                    if not code:
                        continue
                    if (sym in placed and placed[sym][0] != addr) or \
                            (addr in at and sym not in at[addr] and
                             addr not in matches.get(sym, {})):
                        bad.add(n)
                        break
        if not bad:
            return rejected
        for n in bad:
            rejected[n] = "%#x" % placed.pop(n)[0]


def consistent(img, sig, va, known):
    """(agree, disagree) counts of this placement's references vs `known`."""
    ok = bad = 0
    for sym, addr, _ in reloc_targets(img, sig, va):
        if sym in known:
            if known[sym] == addr:
                ok += 1
            else:
                bad += 1
    return ok, bad


# --- main --------------------------------------------------------------------

def run(sigs_path, image_path, names=None, min_fixed=8):
    lib = json.load(open(sigs_path))
    img = Image(image_path)
    pat = re.compile(names) if names else None
    exec_secs = [s for s in img.sections if s.executable]

    sigs, skipped = [], 0
    for rec in lib["functions"]:
        if pat and not pat.search(rec["name"]):
            continue
        sig = Signature(rec)
        if sig.fixed < min_fixed:
            skipped += 1
            continue
        sigs.append(sig)

    # name -> {va: Signature}; one name can have several body variants.
    matches = {}
    for sig in sigs:
        hits = matches.setdefault(sig.name, {})
        for sec in exec_secs:
            for va in sig.search(sec):
                hits.setdefault(va, sig)

    placed = {n: next(iter(h.items())) for n, h in matches.items()
              if len(h) == 1}
    rejected = reject_contradicted(img, placed, matches)
    n_sig_unique = len(placed)
    how = {n: "signature" for n in placed}

    # Iteratively pin down ambiguous names: a candidate survives only if
    # every reference it makes to an already-known symbol agrees, and a
    # candidate that is itself a known call/pointer target wins outright.
    # A placed function is known at its own address too, so thunks that
    # differ only in the function they call are told apart by it.
    while True:
        known, is_code, _ = collect_refs(img, placed)
        known.update((n, va) for n, (va, _) in placed.items())
        progress = False
        for name, hits in matches.items():
            if name in placed or len(hits) < 2:
                continue
            if name in known and known[name] in hits:
                va = known[name]
                placed[name], how[name] = (va, hits[va]), "reference"
                progress = True
                continue
            scored = [(va, consistent(img, sig, va, known))
                      for va, sig in hits.items()]
            good = [(va, ok) for va, (ok, bad) in scored if bad == 0 and ok]
            if len(good) == 1 and all(
                    bad for va, (ok, bad) in scored if va != good[0][0]):
                va = good[0][0]
                placed[name], how[name] = (va, hits[va]), "relocations"
                progress = True
        if not progress:
            break
    rejected.update(reject_contradicted(img, placed, matches))

    known, is_code, conflicts = collect_refs(img, placed)
    ambiguous = {n: sorted(h) for n, h in matches.items()
                 if len(h) > 1 and n not in placed}
    missing = sorted(n for n, h in matches.items() if not h)
    # Functions with no signature hit, located through references alone
    # (e.g. functions too short for a signature or not in SIGS.json).
    via_ref = {s: a for s, a in known.items()
               if is_code[s] and s not in placed and s not in ambiguous}
    # A placed function that disagrees with how others reference it.
    disagree = {n: {"placed": "%#x" % placed[n][0], "ref": "%#x" % known[n]}
                for n in placed if n in known and known[n] != placed[n][0]}
    data_syms = {s: a for s, a in known.items() if not is_code[s]}
    # Several names at one address: identical bodies folded by /OPT:ICF
    # (e.g. Push1/Push1f) -- any of the names is correct.
    by_va = {}
    for n, (va, _) in placed.items():
        by_va.setdefault(va, []).append(n)
    shared = {"%#x" % va: sorted(ns) for va, ns in sorted(by_va.items())
              if len(ns) > 1}

    fmt = lambda d: {n: "%#x" % v for n, v in sorted(d.items(),
                                                    key=lambda x: x[1])}
    return dict(
        image=image_path, kind=img.kind, base="%#x" % img.base,
        sigs=lib.get("lib"),
        stats=dict(searched=len(sigs), skipped_too_short=skipped,
                   unique_by_signature=n_sig_unique,
                   disambiguated=len(placed) - n_sig_unique,
                   ambiguous=len(ambiguous), rejected=len(rejected),
                   not_found=len(missing),
                   code_refs_without_sig_match=len(via_ref),
                   shared_addresses=len(shared),
                   data_symbols=len(data_syms),
                   ref_conflicts=len(conflicts)),
        unique={n: {"va": "%#x" % va, "how": how[n]} for n, (va, _) in
                sorted(placed.items(), key=lambda x: x[1][0])},
        shared_addresses=shared,
        rejected=rejected,
        ambiguous={n: ["%#x" % v for v in vs] for n, vs in
                   sorted(ambiguous.items())},
        not_found=missing,
        code_refs=fmt(via_ref),
        disagreements=disagree,
        data_symbols=fmt(data_syms),
        conflicts=conflicts)


def print_report(r, show_strings=False):
    print("%s (%s, base %s) vs %s" % (r["image"], r["kind"], r["base"],
                                      r["sigs"]))
    print("stats: " + ", ".join("%s=%s" % kv for kv in r["stats"].items()))
    print("\n== unique matches (how: signature | relocations | reference) ==")
    for n, u in r["unique"].items():
        print("  %s  %-11s %s" % (u["va"], u["how"], n))
    print("\n== addresses claimed by several names (folded bodies) ==")
    for va, ns in r["shared_addresses"].items():
        print("  %s  %s" % (va, " = ".join(ns)))
    print("\n== rejected (shared address, references contradict) ==")
    for n, va in r["rejected"].items():
        print("  %s  %s" % (va, n))
    print("\n== ambiguous ==")
    for n, vs in r["ambiguous"].items():
        print("  %s  %s" % (n, " ".join(vs)))
    print("\n== code symbols located only through references ==")
    for n, v in r["code_refs"].items():
        print("  %s  %s" % (v, n))
    strings = [n for n in r["data_symbols"] if n.startswith("??_C@")]
    print("\n== data symbols (%d, plus %d string literals%s) ==" % (
        len(r["data_symbols"]) - len(strings), len(strings),
        "" if show_strings else " not shown"))
    for n, v in r["data_symbols"].items():
        if show_strings or not n.startswith("??_C@"):
            print("  %s  %s" % (v, n))
    if r["disagreements"]:
        print("\n== placed function disagrees with references to it ==")
        for n, d in r["disagreements"].items():
            print("  %s  %s" % (n, d))
    if r["conflicts"]:
        print("\n== reference conflicts (majority value used) ==")
        for n, d in r["conflicts"].items():
            print("  %s  %s" % (n, d))
    print("\n== not found (%d) ==" % len(r["not_found"]))
    for n in r["not_found"]:
        print("  " + n)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sigs", metavar="SIGS.json")
    ap.add_argument("image", metavar="IMAGE")
    ap.add_argument("--names", help="only search functions matching REGEX")
    ap.add_argument("--min-fixed", type=int, default=8,
                    help="minimum non-wildcard bytes (default 8)")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--strings", action="store_true",
                    help="also list string-literal symbols (??_C@...)")
    args = ap.parse_args()
    r = run(args.sigs, args.image, args.names, args.min_fixed)
    if args.json:
        json.dump(r, sys.stdout, indent=1)
        print()
    else:
        print_report(r, args.strings)


if __name__ == "__main__":
    main()
