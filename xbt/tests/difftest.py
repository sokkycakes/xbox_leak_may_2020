#!/usr/bin/env python3
"""
Differential test driver: generates random tests, runs them natively
(runner32 on this x86 machine) and through the translator (xbt --test,
under qemu-aarch64 when not on an AArch64 host) and compares the results.

usage: difftest.py [--count N] [--seed N] [--kinds ...] [--xbt-args "..."] [--show N]
"""
import argparse
import os
import platform
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RES_SIZE = 32 + 4 + 4 + 128 + 0x1000 + 512
FLAGNAMES = [(0x1, "CF"), (0x4, "PF"), (0x10, "AF"), (0x40, "ZF"), (0x80, "SF"), (0x800, "OF"), (0x400, "DF")]
R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def build_runner():
    exe = os.path.join(HERE, "build", "runner32")
    src = os.path.join(HERE, "runner32.c")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        os.makedirs(os.path.dirname(exe), exist_ok=True)
        subprocess.check_call(["clang", "--target=i386-linux-gnu", "-ffreestanding", "-nostdlib",
                               "-static", "-O1", "-msse", "-fno-pic", "-o", exe, src])
    return exe


def xbt_cmd():
    exe = os.path.join(ROOT, "build", "xbt")
    if platform.machine() == "aarch64":
        return [exe]
    return ["qemu-aarch64", "-L", "/usr/aarch64-linux-gnu", exe]


def read_results(path):
    data = open(path, "rb").read()
    assert data[:4] == b"XTR1"
    n = struct.unpack_from("<I", data, 4)[0]
    out = []
    for i in range(n):
        o = 8 + i * RES_SIZE
        regs = struct.unpack_from("<8I", data, o)
        eflags, status = struct.unpack_from("<II", data, o + 32)
        xmm = data[o + 40:o + 168]
        mem = data[o + 168:o + RES_SIZE]
        out.append((regs, eflags, status, xmm, mem))
    return out


def read_masks(path):
    data = open(path, "rb").read()
    n = struct.unpack_from("<I", data, 4)[0]
    pos = 8
    masks = []
    for i in range(n):
        code_len = struct.unpack_from("<I", data, pos)[0]
        mask = struct.unpack_from("<I", data, pos + 4 + 4 + 32 + 4)[0]
        masks.append(mask)
        pos += 4 + 4 + 32 + 4 + 4 + 128 + ((code_len + 3) & ~3)
    return masks


# SSE operations that can produce a default NaN (0/0, inf-inf, sqrt(-1)).
# x86 gives it the sign bit, AArch64 does not; xbt only emulates that with
# --x86-nan. A failing test containing one of them is re-run in that mode.
NAN_SOURCES = re.compile(r"\b(add|sub|mul|div|sqrt)(ps|ss)\b")


def flagstr(v):
    return " ".join(n for b, n in FLAGNAMES if v & b)


def diff_one(ra, rb, m, xmm_check=True):
    diffs = []
    if rb[2] != 0:
        diffs.append("translator exit status %d" % rb[2])
    for r in range(8):
        if ra[0][r] != rb[0][r]:
            diffs.append("%s: native %08x xbt %08x" % (R32[r], ra[0][r], rb[0][r]))
    if (ra[1] ^ rb[1]) & m & 0xfff:
        diffs.append("flags (mask %s): native %s | xbt %s" % (flagstr(m), flagstr(ra[1] & m), flagstr(rb[1] & m)))
    if xmm_check and ra[3] != rb[3]:
        for r in range(8):
            x, y = ra[3][16 * r:16 * r + 16], rb[3][16 * r:16 * r + 16]
            if x == y:
                continue
            ok = True
            for k in range(4):
                fx = struct.unpack_from("<f", x, 4 * k)[0]
                fy = struct.unpack_from("<f", y, 4 * k)[0]
                if x[4 * k:4 * k + 4] == y[4 * k:4 * k + 4] or (fx != fx and fy != fy):
                    continue      # identical, or both NaN (NaN sign is not emulated)
                if (m & 0x10000) and abs(fx - fy) <= 2.0 ** -11 * max(abs(fx), abs(fy)):
                    continue      # rcpps/rsqrtps: approximation precision differs
                ok = False
            if not ok:
                diffs.append("xmm%d: native %s xbt %s" % (r, x.hex(), y.hex()))
    ma, mb = bytearray(ra[4]), bytearray(rb[4])
    # x87 transcendental results: relative tolerance on doubles
    for off in range(0xd00, 0xe00, 8):
        x = struct.unpack_from("<d", ma, off)[0]
        y = struct.unpack_from("<d", mb, off)[0]
        # the absolute floor covers cancellation, e.g. fldlg2 ... fsub of the
        # same constant: 0 in doubles, the 80-bit rounding residue natively
        if x != y and abs(x - y) <= max(1e-12 * max(abs(x), abs(y)), 1e-15):
            mb[off:off + 8] = ma[off:off + 8]
    # NaN sign/payload is not emulated (x87 "real indefinite" is negative)
    for off in range(0xc00, 0xe00, 8):
        x = struct.unpack_from("<d", ma, off)[0]
        y = struct.unpack_from("<d", mb, off)[0]
        if x != x and y != y:
            mb[off:off + 8] = ma[off:off + 8]
    for off in range(0xc40, 0xc80, 4):
        x = struct.unpack_from("<f", ma, off)[0]
        y = struct.unpack_from("<f", mb, off)[0]
        if x != x and y != y:
            mb[off:off + 4] = ma[off:off + 4]
    if ma != mb:
        for off in range(len(ma)):
            if ma[off] != mb[off]:
                where = ("data+0x%x" % off) if off < 0x1000 else ("stack-0x%x" % (0x1000 + 512 - off))
                o8 = off & ~7
                diffs.append("memory %s: native %02x xbt %02x (qword native %s xbt %s)" % (
                    where, ma[off], mb[off], ma[o8:o8 + 8].hex(), mb[o8:o8 + 8].hex()))
                break
    return diffs


