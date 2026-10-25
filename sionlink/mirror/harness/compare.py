"""Compare the emulator's frames with the replay client's frames.

usage: compare.py EMU.raw CLIENT.raw OUTDIR CLIENT.idx [MIRROR.bin]
Raw files hold 256x384 32-bit pixels per frame (top screen, then bottom).
CLIENT.idx says how many bundles the client had applied by the end of each of
its frames; bundle k shows from the client frame after the one in which it was
applied. Bundle k is emulator frame k unless MIRROR.bin is given, whose bundle
headers name the frame (30 fps recordings skip every other frame).
"""
import sys, os, struct
import numpy as np
from PIL import Image

FR = 256 * 384

def load(p):
    a = np.fromfile(p, dtype=np.uint32)
    return (a & 0x00FFFFFF).reshape(-1, 384, 256)

emu, cli, out = load(sys.argv[1]), load(sys.argv[2]), sys.argv[3]
os.makedirs(out, exist_ok=True)
applied = [int(l.split()[1]) for l in open(sys.argv[4])]

bundleFrame = None
if len(sys.argv) > 5:
    bundleFrame = []
    with open(sys.argv[5], "rb") as f:
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            magic, frame, size, flags = struct.unpack("<IIII", h)
            bundleFrame.append(frame)
            f.seek(size - 16, 1)

shown = {}  # emulator frame -> client frame showing it
for j in range(len(applied) - 1):
    k = applied[j] - 1
    if k >= 0 and (j == 0 or applied[j - 1] - 1 != k):
        if bundleFrame is not None:
            if k >= len(bundleFrame):
                continue
            k = bundleFrame[k]
        if k not in shown:
            shown[k] = j + 1

def diff(i, j):
    d = emu[i] != cli[j]
    return int(d[:192].sum()), int(d[192:].sum())

rows = []
for i in range(len(emu)):
    if i not in shown or shown[i] >= len(cli):
        continue
    j = shown[i]
    t, b = diff(i, j)
    rows.append((i, t, b))
order = sorted(shown)
gaps = [shown[b] - shown[a] for a, b in zip(order, order[1:])]
off = 0
n = len(rows)
exact = sum(1 for _, t, b in rows if t == 0 and b == 0)
top_bad = sum(1 for _, t, _ in rows if t)
bot_bad = sum(1 for _, _, b in rows if b)
worst = max(rows, key=lambda r: r[1] + r[2])
nb = len(bundleFrame) if bundleFrame is not None else len(emu)
print(f"client showed {len(shown)} of {nb} bundles, {sum(1 for g in gaps if g > 1)} held for more than one frame")
print(f"{n} frames compared; {exact} pixel-identical ({100*exact/n:.1f}%)")
print(f"top screen differs in {top_bad} frames, bottom in {bot_bad}")
print(f"worst frame {worst[0]}: {worst[1]} top / {worst[2]} bottom pixels differ (of 49152 each)")

with open(os.path.join(out, "diff.csv"), "w") as f:
    f.write("frame,top_diff,bottom_diff\n")
    for r in rows:
        f.write("%d,%d,%d\n" % r)

def img(a):
    rgb = np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255], -1).astype(np.uint8)
    return Image.fromarray(rgb)

def side(i, name):
    j = shown[i]
    d = (emu[i] != cli[j])
    mask = np.where(d, 0xFF0000, 0x202020).astype(np.uint32)
    W = Image.new("RGB", (256 * 3 + 16, 384), (40, 40, 40))
    W.paste(img(emu[i]), (0, 0)); W.paste(img(cli[j]), (264, 0)); W.paste(img(mask), (528, 0))
    W.save(os.path.join(out, name))

side(worst[0], "worst.png")
for k in (n // 4, n // 2, 3 * n // 4, n - 1):
    side(rows[k][0], "frame%05d.png" % rows[k][0])
