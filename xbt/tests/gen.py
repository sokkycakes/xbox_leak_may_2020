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



# --------------------------------------------------------------------------
# x87: values stay finite (x87 and AArch64 produce different default NaNs).
# Precision control is set to double so basic arithmetic must match exactly;
# results of transcendental instructions are compared with a tolerance.
FP_SRC = DATA + 0xa00      # input constants
FP_EXACT = DATA + 0xc00    # results compared bit-exactly
FP_APPROX = DATA + 0xd00   # results compared with a relative tolerance
FP_CW = DATA + 0xbf0


def fp_value(r):
    c = r.random()
    if c < 0.15:
        return r.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0, 10.0, 1e10, -1e-10, 123456.789])
    if c < 0.5:
        return float(r.randint(-1000, 1000)) / r.choice([1, 2, 4, 8, 3, 7])
    return r.uniform(-1000.0, 1000.0) * 10.0 ** r.randint(-3, 3)


class FPGen:
    def __init__(self, g):
        self.g = g
        self.r = g.r
        self.depth = 0
        self.vals = []           # python mirror of the stack (approximate)
        self.nsrc = 0
        self.approx = False
        self.nout = 0

    def emit(self, x):
        self.g.emit(x)

    def const(self, v, size=8):
        addr = FP_SRC + 8 * (self.nsrc % 48)
        self.nsrc += 1
        if size == 8:
            lo, hi = struct.unpack("<II", struct.pack("<d", v))
            self.emit("mov dword [0x%x], 0x%x" % (addr, lo))
            self.emit("mov dword [0x%x], 0x%x" % (addr + 4, hi))
        else:
            self.emit("mov dword [0x%x], 0x%x" % (addr, struct.unpack("<I", struct.pack("<f", v))[0]))
        return addr

    def push_val(self, v):
        self.depth += 1
        self.vals.insert(0, v)

    def pop_val(self):
        self.depth -= 1
        return self.vals.pop(0)

    def load(self):
        r = self.r
        k = r.randint(0, 6)
        v = fp_value(r)
        if k == 0:
            self.emit("fld qword [0x%x]" % self.const(v))
        elif k == 1:
            v = struct.unpack("<f", struct.pack("<f", v))[0]
            self.emit("fld dword [0x%x]" % self.const(v, 4))
        elif k == 2:
            iv = r.randint(-100000, 100000)
            a = self.const(0.0)
            self.emit("mov dword [0x%x], %d" % (a, iv))
            self.emit("fild dword [0x%x]" % a)
            v = float(iv)
        elif k == 3:
            iv = r.randint(-30000, 30000)
            a = self.const(0.0)
            self.emit("mov word [0x%x], %d" % (a, iv & 0xffff))
            self.emit("fild word [0x%x]" % a)
            v = float(iv)
        elif k == 4:
            iv = r.randint(-(1 << 52), 1 << 52)
            a = self.const(0.0)
            self.emit("mov dword [0x%x], 0x%x" % (a, iv & 0xffffffff))
            self.emit("mov dword [0x%x], 0x%x" % (a + 4, (iv >> 32) & 0xffffffff))
            self.emit("fild qword [0x%x]" % a)
            v = float(iv)
        elif k == 5:
            op, v = r.choice([("fld1", 1.0), ("fldz", 0.0), ("fldpi", 3.14159265358979), ("fldl2e", 1.4427),
                              ("fldln2", 0.693), ("fldlg2", 0.30103), ("fldl2t", 3.3219)])
            self.emit(op)
            if op not in ("fld1", "fldz"):
                # the x87 constant ROM holds 64-bit mantissas; doubles lose the last bits
                self.approx = True
        else:
            if self.depth == 0:
                return self.load()
            i = r.randint(0, self.depth - 1)
            self.emit("fld st%d" % i)
            v = self.vals[i]
        self.push_val(v)

    def arith(self):
        r = self.r
        op = r.choice(["fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr"])
        form = r.randint(0, 4)
        if form <= 1 or self.depth < 2:
            v = fp_value(r)
            if op in ("fdiv", "fdivr") and v == 0:
                v = 3.0
            if form == 0:
                self.emit("%s qword [0x%x]" % (op, self.const(v)))
            else:
                v = struct.unpack("<f", struct.pack("<f", v))[0]
                self.emit("%s dword [0x%x]" % (op, self.const(v, 4)))
        elif form == 2:
            i = r.randint(0, self.depth - 1)
            if i == 0 and op in ("fdiv", "fdivr"):
                op = "fmul"                     # 0/0 gives x87's negative default NaN
            self.emit("%s st0, st%d" % (op, i))
        elif form == 3:
            i = r.randint(0, self.depth - 1)
            self.emit("%s st%d, st0" % (op, i))
        else:
            i = r.randint(1, self.depth - 1)
            self.emit("%sp st%d, st0" % (op, i))
            self.pop_val()

    def compare(self):
        r = self.r
        g = self.g
        k = r.randint(0, 3)
        if self.depth < 2:
            self.load()
        i = r.randint(1, self.depth - 1)
        if k == 0:
            op = r.choice(["fcomi", "fucomi"])
            if r.random() < 0.5 and self.depth > 2:
                op += "p"
                self.emit("%s st0, st%d" % (op, i))
                self.pop_val()
            else:
                self.emit("%s st0, st%d" % (op, i))
            g.write_flags(ALL)
            c = r.choice(["b", "ae", "e", "ne", "be", "a", "p", "np"])
            if r.random() < 0.5:
                self.emit("set%s %s" % (c, R8[r.randint(0, 3)]))
            else:
                l = g.newlabel()
                self.emit("j%s %s" % (c, l))
                self.emit("inc esi")
                self.emit("%s:" % l)
        elif k == 1:
            op = r.choice(["fcom", "fcomp", "fucom", "fucomp"])
            self.emit("%s st%d" % (op, i))
            if op.endswith("p"):
                self.pop_val()
            self.emit("fnstsw ax")
            self.emit("and ax, 0x7d00")
            g.write_flags(ALL, AF)
            if r.random() < 0.5:
                self.emit("sahf")
                c = r.choice(["b", "ae", "e", "ne", "be", "a", "p", "np"])
                self.emit("set%s dl" % c)
            else:
                self.emit("test ah, 0x%x" % r.choice([0x41, 0x05, 0x44, 0x01, 0x40, 0x45]))
                c = r.choice(["e", "ne", "p", "np"])
                l = g.newlabel()
                self.emit("j%s %s" % (c, l))
                self.emit("inc edi")
                self.emit("%s:" % l)
        elif k == 2:
            v = fp_value(r)
            self.emit("%s qword [0x%x]" % (r.choice(["fcom", "fcomp"]), self.const(v)))
            if self.g.lines[-1].startswith("fcomp"):
                self.pop_val()
            self.emit("fnstsw ax")
            self.emit("and ax, 0x7d00")
        else:
            # fcmov after a compare
            self.emit("fucomi st0, st%d" % i)
            g.write_flags(ALL)
            c = r.choice(["b", "e", "be", "u", "nb", "ne", "nbe", "nu"])
            self.emit("fcmov%s st0, st%d" % (c, r.randint(1, self.depth - 1)))

    def unary(self):
        r = self.r
        op = r.choice(["fchs", "fabs", "fsqrt", "frndint", "fxch"])
        if op == "fsqrt":
            self.emit("fabs")
        if op == "fxch":
            if self.depth < 2:
                return
            i = r.randint(1, self.depth - 1)
            self.emit("fxch st%d" % i)
            self.vals[0], self.vals[i] = self.vals[i], self.vals[0]
            return
        self.emit(op)

    def fist(self):
        r = self.r
        rc = r.randint(0, 3)
        self.emit("mov word [0x%x], 0x%x" % (FP_CW, 0x27f | rc << 10))
        self.emit("fldcw [0x%x]" % FP_CW)
        size = r.choice([2, 4, 8])
        out = FP_EXACT + 0x80 + 8 * (self.nout % 16)
        self.nout += 1
        kw = {2: "word", 4: "dword", 8: "qword"}[size]
        # keep values in range: scale down first
        self.emit("fld qword [0x%x]" % self.const(0.001 if size == 2 else 1.0))
        self.emit("fmulp st1, st0")
        if r.random() < 0.5 and size != 8:      # no fist m64, only fistp
            self.emit("fist %s [0x%x]" % (kw, out))
        else:
            self.emit("fistp %s [0x%x]" % (kw, out))
            self.pop_val()
        self.emit("mov word [0x%x], 0x27f" % FP_CW)
        self.emit("fldcw [0x%x]" % FP_CW)

    def transcendental(self):
        r = self.r
        op = r.choice(["fsin", "fcos", "fptan", "fpatan", "f2xm1", "fyl2x", "fscale", "fsincos",
                       "fprem", "fprem1", "fxtract", "fyl2xp1"])
        self.approx = True
        if op in ("fsin", "fcos", "fsincos", "fptan"):
            self.emit("fld qword [0x%x]" % self.const(r.uniform(-3.0, 3.0)))
            self.push_val(0)
            self.emit(op)
            if op in ("fsincos", "fptan"):
                self.push_val(0)
        elif op == "f2xm1":
            self.emit("fld qword [0x%x]" % self.const(r.uniform(-0.99, 0.99)))
            self.push_val(0)
            self.emit(op)
        elif op in ("fyl2x", "fyl2xp1"):
            self.emit("fld qword [0x%x]" % self.const(r.uniform(-50, 50)))
            self.emit("fld qword [0x%x]" % self.const(r.uniform(0.01, 0.25) if op == "fyl2xp1" else r.uniform(0.1, 1000)))
            self.emit(op)
            self.push_val(0)
        elif op == "fpatan":
            self.emit("fld qword [0x%x]" % self.const(r.uniform(-50, 50)))
            self.emit("fld qword [0x%x]" % self.const(r.uniform(-50, 50)))
            self.emit(op)
            self.push_val(0)
        elif op == "fscale":
            self.emit("fld qword [0x%x]" % self.const(float(r.randint(-20, 20))))
            self.emit("fld qword [0x%x]" % self.const(fp_value(r)))
            self.emit(op)
            self.emit("fstp st1")
            self.push_val(0)
        elif op in ("fprem", "fprem1"):
            self.emit("fld qword [0x%x]" % self.const(r.uniform(0.5, 20) * r.choice([1, -1])))
            self.emit("fld qword [0x%x]" % self.const(r.uniform(-1000, 1000)))
            self.emit(op)
            self.emit("fnstsw ax")              # before fstp, which rewrites C1
            self.emit("and ax, 0x4700")
            self.emit("fstp st1")
            self.push_val(0)
        else:
            self.emit("fld qword [0x%x]" % self.const(fp_value(r) or 2.0))
            self.push_val(0)
            self.emit(op)
            self.push_val(0)

    def finish(self):
        base = FP_APPROX if self.approx else FP_EXACT
        k = 0
        while self.depth > 0:
            if self.r.random() < 0.2 and not self.approx:
                self.emit("fst dword [0x%x]" % (base + 0x40 + 4 * k))
            self.emit("fstp qword [0x%x]" % (base + 8 * k))
            self.pop_val()
            k += 1


