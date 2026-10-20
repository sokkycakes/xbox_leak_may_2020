# Extract PNGs from .xzp packages into assets/ as .xtx (XTX1 BGRA, like mkassets.py) and .png
import sys, os, struct
sys.path.insert(0, '/home/user/xbox_leak_may_2020/xbcompat/tools/frolickdash/tools')
from xur2xui import xzp_entries
from PIL import Image
import io
out = sys.argv[1]
for pkg in sys.argv[2:]:
    ents = xzp_entries(open(pkg, 'rb').read())
    for name, b in ents.items():
        if '\\' in name or not name.lower().endswith('.png'):
            continue
        im = Image.open(io.BytesIO(b))
        mode = im.mode
        im = im.convert('RGBA')
        w, h = im.size
        open(os.path.join(out, name[:-4] + '.xtx'), 'wb').write(b'XTX1' + struct.pack('<II', w, h) + im.tobytes('raw', 'BGRA'))
        open(os.path.join(out, name), 'wb').write(b)
        print(os.path.basename(pkg), name, mode, w, h)
