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

LINE = re.compile(r"^(\w+?)__(FUN|VAR)__(?:(\w+?)__)?(\w+)(?:\((.*)\))? = (0x[0-9a-fA-F]+)$")


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
            found.setdefault(name, (kind, conv, args, int(va, 16), lib))

    lines, skipped = [], []
    for name in sorted(host_names(a.binary)):
        k = host_key(name)
        if not k or k[0] not in found:
            continue
        key, conv, nbytes = k
        kind, fconv, args, va, _ = found[key]
        stack, regs = parse_args_list(args)
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
    for name, (kind, fconv, args, va, lib) in sorted(found.items()):
        if kind != "FUN" or va in mapped_va or not lib.startswith(("D3D8", "DSOUND")):
            continue
        stack, regs = parse_args_list(args)
        if fconv == "fastcall":
            lines.append(f"@{name}@{stack + 4 * len(regs)} {va:#x}")
        elif re.match(r"^(D3D|IDirect|Direct|XAudio|XWave|XFile)", name):
            lines.append(f"_{name}@{stack}{'_' + '_'.join(regs) if regs else ''} {va:#x}")
    # Render states with symbols of their own place the title's state layout.
    for name, (kind, _, _, va, lib) in sorted(found.items()):
        if kind == "VAR" and name.startswith("D3DRS_"):
            lines.append(f"_{name} {va:#x} data")
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
