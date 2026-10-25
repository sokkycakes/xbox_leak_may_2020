#!/usr/bin/env python3
"""Prepare an exact-XBE map for the Pi's offline title launcher."""
import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", type=Path)
    ap.add_argument("--map", type=Path, help="already generated map for this exact XBE")
    ap.add_argument("--cache", type=Path, required=True,
                    help="output maps directory; copy its contents to the Pi cache/xbcompat/maps")
    args = ap.parse_args()
    image = args.image.resolve()
    digest = hashlib.sha1(image.read_bytes()).hexdigest()
    if args.map:
        source = args.map
    else:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        import xbrun
        source = xbrun.make_map(str(image))
    data = Path(source).read_bytes() if source else b"# no replaced XDK libraries\n"
    if not data.strip():
        ap.error("the source map is empty")
    args.cache.mkdir(parents=True, exist_ok=True)
    dest = args.cache / (digest + ".map")
    with tempfile.NamedTemporaryFile(dir=args.cache, delete=False) as f:
        temporary = f.name
        f.write(data)
    try:
        os.replace(temporary, dest)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(dest)

if __name__ == "__main__":
    main()
