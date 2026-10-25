#!/usr/bin/env python3
"""Generate guest-call adapters for one xbcompat source file (non-x86 builds).

Input is the file preprocessed with -DXBC_GEN, where the NTAPI, FASTCALL and
CDECLAPI markers survive as __xbc_conv_std/_fast/_cdecl.  Every function
*defined* with one of them is callable by guest x86 code, so each gets an
adapter that reads its arguments the way the x86 convention placed them
(see src/cpu.h) and a table entry registered at startup.

The output is meant to be compiled in the same translation unit as the
source file (the build includes both from a wrapper), so static functions
are reachable.

usage: gen_adapters.py file.i path/of/source.c > file.xa.inc
"""
import os
import re
import sys

CONV = {"std": "CONV_STD", "fast": "CONV_FAST", "cdecl": "CONV_CDECL"}
MARK = re.compile(r"__xbc_conv_(std|fast|cdecl)\s+([A-Za-z_]\w*)\s*\(")
# Variadic functions get hand-written adapters next to them.
SKIP_VARIADIC = True


def main_file_text(path, source):
    """The preprocessed text that came from the source file itself or other
    files under src/ (macro-generated definitions included)."""
    keep = []
    cur_ok = True
    src_dir = os.path.dirname(os.path.abspath(source))
    root = src_dir
    while os.path.basename(root) != "src" and os.path.dirname(root) != root:
        root = os.path.dirname(root)
    for line in open(path, encoding="latin-1"):
        m = re.match(r'#\s*\d+\s+"([^"]*)"', line)
        if m:
            f = os.path.abspath(m.group(1))
            cur_ok = f == os.path.abspath(source)
            keep.append("\n")
            continue
        keep.append(line if cur_ok else "\n")
    return "".join(keep)


def balanced(text, i):
    """text[i] == '(' -> index just past the matching ')'."""
    depth = 0
    for j in range(i, len(text)):
        c = text[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def split_params(s):
    out, depth, cur = [], 0, ""
    for c in s:
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return out


def strip_marks(s):
    return re.sub(r"__xbc_conv_\w+", "", s)


def param_decl(p, idx):
    """(local declaration, name) for one parameter."""
    p = " ".join(p.split())
    m = re.search(r"\(\s*(?:__xbc_conv_\w+\s*)?\*\s*([A-Za-z_]\w*)\s*\)", p)
    if m:
        return strip_marks(p), m.group(1)
    arr = re.search(r"([A-Za-z_]\w*)\s*(\[[^\]]*\])+\s*$", p)
    if arr:
        name = arr.group(1)
        p = p[:arr.start()] + "*" + name
    else:
        m = re.search(r"([A-Za-z_]\w*)\s*$", p)
        name = m.group(1) if m else None
        type_only = {"void", "int", "char", "short", "long", "unsigned", "signed", "float", "double"}
        if name is None or name in type_only or re.fullmatch(r"(const\s+)?[A-Za-z_]\w*", p) and "*" not in p:
            # unnamed parameter: name it
            name = f"xa_p{idx}"
            p = p + " " + name
    if "*" not in p and p.startswith("const "):
        p = p[len("const "):]
    return strip_marks(p), name


def return_is_void(text, start):
    j = start - 1
    depth = 0
    while j >= 0:
        c = text[j]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
        elif depth == 0 and c in ";{}":
            break
        j -= 1
    ret = text[j + 1:start]
    ret = re.sub(r"__attribute__\s*\(\(.*?\)\)", " ", ret, flags=re.S)
    toks = [t for t in re.findall(r"[A-Za-z_]\w*|\*", ret)
            if t not in ("static", "inline", "extern", "__inline", "__inline__", "_Noreturn")]
    return toks == ["void"], " ".join(toks)


def main():
    pre, source = sys.argv[1], sys.argv[2]
    text = main_file_text(pre, source)
    tag = re.sub(r"\W", "_", os.path.relpath(source))
    funcs, seen = [], set()
    for m in MARK.finditer(text):
        conv, name = m.group(1), m.group(2)
        open_paren = m.end() - 1
        close = balanced(text, open_paren)
        if close < 0:
            continue
        rest = text[close:close + 400].lstrip()
        rest = re.sub(r"^(__attribute__\s*\(\(.*?\)\)\s*)+", "", rest, flags=re.S)
        if not rest.startswith("{"):
            continue        # a declaration, not the definition
        if name in seen:
            continue
        params = split_params(text[open_paren + 1:close - 1])
        if params == ["void"] or params == []:
            params = []
        if any(p == "..." for p in params):
            if SKIP_VARIADIC:
                print(f"/* {name}: variadic, adapter written by hand */")
                continue
        seen.add(name)
        is_void, rtype = return_is_void(text, m.start())
        decls = [param_decl(p, i) for i, p in enumerate(params)]
        funcs.append((name, conv, is_void, rtype, decls))

    print(f"/* Generated by tools/gen_adapters.py from {os.path.relpath(source)}. */")
    print('#include "cpu.h"')
    for name, conv, is_void, rtype, decls in funcs:
        print(f"static uint64_t xa_{name}(struct xa_frame *xa_f)\n{{")
        for d, n in decls:
            print(f"    {d}; XA_ARG({n});")
        args = ", ".join(n for _, n in decls)
        if is_void:
            print(f"    {name}({args});\n    return 0;\n}}")
        else:
            print(f"    XA_RET({name}({args}));\n}}")
    print(f"static const struct xa_entry xa_table_{tag}[] = {{")
    for name, conv, *_ in funcs:
        print(f'    {{ (void *){name}, xa_{name}, {CONV[conv]}, "{name}" }},')
    print("    { 0, 0, 0, 0 }\n};")
    print(f"__attribute__((constructor)) static void xa_register_{tag}(void)\n{{")
    print(f"    xa_register(xa_table_{tag}, {len(funcs)});\n}}")


if __name__ == "__main__":
    main()