def compare(cases, native, xbt, srcs, rerun_nan=None):
    """Return {test index: diffs} of the failing tests. rerun_nan() gives the
    results of the --x86-nan run, used to clear failures caused by the
    default-NaN sign only."""
    masks = read_masks(cases)
    a = read_results(native)
    b = read_results(xbt)
    fails = {}
    for i, (ra, rb, m) in enumerate(zip(a, b, masks)):
        d = diff_one(ra, rb, m)
        if d:
            fails[i] = d
    nan_only = [i for i in fails if NAN_SOURCES.search(srcs.get(i, ""))]
    cleared = []
    if nan_only and rerun_nan:
        c = rerun_nan()
        for i in nan_only:
            if not diff_one(a[i], c[i], masks[i]):
                cleared.append(i)
                del fails[i]
    return fails, cleared, len(a)


def load_sources(path):
    srcs = {}
    cur = None
    for line in open(path):
        if line.startswith("=== test "):
            cur = int(line.split()[2])
            srcs[cur] = ""
        elif cur is not None:
            srcs[cur] += "      " + line
    return srcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--length", type=int, default=6)
    ap.add_argument("--kinds", default="")
    ap.add_argument("--gen", default="gen.py")
    ap.add_argument("--xbt-args", default="")
    ap.add_argument("--show", type=int, default=5)
    ap.add_argument("--keep", default="")
    ap.add_argument("--strict", action="store_true", help="test with --strict-abi")
    a = ap.parse_args()
    runner = build_runner()
    d = a.keep or tempfile.mkdtemp()
    os.makedirs(d, exist_ok=True)
    pre = os.path.join(d, "cases")
    cmd = [sys.executable, os.path.join(HERE, a.gen), "--seed", str(a.seed), "--count", str(a.count),
           "--length", str(a.length)]
    if a.kinds:
        cmd += ["--kinds", a.kinds]
    if a.strict:
        cmd += ["--strict"]
        a.xbt_args += " --strict-abi"
    subprocess.check_call(cmd + [pre])
    subprocess.check_call([runner, pre + ".bin", pre + ".native"])
    subprocess.check_call(xbt_cmd() + a.xbt_args.split() + ["--test", pre + ".bin", pre + ".xbt"])

    def rerun_nan():
        subprocess.check_call(xbt_cmd() + a.xbt_args.split() + ["--x86-nan", "--test", pre + ".bin",
                                                                pre + ".xbt-nan"])
        return read_results(pre + ".xbt-nan")

    srcs = load_sources(pre + ".txt")
    fails, cleared, n = compare(pre + ".bin", pre + ".native", pre + ".xbt", srcs, rerun_nan)
    for k, i in enumerate(sorted(fails)):
        if k >= a.show:
            break
        print("---- test %d FAILED" % i)
        print(srcs.get(i, "?"))
        for d in fails[i]:
            print("   " + d)
    print("%d/%d tests passed" % (n - len(fails), n) +
          (" (%d of them differ only in the default-NaN sign: identical with --x86-nan)" % len(cleared)
           if cleared else ""))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
