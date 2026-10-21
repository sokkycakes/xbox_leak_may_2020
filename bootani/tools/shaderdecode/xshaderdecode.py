#!/usr/bin/env python3
"""
xshaderdecode.py - decode precompiled original-Xbox (NV2A) shaders embedded as
byte arrays in a C header (e.g. ani2/shaders.h), and optionally translate them
to GLSL 330 core.

  *_xvu : Xbox vertex shader microcode (xsasm output)
            WORD  0x2078 (ordinary) / 0x7778 (read-write) / 0x7378 (state shader)
            WORD  instruction count
            count * { DWORD 0 (pad), DWORD Y, DWORD Z, DWORD W }   (NV2A "kelvin" VP words)
  *_xpu : D3DPIXELSHADERDEF_FILE  ("PSB0" + D3DPIXELSHADERDEF, 60 DWORDs)

Authoritative references in the leak (xbox trunk/xbox/private/windows/directx/dxg):
  xgraphics/shadeasm/microcodeformat.h  - D3DVsInstruction bitfields + PGM_UWORD{Y,Z,W}
  xgraphics/shadeasm/api.cpp            - InstructionDisassembler, ConvertMicrocodeToVsInstructions
  xgraphics/shadeasm/pixelshader.cpp    - xps -> D3DPIXELSHADERDEF assembler
  d3d8/se/pshader.cpp, d3d8/se/lazy.cpp - how the def is loaded into the NV2A combiners
  public/xdk/inc/d3d8types.h            - D3DPIXELSHADERDEF, PS_* enums

Usage:
  xshaderdecode.py HEADER                       # list arrays
  xshaderdecode.py HEADER NAME [NAME...]        # disassemble arrays (NAME without g_/_xvu is ok)
  xshaderdecode.py HEADER NAME --glsl           # also emit GLSL
  xshaderdecode.py HEADER --all                 # disassemble everything
"""
import re
import struct
import sys

# ----------------------------------------------------------------------------
# header parsing
# ----------------------------------------------------------------------------

def parse_header(path):
    src = open(path, encoding="latin-1").read()
    arrays = {}
    for m in re.finditer(r"const\s+BYTE\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", src, re.S):
        arrays[m.group(1)] = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})", m.group(2)))
    return arrays

# ----------------------------------------------------------------------------
# vertex shader
# ----------------------------------------------------------------------------

MAC_OPS = ["nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4", "dst",
           "min", "max", "slt", "sge", "arl", "??e", "??f"]
ILU_OPS = ["nop", "mov", "rcp", "rcc", "rsq", "expp", "logp", "lit"]
MAC_ARGS = [0, 1, 3, 5, 7, 3, 3, 3, 3, 3, 3, 3, 3, 1, 0, 0]   # bit0=a bit1=b bit2=c
OUT_NAMES = ["oPos", "o1?", "o2?", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
             "oT0", "oT1", "oT2", "oT3", "???"]
MASKS = [".null", ".w", ".z", ".zw", ".y", ".yw", ".yz", ".yzw", ".x", ".xw",
         ".xz", ".xzw", ".xy", ".xyw", ".xyz", ""]
SWZ = "xyzw"
VS_MAGIC = {0x2078: "xvs", 0x7778: "xvsw", 0x7378: "xvss"}


class VsInstr:
    FIELDS = ("eos cin om oc owm swm rw rwm cmx crr cws czs cys cxs cne bmx brr bws bzs bys bxs bne "
              "amx arr aws azs ays axs ane va ca mac ilu").split()

    def __init__(self, y, z, w):
        self.raw = (y, z, w)
        b = lambda v, s, n: (v >> s) & ((1 << n) - 1)
        # PGM_UWORDW
        self.eos, self.cin, self.om = b(w, 0, 1), b(w, 1, 1), b(w, 2, 1)
        self.oc, self.owm, self.swm = b(w, 3, 9), b(w, 12, 4), b(w, 16, 4)
        self.rw, self.rwm, self.cmx = b(w, 20, 4), b(w, 24, 4), b(w, 28, 2)
        self.crr = b(w, 30, 2) | (b(z, 0, 2) << 2)
        # PGM_UWORDZ
        self.cws, self.czs, self.cys, self.cxs = b(z, 2, 2), b(z, 4, 2), b(z, 6, 2), b(z, 8, 2)
        self.cne, self.bmx, self.brr = b(z, 10, 1), b(z, 11, 2), b(z, 13, 4)
        self.bws, self.bzs, self.bys, self.bxs = b(z, 17, 2), b(z, 19, 2), b(z, 21, 2), b(z, 23, 2)
        self.bne, self.amx, self.arr = b(z, 25, 1), b(z, 26, 2), b(z, 28, 4)
        # PGM_UWORDY
        self.aws, self.azs, self.ays, self.axs = b(y, 0, 2), b(y, 2, 2), b(y, 4, 2), b(y, 6, 2)
        self.ane, self.va, self.ca = b(y, 8, 1), b(y, 9, 4), b(y, 13, 8)
        self.mac, self.ilu = b(y, 21, 4), b(y, 25, 3)

    # operand description: (kind, index, swizzle[4], negate)
    def operand(self, which, expand_x=False):
        mx, rr, ws, zs, ys, xs, ne = [getattr(self, which + f) for f in
                                      ("mx", "rr", "ws", "zs", "ys", "xs", "ne")]
        sw = [xs, ys, zs, ws]
        if expand_x:
            sw = [xs] * 4
        if mx == 1:
            return ("r", rr, sw, ne)
        if mx == 2:
            return ("v", self.va, sw, ne)
        if mx == 3:
            return ("c", self.ca - 96, sw, ne, bool(self.cin))
        return ("?", 0, sw, ne)

    def outputs(self):
        """List of (unit, kind, index, mask). unit in {'mac','ilu'}, kind in {'r','o','c','a'}."""
        res = []
        if self.mac and (self.rwm or self.mac == 13):
            if self.mac == 13:
                res.append(("mac", "a", 0, 8))
            else:
                res.append(("mac", "r", self.rw, self.rwm))
        if self.mac and self.owm and self.om == 0:
            res.append(("mac",) + self.oreg())
        if self.ilu and self.swm:
            res.append(("ilu", "r", 1 if self.mac else self.rw, self.swm))
        if self.ilu and self.owm and self.om == 1:
            res.append(("ilu",) + self.oreg())
        return res

    def oreg(self):
        if self.oc & 0x100:
            return ("o", min(self.oc & 0xff, 13), self.owm)
        return ("c", (self.oc & 0xff) - 96, self.owm)

    def args(self, unit):
        if unit == "ilu":
            if not self.ilu:
                return []
            return [self.operand("c", expand_x=self.ilu not in (1, 7))]
        m = MAC_ARGS[self.mac]
        return [self.operand(x) for i, x in enumerate("abc") if m & (1 << i)]


