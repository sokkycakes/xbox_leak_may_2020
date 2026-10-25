#!/usr/bin/env python3
"""Install the xbcompat wrapper into a separate Box86 source checkout.
Idempotent; refuses ambiguous upstream anchors instead of guessing.
"""
import sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
here = Path(__file__).resolve().parent
for name in ("wrappedxbcompat_renderer.c", "wrappedxbcompat_renderer_private.h"):
    target = root / "src/wrapped" / name
    target.write_bytes((here / name).read_bytes())
def insert(path, anchor, line):
    text = path.read_text()
    if line in text:
        return
    if text.count(anchor) != 1:
        raise SystemExit(f"unexpected Box86 layout: {path}")
    path.write_text(text.replace(anchor, anchor + "\n" + line))
insert(root / "CMakeLists.txt",
       '    "${BOX86_ROOT}/src/wrapped/wrappedlibbsd.c"',
       '    "${BOX86_ROOT}/src/wrapped/wrappedxbcompat_renderer.c"')
insert(root / "src/library_list.h",
       'GO("libbsd.so.0", libbsd)',
       'GO("libxbcompat_renderer.so.1", xbcompat_renderer)')
print("Installed xbcompat native renderer wrapper")