def g_x87(g):
    f = FPGen(g)
    r = g.r
    g.emit("mov word [0x%x], 0x27f" % FP_CW)
    g.emit("fldcw [0x%x]" % FP_CW)
    for _ in range(r.randint(1, 3)):
        f.load()
    for _ in range(r.randint(2, 8)):
        k = r.random()
        if f.depth >= 6:
            g.emit("fstp qword [0x%x]" % (FP_EXACT + 0x100 + 8 * (f.nout % 16)))
            f.nout += 1
            f.pop_val()
        elif k < 0.25:
            f.load()
        elif k < 0.55 and f.depth > 0:
            f.arith()
        elif k < 0.7 and f.depth > 0:
            f.compare()
        elif k < 0.82 and f.depth > 0:
            f.unary()
        elif k < 0.92 and f.depth > 0 and not f.approx:
            f.fist()
        elif f.depth < 4:
            f.transcendental()
    f.finish()


def g_x87_misc(g):
    """state save/restore and control/status words"""
    r = g.r
    k = r.randint(0, 3)
    g.emit("fninit")
    g.emit("fld qword [0x%x]" % (FP_SRC))
    g.emit("fld1")
    if k == 0:
        g.emit("fnstcw [0x%x]" % (DATA + 0xb00))
        g.emit("fnstsw [0x%x]" % (DATA + 0xb04))
        g.emit("and word [0x%x], 0x7d00" % (DATA + 0xb04))
    elif k == 1:
        g.emit("fnsave [0x%x]" % (DATA + 0x100))
        g.emit("fld1")
        g.emit("frstor [0x%x]" % (DATA + 0x100))
        g.emit("mov dword [0x%x], 0" % (DATA + 0x100 + 12))   # FIP/FDP differ by design
        g.emit("mov dword [0x%x], 0" % (DATA + 0x100 + 16))
        g.emit("mov dword [0x%x], 0" % (DATA + 0x100 + 20))
        g.emit("mov dword [0x%x], 0" % (DATA + 0x100 + 24))
        g.emit("and word [0x%x], 0x7d00" % (DATA + 0x100 + 4))
        g.emit("mov word [0x%x], 0" % (DATA + 0x100 + 8))     # tag word is not tracked
    elif k == 2:
        g.emit("fstp tword [0x%x]" % (DATA + 0x200))
        g.emit("fld tword [0x%x]" % (DATA + 0x200))
    else:
        g.emit("fbstp [0x%x]" % (DATA + 0x300))
        g.emit("fbld [0x%x]" % (DATA + 0x300))
    g.emit("fstp qword [0x%x]" % (FP_EXACT + 0x180))
    g.emit("fstp qword [0x%x]" % (FP_EXACT + 0x188))


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
FP_GROUPS = {"x87": (g_x87, 4), "x87misc": (g_x87_misc, 1)}

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
ALL_GROUPS.update(FP_GROUPS)


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
