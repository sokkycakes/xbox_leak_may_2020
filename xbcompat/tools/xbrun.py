#!/usr/bin/env python3
"""Run an Xbox executable with xbcompat.

Accepts an XBE or an Xbox PE (obj/i386/*.exe from the XDK build).  PE files
are converted with pe2xbe.py into a scratch directory next to the media they
need.  The library map that tells the loader where the replaced libraries (D3D8,
XAPILIB's input functions, DSOUND) live in the image is generated with
findsigs.py from the leak's own libraries (XDK 4400), or with XbSymbolDatabase
for titles built with other XDKs, and cached.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LEAK = os.environ.get("XBOX_LEAK", os.path.join(ROOT, "..", "xbox_leak_may_2020", "xbox trunk", "xbox"))
LIBS = os.path.join(LEAK, "public", "xdk", "lib")
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")), "xbcompat")


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def library_versions(xbe):
    out = subprocess.run([sys.executable, os.path.join(HERE, "xbedump.py"), xbe],
                         check=True, capture_output=True, text=True).stdout
    return [l.split()[1] for l in out.splitlines() if l.strip().startswith("lib ")]


# XDK library name as recorded in the XBE's library version table -> .lib file.
REPLACED_LIBS = {"D3D8": "d3d8", "D3D8D": "d3d8d", "XAPILIB": "xapilib", "XAPILIBD": "xapilibd",
                 "DSOUND": "dsound", "DSOUNDD": "dsoundd"}


def signatures(libs):
    """One signature file for the given set of .lib names (sorted, joined by +)."""
    os.makedirs(CACHE, exist_ok=True)
    libs = sorted(libs)
    path = os.path.join(CACHE, "+".join(libs) + ".json")
    if not os.path.exists(path):
        run(sys.executable, os.path.join(HERE, "mksigs.py"), *[os.path.join(LIBS, l + ".lib") for l in libs],
            "-o", path)
    return path


# XbSymbolDatabase (MIT) finds the libraries of XDK builds the leak does not
# have.  It is fetched at this commit and built into the cache on first use.
XBSYMDB_URL = "https://github.com/Cxbx-Reloaded/XbSymbolDatabase.git"
XBSYMDB_COMMIT = "20eced544726f5558c5a408458f38a086cc4e543"
LEAK_XDK_BUILD = 4400


def xbsymdb_cli():
    cli = os.environ.get("XBSYMDB_CLI")
    if cli:
        return cli
    src = os.path.join(CACHE, "XbSymbolDatabase")
    cli = os.path.join(src, "build", "projects", "cli", "XbSymbolDatabaseCLI")
    if not os.path.exists(cli):
        print(f"xbrun: building XbSymbolDatabase {XBSYMDB_COMMIT[:12]} in {src}", file=sys.stderr)
        if not os.path.exists(src):
            run("git", "clone", "-q", XBSYMDB_URL, src)
        run("git", "-C", src, "checkout", "-q", XBSYMDB_COMMIT)
        run("cmake", "-S", src, "-B", os.path.join(src, "build"), "-DCMAKE_BUILD_TYPE=Release",
            stdout=subprocess.DEVNULL)
        run("cmake", "--build", os.path.join(src, "build"), "--target", "XbSymbolDatabaseCLI", "-j8",
            stdout=subprocess.DEVNULL)
    return cli


def xdk_build(xbe):
    out = subprocess.run([sys.executable, os.path.join(HERE, "xbedump.py"), xbe],
                         check=True, capture_output=True, text=True).stdout
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 3 and p[0] == "lib" and p[1] in ("D3D8", "D3D8LTCG", "D3D8D"):
            return int(p[2].split(".")[2])
    return LEAK_XDK_BUILD


def make_map(xbe):
    if xdk_build(xbe) != LEAK_XDK_BUILD:
        digest = hashlib.sha1(open(xbe, "rb").read()).hexdigest()[:16]
        binary = os.environ.get("XBCOMPAT_BIN", os.path.join(ROOT, "build", "xbcompat"))
        # The map depends on the binary's host tables and on xbsymmap itself.
        bdigest = hashlib.sha1(open(binary, "rb").read() +
                               open(os.path.join(HERE, "xbsymmap.py"), "rb").read()).hexdigest()[:8]
        path = os.path.join(CACHE, f"{digest}-xbsym-{bdigest}.map")
        if not os.path.exists(path):
            os.makedirs(CACHE, exist_ok=True)
            run(sys.executable, os.path.join(HERE, "xbsymmap.py"), xbsymdb_cli(), xbe, binary, "-o", path)
        return path
    libs = [REPLACED_LIBS[l] for l in library_versions(xbe) if l in REPLACED_LIBS]
    if not libs:
        return None
    digest = hashlib.sha1(open(xbe, "rb").read()).hexdigest()[:16]
    path = os.path.join(CACHE, digest + "-" + "+".join(sorted(libs)) + ".map")
    if os.path.exists(path):
        return path
    res = subprocess.run([sys.executable, os.path.join(HERE, "findsigs.py"), signatures(libs), xbe, "--json"],
                         check=True, capture_output=True, text=True).stdout
    d = json.loads(res)
    with open(path, "w") as f:
        # "name va" for functions, "name va data" for variables (never patched).
        for section in ("unique", "code_refs", "data_symbols"):
            for name, v in d.get(section, {}).items():
                va = v["va"] if isinstance(v, dict) else v
                f.write(f"{name} {va}{' data' if section == 'data_symbols' else ''}\n")
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image")
    ap.add_argument("args", nargs=argparse.REMAINDER, help="extra xbcompat options")
    a = ap.parse_args()

    image = os.path.abspath(a.image)
    if open(image, "rb").read(2) == b"MZ":
        # An XDK build puts media next to the sources, one or two levels above obj/i386.
        project = os.path.dirname(os.path.dirname(os.path.dirname(image)))
        name = os.path.splitext(os.path.basename(image))[0]
        game = os.path.join(CACHE, "games", name)
        if os.path.exists(game):
            shutil.rmtree(game)
        os.makedirs(game)
        media = os.path.join(game, "media")
        for entry in os.listdir(project):
            if entry.lower() == "media":
                media = os.path.join(game, entry)   # keep the project's spelling: one media directory
                shutil.copytree(os.path.join(project, entry), media)
        for entry in os.listdir(os.path.dirname(image)):
            if entry.lower().endswith(".xpr"):
                os.makedirs(media, exist_ok=True)
                shutil.copy(os.path.join(os.path.dirname(image), entry), os.path.join(media, entry))
        xbe = os.path.join(game, "default.xbe")
        run(sys.executable, os.path.join(HERE, "pe2xbe.py"), image, xbe)
    else:
        xbe = image

    # A title that calls XLaunchNewImage comes back through this script, so
    # the next image gets its own library map.
    os.environ["XBCOMPAT_LAUNCHER"] = f"{sys.executable} {os.path.abspath(__file__)}"
    cmd = [os.environ.get("XBCOMPAT_BIN", os.path.join(ROOT, "build", "xbcompat"))]
    m = make_map(xbe)
    if m:
        cmd += ["--hle", m]
    cmd += a.args + [xbe]
    os.execv(cmd[0], cmd)


if __name__ == "__main__":
    main()
