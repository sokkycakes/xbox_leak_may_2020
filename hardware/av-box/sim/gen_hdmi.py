#!/usr/bin/env python3
"""Make an HDMI 2880x480i (VIC 10) TMDS symbol stream from a 720x480 picture.

Each frame line: sync 248, back porch 228, active 2880, front porch 76 (3432
clocks at 54 MHz). Negative syncs, limited range RGB, every pixel sent 4
times, a video preamble + guard band before each active line and a data
island across every hsync edge (as i915 sends audio). Each lane gets its own
bit offset so the receiver has to find word alignment and lane skew itself.

Output: little-endian uint32 per clock = lane0 | lane1 << 10 | lane2 << 20.
"""
import sys, random, struct
import numpy as np
from PIL import Image

LINE, SYNC, BP, ACT, FP = 3432, 248, 228, 2880, 76
AST = SYNC + BP            # active start within the line
CTRL = {0: 0b1101010100, 1: 0b0010101011, 2: 0b0101010100, 3: 0b1010101011}
TERC4 = [0b1010011100, 0b1001100011, 0b1011100100, 0b1011100010,
         0b0101110001, 0b0100011110, 0b0110001110, 0b0100111100,
         0b1011001100, 0b0100111001, 0b0110011100, 0b1011000110,
         0b1010001110, 0b1001110001, 0b0101100011, 0b1011000011]
VGB = (0b1011001100, 0b0100110011, 0b1011001100)
DGB = 0b0100110011

def tmds_encode(d, cnt):
    n1 = bin(d).count('1')
    qm = d & 1
    xnor = n1 > 4 or (n1 == 4 and (d & 1) == 0)
    for i in range(1, 8):
        b = ((qm >> (i - 1)) & 1) ^ ((d >> i) & 1)
        if xnor: b ^= 1
        qm |= b << i
    q8 = 0 if xnor else 1
    n1q = bin(qm).count('1'); n0q = 8 - n1q
    if cnt == 0 or n1q == n0q:
        q9 = 1 - q8
        q = (q9 << 9) | (q8 << 8) | (qm if q8 else (~qm & 0xff))
        cnt += (n1q - n0q) if q8 else (n0q - n1q)
    elif (cnt > 0 and n1q > n0q) or (cnt < 0 and n0q > n1q):
        q = (1 << 9) | (q8 << 8) | (~qm & 0xff)
        cnt += 2 * q8 + (n0q - n1q)
    else:
        q = (q8 << 8) | qm
        cnt += -2 * (1 - q8) + (n1q - n0q)
    return q, cnt

def main(img_path, out_path, frames=2, offsets=(3, 7, 0)):
    im = Image.open(img_path).convert('RGB').resize((720, 480), Image.LANCZOS)
    a = np.asarray(im).astype(np.float64)
    lim = np.clip(np.round(16 + a * 219 / 255), 16, 235).astype(np.uint8)
    rng = random.Random(1)
    cache = {}
    def enc(d, cnt):
        k = (d, cnt)
        if k not in cache: cache[k] = tmds_encode(d, cnt)
        return cache[k]

    total = LINE * 525 * frames
    lanes = np.zeros((3, total), dtype=np.uint16)
    T = 0
    for f in range(frames):
        for L in range(1, 526):
            base = (L - 1) * LINE
            if 22 <= L <= 261: row = 2 * (L - 22)
            elif 285 <= L <= 524: row = 2 * (L - 285) + 1
            else: row = None
            cnt = [0, 0, 0]
            for t in range(LINE):
                ft = base + t
                hs = 0 if t < SYNC else 1                        # negative sync
                vs = 0 if (3 * LINE <= ft < 6 * LINE or
                           265 * LINE + 1716 <= ft < 268 * LINE + 1716) else 1
                active = row is not None and AST <= t < AST + ACT
                if active:
                    px = lim[row, (t - AST) // 4]
                    s = []
                    for ln, val in ((0, px[2]), (1, px[1]), (2, px[0])):
                        q, cnt[ln] = enc(int(val), cnt[ln])
                        s.append(q)
                elif row is not None and AST - 2 <= t < AST:
                    s = list(VGB)
                elif row is not None and AST - 10 <= t < AST - 2:
                    s = [CTRL[(vs << 1) | hs], CTRL[1], CTRL[0]]   # video preamble
                else:
                    # data island across the hsync edge: preamble 8, guard 2,
                    # 32 TERC4 chars, guard 2 (positions wrap round the line)
                    p = (t - (LINE - 20)) % LINE
                    if p < 8:
                        s = [CTRL[(vs << 1) | hs], CTRL[1], CTRL[1]]
                    elif p < 10 or 42 <= p < 44:
                        s = [TERC4[0b1100 | (vs << 1) | hs], DGB, DGB]
                    elif p < 42:
                        b2 = 0 if p == 10 else 1
                        s = [TERC4[(rng.getrandbits(1) << 3) | (b2 << 2) | (vs << 1) | hs],
                             TERC4[rng.getrandbits(4)], TERC4[rng.getrandbits(4)]]
                    else:
                        s = [CTRL[(vs << 1) | hs], CTRL[0], CTRL[0]]
                    cnt = [0, 0, 0]
                lanes[0, T], lanes[1, T], lanes[2, T] = s
                T += 1
        print('frame', f, 'done', file=sys.stderr)

    # serialise each lane LSB first, shift by its bit offset, regroup into words
    words = np.zeros(total, dtype=np.uint32)
    for ln in range(3):
        bits = ((lanes[ln][:, None] >> np.arange(10)) & 1).astype(np.uint8).ravel()
        off = offsets[ln]
        bits = np.concatenate([np.zeros(off, np.uint8), bits])[:total * 10]
        w = (bits.reshape(-1, 10) << np.arange(10)).sum(1).astype(np.uint32)
        words |= w << (10 * ln)
    words.astype('<u4').tofile(out_path)
    Image.fromarray(lim).save(out_path + '.src.png')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
