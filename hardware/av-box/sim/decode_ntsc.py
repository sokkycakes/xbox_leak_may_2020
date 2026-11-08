#!/usr/bin/env python3
"""A software TV: decode the composite (or S-video) samples back to a picture.

Works only from the analog waveform (54 MHz, 8-bit DAC codes): finds sync
pulses by level, tells broad pulses from line syncs by width, finds each
field's parity from where its broad pulses start, locks to the colour burst
on every line and demodulates chroma. Writes a 720x480 picture.
"""
import sys
import numpy as np
from PIL import Image

LINE, HALF = 3432, 1716
FS = 54e6
FSC = FS * 227.5 / LINE
AST = 476                       # active video starts 476 clocks after sync
BLANK, SETUP0, GAIN = 57, 67, 131 / 256   # encoder levels (see ntsc_enc.v)

def boxf(x, n):
    k = np.ones(n) / n
    return np.convolve(x, k, mode='same')

def main(path, out_png, mode='comp', src_png=None):
    rec = np.fromfile(path, dtype=np.uint8).reshape(-1, 8)
    comp = rec[:, 0].astype(float) if mode == 'comp' else rec[:, 1].astype(float)
    chroma = None if mode == 'comp' else rec[:, 2].astype(float) - 128
    # sync slicer at half the sync depth
    low = comp < BLANK / 2
    edges = np.flatnonzero(np.diff(low.astype(int)) == 1) + 1
    ends = np.flatnonzero(np.diff(low.astype(int)) == -1) + 1
    ends = ends[np.searchsorted(ends, edges[0]):]
    n = min(len(edges), len(ends))
    widths = ends[:n] - edges[:n]
    edges = edges[:n]
    broad = edges[widths > 1000]
    hsyncs = edges[(widths > 200) & (widths < 320)]
    # field starts = first broad pulse of each group
    fstarts = [b for i, b in enumerate(broad) if i == 0 or b - broad[i - 1] > 4 * LINE]
    print('fields found:', len(fstarts))
    frame = np.zeros((480, 720, 3))
    used = set()
    for fs in fstarts[-2:]:   # the last full frame
        prev = hsyncs[hsyncs < fs]
        phase = (fs - prev[-1]) % LINE if len(prev) else 0
        second = abs(phase - HALF) < 200
        # line L start relative to the first broad pulse
        if not second:
            lines = range(22, 262); l0 = fs - 3 * LINE; first = 22
        else:
            lines = range(285, 525); l0 = fs - 265 * LINE - HALF; first = 285
        used.add(second)
        for L in lines:
            t0 = l0 + (L - 1) * LINE
            # snap to the nearest real hsync, as a TV's flywheel would
            j = np.searchsorted(hsyncs, t0 - 20)
            if j < len(hsyncs) and abs(hsyncs[j] - t0) < 20: t0 = hsyncs[j]
            seg = comp[t0:t0 + LINE]
            if len(seg) < LINE: continue
            n_abs = np.arange(t0, t0 + LINE)
            w = 2 * np.pi * FSC / FS * n_abs
            c = seg if chroma is None else chroma[t0:t0 + LINE]
            # burst: measure its phase, it sits at -U (180 degrees)
            bs = slice(296, 412)
            bi = np.mean(c[bs] * np.sin(w[bs])); bq = np.mean(c[bs] * np.cos(w[bs]))
            ang = np.arctan2(bq, bi) - np.pi      # rotation that puts burst at -U
            if chroma is None:
                yl = boxf(seg, 15)                # 15-sample box nulls the subcarrier
                cc = seg - yl
            else:
                yl, cc = seg, c
            U = 2 * boxf(cc * np.sin(w + ang), 31)
            V = 2 * boxf(cc * np.cos(w + ang), 31)
            act = slice(AST, AST + 2880)
            Y = (yl[act] - SETUP0) / GAIN / 255
            U = U[act] / GAIN / 255; V = V[act] / GAIN / 255
            R = Y + V / 0.877; B = Y + U / 0.492
            G = (Y - 0.299 * R - 0.114 * B) / 0.587
            rgb = np.stack([R, G, B], 1).reshape(720, 4, 3).mean(1)
            row = 2 * (L - first) + (1 if second else 0)
            frame[row] = rgb
    print('fields used (second?):', used)
    out = np.clip(frame * 255, 0, 255).astype(np.uint8)
    Image.fromarray(out).save(out_png)
    if src_png:
        src = np.asarray(Image.open(src_png).convert('RGB')).astype(float)
        full = np.clip((src - 16) * 255 / 219, 0, 255)
        mse = np.mean((out.astype(float) - full) ** 2)
        print('PSNR vs source: %.1f dB' % (10 * np.log10(255 ** 2 / mse)))
        side = np.concatenate([full.astype(np.uint8), out], 1)
        Image.fromarray(side).save(out_png.replace('.png', '-side.png'))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else 'comp',
         sys.argv[4] if len(sys.argv) > 4 else None)
