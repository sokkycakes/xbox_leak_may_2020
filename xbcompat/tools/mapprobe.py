#!/usr/bin/env python3
"""Guest addresses for XBCOMPAT_PROBE from a title's linker .map.

  tools/mapprobe.py TITLE.map RUN.log NAME [NAME...]

RUN.log is any xbcompat log of the title (for the "section NAME ADDR" lines);
each NAME matches symbols by substring.  Prints NAME, symbol and address, and
last the comma list to put in XBCOMPAT_PROBE."""
import re
import sys

mapfile, logfile, names = sys.argv[1], sys.argv[2], sys.argv[3:]
loaded = {m.group(1): int(m.group(2), 16)
          for m in re.finditer(r'section (\S+)\s+0x([0-9a-f]+)', open(logfile, errors='replace').read())}
lines = open(mapfile, errors='replace').read().splitlines()
groups = {}
for ln in lines:      # " 0008:00000000 00017922H XONLINE  CODE": the section number of each group
    m = re.match(r'\s*([0-9a-f]{4}):00000000 [0-9a-f]+H (\S+)\s+(CODE|DATA)', ln)
    if m and m.group(1) not in groups:
        groups[m.group(1)] = m.group(2)
out = []
for name in names:
    for ln in lines:
        m = re.match(r'\s*([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)', ln)
        if not m or name not in m.group(3):
            continue
        sec = groups.get(m.group(1))
        base = loaded.get((sec or '')[:8])
        if base is None:
            continue
        a = base + int(m.group(2), 16)
        print(f'{name}: {m.group(3)} {a:#x}')
        out.append(f'{a:#x}')
        break
    else:
        print(f'{name}: not found', file=sys.stderr)
print(','.join(out))
