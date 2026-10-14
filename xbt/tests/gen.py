#!/usr/bin/env python3
"""
Random differential test generator for the x86 translator.

Each test is a short random instruction sequence (NASM syntax) plus an initial
register/flag state. The sequence is executed natively by runner32 and by
`xbt --test`; the final registers, flags, XMM registers and memory must match.
The generator tracks which flags are architecturally *defined* after every
instruction so undefined flags are masked out of the comparison and never
consumed by later instructions.

usage: gen.py [--seed N] [--count N] [--kinds k1,k2,...] out_prefix
writes out_prefix.bin (test cases) and out_prefix.txt (sources)
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

DATA = 0x00500000
DATA_SIZE = 0x1000
STACK_TOP = 0x0060f000

CF, PF, AF, ZF, SF, OF, DF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x800, 0x400
ALL = CF | PF | AF | ZF | SF | OF

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
R8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
GPR = [0, 1, 2, 3, 5, 6, 7]          # never pick esp as a general operand

CC = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]
CC_FLAGS = [OF, OF, CF, CF, ZF, ZF, CF | ZF, CF | ZF, SF, SF, PF, PF, SF | OF, SF | OF,
            ZF | SF | OF, ZF | SF | OF]
SIZE_KW = {1: "byte", 2: "word", 4: "dword"}


def regname(r, size):
    return {1: R8, 2: R16, 4: R32}[size][r]


class Gen:
    def __init__(self, rng):
        self.r = rng
        self.lines = []
        self.undef = 0
        self.label = 0

    # -- helpers -----------------------------------------------------
    def emit(self, s):
        self.lines.append(s)

    def newlabel(self):
        self.label += 1
        return ".L%d" % self.label

    def defined(self, mask):
        return (self.undef & mask) == 0

    def write_flags(self, written, undefined=0):
        """written: flags the instruction writes; undefined: subset left undefined"""
        self.undef = (self.undef & ~written) | undefined

    def imm(self, size):
        r = self.r
        choice = r.random()
        if choice < 0.2:
            v = r.choice([0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff,
                          0x7fffffff, 0x80000000, 0xffffffff])
        elif choice < 0.5:
            v = r.randint(0, 16)
        else:
            v = r.getrandbits(32)
        return v & ((1 << (8 * size)) - 1)

    def reg8(self):
        return self.r.choice([0, 1, 2, 3, 4, 5, 6, 7])

    def gpr(self):
        return self.r.choice(GPR)

    def mem(self, size, avoid=()):
        """Returns an address expression into the data region (may emit setup)."""
        r = self.r
        kind = r.random()
        off = r.randint(0, DATA_SIZE - 64)
        if kind < 0.3:
            return "[0x%x]" % (DATA + off)
        regs = [x for x in [0, 1, 2, 3, 5, 6, 7] if x not in avoid]
        if kind < 0.65:
            b = r.choice(regs)
            disp = r.choice([0, r.randint(-128, 127), r.randint(-2000, 2000)])
            base = DATA + off - disp
            if not (DATA <= base + disp < DATA + DATA_SIZE - 16):
                disp = 0
                base = DATA + off
            self.emit("mov %s, 0x%x" % (R32[b], base))
            return "[%s%+d]" % (R32[b], disp) if disp else "[%s]" % R32[b]
        b = r.choice(regs)
        i = r.choice([x for x in regs if x != b] or regs)
        scale = r.choice([1, 2, 4, 8])
        idx = r.randint(-40, 40)
        disp = r.choice([0, r.randint(-100, 100)])
        base = DATA + off - idx * scale - disp
        if not (DATA <= base + idx * scale + disp < DATA + DATA_SIZE - 16) or base < DATA - 0x1000:
            idx, disp, base = 0, 0, DATA + off
        self.emit("mov %s, 0x%x" % (R32[b], base & 0xffffffff))
        self.emit("mov %s, %d" % (R32[i], idx))
        s = "[%s+%s*%d%+d]" % (R32[b], R32[i], scale, disp) if disp else \
            "[%s+%s*%d]" % (R32[b], R32[i], scale)
        return s

    def rm(self, size, avoid=()):
        if self.r.random() < 0.5:
            return SIZE_KW[size] + " " + self.mem(size, avoid)
        return regname(self.reg_for(size), size)

    def reg_for(self, size):
        return self.reg8() if size == 1 else self.gpr()

    def size(self):
        return self.r.choice([1, 2, 4, 4, 4])

    # -- instruction groups -----------------------------------------
    def g_alu(self):
        r = self.r
        op = r.choice(["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"])
        if op in ("adc", "sbb") and not self.defined(CF):
            op = "add"
        size = self.size()
        form = r.randint(0, 4)
        if form == 0:
            self.emit("%s %s, %s" % (op, regname(self.reg_for(size), size), regname(self.reg_for(size), size)))
        elif form == 1:
            self.emit("%s %s, %s" % (op, self.rm(size), regname(self.reg_for(size), size)))
        elif form == 2:
            m = self.mem(size)
            self.emit("%s %s, %s %s" % (op, regname(self.reg_for(size), size), SIZE_KW[size], m))
        elif form == 3:
            self.emit("%s %s, 0x%x" % (op, self.rm(size), self.imm(size)))
        else:
            if size == 1:
                self.emit("%s al, 0x%x" % (op, self.imm(1)))
            else:
                v = r.choice([self.imm(size), r.randint(-128, 127) & ((1 << (8 * size)) - 1)])
                self.emit("%s %s, strict %s 0x%x" % (op, regname(self.gpr(), size), SIZE_KW[size], v)
                          if r.random() < 0.3 else "%s %s, 0x%x" % (op, regname(self.gpr(), size), v))
        logic = op in ("or", "and", "xor")
        self.write_flags(ALL, AF if logic else 0)

    def g_test(self):
        size = self.size()
        if self.r.random() < 0.5:
            self.emit("test %s, %s" % (self.rm(size), regname(self.reg_for(size), size)))
        else:
            self.emit("test %s, 0x%x" % (self.rm(size), self.imm(size)))
        self.write_flags(ALL, AF)

    def g_incdec(self):
        size = self.size()
        op = self.r.choice(["inc", "dec"])
        self.emit("%s %s" % (op, self.rm(size)))
        self.write_flags(ALL & ~CF)

    def g_negnot(self):
        size = self.size()
        op = self.r.choice(["neg", "not"])
        self.emit("%s %s" % (op, self.rm(size)))
        if op == "neg":
            self.write_flags(ALL)

    def g_mul(self):
        r = self.r
        size = self.size()
        form = r.randint(0, 3)
        if form == 0 or size == 1:
            op = r.choice(["mul", "imul"])
            self.emit("%s %s" % (op, self.rm(size)))
        elif form == 1:
            self.emit("imul %s, %s" % (regname(self.gpr(), size), self.rm(size)))
        else:
            self.emit("imul %s, %s, %d" % (regname(self.gpr(), size), self.rm(size),
                                           r.choice([r.randint(-128, 127), r.randint(-40000, 40000)])))
        self.write_flags(ALL, SF | ZF | AF | PF)

    def g_div(self):
        r = self.r
        size = self.size()
        sgn = r.random() < 0.5
        # divisor in ecx/cx/cl (or bl), dividend set up to avoid #DE
        dr = r.choice([1, 3, 5, 6, 7]) if size != 1 else r.choice([1, 3, 5, 7])
        if size == 1:
            d = r.randint(1, 255)
            if sgn:
                d = r.choice([r.randint(1, 127), -r.randint(1, 127)])
                q = r.randint(-100, 100)
                n = q * d + (r.randint(0, abs(d) - 1) if q >= 0 else -r.randint(0, abs(d) - 1))
                if not (-128 <= int(n / d) <= 127):
                    n = d
            else:
                n = r.randint(0, d * 256 - 1)
            self.emit("mov ax, %d" % (n & 0xffff))
            self.emit("mov %s, %d" % (R8[dr], d & 0xff))
            self.emit("%s %s" % ("idiv" if sgn else "div", R8[dr]))
        else:
            bits = 8 * size
            d = r.randint(1, (1 << bits) - 1)
            if sgn:
                d = r.randint(1, (1 << (bits - 1)) - 1) * r.choice([1, -1])
                q = r.randint(-(1 << (bits - 2)), (1 << (bits - 2)))
                n = q * d + r.randint(0, abs(d) - 1) * (1 if q >= 0 else -1)
                if not (-(1 << (bits - 1)) <= int(n / d) < (1 << (bits - 1))):
                    n = d
                lo = n & ((1 << bits) - 1)
                hi = (n >> bits) & ((1 << bits) - 1)
            else:
                n = r.randint(0, d * (1 << bits) - 1)
                lo = n & ((1 << bits) - 1)
                hi = n >> bits
            a, dd = ("eax", "edx") if size == 4 else ("ax", "dx")
            self.emit("mov %s, 0x%x" % (a, lo))
            self.emit("mov %s, 0x%x" % (dd, hi))
            rn = regname(dr, size)
            if r.random() < 0.5:
                self.emit("mov %s, 0x%x" % (rn, d & ((1 << bits) - 1)))
                self.emit("%s %s" % ("idiv" if sgn else "div", rn))
            else:
                m = "[0x%x]" % (DATA + r.randint(0, 1000) * 4)
                self.emit("mov %s %s, 0x%x" % (SIZE_KW[size], m, d & ((1 << bits) - 1)))
                self.emit("%s %s %s" % ("idiv" if sgn else "div", SIZE_KW[size], m))
        self.write_flags(ALL, ALL)

    def g_shift(self):
        r = self.r
        size = self.size()
        bits = 8 * size
        op = r.choice(["shl", "shr", "sar", "rol", "ror", "sal", "rcl", "rcr"])
        if op in ("rcl", "rcr") and not self.defined(CF):
            op = "rol"
        form = r.randint(0, 2)
        dst = self.rm(size, avoid=(1,))
        if form == 0:
            c = 1
            self.emit("%s %s, 1" % (op, dst))
        elif form == 1:
            c = r.choice([0, 1, 2, 7, 8, 15, 16, 31, r.randint(0, 255)])
            self.emit("%s %s, %d" % (op, dst, c))
        else:
            c = r.choice([0, 1, 3, 8, 9, 16, 17, 31, 32, 33, r.randint(0, 255)])
            self.emit("mov cl, %d" % c)
            self.emit("%s %s, cl" % (op, dst))
        mc = c & 31
        if mc == 0:
            return
        if op in ("rol", "ror", "rcl", "rcr"):
            self.write_flags(CF | OF, 0 if mc == 1 else OF)
        else:
            und = AF | (0 if mc == 1 else OF)
            if mc > bits and op != "sar":
                und |= CF
            if mc >= bits and op == "sar" and size < 4:
                pass
            self.write_flags(ALL, und)

    def g_shxd(self):
        r = self.r
        size = r.choice([2, 4, 4])
        op = r.choice(["shld", "shrd"])
        dst = self.rm(size, avoid=(1,))
        src = regname(r.choice([0, 2, 3, 5, 6, 7]), size)
        if r.random() < 0.5:
            c = r.randint(0, 31 if size == 4 else 16)
            self.emit("%s %s, %s, %d" % (op, dst, src, c))
        else:
            c = r.randint(0, 31 if size == 4 else 16)
            self.emit("mov cl, %d" % c)
            self.emit("%s %s, %s, cl" % (op, dst, src))
        if c & 31:
            self.write_flags(ALL, AF | (0 if (c & 31) == 1 else OF))

    def g_bt(self):
        r = self.r
        size = r.choice([2, 4])
        op = r.choice(["bt", "bts", "btr", "btc"])
        form = r.randint(0, 2)
        if form == 0:
            self.emit("%s %s, %s" % (op, regname(self.gpr(), size), regname(self.gpr(), size)))
        elif form == 1:
            self.emit("%s %s, %d" % (op, self.rm(size), r.randint(0, 255)))
        else:
            base = DATA + 0x800
            self.emit("mov esi, 0x%x" % base)
            self.emit("mov edi, %d" % r.randint(-1000, 1000))
            self.emit("%s %s [esi], %s" % (op, SIZE_KW[size], regname(7, size)))
        self.write_flags(ALL & ~ZF, OF | SF | AF | PF)

    def g_bsx(self):
        size = self.r.choice([2, 4])
        op = self.r.choice(["bsf", "bsr"])
        src = self.rm(size)
        self.emit("%s %s, %s" % (op, regname(self.gpr(), size), src))
        self.write_flags(ALL, ALL & ~ZF)

    def g_cond(self):
        r = self.r
        ok = [i for i in range(16) if self.defined(CC_FLAGS[i])]
        if not ok:
            return self.g_alu()
        c = CC[r.choice(ok)]
        form = r.randint(0, 3)
        if form == 0:
            self.emit("set%s %s" % (c, self.rm(1)))
        elif form == 1:
            size = r.choice([2, 4])
            self.emit("cmov%s %s, %s" % (c, regname(self.gpr(), size), self.rm(size)))
        else:
            l = self.newlabel()
            self.emit("j%s %s" % (c, l))
            self.g_simple()
            self.emit("%s:" % l)

    def g_simple(self):
        """an instruction that does not touch flags"""
        r = self.r
        k = r.randint(0, 6)
        size = self.size()
        if k == 0:
            self.emit("mov %s, 0x%x" % (regname(self.reg_for(size), size), self.imm(size)))
        elif k == 1:
            self.emit("mov %s, %s" % (regname(self.reg_for(size), size), self.rm(size)))
        elif k == 2:
            self.emit("mov %s, %s" % (self.rm(size), regname(self.reg_for(size), size)))
        elif k == 3:
            self.emit("lea %s, [%s+%s*%d+%d]" % (R32[self.gpr()], R32[self.gpr()], R32[self.gpr()],
                                                r.choice([1, 2, 4, 8]), r.randint(-5000, 5000)))
        elif k == 4:
            op = r.choice(["movzx", "movsx"])
            ssz = r.choice([1, 2])
            dsz = 4 if ssz == 2 else r.choice([2, 4])
            self.emit("%s %s, %s" % (op, regname(self.gpr(), dsz), self.rm(ssz)))
        elif k == 5:
            self.emit(r.choice(["cdq", "cwde", "cbw", "cwd", "bswap %s" % R32[self.gpr()]]))
        else:
            if r.random() < 0.5:
                self.emit("xchg %s, %s" % (regname(self.reg_for(size), size), regname(self.reg_for(size), size)))
            else:
                self.emit("xchg %s, %s" % (self.rm(size), regname(self.reg_for(size), size)))

    def g_atomic(self):
        r = self.r
        size = self.size()
        op = r.choice(["cmpxchg", "xadd"])
        if r.random() < 0.3:
            # make cmpxchg succeed sometimes
            self.emit("mov %s, %s" % (regname(0, size), regname(self.reg_for(size), size)))
        dst = self.rm(size)
        pre = "lock " if "[" in dst and r.random() < 0.5 else ""   # lock needs a memory operand
        self.emit("%s%s %s, %s" % (pre, op, dst, regname(self.reg_for(size), size)))
        self.write_flags(ALL)

    def g_stack(self):
        r = self.r
        k = r.randint(0, 6)
        if k == 0:
            self.emit("push %s" % R32[r.choice([0, 1, 2, 3, 4, 5, 6, 7])])
            self.emit("pop %s" % R32[self.gpr()])
        elif k == 1:
            self.emit("push dword 0x%x" % self.imm(4))
            self.emit("pop %s" % self.rm(4, avoid=()))
        elif k == 2 and self.undef == 0:
            self.emit("pushfd")
            self.emit("pop %s" % R32[self.gpr()])
        elif k == 3:
            v = r.getrandbits(32) & (ALL | DF)
            self.emit("push dword 0x%x" % v)
            self.emit("popfd")
            self.emit("cld")
            self.undef = 0
        elif k == 4 and self.defined(SF | ZF | AF | PF | CF):
            self.emit("lahf")
        elif k == 5:
            self.emit("sahf")
            self.write_flags(SF | ZF | AF | PF | CF)
        else:
            self.emit("pushad")
            self.emit("mov eax, 0x%x" % self.imm(4))
            self.emit("popad")

    def g_flagops(self):
        r = self.r
        k = r.choice(["clc", "stc", "cmc", "std", "cld"])
        if k == "cmc" and not self.defined(CF):
            k = "stc"
        self.emit(k)
        if k in ("clc", "stc", "cmc"):
            self.write_flags(CF)

    def g_string(self):
        r = self.r
        size = r.choice([1, 2, 4])
        sfx = {1: "b", 2: "w", 4: "d"}[size]
        df = r.random() < 0.25
        self.emit("std" if df else "cld")
        n = r.randint(0, 12)
        src = DATA + 0x400 + r.randint(0, 64)
        dst = DATA + 0x800 + r.randint(0, 64)
        self.emit("mov esi, 0x%x" % src)
        self.emit("mov edi, 0x%x" % dst)
        self.emit("mov ecx, %d" % n)
        op = r.choice(["movs", "stos", "lods", "cmps", "scas"])
        if op in ("cmps", "scas"):
            pre = r.choice(["", "repe ", "repne "])
            self.emit("%s%s%s" % (pre, op, sfx))
            if pre == "" or n > 0:
                self.write_flags(ALL)
        else:
            pre = r.choice(["", "rep "]) if op != "lods" else ""
            self.emit("%s%s%s" % (pre, op, sfx))
        self.emit("cld")

    def g_loop(self):
        r = self.r
        n = r.randint(1, 6)
        self.emit("mov ecx, %d" % n)
        l = self.newlabel()
        self.emit("%s:" % l)
        save = self.undef
        self.emit("push ecx")           # the body may clobber ecx
        self.g_alu()
        self.g_simple()
        self.emit("pop ecx")
        op = r.choice(["loop", "loop", "loope", "loopne"])
        if op != "loop" and not self.defined(ZF):
            op = "loop"
        self.emit("%s %s" % (op, l))
        self.undef |= save

    def g_call(self):
        l1, l2 = self.newlabel(), self.newlabel()
        self.emit("call %s" % l1)
        self.emit("jmp %s" % l2)
        self.emit("%s:" % l1)
        save = self.undef
        if ABI:
            self.undef = ALL    # a callee cannot rely on the caller's flags
        self.g_alu()
        self.g_simple()
        if self.r.random() < 0.5:
            self.emit("ret")
        else:
            self.emit("ret 0")
        self.emit("%s:" % l2)
        self.undef |= save
        if ABI:
            # MSVC calling convention: no flag state survives a call boundary
            self.undef = ALL

    def g_bcd(self):
        r = self.r
        op = r.choice(["daa", "das", "aaa", "aas", "aam", "aad"])
        if op in ("daa", "das", "aaa", "aas") and not self.defined(CF | AF):
            self.emit("add al, 0x%x" % r.randint(0, 255))
            self.write_flags(ALL)
        self.emit(op)
        und = {"daa": OF, "das": OF, "aaa": OF | SF | ZF | PF, "aas": OF | SF | ZF | PF,
               "aam": OF | AF | CF, "aad": OF | AF | CF}[op]
        self.write_flags(ALL, und)

    def g_misc(self):
        r = self.r
        k = r.randint(0, 3)
        if k == 0:
            self.emit("mov ebx, 0x%x" % (DATA + r.randint(0, 0x700)))
            self.emit("xlatb")
        elif k == 1 and self.defined(CF):
            self.emit("db 0xd6 ; salc")
        elif k == 2:
            self.emit("enter %d, 0" % r.randint(0, 64))
            self.emit("mov dword [ebp-4], 0x%x" % self.imm(4))
            self.emit("leave")
        else:
            self.emit("nop")

    def g_jecxz(self):
        l = self.newlabel()
        self.emit("mov ecx, %d" % self.r.choice([0, 1]))
        self.emit("jecxz %s" % l)
        self.g_simple()
        self.emit("%s:" % l)

    def g_flagcross(self):
        """producer, then branch to another block, then consume there"""
        l = self.newlabel()
        self.g_alu()
        self.emit("jmp %s" % l)
        self.emit("ud2")
        self.emit("%s:" % l)
        self.g_cond()


GROUPS = {
    "alu": (Gen.g_alu, 10), "test": (Gen.g_test, 3), "incdec": (Gen.g_incdec, 3),
    "negnot": (Gen.g_negnot, 2), "mul": (Gen.g_mul, 3), "div": (Gen.g_div, 2),
    "shift": (Gen.g_shift, 5), "shxd": (Gen.g_shxd, 2), "bt": (Gen.g_bt, 2),
    "bsx": (Gen.g_bsx, 1), "cond": (Gen.g_cond, 8), "simple": (Gen.g_simple, 4),
    "atomic": (Gen.g_atomic, 2), "stack": (Gen.g_stack, 3), "flagops": (Gen.g_flagops, 2),
    "string": (Gen.g_string, 2), "loop": (Gen.g_loop, 1), "call": (Gen.g_call, 1),
    "bcd": (Gen.g_bcd, 1), "misc": (Gen.g_misc, 1), "jecxz": (Gen.g_jecxz, 1),
    "flagcross": (Gen.g_flagcross, 3),
}

EXTRA_GROUPS = {}   # filled by other modules (x87, sse)
ABI = True          # flags are dead across call/ret (matches xbt default)


def make_test(rng, kinds, length):
    g = Gen(rng)
    for _ in range(length):
        k = rng.choices(kinds, weights=[ALL_GROUPS[k][1] for k in kinds])[0]
        ALL_GROUPS[k][0](g)
    regs = [rng.getrandbits(32) for _ in range(8)]
    regs[4] = STACK_TOP
    eflags = rng.getrandbits(32) & ALL
    xmm = bytes(rng.getrandbits(8) for _ in range(128))
    return g.lines, regs, eflags, (ALL & ~g.undef) | DF, xmm


ALL_GROUPS = dict(GROUPS)


def assemble(tests):
    src = ["BITS 32", "section .text"]
    for i, t in enumerate(tests):
        src.append("t%d:" % i)          # .L labels are local to t<i> in NASM
        src.extend(l if l.endswith(":") else "  " + l for l in t[0])
        src.append("t%d_end:" % i)
    src.append("align 4")
    src.append("table:")
    for i in range(len(tests)):
        src.append("dd t%d, t%d_end" % (i, i))
    text = "\n".join(src) + "\n"
    with tempfile.TemporaryDirectory() as d:
        a = os.path.join(d, "t.asm")
        o = os.path.join(d, "t.bin")
        open(a, "w").write(text)
        p = subprocess.run(["nasm", "-f", "bin", "-O1", "-o", o, a], capture_output=True, text=True)
        if p.returncode:
            sys.stderr.write(p.stderr[:4000])
            raise SystemExit("nasm failed")
        blob = open(o, "rb").read()
    n = len(tests)
    tab = blob[len(blob) - 8 * n:]
    out = []
    for i in range(n):
        s, e = struct.unpack_from("<II", tab, 8 * i)
        out.append(blob[s:e])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--length", type=int, default=6)
    ap.add_argument("--kinds", default="")
    ap.add_argument("--strict", action="store_true", help="flags live across call/ret")
    ap.add_argument("out")
    a = ap.parse_args()
    global ABI
    ABI = not a.strict
    rng = random.Random(a.seed)
    kinds = a.kinds.split(",") if a.kinds else list(GROUPS)
    tests = []
    for i in range(a.count):
        tests.append(make_test(rng, kinds, rng.randint(1, a.length)))
    codes = assemble(tests)
    with open(a.out + ".bin", "wb") as f, open(a.out + ".txt", "w") as ft:
        f.write(b"XTC1" + struct.pack("<I", len(tests)))
        for i, ((lines, regs, eflags, mask, xmm), code) in enumerate(zip(tests, codes)):
            seed = rng.getrandbits(32)
            f.write(struct.pack("<II8III", len(code), seed, *regs, eflags, mask))
            f.write(xmm)
            f.write(code + b"\0" * (-len(code) % 4))
            ft.write("=== test %d (mask %03x)\n%s\n" % (i, mask, "\n".join(lines)))


if __name__ == "__main__":
    main()