def fmt_operand(op):
    kind, idx, sw, ne = op[:4]
    s = "-" if ne else ""
    if kind == "c":
        rel = op[4]
        if rel:
            s += "c[a0.x%s]" % ("" if idx == 0 else "%+d" % idx)
        else:
            s += "c%d" % idx
    else:
        s += "%s%d" % (kind, idx)
    x, y, z, w = sw
    if sw == [0, 1, 2, 3]:
        pass
    elif x == y == z == w:
        s += "." + SWZ[x]
    elif y == z == w:
        s += "." + SWZ[x] + SWZ[y]
    elif z == w:
        s += "." + SWZ[x] + SWZ[y] + SWZ[z]
    else:
        s += "." + "".join(SWZ[c] for c in sw)
    return s


def fmt_dest(kind, idx, mask):
    if kind == "a":
        return "a0.x"
    if kind == "o":
        return OUT_NAMES[idx] + MASKS[mask]
    if kind == "c":
        return "c%d%s" % (idx, MASKS[mask])
    return "r%d%s" % (idx, MASKS[mask])


def is_viewport_part(ins, unit):
    """True for the xsasm/D3D screen-space postfix (reads c-38/c-37 into oPos, or rcc of r12.w)."""
    if unit == "mac":
        if ins.mac and ins.om == 0 and ins.owm and (ins.oc & 0x100) and (ins.oc & 0xff) == 0:
            for a in ins.args("mac"):
                if a[0] == "c" and a[1] in (-38, -37):
                    return True
        return False
    if unit == "ilu":
        if ins.ilu == 3:  # rcc
            a = ins.args("ilu")[0]
            return a[0] == "r" and a[1] == 12 and a[2][0] == 3
    return False


def decode_vs(data):
    magic, count = struct.unpack_from("<HH", data, 0)
    if magic not in VS_MAGIC:
        raise ValueError("not an xvu (magic %04x)" % magic)
    if 4 + 16 * count != len(data):
        raise ValueError("size mismatch: %d instrs, %d bytes" % (count, len(data)))
    ins = []
    for i in range(count):
        x, y, z, w = struct.unpack_from("<4I", data, 4 + 16 * i)
        ins.append(VsInstr(y, z, w))
    return magic, ins


def disasm_vs(data, name=""):
    magic, ins = decode_vs(data)
    out = []
    out.append("; %s  (magic 0x%04x, %d microcode instructions)" % (name, magic, len(ins)))
    out.append("; each line = one NV2A instruction; '+' marks a co-issued op (MAC and ILU pair,")
    out.append("; or MAC writing both a temp and an output). r12 reads = oPos (read-only alias).")
    out.append("; [vp] = D3D screen-space postfix (c-38 = viewport scale, c-37 = viewport offset)")
    out.append("xvs.1.1")
    for n, I in enumerate(ins):
        parts = []
        for unit, kind, idx, mask in I.outputs():
            op = ILU_OPS[I.ilu] if unit == "ilu" else MAC_OPS[I.mac]
            args = ", ".join(fmt_operand(a) for a in I.args(unit))
            txt = "%s %s, %s" % (op, fmt_dest(kind, idx, mask), args)
            if is_viewport_part(I, unit):
                txt += "   ; [vp]"
            parts.append(txt)
        if not parts:
            parts = ["nop"]
        raw = "%08x %08x %08x" % I.raw
        for k, p in enumerate(parts):
            prefix = "    " if k == 0 else "  + "
            out.append("%s%-48s ; %s" % (prefix, p, ("#%02d  %s" % (n, raw)) if k == 0 else ""))
        if I.eos:
            out.append("    ; end")
    return "\n".join(out) + "\n"


