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


def flagstr(v):
    return " ".join(n for b, n in FLAGNAMES if v & b)


def compare(cases, native, xbt, srcs, show, xmm_check=True):
    masks = read_masks(cases)
    a = read_results(native)
    b = read_results(xbt)
    bad = 0
    for i, (ra, rb, m) in enumerate(zip(a, b, masks)):
        diffs = []
        if rb[2] != 0:
            diffs.append("translator exit status %d" % rb[2])
        for r in range(8):
            if ra[0][r] != rb[0][r]:
                diffs.append("%s: native %08x xbt %08x" % (R32[r], ra[0][r], rb[0][r]))
        if (ra[1] ^ rb[1]) & m:
            diffs.append("flags (mask %s): native %s | xbt %s" % (flagstr(m), flagstr(ra[1] & m), flagstr(rb[1] & m)))
        if xmm_check and ra[3] != rb[3]:
            for r in range(8):
                x, y = ra[3][16 * r:16 * r + 16], rb[3][16 * r:16 * r + 16]
                if x != y:
                    diffs.append("xmm%d: native %s xbt %s" % (r, x.hex(), y.hex()))
        if ra[4] != rb[4]:
            for off in range(len(ra[4])):
                if ra[4][off] != rb[4][off]:
                    where = ("data+0x%x" % off) if off < 0x1000 else ("stack-0x%x" % (0x1000 + 512 - off))
                    diffs.append("memory %s: native %02x xbt %02x" % (where, ra[4][off], rb[4][off]))
                    break
        if diffs:
            bad += 1
            if bad <= show:
                print("---- test %d FAILED" % i)
                print(srcs.get(i, "?"))
                for d in diffs:
                    print("   " + d)
    return bad, len(a)


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
    bad, n = compare(pre + ".bin", pre + ".native", pre + ".xbt", load_sources(pre + ".txt"), a.show)
    print("%d/%d tests passed" % (n - bad, n))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
