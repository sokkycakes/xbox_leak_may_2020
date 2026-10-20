#!/usr/bin/env python3
"""Builds the Guide overlay's asset directory from the Games for Windows -
LIVE Guide: the XUI packages inside xlive.dll (GFWL-Guide.exe carries the
lifted image, /mnt/project-files/xui360/build/GFWL-Guide.exe).

  mkguide.py GFWL-Guide.exe CLASSES.LOG OUT_DIR [FONT.ttf]

CLASSES.LOG is GFWL-Guide.exe --dump-classes (xui360/classes.log); xurconv.py
(xui360/tools) folds the Guide's own classes (HUDBkgndScene, ...) into the
built-in ones.  xbcompat looks for OUT_DIR as "guide" beside its binary, or
XBCOMPAT_GUIDE_DIR.  Writes hudbkgnd.xui (the frame), hudskin.xui,
GuideMain.xui (MainMenuSignedIn), GuideSettings.xui (Options), every PNG as
.xtx, and font.ttf.
"""
import io, os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'frolickdash', 'tools'))
from xur2xui import xzp_entries  # noqa: E402

XURCONV = os.environ.get('XURCONV', '/mnt/project-files/xui360/tools/xurconv.py')
XUR2XUI = os.path.join(HERE, '..', 'frolickdash', 'tools', 'xur2xui.py')
SCENES = [  # (scene in a package, output name)
    ('hudbkgnd.xur', 'hudbkgnd.xui'),
    ('skin.xur', 'hudskin.xui'),
    ('MainMenuSignedIn.xur', 'GuideMain.xui'),
    ('Options.xur', 'GuideSettings.xui'),
]
CONTROLS = ('XuiControl', 'XuiButton', 'XuiNavButton', 'XuiBackButton', 'XuiLabel', 'XuiCheckbox',
            'XuiRadioButton', 'XuiList', 'XuiCommonList', 'XuiListItem', 'XuiSlider', 'XuiProgressBar', 'XuiEdit')


def gfwl_text(xui):
    """xur2xui's class table has the 360's XuiControl slot order (Text before
    ImagePath); GFWL scenes store their strings in the other slot.  Move a
    control's ImagePath that isn't a file name into Text."""
    def fix(m):
        props = m.group(2)
        im = re.search(r'<ImagePath>(.*?)</ImagePath>', props, re.S)
        if im and not re.search(r'\.(png|xur|dds|jpg)$|://', im.group(1), re.I) and '<Text>' not in props:
            props = props.replace(im.group(0), '<Text>%s</Text>' % im.group(1))
        return m.group(1) + props + m.group(3)
    pat = r'(<(?:%s)>\s*<Properties>)(.*?)(</Properties>)' % '|'.join(CONTROLS)
    return re.sub(pat, fix, xui, flags=re.S)


def main():
    exe, classes, out = sys.argv[1:4]
    font = sys.argv[4] if len(sys.argv) > 4 else '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
    os.makedirs(out, exist_ok=True)
    d = open(exe, 'rb').read()
    offs = [m.start() for m in re.finditer(b'XUIZ\x00\x00\x00\x01', d)] + [len(d)]
    tmp = tempfile.mkdtemp()
    want = dict(SCENES)
    images = 0
    for i in range(len(offs) - 1):
        blob = d[offs[i]:offs[i + 1]]
        entries = xzp_entries(blob)
        for name, data in entries.items():
            if '\\' in name or not name.lower().endswith('.png'):
                continue
            from PIL import Image
            im = Image.open(io.BytesIO(data)).convert('RGBA')
            w, h = im.size
            with open(os.path.join(out, name[:-4] + '.xtx'), 'wb') as f:
                f.write(b'XTX1' + struct.pack('<II', w, h) + im.tobytes('raw', 'BGRA'))
            images += 1
        scenes = [n for n in entries if n in want]
        if not scenes:
            continue
        raw = os.path.join(tmp, 'p%02d.xzp' % i)
        conv = os.path.join(tmp, 'c%02d.xzp' % i)
        open(raw, 'wb').write(blob)
        subprocess.run([sys.executable, XURCONV, classes, '--xzp', raw, conv], check=True,
                       stdout=subprocess.DEVNULL)
        for n in scenes:
            dst = os.path.join(out, want.pop(n))
            subprocess.run([sys.executable, XUR2XUI, conv, n, dst], check=True, stdout=subprocess.DEVNULL)
            text = gfwl_text(open(dst).read())
            open(dst, 'w').write(text)
    if want:
        sys.exit('not found: ' + ', '.join(want))
    with open(font, 'rb') as f, open(os.path.join(out, 'font.ttf'), 'wb') as g:
        g.write(f.read())
    print('%s: %d scenes, %d images' % (out, len(SCENES), images))


if __name__ == '__main__':
    main()
