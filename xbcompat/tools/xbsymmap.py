#!/usr/bin/env python3
"""Build an xbcompat library map with XbSymbolDatabase.

The leak's own libraries are XDK 4400 (and 4361), so findsigs.py only finds
titles built with those.  XbSymbolDatabase (MIT, github.com/Cxbx-Reloaded/
XbSymbolDatabase) carries signatures for every XDK from 3911 to 5933.  This
tool runs its command-line scanner on an XBE and renames what it finds to the
decorated names xbcompat's host tables use (_D3DDevice_Clear@24,
?PlayEx@CDirectSoundBuffer@DirectSound@@..., ...).

A function is only mapped when its calling convention and stack argument
count agree with the host implementation's decorated name; the others are
listed on stderr so they can be given adapters.

usage: xbsymmap.py CLI XBE XBCOMPAT_BINARY [-o MAP]
"""
import argparse
import os
import re
import subprocess
import sys

# XbSymbolDatabase names for the data symbols the host looks up.
DATA_ALIASES = {
    "D3D_g_RenderState": ["_D3D__RenderState"],
    "D3D_g_DeferredTextureState": ["_D3D__TextureState"],
    "D3D_g_pDevice": ["?g_pDevice@D3D@@3PAVCDevice@1@A"],
    "g_DeviceType_Gamepad": ["_XDEVICE_TYPE_GAMEPAD_TABLE"],
    "g_DeviceType_MU": ["_XDEVICE_TYPE_MEMORY_UNIT_TABLE"],
}

# XbSymbolDatabase argument lists that disagree with the code (checked by the
# callee's ret): the stack bytes the function really pops.
STACK_FIX = {
    "D3DDevice_CreateTexture2": 28,     # (Width, Height, Depth, Levels, Usage, Format, Type)
    "D3DDevice_GetPixelShader": 4,      # (pHandle)
}

# Argument lists XbSymbolDatabase gets wrong: these take every argument on the
# stack (the function overwrites the register it names before reading it).
ARGS_FIX = {
    "D3DDevice_DrawIndexedVertices": "psh PrimitiveType, psh VertexCount, psh pIndexData",
    "D3DDevice_DrawIndexedVerticesUP": "psh PrimitiveType, psh VertexCount, psh pIndexData, "
                                       "psh pVertexStreamZeroData, psh VertexStreamZeroStride",
}

LINE = re.compile(r"^(\w+?)__(FUN|VAR)__(?:(\w+?)__)?(\w+)(?:\((.*)\))? = (0x[0-9a-fA-F]+)$")


def xbe_reader(path):
    """read(va, n): bytes of the XBE image at a virtual address."""
    import struct
    d = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    base, nsec, sh = u32(0x104), u32(0x11C), u32(0x120)
    secs = [struct.unpack_from("<6I", d, sh - base + i * 56) for i in range(nsec)]

    def read(va, n):
        for _, sva, _, raw, rs, _ in secs:
            if sva <= va < sva + rs:
                return d[raw + va - sva: raw + min(va - sva + n, rs)]
        return b""
    return read


def index_data_global(read, set_indices):
    """D3D__IndexData, which the inlined DrawIndexedPrimitive reads, from the
    D3DDevice_SetIndices that writes it: "mov [g], eax ... mov dword [g], 0"."""
    code = read(set_indices, 0x60)
    for m in re.finditer(rb"\xc7\x05(....)\x00\x00\x00\x00", code, re.S):
        if b"\xa3" + m.group(1) in code:
            return int.from_bytes(m.group(1), "little")
    return None


def host_names(binary):
    """Decorated library names compiled into the xbcompat binary's HLE tables
    (string literals; some are pasted together by macros, so the sources are
    not enough)."""
    data = open(binary, "rb").read()
    pat = rb"\0([_@?][A-Za-z_][\x21-\x7e]{2,200})(?=\0)"
    return {m.group(1).decode() for m in re.finditer(pat, data)
            if re.match(rb"^(_\w+@\d+|@\w+@\d+|\?\w+@\w+@\w*@@.*|_\w+)$", m.group(1))}


def host_key(name):
    """(undecorated key, convention, stack bytes or None) for a decorated name."""
    m = re.match(r"^_(\w+)@(\d+)$", name)
    if m:
        return m.group(1), "stdcall", int(m.group(2))
    m = re.match(r"^@(\w+)@(\d+)$", name)
    if m:
        return m.group(1), "fastcall", int(m.group(2))
    m = re.match(r"^\?(\w+)@(\w+)@", name)
    if m:
        # ?Method@Class@Namespace@@Q[AE|AG]... : QAG is a __stdcall member, QAE __thiscall.
        conv = "thiscall" if re.search(r"@@[QUIAE]AE", name) else "stdcall"
        return m.group(2) + "_" + m.group(1), conv, None
    m = re.match(r"^_(\w+)$", name)
    if m:
        return m.group(1), "cdecl", None
    return None


