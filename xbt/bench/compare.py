#!/usr/bin/env python3
"""
Instruction-count comparison of xbt and box86 on the same i386 programs.

Every run happens under qemu user mode with the `libinsn` TCG plugin, which
counts the instructions the emulated CPU executes: x86 instructions for the
native reference (qemu-i386), AArch64 instructions for xbt (qemu-aarch64) and
ARMv7 instructions for box86 (qemu-arm). The counts include each
translator's own work (decoding, translation, dispatch), not just the
generated code. Start-up cost is removed by subtracting the empty program.

usage: compare.py --qemu-build DIR --qemu-i386-build DIR --box86 PATH [--out results.json]
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BENCHES = ["b_null", "b_int", "b_x87", "b_sse"]


def count(cmd, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    m = re.search(r"total insns: (\d+)", p.stderr)
    out = p.stdout.strip()
    if not m:
        sys.stderr.write("failed: %s\n%s\n%s\n" % (" ".join(cmd), p.stdout[-500:], p.stderr[-2000:]))
        return None, out
    return int(m.group(1)), out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu-build", required=True)
    ap.add_argument("--qemu-i386-build", required=True)
    ap.add_argument("--box86", required=True)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    plug_a64 = [os.path.join(a.qemu_build, "qemu-aarch64"), "-plugin",
                os.path.join(a.qemu_build, "tests/plugin/libinsn.so"), "-d", "plugin"]
    plug_arm = [os.path.join(a.qemu_build, "qemu-arm"), "-plugin",
                os.path.join(a.qemu_build, "tests/plugin/libinsn.so"), "-d", "plugin"]
    plug_x86 = [os.path.join(a.qemu_i386_build, "qemu-i386"), "-plugin",
                os.path.join(a.qemu_i386_build, "tests/plugin/libinsn.so"), "-d", "plugin"]
    xbt = os.path.join(ROOT, "build", "xbt")
    configs = {
        "x86 (native)": lambda b: plug_x86 + [b],
        "xbt": lambda b: plug_a64 + ["-L", "/usr/aarch64-linux-gnu", xbt, "--elf", b],
        "xbt, no flag elimination": lambda b: plug_a64 + ["-L", "/usr/aarch64-linux-gnu", xbt,
                                                         "--no-flag-elim", "--elf", b],
        "xbt, no NZCV fusion": lambda b: plug_a64 + ["-L", "/usr/aarch64-linux-gnu", xbt, "--no-fusion",
                                                    "--elf", b],
        "xbt, no block linking": lambda b: plug_a64 + ["-L", "/usr/aarch64-linux-gnu", xbt, "--no-link",
                                                      "--elf", b],
        "xbt, none of the three": lambda b: plug_a64 + ["-L", "/usr/aarch64-linux-gnu", xbt, "--no-flag-elim",
                                                       "--no-fusion", "--no-link", "--elf", b],
        "box86": lambda b: plug_arm + ["-L", "/usr/arm-linux-gnueabihf", a.box86, b],
    }
    env = dict(os.environ, BOX86_LOG="0", BOX86_NOBANNER="1")
    res = {}
    for name, mk in configs.items():
        res[name] = {}
        for bn in BENCHES:
            n, out = count(mk(os.path.join(HERE, "build", bn)), env)
            res[name][bn] = n
            sys.stderr.write("%-26s %-7s %14s  %s\n" % (name, bn, n, out))
    # per-benchmark cost: subtract start-up (empty program)
    print("\nInstructions executed per benchmark (start-up subtracted), and per x86 instruction:\n")
    hdr = "%-26s" % "" + "".join("%26s" % b for b in BENCHES[1:])
    print(hdr)
    x86 = res["x86 (native)"]
    for name in configs:
        row = "%-26s" % name
        for bn in BENCHES[1:]:
            v, z, g = res[name][bn], res[name]["b_null"], x86[bn] - x86["b_null"]
            if v is None or z is None:
                row += "%26s" % "n/a"
            else:
                row += "%16d (%5.2f)  " % (v - z, (v - z) / g)
        print(row)
    print("\nStart-up (empty program): " +
          ", ".join("%s %s" % (k, res[k]["b_null"]) for k in configs))
    if a.out:
        json.dump(res, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
