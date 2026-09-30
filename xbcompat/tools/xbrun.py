#!/usr/bin/env python3
"""Run an Xbox executable with xbcompat.

Accepts an XBE or an Xbox PE (obj/i386/*.exe from the XDK build).  PE files
are converted with pe2xbe.py into a scratch directory next to the media they
need.  The library map that tells the loader where D3D8 lives in the image is
generated with findsigs.py from the leak's own libraries and cached.
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


def signatures(debug):
    os.makedirs(CACHE, exist_ok=True)
    name = "d3d8d" if debug else "d3d8"
    path = os.path.join(CACHE, name + ".json")
    if not os.path.exists(path):
        run(sys.executable, os.path.join(HERE, "mksigs.py"), os.path.join(LIBS, name + ".lib"), "-o", path)
    return path


def make_map(xbe):
    digest = hashlib.sha1(open(xbe, "rb").read()).hexdigest()[:16]
    path = os.path.join(CACHE, digest + ".map")
    if os.path.exists(path):
        return path
    libs = library_versions(xbe)
    debug = "D3D8D" in libs
    if "D3D8" not in libs and not debug:
        return None
    res = subprocess.run([sys.executable, os.path.join(HERE, "findsigs.py"), signatures(debug), xbe, "--json"],
                         check=True, capture_output=True, text=True).stdout
    d = json.loads(res)
    with open(path, "w") as f:
        for section in ("unique", "code_refs", "data_symbols"):
            for name, v in d.get(section, {}).items():
                va = v["va"] if isinstance(v, dict) else v
                f.write(f"{name} {va}\n")
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
        for entry in os.listdir(project):
            if entry.lower() == "media":
                shutil.copytree(os.path.join(project, entry), os.path.join(game, entry))
        for entry in os.listdir(os.path.dirname(image)):
            if entry.lower().endswith(".xpr"):
                os.makedirs(os.path.join(game, "media"), exist_ok=True)
                shutil.copy(os.path.join(os.path.dirname(image), entry), os.path.join(game, "media", entry))
        xbe = os.path.join(game, "default.xbe")
        run(sys.executable, os.path.join(HERE, "pe2xbe.py"), image, xbe)
    else:
        xbe = image

    cmd = [os.path.join(ROOT, "build", "xbcompat")]
    m = make_map(xbe)
    if m:
        cmd += ["--hle", m]
    cmd += a.args + [xbe]
    os.execv(cmd[0], cmd)


if __name__ == "__main__":
    main()