def parse_args_list(s):
    """Stack bytes and register use from the CLI's '(ecx this, psh x, psh2 y)' list."""
    stack, regs = 0, []
    for a in filter(None, (x.strip() for x in (s or "").split(","))):
        kind = a.split()[0]
        if kind == "psh":
            stack += 4
        elif kind == "psh2":
            stack += 8
        else:
            regs.append(kind)
    return stack, regs


def arg_spec(s):
    """Where each argument arrives, in order: a register name, 's' (a stack
    dword) or 's2' (a stack qword)."""
    out = []
    for a in filter(None, (x.strip() for x in (s or "").split(","))):
        kind = a.split()[0]
        out.append({"psh": "s", "psh2": "s2"}.get(kind, kind))
    return out


def base_name(name):
    """The API name under an LTCG build's internal entry point:
    D3DDevice_SetTransform_0__LTCG_eax1_edx2 -> D3DDevice_SetTransform."""
    return re.sub(r"(_\d+)?__LTCG_\w*$", "", name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cli")
    ap.add_argument("xbe")
    ap.add_argument("binary")
    ap.add_argument("-o", "--output")
    a = ap.parse_args()

    out = subprocess.run([a.cli, a.xbe, "-e"], check=True, capture_output=True, text=True).stdout
    found = {}   # undecorated name -> (kind, conv, args, va)
    for line in out.splitlines():
        m = LINE.match(line.strip())
        if m:
            lib, kind, conv, name, args, va = m.groups()
            if name in ARGS_FIX:
                conv, args = "stdcall", ARGS_FIX[name]
            found.setdefault(name, (kind, conv, args, int(va, 16), lib))

    lines, skipped = [], []
    for name in sorted(host_names(a.binary)):
        k = host_key(name)
        if not k or k[0] not in found:
            continue
        key, conv, nbytes = k
        kind, fconv, args, va, _ = found[key]
        stack, regs = parse_args_list(args)
        stack = STACK_FIX.get(key, stack)
        if fconv == "thiscall" and conv == "stdcall" and nbytes is None:
            # A C++ __stdcall member pushes `this`; the scanner names it a thiscall
            # only when the library passes it in ecx.
            skipped.append(f"{name}: library passes this in ecx")
            continue
        if conv == "stdcall" and nbytes is not None and (fconv not in ("stdcall", "stdcall_") or regs or stack != nbytes):
            skipped.append(f"{name}: library is {fconv}({args}), host takes {nbytes} stack bytes")
            continue
        if conv == "fastcall" and fconv != "fastcall":
            skipped.append(f"{name}: library is {fconv}({args}), host is fastcall")
            continue
        lines.append(f"{name} {va:#x}")
    # The rest of the replaced libraries' functions are named after their own
    # convention, so the loader traps them (with that name) instead of letting
    # code that drives the NV2A or the APU run.
    mapped_va = {int(l.split()[1], 16) for l in lines}
    hosts = host_names(a.binary)
    for name, (kind, fconv, args, va, lib) in sorted(found.items()):
        if kind != "FUN" or va in mapped_va or not lib.startswith(("D3D8", "DSOUND")):
            continue
        stack, regs = parse_args_list(args)
        if regs and fconv not in ("fastcall", "thiscall") and "cl" not in regs:
            # Arguments in registers (an LTCG build's internal entry point, or
            # one the optimizer gave a register argument): name the API it
            # implements, by argument count, with where each argument arrives,
            # so the loader can put them on the stack for the host function.
            spec = arg_spec(args)
            base = base_name(name)
            nbytes = sum(8 if x == "s2" else 4 for x in spec)
            host = f"_{base}_LTCG@{nbytes}"
            if host not in hosts:
                host = f"_{base}@{nbytes}"
            lines.append(f"{host} {va:#x} regs={','.join(spec)}")
            continue
        if fconv == "fastcall":
            lines.append(f"@{name}@{stack + 4 * len(regs)} {va:#x}")
        elif re.match(r"^(D3D|IDirect|Direct|XAudio|XWave|XFile)", name):
            lines.append(f"_{name}@{stack}{'_' + '_'.join(regs) if regs else ''} {va:#x}")
    # Render states with symbols of their own place the title's state layout.
    for name, (kind, _, _, va, lib) in sorted(found.items()):
        if kind == "VAR" and name.startswith("D3DRS_"):
            lines.append(f"_{name} {va:#x} data")
    if "D3DDevice_SetIndices" in found:
        g = index_data_global(xbe_reader(a.xbe), found["D3DDevice_SetIndices"][3])
        if g:
            lines.append(f"_D3D__IndexData {g:#x} data")
    for key, aliases in DATA_ALIASES.items():
        if key in found:
            for alias in aliases:
                lines.append(f"{alias} {found[key][3]:#x} data")

    text = "\n".join(lines) + "\n"
    if a.output:
        open(a.output, "w").write(text)
    else:
        sys.stdout.write(text)
    print(f"xbsymmap: {len(found)} symbols found, {len(lines)} mapped, {len(skipped)} need adapters",
          file=sys.stderr)
    for s in skipped:
        print("  " + s, file=sys.stderr)


if __name__ == "__main__":
    main()