# ---- VS -> GLSL -------------------------------------------------------------

GLSL_OUT = {0: "oPos", 3: "vD0", 4: "vD1", 5: "oFog", 6: "oPts", 7: "oB0", 8: "oB1",
            9: "vT0", 10: "vT1", 11: "vT2", 12: "vT3"}


def glsl_operand(op):
    kind, idx, sw, ne = op[:4]
    if kind == "c":
        base = "c[int(a0.x)%s]" % ("" if idx == 0 else "%+d" % idx) if op[4] else "c[%d]" % idx
    elif kind == "r":
        base = "oPos" if idx == 12 else "r%d" % idx
    else:
        base = "v%d" % idx
    if sw != [0, 1, 2, 3]:
        base += "." + "".join(SWZ[c] for c in sw)
    return ("-" if ne else "") + base


def glsl_mac(op, a):
    A = a[0] if a else None
    B = a[1] if len(a) > 1 else None
    if op == "mov": return A
    if op == "mul": return "%s * %s" % (A, B)
    if op == "add": return "%s + %s" % (A, a[1])      # add uses A and C
    if op == "mad": return "%s * %s + %s" % (A, B, a[2])
    if op == "dp3": return "vec4(dot((%s).xyz, (%s).xyz))" % (A, B)
    if op == "dph": return "vec4(dot((%s).xyz, (%s).xyz) + (%s).w)" % (A, B, B)
    if op == "dp4": return "vec4(dot(%s, %s))" % (A, B)
    if op == "dst": return "vec4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)" % (A, B, A, B)
    if op == "min": return "min(%s, %s)" % (A, B)
    if op == "max": return "max(%s, %s)" % (A, B)
    if op == "slt": return "vec4(lessThan(%s, %s))" % (A, B)
    if op == "sge": return "vec4(greaterThanEqual(%s, %s))" % (A, B)
    if op == "arl": return "vec4(floor((%s).x))" % A
    raise ValueError(op)


def glsl_ilu(op, c):
    x = "(%s).x" % c
    if op == "mov": return c
    if op == "rcp": return "vec4(1.0 / %s)" % x
    if op == "rcc": return "vec4(xb_rcc(%s))" % x
    if op == "rsq": return "vec4(inversesqrt(abs(%s)))" % x
    if op == "expp": return "xb_expp(%s)" % x
    if op == "logp": return "xb_logp(%s)" % x
    if op == "lit": return "xb_lit(%s)" % c
    raise ValueError(op)


def mask_swz(mask):
    return "".join(c for bit, c in ((8, "x"), (4, "y"), (2, "z"), (1, "w")) if mask & bit)


def vs_to_glsl(data, name=""):
    magic, ins = decode_vs(data)
    used_v = set()
    used_r = set()
    body = []
    for n, I in enumerate(ins):
        results = {}
        pre = []
        for unit in ("mac", "ilu"):
            if (unit == "mac" and not I.mac) or (unit == "ilu" and not I.ilu):
                continue
            if is_viewport_part(I, unit):
                continue
            args = I.args(unit)
            for a in args:
                if a[0] == "v": used_v.add(a[1])
                if a[0] == "r" and a[1] != 12: used_r.add(a[1])
            ga = [glsl_operand(a) for a in args]
            expr = glsl_mac(MAC_OPS[I.mac], ga) if unit == "mac" else glsl_ilu(ILU_OPS[I.ilu], ga[0])
            tmp = "t%s%d" % (unit[0], n)
            pre.append("    vec4 %s = %s;" % (tmp, expr))
            results[unit] = tmp
        if not results:
            continue
        body.append("    // #%02d" % n)
        body.extend(pre)
        for unit, kind, idx, mask in I.outputs():
            if unit not in results:
                continue
            m = mask_swz(mask)
            src = results[unit]
            if kind == "a":
                dst = "a0.x"; m = "x"
                body.append("    a0.x = %s.x;" % src)
                continue
            if kind == "r":
                used_r.add(idx)
                dst = "r%d" % idx
            elif kind == "o":
                dst = GLSL_OUT.get(idx, "o_unknown%d" % idx)
            else:
                dst = "c_write%d" % idx  # read/write shaders only
            if m == "xyzw":
                body.append("    %s = %s;" % (dst, src))
            else:
                body.append("    %s.%s = %s.%s;" % (dst, m, src, m))
    head = ["#version 330 core",
            "// vertex shader translated from %s by xshaderdecode.py" % name,
            "// (D3D screen-space postfix omitted: gl_Position = clip-space oPos, D3D z range)"]
    for v in sorted(used_v | {0, 1}):
        head.append("layout(location=%d) in vec4 v%d;" % (v, v))
    head += ["uniform vec4 c[96];",
             "out vec4 vD0; out vec4 vD1; out vec4 vT0; out vec4 vT1; out vec4 vT2; out vec4 vT3;",
             "",
             "float xb_rcc(float x) { float r = 1.0 / x; float a = clamp(abs(r), 5.42101e-20, 1.884467e19); return r < 0.0 ? -a : a; }",
             "vec4 xb_expp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }",
             "vec4 xb_logp(float x) { float a = abs(x); float e = floor(log2(a)); return vec4(e, a / exp2(e), log2(a), 1.0); }",
             "vec4 xb_lit(vec4 s) { float d = max(s.x, 0.0), n = max(s.y, 0.0), p = clamp(s.w, -127.9961, 127.9961);",
             "    return vec4(1.0, d, (s.x > 0.0) ? pow(n, p) : 0.0, 1.0); }",
             "",
             "void main()",
             "{",
             "    vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);",
             "    vec4 a0 = vec4(0.0);",
             "    vD0 = vec4(0.0); vD1 = vec4(0.0);",
             "    vT0 = vec4(0.0, 0.0, 0.0, 1.0); vT1 = vec4(0.0, 0.0, 0.0, 1.0);",
             "    vT2 = vec4(0.0, 0.0, 0.0, 1.0); vT3 = vec4(0.0, 0.0, 0.0, 1.0);",
             "    vec4 oFog = vec4(0.0), oPts = vec4(0.0), oB0 = vec4(0.0), oB1 = vec4(0.0);"]
    for r in sorted(used_r):
        head.append("    vec4 r%d = vec4(0.0);" % r)
    tail = ["    gl_Position = oPos;", "}"]
    return "\n".join(head + body + tail) + "\n"


