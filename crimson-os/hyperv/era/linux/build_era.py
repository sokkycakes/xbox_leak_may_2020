#!/usr/bin/env python3.12
"""Linux build of the Era-style Crimson partitions, for testing in QEMU.

Does what New-CrimsonEraImage.ps1 does, with wimlib and hivex instead of
DISM and reg.exe, starting from a Windows ISO (its boot.wim index 1 is plain
Windows PE, its install.wim is the donor). No ADK optional components and no
GPU driver: QEMU has no GPU partitioning.

  build_era.py --iso win11.iso --out outdir [--host-addr 10.0.2.2] [--shell DIR/EXE]

Writes outdir/crimson-{game,system}.wim and .iso (BIOS + UEFI bootable).
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

import hivex

HERE = os.path.dirname(os.path.abspath(__file__))
ERA = os.path.dirname(HERE)
AGENT = os.path.join(ERA, "..", "..", "build", "crimson-agent.exe")


def run(*cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def wim_update(wim, commands):
    p = subprocess.run(["wimlib-imagex", "update", wim, "1"],
                       input="\n".join(commands) + "\n", text=True, capture_output=True)
    if p.returncode:
        print(p.stdout, p.stderr)
        raise SystemExit("wimlib update failed")


def wim_paths(wim, index):
    out = subprocess.run(["wimlib-imagex", "dir", wim, str(index)], check=True,
                         capture_output=True, text=True).stdout
    return out.splitlines()


# ---------- registry ----------

def copy_key(src, snode, dst, dparent, name):
    """Recursively copy src key snode into dst under dparent as name (replacing it)."""
    old = dst.node_get_child(dparent, name)
    if old:
        dst.node_delete_child(old)
    dnode = dst.node_add_child(dparent, name)
    vals = []
    for v in src.node_values(snode):
        t, raw = src.value_value(v)
        vals.append({"key": src.value_key(v), "t": t, "value": raw})
    dst.node_set_values(dnode, vals)
    for c in src.node_children(snode):
        copy_key(src, c, dst, dnode, src.node_name(c))


def find(h, path):
    node = h.root()
    for part in path.split("\\"):
        node = h.node_get_child(node, part)
        if not node:
            return None
    return node


def ensure(h, path):
    node = h.root()
    for part in path.split("\\"):
        nxt = h.node_get_child(node, part)
        node = nxt if nxt else h.node_add_child(node, part)
    return node


def merge_values(src, snode, dst, dnode):
    """Add the donor's values that PE lacks; union REG_MULTI_SZ lists (svchost groups)."""
    have = {dst.value_key(v).lower(): v for v in dst.node_values(dnode)}
    vals = []
    for v in dst.node_values(dnode):
        t, raw = dst.value_value(v)
        vals.append({"key": dst.value_key(v), "t": t, "value": raw})
    index = {d["key"].lower(): d for d in vals}
    for v in src.node_values(snode):
        k = src.value_key(v)
        t, raw = src.value_value(v)
        if k.lower() not in have:
            vals.append({"key": k, "t": t, "value": raw})
        elif t == 7 and index[k.lower()]["t"] == 7:
            def items(b):
                return [x for x in b.decode("utf-16-le").split("\0") if x]
            cur = items(index[k.lower()]["value"])
            for x in items(raw):
                if x.lower() not in (c.lower() for c in cur):
                    cur.append(x)
            index[k.lower()]["value"] = ("\0".join(cur) + "\0\0").encode("utf-16-le")
    dst.node_set_values(dnode, vals)
    for c in src.node_children(snode):
        name = src.node_name(c)
        existing = dst.node_get_child(dnode, name)
        if existing:
            merge_values(src, c, dst, existing)
        else:
            copy_key(src, c, dst, dnode, name)


MERGE_KEYS = {"Microsoft\\Windows NT\\CurrentVersion\\Svchost"}


def merge_registry(donor_dir, target_dir):
    plan = {
        "SYSTEM": [f"ControlSet001\\Services\\{s}" for s in
                   ("vpci", "Audiosrv", "AudioEndpointBuilder", "MMCSS")],
        # Svchost is left alone: WinPE's groups already hold the audio services, and
        # the donor's LocalServiceNoNetwork lacks BFE, which takes networking down.
        "SOFTWARE": ["Microsoft\\Windows NT\\CurrentVersion\\Multimedia",
                     "Microsoft\\Windows NT\\CurrentVersion\\Drivers32",
                     "Classes\\CLSID\\{BCDE0395-E52F-467C-8E3D-C4579291692E}"],
    }
    for hive, keys in plan.items():
        src = hivex.Hivex(os.path.join(donor_dir, hive))
        dst = hivex.Hivex(os.path.join(target_dir, hive), write=True)
        for k in keys:
            s = find(src, k)
            if not s:
                print(f"  registry: not in donor: {hive}\\{k}")
                continue
            parent, name = k.rsplit("\\", 1)
            existing = find(dst, k)
            if k in MERGE_KEYS and existing:
                # replacing this key removes PE's own groups and PE loses networking
                merge_values(src, s, dst, existing)
            else:
                copy_key(src, s, dst, ensure(dst, parent), name)
            print(f"  registry: {hive}\\{k}")
        dst.commit(None)


