#!/usr/bin/env python3
"""Check the receiver: RGB outputs during DE must match the source picture."""
import sys
import numpy as np
from PIL import Image
LINE = 3432
rec = np.fromfile(sys.argv[1], dtype=np.uint8).reshape(-1, 8)
src = np.asarray(Image.open(sys.argv[2]).convert('RGB')).astype(int)
full = np.where(src >= 235, 255, np.clip(((src - 16) * 298) >> 8, 0, 255)) >> 2  # same maths as the RTL
fl = rec[:, 6]
de = (fl >> 1) & 1
# second frame only
start = LINE * 525
de2 = de[start:]
idx = np.flatnonzero(np.diff(de2.astype(int)) == 1) + 1 + start
print('active lines in frame 2:', len(idx))
lens = [int(np.argmin(de[i:i + 4000])) for i in idx]
print('active widths seen:', sorted(set(lens)))
# RGB outputs are registered one clock after DE
img = np.zeros((480, 720, 3), int)
f2 = (fl[idx] >> 4) & 1
rows1 = [i for i, f in zip(idx, f2)]
# field order: lines arrive field 1 then field 2 within the frame
half = len(idx) // 2
bad = 0
for n, i in enumerate(idx):
    row = 2 * n if n < half else 2 * (n - half) + 1
    seg = rec[i + 1:i + 1 + 2880, 3:6][::4]
    img[row] = seg
mism = np.abs(img - full)
print('max abs diff (6-bit units):', mism.max(), ' pixels off:', int((mism > 0).any(2).sum()))