# ----------------------------------------------------------------------------
# pixel shader (D3DPIXELSHADERDEF)
# ----------------------------------------------------------------------------

PSD_FIELDS = ([("PSAlphaInputs", 8), ("PSFinalCombinerInputsABCD", 1), ("PSFinalCombinerInputsEFG", 1),
               ("PSConstant0", 8), ("PSConstant1", 8), ("PSAlphaOutputs", 8), ("PSRGBInputs", 8),
               ("PSCompareMode", 1), ("PSFinalCombinerConstant0", 1), ("PSFinalCombinerConstant1", 1),
               ("PSRGBOutputs", 8), ("PSCombinerCount", 1), ("PSTextureModes", 1), ("PSDotMapping", 1),
               ("PSInputTexture", 1), ("PSC0Mapping", 1), ("PSC1Mapping", 1),
               ("PSFinalCombinerConstants", 1)])

PS_REG = {0: "zero", 1: "c0", 2: "c1", 3: "fog", 4: "v0", 5: "v1", 6: "?6", 7: "?7",
          8: "t0", 9: "t1", 10: "t2", 11: "t3", 12: "r0", 13: "r1", 14: "v1r0", 15: "prod"}
TEXMODES = ["NONE", "PROJECT2D", "PROJECT3D", "CUBEMAP", "PASSTHRU", "CLIPPLANE", "BUMPENVMAP",
            "BUMPENVMAP_LUM", "BRDF", "DOT_ST", "DOT_ZW", "DOT_RFLCT_DIFF", "DOT_RFLCT_SPEC",
            "DOT_STR_3D", "DOT_STR_CUBE", "DPNDNT_AR", "DPNDNT_GB", "DOTPRODUCT", "DOT_RFLCT_SPEC_CONST"]
TEXOPS = {1: "tex", 2: "tex", 3: "tex", 4: "texcoord", 5: "texkill", 6: "texbem", 7: "texbeml",
          8: "texbrdf", 9: "texm3x2tex", 10: "texm3x2depth", 11: "texm3x3diff", 12: "texm3x3vspec",
          13: "texm3x3tex", 14: "texm3x3tex", 15: "texreg2ar", 16: "texreg2gb", 17: "texdp3/texm3x2pad",
          18: "texm3x3spec"}
OUTMAP = {0: "", 1: "_bias", 2: "_x2", 3: "_bx2", 4: "_x4", 6: "_d2"}


def parse_psd(data):
    fid = struct.unpack_from("<I", data, 0)[0]
    if fid != 0x30425350:
        raise ValueError("not a PSB0 file")
    vals = struct.unpack_from("<60I", data, 4)
    psd, i = {}, 0
    for name, n in PSD_FIELDS:
        psd[name] = list(vals[i:i + n]) if n > 1 else vals[i]
        i += n
    return psd


def split_inputs(v):
    return [(v >> s) & 0xff for s in (24, 16, 8, 0)]   # A B C D


class PsOperand:
    def __init__(self, byte, alpha_portion, stage):
        self.reg = byte & 0x0f
        self.alpha_ch = bool(byte & 0x10)
        self.map = (byte >> 5) & 7
        self.alpha_portion = alpha_portion
        self.stage = stage

    def text(self, const_name=None):
        r, m = self.reg, self.map
        if r == 0:
            return {0: "zero", 1: "1", 2: "-1", 3: "1", 4: "-0.5", 5: "0.5", 6: "zero", 7: "zero"}[m]
        name = PS_REG[r]
        if const_name and r in (1, 2):
            name = const_name(r)
        if self.alpha_portion:
            ch = "" if self.alpha_ch else ".b"
        else:
            ch = ".a" if self.alpha_ch else ""
        n = name + ch
        return {0: n + "_sat", 1: "1-" + n, 2: n + "_bx2", 3: "-" + n + "_bx2", 4: n + "_bias",
                5: "-" + n + "_bias", 6: n, 7: "-" + n}[m]