def merge_winners(donor_dir, target_dir):
    """32-bit SxS: Winners maps a requested version (e.g. common controls
    6.0.0.0) to the installed one. PE lists only its 64-bit assemblies."""
    k = "Microsoft\\Windows\\CurrentVersion\\SideBySide\\Winners"
    src = hivex.Hivex(os.path.join(donor_dir, "SOFTWARE"))
    dst = hivex.Hivex(os.path.join(target_dir, "SOFTWARE"), write=True)
    s = find(src, k)
    if not s:
        print("  registry: donor has no SxS Winners")
        return
    d = ensure(dst, k)
    n = 0
    for c in src.node_children(s):
        name = src.node_name(c)
        if name.lower().startswith(("x86_", "wow64_")):
            copy_key(src, c, dst, d, name)
            n += 1
    dst.commit(None)
    print(f"  registry: {n} 32-bit SxS winners")


# ---------- build ----------

def donor_list(role):
    out = []
    for line in open(os.path.join(ERA, "era-files.txt"), encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        roles, rel = line.split(None, 1)
        if roles in ("both", role):
            out.append(rel.replace("\\", "/"))
    return out


# Era profile: what a VM partition needs and nothing else.
KEEP_DRIVERS = (
    "c_", "acpi", "acpidev", "basicdisplay", "basicrender", "cdrom", "compdev", "cpu", "disk", "errdev",
    "genpass", "hal", "input", "keyboard", "machine", "msmouse", "mssmbios", "pci", "prm", "ramdisk",
    "rawsilo", "scsidev", "swenum", "umbus", "unknown", "vdrvroot", "volmgr", "volsnap", "volume", "uefi",
    "tpm", "kdnic", "ndis", "netloop", "netip6", "nettcpip", "netserv", "netnb", "netmscli",
    "hdaudbus", "hdaudio", "audioendpoint", "wdmaudio", "wdmaudiocoresystem",
    # Hyper-V guest devices
    "wvmbus", "wvmbushid", "wvmbusvideo", "wvpci", "wnetvsc", "wstorvsc", "wstorflt", "wdmvsc", "whyperkbd",
    # QEMU test devices (e1000e, AHCI/IDE CD)
    "net1ix64", "nete1e3e", "nete1g3e", "mshdc",
)
WINSXS_KEEP = ("microsoft.windows.common-controls", "microsoft.windows.gdiplus", "microsoft.windows.c..-controls")


def era_strip(lower, role, no_wow64):
    out = []
    for lp in lower:
        parts = lp.split("/")
        # driver packages for hardware a VM never has
        if lp.startswith("/windows/system32/driverstore/filerepository/") and len(parts) == 6:
            if not parts[5].startswith(KEEP_DRIVERS):
                out.append(lp)
        # component store: only the side-by-side assemblies apps load at run time
        if lp.startswith("/windows/winsxs/") and len(parts) == 4 and parts[3] not in ("manifests",):
            if not any(k in parts[3] for k in WINSXS_KEEP):
                out.append(lp)
        # WinSxS/Manifests stays whole (~4 MB): without it PE bugchecks with
        # CRITICAL_PROCESS_DIED shortly after the kernel starts.
    out += ["/windows/boot", "/windows/servicing", "/windows/speech", "/windows/speech_onecore",
            "/windows/system32/speech", "/windows/system32/speech_onecore", "/windows/system32/dism",
            "/windows/system32/ntkrla57.exe", "/windows/system32/dbgeng.dll", "/windows/system32/chakra.dll",
            "/windows/system32/wdi"]
    if no_wow64 or role == "system":
        out.append("/windows/syswow64")
    return [x for x in out if x]


def wow64_files(pe_paths, donor_index, donated):
    """32-bit support. WinPE ships SysWOW64 half-empty and without the WoW64
    layer itself, so 32-bit programs fail with a side-by-side error. Mirror the
    64-bit set: every System32 file the image has gets its SysWOW64 twin from
    the donor, plus the x86 common-controls/GDI+ assemblies."""
    names = set()
    for p in list(pe_paths) + donated:
        lp = p.lower()
        if lp.startswith("/windows/system32/") and lp.count("/") == 3:
            names.add(lp.rsplit("/", 1)[1])
    out = []
    for n in sorted(names):
        for cand in (f"/windows/syswow64/{n}", f"/windows/syswow64/en-us/{n}.mui"):
            if cand in donor_index:
                out.append(donor_index[cand])
    for lp, p in donor_index.items():
        parts = lp.split("/")
        if len(parts) >= 4 and parts[1:3] == ["windows", "winsxs"] and \
                (parts[3].startswith("x86_microsoft.windows.common-controls") or
                 parts[3].startswith("x86_microsoft.windows.gdiplus")):
            if len(parts) == 5:          # files inside the assembly folder
                out.append(p)
        # every x86/wow64 manifest: WoW64's default context needs
        # Microsoft.Windows.SystemCompatible (x86), which PE lacks
        if lp.startswith(("/windows/winsxs/manifests/x86_", "/windows/winsxs/manifests/wow64_")):
            out.append(p)
        # catalogs: once a 32-bit ntdll is present smss maps the 32-bit
        # KnownDLLs at boot, and code integrity rejects them without these
        if lp.startswith("/windows/system32/catroot/") and lp.endswith(".cat") and len(parts) == 6:
            out.append(p)
    print(f"  wow64: {len(out)} files from the donor")
    return out


def build_role(role, iso_dir, work, out, host_addr, shell, minimal=False, no_wow64=False):
    print(f"\n=== {role} ===", flush=True)
    rw = os.path.join(work, role)
    shutil.rmtree(rw, ignore_errors=True)
    os.makedirs(rw)
    wim = os.path.join(out, f"crimson-{role}.wim")
    if os.path.exists(wim):
        os.remove(wim)
    run("wimlib-imagex", "export", os.path.join(iso_dir, "sources/boot.wim"), "1", wim, "--compress=LZX")
    install = glob.glob(os.path.join(iso_dir, "sources/install.*"))[0]

    # donor files (case-insensitive match against the donor image listing)
    donor_index = {p.lower(): p for p in wim_paths(install, 1)}
    want = []
    for pat in donor_list(role):
        import fnmatch
        hits = [p for lp, p in donor_index.items() if fnmatch.fnmatch(lp, "/" + pat.lower())]
        if not hits:
            print(f"  donor: missing {pat}")
        want += hits
    if role == "game" and not no_wow64:
        want += wow64_files(wim_paths(wim, 1), donor_index, want)
    listfile = os.path.join(rw, "donor.lst")
    with open(listfile, "w") as f:
        f.write("\n".join(want) + "\n")
    ddir = os.path.join(rw, "donor")
    run("wimlib-imagex", "extract", install, "1", "@" + listfile, f"--dest-dir={ddir}",
        "--preserve-dir-structure", "--no-acls", "--nullglob")
    run("wimlib-imagex", "extract", install, "1", "/Windows/System32/config/SYSTEM",
        "/Windows/System32/config/SOFTWARE", f"--dest-dir={rw}/donor-hives", "--no-acls")
    run("wimlib-imagex", "extract", wim, "1", "/Windows/System32/config/SYSTEM",
        "/Windows/System32/config/SOFTWARE", f"--dest-dir={rw}/pe-hives", "--no-acls")
    merge_registry(f"{rw}/donor-hives", f"{rw}/pe-hives")
    if role == "game" and not no_wow64:
        merge_winners(f"{rw}/donor-hives", f"{rw}/pe-hives")

    # crimson payload
    cdir = os.path.join(rw, "Crimson")
    os.makedirs(cdir)
    shutil.copy(AGENT, cdir)
    conn = f"--host {host_addr}" if host_addr else "--hv"
    if role == "game":
        args = f'--role game --name "Game OS (era)" {conn} --desktop --size 1280x720 --fps 15'
        launch = "GAME"  # found on whichever drive the game disk mounts as
    else:
        args = f'--role system --name "System OS (era)" {conn} --desktop --size 1280x720 --fps 10 --guide-key F1'
        launch = ""
        if shell:
            shutil.copytree(os.path.dirname(shell), os.path.join(cdir, "shell"))
            launch = "X:\\Crimson\\shell\\" + os.path.basename(shell)
    # start.cmd runs with no window (the agent starts it hidden); only the game
    # or shell draws on the partition's screen.
    lines = ["@echo off",
             "net start AudioEndpointBuilder >nul 2>&1", "net start Audiosrv >nul 2>&1",
             "echo crimson era %DATE% %TIME% > X:\\Crimson\\booted.txt"]
    if launch == "GAME":
        lines.append('for %%d in (C D E F G H I) do if exist %%d:\\Crimson\\launch-game.cmd (set CRIMSON_GAME=%%d:& call %%d:\\Crimson\\launch-game.cmd)')
    elif launch:
        lines.append(f'if exist "{launch}" start "" "{launch}"')
    open(os.path.join(cdir, "start.cmd"), "w", newline="\r\n").write("\n".join(lines) + "\n")
    open(os.path.join(rw, "winpeshl.ini"), "w", newline="\r\n").write(
        "[LaunchApps]\n%SYSTEMROOT%\\System32\\wpeinit.exe\n"
        f"%SYSTEMDRIVE%\\Crimson\\crimson-agent.exe, {args} --log X:\\Crimson\\agent.log"
        " --run \"cmd.exe /c X:\\Crimson\\start.cmd\"\n")
    # plain desktop: black for the game, the see-through key colour for the system
    wall = os.path.join(rw, "winpe.jpg")
    subprocess.run(["convert", "-size", "64x64", "xc:" + ("#FF00FF" if role == "system" else "#000000"),
                    "-quality", "100", wall], check=True)

    cmds = [f'add "{ddir}" /', f'add "{rw}/pe-hives" /Windows/System32/config',
            f'add "{cdir}" /Crimson', f'add "{rw}/winpeshl.ini" /Windows/System32/winpeshl.ini',
            f'add "{wall}" /Windows/System32/winpe.jpg']
    # strip what neither partition uses
    pe_paths = wim_paths(wim, 1)
    lower = {p.lower(): p for p in pe_paths}
    strip = ["/windows/system32/recovery", "/windows/system32/windowspowershell", "/windows/system32/oobe",
             "/windows/system32/sysprep", "/windows/system32/migration", "/windows/system32/spool",
             "/sources", "/setup.exe"]
    keep_fonts = ("segoeui", "seguisym", "consola", "cour", "arial", "marlett", "vga", "app", "cga",
                  "ega", "8514", "ssee", "sseri", "serife", "smalle")
    for lp, p in lower.items():
        if lp.startswith("/windows/fonts/") and lp.count("/") == 3 and not os.path.basename(lp).startswith(keep_fonts):
            strip.append(lp)
        parts = lp.split("/")
        if len(parts) == 4 and parts[1:3] == ["windows", "system32"] and len(parts[3]) == 5 \
                and parts[3][2] == "-" and parts[3] != "en-us" and "." not in parts[3]:
            strip.append(lp)
    if minimal:
        strip += era_strip(lower, role, no_wow64)
    for s in sorted(set(strip)):
        if s in lower:
            cmds.append(f'delete --force --recursive "{lower[s]}"')
    wim_update(wim, cmds)
    run("wimlib-imagex", "optimize", wim, "--recompress", "--compress=LZX")
    run("wimlib-imagex", "info", wim, "--boot", "1")

    iso = os.path.join(out, f"crimson-{role}.iso")
    run("mkwinpeimg", "--iso", f"--windows-dir={iso_dir}", f"--wim={wim}", iso)
    info = subprocess.run(["wimlib-imagex", "info", wim, "1"], capture_output=True, text=True).stdout
    total = next((l.split(":")[1].strip() for l in info.splitlines() if l.startswith("Total Bytes")), "?")
    print(f"*** crimson-{role}: WIM {os.path.getsize(wim) // 2**20} MB, expanded {int(total) // 2**20 if total.isdigit() else total} MB, ISO {os.path.getsize(iso) // 2**20} MB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", default=None)
    ap.add_argument("--host-addr", default="", help="connect over TCP to this address instead of a Hyper-V socket")
    ap.add_argument("--shell", default="", help="System OS shell exe (its folder is copied)")
    ap.add_argument("--roles", default="game,system")
    ap.add_argument("--minimal", action="store_true", help="Era profile: strip drivers, component store, WoW64 (system)")
    ap.add_argument("--no-wow64", action="store_true", help="also drop 32-bit support from the game OS")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or os.path.join(out, "work"))
    os.makedirs(out, exist_ok=True)
    iso_dir = os.path.join(work, "iso")
    if not os.path.exists(os.path.join(iso_dir, "sources/boot.wim")):
        os.makedirs(iso_dir, exist_ok=True)
        run("7z", "x", "-y", f"-o{iso_dir}", a.iso, "bootmgr", "bootmgr.efi", "boot", "efi",
            "sources/boot.wim", "sources/install.wim", "sources/install.esd", stdout=subprocess.DEVNULL)
    for role in a.roles.split(","):
        build_role(role, iso_dir, work, out, a.host_addr, a.shell, a.minimal, a.no_wow64)


if __name__ == "__main__":
    sys.exit(main())
