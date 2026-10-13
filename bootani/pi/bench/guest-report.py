#!/usr/bin/env python3
"""Add up an XBCOMPAT_PROFILE file by where the time went.

    guest-report.py PROFILE TITLE.map default.xbe [xbcompat-native.x86]

PROFILE is the file XBCOMPAT_PROFILE wrote (src/prof.c), TITLE.map the
library map xbcompat made for the title (XDG_CACHE_HOME/xbcompat/maps or the
cache folder beside it), default.xbe the title itself and
xbcompat-native.x86 the unstripped x86 runtime that ran (for addr2line).

Prints the share of the sampled CPU time in each XBE section (the XDK's
libraries mostly have their own: D3D, DSOUND, XGRPH, ...), in library
functions the map names (wherever they sit), in the title's own code, in
xbcompat's own x86 code, and in native code (box86 bridges: the renderer,
the mixer, libc), then the busiest functions of each kind.  Title code
without a name is grouped by the nearest function start found in the
code (after padding or a return).
"""
import bisect, os, re, shutil, struct, subprocess, sys
from collections import defaultdict


def read_profile(path):
    addrs, callers, header = [], [], []
    mode = None
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("all addresses"):
            mode = "a"; continue
        if line.startswith("all title code on the stack"):
            mode = "c"; continue
        if mode is None:
            header.append(line)
            continue
        m = re.match(r"(\d+) ([0-9a-f]{8})$", line)
        if not m:
            continue
        (addrs if mode == "a" else callers).append((int(m.group(1)), int(m.group(2), 16)))
    return header, addrs, callers


def read_map(path):
    syms = []
    for line in open(path, errors="replace"):
        parts = line.split()
        if len(parts) >= 2 and parts[-1].startswith("0x"):
            try:
                syms.append((int(parts[-1], 16), " ".join(parts[:-1])))
            except ValueError:
                pass
    syms.sort()
    return syms


def read_xbe(path):
    d = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    assert d[:4] == b"XBEH", "not an XBE"
    base = u32(0x104)
    nsec, sechdr = u32(0x11C), u32(0x120)
    secs = []
    for i in range(nsec):
        o = sechdr - base + i * 56
        fl, va, vs, raw, rs, nm = struct.unpack_from("<6I", d, o)
        name = d[nm - base:d.index(b"\0", nm - base)].decode(errors="replace")
        secs.append((va, va + vs, name, raw, rs))
    secs.sort()
    return d, secs


def function_starts(d, secs):
    """Likely function starts in the code sections: targets of direct calls
    (E8 rel32) that land in a section, and the first byte after padding
    (int3 or nop) that follows a return."""
    starts = set()
    spans = [(va, va + rs, raw) for va, end, name, raw, rs in secs]
    def inside(a):
        return any(lo <= a < hi for lo, hi, _ in spans)
    for va, end, raw in spans:
        code = d[raw:raw + (end - va)]
        n = len(code)
        i = 0
        while i < n - 5:
            if code[i] == 0xE8:
                t = va + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
                if inside(t):
                    starts.add(t)
            i += 1
        after_ret = False
        for i in range(n):
            b = code[i]
            if b in (0xCC, 0x90):
                if after_ret and i + 1 < n and code[i + 1] not in (0xCC, 0x90):
                    starts.add(va + i + 1)
                continue
            after_ret = b == 0xC3 or (i >= 2 and code[i - 2] == 0xC2)
    return sorted(starts)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    header, addrs, callers = read_profile(sys.argv[1])
    syms = read_map(sys.argv[2])
    sym_addrs = [a for a, _ in syms]
    d, secs = read_xbe(sys.argv[3])
    runtime = sys.argv[4] if len(sys.argv) > 4 else None
    starts = function_starts(d, secs)
    sec_lo = [s[0] for s in secs]
    total = sum(n for n, _ in addrs) or 1

    def section(a):
        i = bisect.bisect_right(sec_lo, a) - 1
        if i >= 0 and a < secs[i][1]:
            return secs[i][2]
        return None

    def lib_name(a):
        i = bisect.bisect_right(sym_addrs, a) - 1
        if i < 0:
            return None
        s_addr, s_name = syms[i]
        if section(s_addr) != section(a):
            return None
        # A named function runs to the next named one or function start.
        j = bisect.bisect_right(starts, s_addr)
        nxt = starts[j] if j < len(starts) else s_addr + 0x10000
        if i + 1 < len(syms):
            nxt = min(nxt, syms[i + 1][0]) if syms[i + 1][0] > s_addr else nxt
        if a - s_addr > 0x4000:
            return None
        return s_name if a < max(nxt, s_addr + 1) or a - s_addr < 0x400 else None

    def func_start(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 else a

    resolve = {}
    if runtime and shutil.which("addr2line"):
        rt = sorted({a & ~1 for _, a in addrs + callers if section(a & ~1) is None})
        if rt:
            out = subprocess.run(["addr2line", "-f", "-e", runtime] + ["%#x" % a for a in rt],
                                 capture_output=True, text=True).stdout.split("\n")
            for k, a in enumerate(rt):
                fn = out[2 * k] if 2 * k < len(out) else "??"
                resolve[a] = fn

    regions = defaultdict(int)
    funcs = defaultdict(int)
    for n, key in addrs:
        native = key & 1
        a = key & ~1
        sec = section(a)
        if sec is not None:
            name = lib_name(a)
            kind = ("native call from " if native else "") + "title " + sec
            regions[kind + (" (library)" if name else " (own code)")] += n
            funcs[(kind, name or "fn_%08x" % func_start(a))] += n
        else:
            fn = resolve.get(a, "%08x" % a)
            kind = "native call from xbcompat" if native else "xbcompat x86 code"
            regions[kind] += n
            funcs[(kind, fn)] += n

    print("\n".join(header[:1]))
    print("\nby region (share of all sampled guest-thread CPU time):")
    for k, n in sorted(regions.items(), key=lambda x: -x[1]):
        print("%6.2f%%  %s" % (100.0 * n / total, k))
    print("\nbusiest functions:")
    for (kind, fn), n in sorted(funcs.items(), key=lambda x: -x[1])[:80]:
        if n * 1000 < total * 2:
            break
        print("%6.2f%%  %-40s %s" % (100.0 * n / total, kind, fn))
    ctot = sum(n for n, _ in callers) or 1
    cf = defaultdict(int)
    for n, a in callers:
        name = lib_name(a)
        cf[(section(a) or "?", name or "fn_%08x" % func_start(a))] += n
    print("\ntitle code on the stack, by function (its own time plus what it called):")
    for (sec, fn), n in sorted(cf.items(), key=lambda x: -x[1])[:40]:
        print("%6.2f%%  %-8s %s" % (100.0 * n / ctot, sec, fn))
    print("\nsections:")
    for va, end, name, raw, rs in secs:
        print("  %-10s %08x-%08x" % (name, va, end))


if __name__ == "__main__":
    main()