def disasm_ps(data, name=""):
    psd = parse_psd(data)
    out = ["; %s  (D3DPIXELSHADERDEF_FILE)" % name]
    cc = psd["PSCombinerCount"]
    nst = cc & 0xf
    out.append("; PSCombinerCount = 0x%08x : %d stage(s), mux on r0.a %s, %s C0, %s C1" % (
        cc, nst, "MSB" if cc & 0x1 << 8 else "LSB", "unique" if cc & 0x10 << 8 else "same",
        "unique" if cc & 0x100 << 8 else "same"))
    tm = psd["PSTextureModes"]
    modes = [(tm >> (5 * s)) & 0x1f for s in range(4)]
    out.append("; PSTextureModes = 0x%08x : %s" % (tm, ", ".join(
        "t%d=%s" % (s, TEXMODES[m] if m < len(TEXMODES) else hex(m)) for s, m in enumerate(modes))))
    out.append("; PSDotMapping = 0x%08x  PSInputTexture = 0x%08x  PSCompareMode = 0x%08x" % (
        psd["PSDotMapping"], psd["PSInputTexture"], psd["PSCompareMode"]))
    c0m, c1m, fcm = psd["PSC0Mapping"], psd["PSC1Mapping"], psd["PSFinalCombinerConstants"]
    out.append("; PSC0Mapping = 0x%08x  PSC1Mapping = 0x%08x  PSFinalCombinerConstants = 0x%08x" % (c0m, c1m, fcm))
    out.append("xps.1.1")
    for s, m in enumerate(modes):
        if m:
            out.append("%s t%d" % (TEXOPS.get(m, "tex?"), s))

    # constants
    def cname_for(stage):
        def f(r):
            if stage == 8:
                nib = (fcm >> (0 if r == 1 else 4)) & 0xf
            else:
                nib = ((c0m if r == 1 else c1m) >> (4 * stage)) & 0xf
            return "c%d" % nib if nib != 0xf else ("C%d_stage%s" % (r - 1, stage if stage < 8 else "F"))
        return f

    for s in range(nst):
        for r, arr, mp in ((1, psd["PSConstant0"], c0m), (2, psd["PSConstant1"], c1m)):
            nib = (mp >> (4 * s)) & 0xf
            if nib != 0xf or arr[s]:
                out.append("; stage %d C%d -> %s, embedded value 0x%08x" % (
                    s, r - 1, "D3D c%d" % nib if nib != 0xf else "unmapped", arr[s]))
    for r, v in ((0, psd["PSFinalCombinerConstant0"]), (1, psd["PSFinalCombinerConstant1"])):
        nib = (fcm >> (4 * r)) & 0xf
        if nib != 0xf or v:
            out.append("; final C%d -> %s, embedded value 0x%08x" % (r, "D3D c%d" % nib if nib != 0xf else "unmapped", v))

    for s in range(nst):
        cn = cname_for(s)
        rgb = stage_ops(psd["PSRGBInputs"][s], psd["PSRGBOutputs"][s], False, s, cn)
        alp = stage_ops(psd["PSAlphaInputs"][s], psd["PSAlphaOutputs"][s], True, s, cn)
        out.append("; ---- stage %d  rgb in %08x out %08x | alpha in %08x out %08x" % (
            s, psd["PSRGBInputs"][s], psd["PSRGBOutputs"][s], psd["PSAlphaInputs"][s], psd["PSAlphaOutputs"][s]))
        merged = merge_ops(rgb, alp)
        out.extend(merged)
    abcd, efg = psd["PSFinalCombinerInputsABCD"], psd["PSFinalCombinerInputsEFG"]
    if abcd | efg:
        cn = cname_for(8)
        A, B, C, D = [PsOperand(x, False, 8).text(cn) for x in split_inputs(abcd)]
        e, f, g, flags = split_inputs(efg)
        E, F = [PsOperand(x, False, 8).text(cn) for x in (e, f)]
        G = PsOperand(g, True, 8).text(cn)
        fl = []
        if flags & 0x80: fl.append("CLAMP_SUM")
        if flags & 0x40: fl.append("COMPLEMENT_V1")
        if flags & 0x20: fl.append("COMPLEMENT_R0")
        out.append("; final combiner: rgb = A*B + (1-A)*C + D, alpha = G  (inputs are 0..1 unsigned)")
        out.append("xfc %s, %s, %s, %s, %s, %s, %s%s" % (
            strip_sat(A), strip_sat(B), strip_sat(C), strip_sat(D), strip_sat(E), strip_sat(F), strip_sat(G),
            ("   ; flags: " + " ".join(fl)) if fl else ""))
    else:
        out.append("; no final combiner in def (ABCD=EFG=0): D3D keeps the fixed-function final combiner,")
        out.append(";   i.e. with fog and specular disabled: out.rgb = sat(r0.rgb), out.a = sat(r0.a)")
    return "\n".join(out) + "\n"


def strip_sat(s):
    return s[:-4] if s.endswith("_sat") else s


