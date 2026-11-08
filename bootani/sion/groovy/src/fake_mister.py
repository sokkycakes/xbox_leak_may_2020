#!/usr/bin/env python3
"""A stand-in for the Groovy core: answers INIT and BLIT with 13-byte ACKs,
reassembles frames (raw or LZ4) and saves the last one as a PNG."""
import socket, struct, sys
import lz4.block
from PIL import Image
port = int(sys.argv[1]) if len(sys.argv) > 1 else 32100
out = sys.argv[2] if len(sys.argv) > 2 else 'groovy-last.png'
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', port))
lz4mode, rgbmode, want, buf, frames, peer = 0, 0, 0, b'', 0, None
W = H = 0
def ack(frame=0, vs=0):
    s.sendto(struct.pack('<IHIHB', frame, vs, frame, vs, 0b00000101), peer)
while True:
    d, peer = s.recvfrom(65536)
    if want:
        buf += d
        if len(buf) >= want:
            raw = lz4.block.decompress(buf[:want], uncompressed_size=W * H * bpp) if csize else buf[:want]
            if bpp == 2:
                import numpy as np
                p = np.frombuffer(bytes(raw), '<u2').reshape(H, W)
                rgb = np.stack([(p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3], 2).astype(np.uint8)
                Image.fromarray(rgb).save(out)
            else:
                img = Image.frombytes('RGB', (W, H), bytes(raw))
                b, g, r = img.split()
                Image.merge('RGB', (r, g, b)).save(out)
            frames += 1
            want, buf = 0, b''
        continue
    cmd = d[0]
    if cmd == 2:
        lz4mode = d[1]; rgbmode = d[4]; print('INIT lz4=%d rgb_mode=%d' % (d[1], d[4]), flush=True); ack()
    elif cmd == 3:
        pclk, = struct.unpack('<d', d[1:9]); m = struct.unpack('<8H', d[9:25])
        W, H = m[0], m[4]
        print('SWITCHRES %.3f MHz %s interlace=%d' % (pclk, m, d[25]), flush=True)
    elif cmd == 7:
        frame, field, vs = struct.unpack('<IBH', d[1:8])
        csize = struct.unpack('<I', d[8:12])[0] if len(d) >= 12 and lz4mode else 0
        bpp = 2 if rgbmode == 2 else 3
        want = csize if csize else W * H * bpp
        ack(frame, vs)
    elif cmd == 1:
        print('CLOSE after %d frames' % frames, flush=True); break
