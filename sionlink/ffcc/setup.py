#!/usr/bin/env python3
"""Prepares the FFCC GBA client source for the DS build (ds/).

Fetches the FFCC decompilation (CC0) at a pinned commit, copies the GBA
client's game code into build/cli with the few edits the DS port needs, and
puts its graphics there: extracted from your FFCC disc image with --disc, or
blank stand-ins with --stub (the client then runs, but shows no text).

usage: setup.py [--region usa|pal|jp] (--disc FFCC.iso | --bin ffcc_cli.bin | --stub)
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

DECOMP_URL = "https://github.com/zcanann/FFCC-Decomp.git"
DECOMP_REV = "8ffc4be"

REGIONS = {
    "usa": ("GCCE01", "config/GCCE01/cli"),
    "pal": ("GCCP01", "config/cli"),
    "jp": ("GCCJGC", "config/GCCJGC/cli"),
}

HERE = Path(__file__).resolve().parent
BUILD = HERE / "build"
DECOMP = BUILD / "decomp"
CLI = BUILD / "cli"

# The client's sound (m4a and its data) is left out for now.
SKIP = {"m4a_tables.c", "sound_data.c"}
ASSET_SOURCES = {"usa": ["font_gfx.s", "backdrop_gfx.s"],
                 "pal": ["font_gfx.s", "backdrop_gfx.s"],
                 "jp": ["font_gfx_jp.s"]}


def fetch():
    if not (DECOMP / ".git").is_dir():
        BUILD.mkdir(exist_ok=True)
        subprocess.run(["git", "clone", "-q", "--filter=blob:none", "--no-checkout",
                        DECOMP_URL, str(DECOMP)], check=True)
    subprocess.run(["git", "-C", str(DECOMP), "checkout", "-q", "-f", DECOMP_REV], check=True)


def edit(path: Path, old: str, new: str, count: int = 1):
    text = path.read_text()
    if text.count(old) != count:
        sys.exit(f"setup.py: {path.name}: expected {count} of {old!r}, found {text.count(old)}")
    path.write_text(text.replace(old, new))


def copy_sources(region):
    src = DECOMP / "gba/src/cli/main"
    if CLI.exists():
        shutil.rmtree(CLI)
    CLI.mkdir(parents=True)
    for f in src.iterdir():
        if f.suffix in (".c", ".h", ".inc") and f.name not in SKIP:
            shutil.copy(f, CLI / f.name)
    for name in ASSET_SOURCES[region]:
        shutil.copy(src / name, CLI / name)
    shutil.copy(DECOMP / "gba/include/gba_types.h", CLI / "gba_types.h")

    # global.h: the DS port's hardware (ds/source/ds_port.h) instead of the GBA's
    g = CLI / "global.h"
    edit(g, '#define ABS(x) ((x) < 0 ? -(x) : (x))\n',
         '#define ABS(x) ((x) < 0 ? -(x) : (x))\n\n#ifdef PORT_DS\n#include "ds_port.h"\n#else\n')
    edit(g, '#define INTR_VECTOR (*(void **)0x03007FFC)\n',
         '#define INTR_VECTOR (*(void **)0x03007FFC)\n'
         '#define BG_SCROLL_REGS ((vu16 *)0x04000010)\n'
         '#define OBJ_VRAM      0x06010000\n'
         '#define MB_HEADER     ((u8 *)0x02000000)\n'
         '#define CART_HEADER   ((u8 *)0x08000000)\n#endif\n')
    edit(g, '/* DMA helpers in the style of the AGB SDK; dmaNo is 0 or 3 */\n#define DmaSet',
         '/* DMA helpers in the style of the AGB SDK; dmaNo is 0 or 3 */\n#ifndef DmaSet\n#define DmaSet')
    edit(g, '        dmaRegs[2];                                       \\\n    }\n',
         '        dmaRegs[2];                                       \\\n    }\n#endif\n')

    # main.c: no IWRAM to clear and no interrupt vector to install on the DS
    m = CLI / "main.c"
    edit(m, '    REG_WAITCNT = 0x4014;\n', '#ifndef PORT_DS\n    REG_WAITCNT = 0x4014;\n')
    edit(m, '    DmaClear32(3, 0, 0x03007E00, value);\n', '    DmaClear32(3, 0, 0x03007E00, value);\n#endif\n')
    edit(m, '    DmaCopy32(3, intr_main, gIntrMainRam, sizeof(gIntrMainRam));\n    INTR_VECTOR = gIntrMainRam;\n',
         '#ifndef PORT_DS\n    DmaCopy32(3, intr_main, gIntrMainRam, sizeof(gIntrMainRam));\n    INTR_VECTOR = gIntrMainRam;\n#endif\n')
    edit(m, '(vu16 *)0x04000010', 'BG_SCROLL_REGS', count=3)

    # Sprite tiles live elsewhere on the DS
    edit(CLI / "obj.c", '0x06010000', 'OBJ_VRAM', count=2)
    edit(CLI / "text.c", '0x06017880', '(OBJ_VRAM + 0x7880)', count=2)
    edit(CLI / "text.c", '0x06017380', '(OBJ_VRAM + 0x7380)', count=2)

    # The multiboot header the GameCube fills in, and the cartridge header
    l = CLI / "link.c"
    for off in ("AC", "C4", "C5", "C8"):
        edit(l, f'0x020000{off}', f'(MB_HEADER + 0x{off})', count=2 if off == "C4" else 1)
    edit(l, '0x080000AC', '(CART_HEADER + 0xAC)')

    # The GameCube's bulk downloads land in EWRAM the DS program occupies
    edit(CLI / "xfer.h", '#if defined(VERSION_GCCJGC)\n#define DOWNLOAD_START',
         '#if defined(PORT_DS)\n#define DOWNLOAD_START PORT_DOWNLOAD\n#define LIST_START     (PORT_DOWNLOAD + 0x4800)\n'
         '#define DETAIL_START   (PORT_DOWNLOAD + 0x7800)\n#elif defined(VERSION_GCCJGC)\n#define DOWNLOAD_START')
    edit(CLI / "xfer.h", '#define DOWNLOAD_END  0x02040000\n',
         '#ifdef PORT_DS\n#define DOWNLOAD_END  (PORT_DOWNLOAD + 0xC000)\n#else\n#define DOWNLOAD_END  0x02040000\n#endif\n')


def weak_storage():
    """Globals the retail image keeps in a common BSS block have no defining
    source file. Every variable declared extern gets a weak definition (a
    real definition elsewhere wins): those declared in headers in
    port_bss.c, the rest at the end of the file declaring them, where their
    type is known."""
    import re
    decl = re.compile(r"^extern\s+(?!const\b)([^;()]+?)\s*;")
    seen, common = set(), []
    headers = sorted(f.name for f in CLI.glob("*.h"))
    for f in sorted(CLI.glob("*.h")) + sorted(CLI.glob("*.c")):
        local = []
        for line in f.read_text(errors="replace").splitlines():
            m = decl.match(line)
            if not m or "[]" in m.group(1):
                continue
            name = re.search(r"(\w+)\s*(\[[^\]]*\]\s*)*$", m.group(1)).group(1)
            if name in seen:
                continue
            seen.add(name)
            (common if f.suffix == ".h" else local).append(f"__attribute__((weak)) {m.group(1)};")
        if local:
            with f.open("a") as out:
                out.write("\n/* DS port: storage the retail image keeps elsewhere (setup.py) */\n")
                out.write("\n".join(local) + "\n")
    text = "// Generated by setup.py: see weak_storage().\n"
    text += "".join(f'#include "{h}"\n' for h in headers)
    (CLI / "port_bss.c").write_text(text + "\n" + "\n".join(common) + "\n")


def assets(region, disc, stub, cli_bin=None):
    version, config = REGIONS[region]
    cfg = DECOMP / "gba" / config
    if stub:
        # Blank assets of the right sizes, from symbols.txt
        for line in (cfg / "symbols.txt").read_text().splitlines():
            if "//" not in line or " asset" not in line or "asset:asm" in line:
                continue
            name = line.split("=")[0].strip()
            size = int(line.split("size:")[1].split()[0], 16)
            (CLI / f"{name}.bin").write_bytes(bytes(size))
        return
    orig = BUILD / "orig" / version
    orig.mkdir(parents=True, exist_ok=True)
    tools = DECOMP / "gba/tools"
    if cli_bin:
        shutil.copy(cli_bin, orig / "ffcc_cli.bin")
    else:
        subprocess.run([sys.executable, str(tools / "extract.py"), str(disc), str(orig), "gba/ffcc_cli.bin"],
                       check=True)
    subprocess.run([sys.executable, str(tools / "assets.py"), str(orig / "ffcc_cli.bin"), str(cfg), str(CLI)],
                   check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--region", choices=REGIONS, default="usa")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--disc", type=Path, help="your FFCC disc image (.iso or .gcm)")
    g.add_argument("--bin", type=Path, help="gba/ffcc_cli.bin from your disc")
    g.add_argument("--stub", action="store_true", help="blank graphics, for build tests")
    a = p.parse_args()
    fetch()
    copy_sources(a.region)
    weak_storage()
    assets(a.region, a.disc, a.stub, a.bin)
    (BUILD / "region").write_text(REGIONS[a.region][0] + "\n")
    print(f"build/cli ready ({a.region}{', blank graphics' if a.stub else ''})")


if __name__ == "__main__":
    main()