def stage_ops(inp, outp, alpha, stage, cn):
    A, B, C, D = [PsOperand(x, alpha, stage) for x in split_inputs(inp)]
    cd_reg, ab_reg, sum_reg = outp & 0xf, (outp >> 4) & 0xf, (outp >> 8) & 0xf
    cd_dot, ab_dot, mux = bool(outp & 0x1000), bool(outp & 0x2000), bool(outp & 0x4000)
    omap = (outp >> 15) & 7
    cd_b2a, ab_b2a = bool(outp & 0x40000), bool(outp & 0x80000)
    mod = OUTMAP.get(omap, "_?%d" % omap)
    mask = ".a" if alpha else ".rgb"
    t = lambda o: o.text(cn)
    ops = []

    one = lambda o: o.reg == 0 and o.map in (1, 3)
    zero = lambda o: o.reg == 0 and o.map in (0, 6, 7)

    def prod_term(x, y):
        # returns (kind, text): kind 'z' zero, 's' single operand, 'p' product
        if zero(x) or zero(y):
            return ("z", "")
        if one(y):
            return ("s", t(x))
        if one(x):
            return ("s", t(y))
        return ("p", "%s, %s" % (t(x), t(y)))

    def prod(dst, x, y, dot):
        if dot:
            return "dp3%s %s, %s, %s" % (mod, dst, t(x), t(y))
        k, txt = prod_term(x, y)
        if k == "z":
            return "mov%s %s, zero" % (mod, dst)
        return ("mov%s %s, %s" if k == "s" else "mul%s %s, %s") % (mod, dst, txt)

    if ab_reg and not sum_reg:
        d = PS_REG[ab_reg] + ("" if (ab_b2a and not alpha) else mask)
        ops.append(prod(d, A, B, ab_dot) + ("   ; AB blue->alpha" if ab_b2a else ""))
    if cd_reg and not sum_reg:
        d = PS_REG[cd_reg] + ("" if (cd_b2a and not alpha) else mask)
        ops.append(prod(d, C, D, cd_dot) + ("   ; CD blue->alpha" if cd_b2a else ""))
    if sum_reg:
        dst = PS_REG[sum_reg] + mask
        if ab_reg or cd_reg or ab_dot or cd_dot:
            ops.append("%s%s %s, %s, %s, %s, %s, %s, %s" % (
                "xmmc" if mux else "xmma", mod,
                PS_REG[ab_reg] + mask if ab_reg else "discard",
                PS_REG[cd_reg] + mask if cd_reg else "discard", dst, t(A), t(B), t(C), t(D)))
        elif mux:
            ops.append("xmmc%s discard, discard, %s, %s, %s, %s, %s   ; r0.a msb ? C*D : A*B" % (
                mod, dst, t(A), t(B), t(C), t(D)))
        else:
            k1, p1 = prod_term(A, B)
            k2, p2 = prod_term(C, D)
            if k1 == "z" and k2 == "z":
                ops.append("mov%s %s, zero" % (mod, dst))
            elif k1 == "z" or k2 == "z":
                k, pp = (k2, p2) if k1 == "z" else (k1, p1)
                ops.append(("mov%s %s, %s" if k == "s" else "mul%s %s, %s") % (mod, dst, pp))
            elif k1 == "s" and k2 == "s":
                ops.append("add%s %s, %s, %s" % (mod, dst, p1, p2))
            elif k1 == "p" and k2 == "s":
                ops.append("mad%s %s, %s, %s" % (mod, dst, p1, p2))
            elif k1 == "s" and k2 == "p":
                ops.append("mad%s %s, %s, %s" % (mod, dst, p2, p1))
            elif t(C) == "1-" + t(A) or t(A) == "1-" + t(C):
                ops.append("lrp%s %s, %s, %s, %s" % (mod, dst, t(A), t(B), t(D)) if t(C) == "1-" + t(A)
                           else "lrp%s %s, %s, %s, %s" % (mod, dst, t(C), t(D), t(B)))
            else:
                ops.append("xmma%s discard, discard, %s, %s, %s, %s, %s" % (mod, dst, t(A), t(B), t(C), t(D)))
    return ops


def merge_ops(rgb, alp):
    # try to merge "op r.rgb, x, y" with "op r.a, x', y'" into one full-mask instruction
    out = []
    if len(rgb) == 1 and len(alp) == 1:
        r, a = rgb[0], alp[0]
        rn = r.replace(".rgb", "", 1)
        an = a.replace(".a", "", 1)
        # alpha-portion operands: plain name means .a; ".b" means blue. rgb-portion ".a" means replicate alpha.
        if rn.replace(".a", "") == an and ".b" not in an:
            out.append(rn)
            return out
    out.extend(rgb)
    for i, a in enumerate(alp):
        out.append(("+" if rgb and i == 0 else "") + a)
    return out


# ---- PS -> GLSL --------------------------------------------------------------

def ps_to_glsl(data, name=""):
    psd = parse_psd(data)
    cc = psd["PSCombinerCount"]
    nst = cc & 0xf
    msb = bool(cc & 0x100)
    tm = psd["PSTextureModes"]
    modes = [(tm >> (5 * s)) & 0x1f for s in range(4)]
    c0m, c1m, fcm = psd["PSC0Mapping"], psd["PSC1Mapping"], psd["PSFinalCombinerConstants"]

    def argb(v):
        return "vec4(%.6f, %.6f, %.6f, %.6f)" % (((v >> 16) & 0xff) / 255.0, ((v >> 8) & 0xff) / 255.0,
                                                 (v & 0xff) / 255.0, ((v >> 24) & 0xff) / 255.0)

    def const(stage, r):
        if stage == 8:
            nib = (fcm >> (0 if r == 1 else 4)) & 0xf
            emb = psd["PSFinalCombinerConstant0" if r == 1 else "PSFinalCombinerConstant1"]
        else:
            nib = ((c0m if r == 1 else c1m) >> (4 * stage)) & 0xf
            emb = (psd["PSConstant0"] if r == 1 else psd["PSConstant1"])[stage]
        # mapped -> app sets it with SetPixelShaderConstant(nib); unmapped -> literal from the def
        return "pc[%d]" % nib if nib != 0xf else argb(emb)

    REGN = {3: "FOG", 4: "D0", 5: "D1", 8: "T0", 9: "T1", 10: "T2", 11: "T3", 12: "R0", 13: "R1",
            14: "V1R0", 15: "EF"}

    def reg(stage, r):
        if r == 0: return "vec4(0.0)"
        if r in (1, 2): return const(stage, r)
        return REGN.get(r, "vec4(0.0)")

    MAPF = ["xb_uid", "xb_uinv", "xb_exn", "xb_exneg", "xb_hbn", "xb_hbneg", "", "-"]

    def inp(byte, stage, alpha, final=False):
        r, ach, m = byte & 0xf, bool(byte & 0x10), (byte >> 5) & 7
        v = reg(stage, r)
        sw = ("a" if ach else "b") if alpha else ("aaa" if ach else "rgb")
        x = "%s.%s" % (v, sw)
        if final:
            m = 1 if m == 1 else 0            # final combiner: only unsigned identity / invert
            return "%s(%s)" % (["xb_fid", "xb_uinv"][m], x)
        f = MAPF[m]
        return "%s(%s)" % (f, x) if f else x

    OMAP = {0: "%s", 1: "(%s - 0.5)", 2: "(%s * 2.0)", 3: "((%s - 0.5) * 2.0)", 4: "(%s * 4.0)",
            6: "(%s * 0.5)"}
    body = []
    for s in range(nst):
        body.append("    // ---- combiner stage %d" % s)
        body.append("    {")
        body.append("        vec4 nR0 = R0, nR1 = R1, nT0 = T0, nT1 = T1, nT2 = T2, nT3 = T3, nD0 = D0, nD1 = D1;")
        post = []
        for alpha in (False, True):
            ins = (psd["PSAlphaInputs"] if alpha else psd["PSRGBInputs"])[s]
            outp = (psd["PSAlphaOutputs"] if alpha else psd["PSRGBOutputs"])[s]
            cd_reg, ab_reg, sum_reg = outp & 0xf, (outp >> 4) & 0xf, (outp >> 8) & 0xf
            if not (cd_reg or ab_reg or sum_reg):
                continue
            A, B, C, D = [inp(x, s, alpha) for x in split_inputs(ins)]
            ty, p, wm = ("float", "a", ".a") if alpha else ("vec3", "c", ".rgb")
            ab_dot = bool(outp & 0x2000) and not alpha
            cd_dot = bool(outp & 0x1000) and not alpha
            ab = "vec3(dot(%s, %s))" % (A, B) if ab_dot else "%s * %s" % (A, B)
            cd = "vec3(dot(%s, %s))" % (C, D) if cd_dot else "%s * %s" % (C, D)
            om = OMAP.get((outp >> 15) & 7, "%s")
            body.append("        %s %sab = %s;" % (ty, p, ab))
            body.append("        %s %scd = %s;" % (ty, p, cd))
            if outp & 0x4000:
                sel = "(R0.a >= 0.5)" if msb else "(mod(floor(R0.a * 255.0 + 0.5), 2.0) != 0.0)"
                body.append("        %s %ssum = %s ? %scd : %sab;   // mux" % (ty, p, sel, p, p))
            else:
                body.append("        %s %ssum = %sab + %scd;" % (ty, p, p, p))
            for nm, r in (("ab", ab_reg), ("cd", cd_reg), ("sum", sum_reg)):
                if r:
                    body.append("        n%s%s = clamp(%s, -1.0, 1.0);" % (REGN[r], wm, om % (p + nm)))
            if not alpha:
                if ab_reg and outp & 0x80000:
                    post.append("        n%s.a = clamp(%s, -1.0, 1.0).b;   // AB blue-to-alpha" % (REGN[ab_reg], om % "cab"))
                if cd_reg and outp & 0x40000:
                    post.append("        n%s.a = clamp(%s, -1.0, 1.0).b;   // CD blue-to-alpha" % (REGN[cd_reg], om % "ccd"))
        body.extend(post)
        body.append("        R0 = nR0; R1 = nR1; T0 = nT0; T1 = nT1; T2 = nT2; T3 = nT3; D0 = nD0; D1 = nD1;")
        body.append("    }")

    abcd, efg = psd["PSFinalCombinerInputsABCD"], psd["PSFinalCombinerInputsEFG"]
    fin = ["    // ---- final combiner"]
    if abcd | efg:
        e, f, g, flags = split_inputs(efg)
        fin.append("    vec3 v1s = %s;" % ("1.0 - clamp(D1.rgb, 0.0, 1.0)" if flags & 0x40 else "D1.rgb"))
        fin.append("    vec3 r0s = %s;" % ("1.0 - clamp(R0.rgb, 0.0, 1.0)" if flags & 0x20 else "R0.rgb"))
        fin.append("    V1R0 = vec4(%s, 0.0);" % ("clamp(v1s + r0s, 0.0, 1.0)" if flags & 0x80 else "v1s + r0s"))
        fin.append("    EF = vec4(%s * %s, 0.0);" % (inp(e, 8, False, True), inp(f, 8, False, True)))
        A, B, C, D = [inp(x, 8, False, True) for x in split_inputs(abcd)]
        fin.append("    vec3 fA = %s;" % A)
        fin.append("    fragColor.rgb = clamp(fA * %s + (1.0 - fA) * %s + %s, 0.0, 1.0);" % (B, C, D))
        fin.append("    fragColor.a = clamp(%s, 0.0, 1.0);" % inp(g, 8, True, True))
    else:
        fin.append("    // the def has no final combiner (ABCD = EFG = 0), so D3D keeps its fixed-function")
        fin.append("    // final combiner; with FOGENABLE and SPECULARENABLE off that is D = r0.rgb, G = r0.a.")
        fin.append("    fragColor = clamp(R0, 0.0, 1.0);")

    head = ["#version 330 core",
            "// fragment shader translated from %s by xshaderdecode.py (NV2A register combiner emulation)" % name,
            "in vec4 vD0; in vec4 vD1; in vec4 vT0; in vec4 vT1; in vec4 vT2; in vec4 vT3;"]
    if any("pc[" in l for l in body + fin):
        head.append("uniform vec4 pc[8];   // indexed by D3D pixel shader constant number (PSC0/PSC1Mapping)")
    tex = []
    for s, m in enumerate(modes):
        if m in (1, 2, 3):
            head.append("uniform sampler2D tex%d;" % s)
            tex.append("    vec4 T%d = texture(tex%d, vT%d.xy / vT%d.w);   // %s" % (s, s, s, s, TEXMODES[m]))
        elif m == 4:
            tex.append("    vec4 T%d = clamp(vT%d, 0.0, 1.0);   // PASSTHRU" % (s, s))
        elif m == 0:
            tex.append("    vec4 T%d = vec4(0.0);" % s)
        else:
            tex.append("    vec4 T%d = vec4(0.0);   // texture mode %s not emulated" % (s, TEXMODES[m]))
    helpers = [
        "layout(location=0) out vec4 fragColor;",
        "",
        "// register-combiner input mappings",
        "vec3  xb_uid(vec3 x)    { return max(x, 0.0); }             float xb_uid(float x)    { return max(x, 0.0); }",
        "vec3  xb_uinv(vec3 x)   { return 1.0 - clamp(x, 0.0, 1.0); } float xb_uinv(float x)   { return 1.0 - clamp(x, 0.0, 1.0); }",
        "vec3  xb_exn(vec3 x)    { return 2.0 * max(x, 0.0) - 1.0; } float xb_exn(float x)    { return 2.0 * max(x, 0.0) - 1.0; }",
        "vec3  xb_exneg(vec3 x)  { return 1.0 - 2.0 * max(x, 0.0); } float xb_exneg(float x)  { return 1.0 - 2.0 * max(x, 0.0); }",
        "vec3  xb_hbn(vec3 x)    { return max(x, 0.0) - 0.5; }       float xb_hbn(float x)    { return max(x, 0.0) - 0.5; }",
        "vec3  xb_hbneg(vec3 x)  { return 0.5 - max(x, 0.0); }       float xb_hbneg(float x)  { return 0.5 - max(x, 0.0); }",
        "vec3  xb_fid(vec3 x)    { return clamp(x, 0.0, 1.0); }      float xb_fid(float x)    { return clamp(x, 0.0, 1.0); }",
        "",
        "void main()",
        "{"]
    init = tex + [
        "    vec4 D0 = clamp(vD0, 0.0, 1.0);",
        "    vec4 D1 = clamp(vD1, 0.0, 1.0);",
        "    vec4 FOG = vec4(0.0);                    // fog colour / factor (fog off)",
        "    vec4 R0 = vec4(0.0, 0.0, 0.0, T0.a);     // r0.a is initialised to t0.a",
        "    vec4 R1 = vec4(0.0);",
        "    vec4 V1R0 = vec4(0.0), EF = vec4(0.0);",
    ]
    return "\n".join(head + helpers + init + body + fin + ["}"]) + "\n"


# ----------------------------------------------------------------------------

def resolve(arrays, n):
    for cand in (n, "g_" + n, "g_%s_xvu" % n, "g_%s_xpu" % n):
        if cand in arrays:
            return [cand]
    hits = [k for k in arrays if n in k]
    if not hits:
        raise SystemExit("no array matching %r" % n)
    return sorted(hits)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    arrays = parse_header(argv[1])
    names = [a for a in argv[2:] if not a.startswith("--")]
    glsl = "--glsl" in argv
    if "--all" in argv:
        names = list(arrays)
    if not names:
        for k, v in arrays.items():
            print("%-28s %4d bytes" % (k, len(v)))
        return 0
    for n in names:
        for k in resolve(arrays, n):
            d = arrays[k]
            if k.endswith("_xvu") or d[:2] in (b"\x78\x20", b"\x78\x77", b"\x78\x73"):
                print(disasm_vs(d, k))
                if glsl:
                    print(vs_to_glsl(d, k))
            else:
                print(disasm_ps(d, k))
                if glsl:
                    print(ps_to_glsl(d, k))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
