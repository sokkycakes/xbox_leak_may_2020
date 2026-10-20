#!/usr/bin/env python3
"""Xbox 360 XUIB v5 scene (.xur, big-endian) to the .xui XML the Frolick
dashboard's XUI runtime loads (the format the XUI Tool saves).

    xur2xui.py SCENE.xur OUT.xui
    xur2xui.py PACKAGE.xzp SCENE.xur OUT.xui
    xur2xui.py --list PACKAGE.xzp

The property layout comes from xui/classes.json, so the scene's classes must
be the engine's own (XuiScene, XuiList, ...): a dashboard package is first
run through xurconv.py (xui360/tools), which folds dash.xex's classes into
those and drops their private property levels.

What is kept: every property the runtime knows, nested objects (Fill,
Stroke, Gradient), figure points, named frames and keyframe tracks of plain
values.  Tracks on object or custom properties (the file stores them as
opaque words) are dropped, with a note on stderr.
"""
import json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TBool, TInt, TUint, TFloat, TString, TColor, TVector, TQuat, TObject, TCustom = range(1, 11)


def load_classes(path=os.path.join(HERE, '..', 'xui', 'classes.json')):
    return json.load(open(path))


# ---- XUIZ -------------------------------------------------------------------

def xzp_entries(d):
    assert d[:4] == b'XUIZ', 'not an XUIZ package'
    ver, dsize, _, hsize, count = struct.unpack('>IIIIH', d[4:22])
    p, ents = 22, []
    for i in range(count):
        s, o, n = struct.unpack('>IIB', d[p:p + 9]); p += 9
        ents.append((d[p:p + n * 2].decode('utf-16-be'), s, o)); p += n * 2
    base = 0x16 + hsize
    return {n: d[base + o:base + o + s] for n, s, o in ents}


# ---- XUIB -------------------------------------------------------------------

class Reader:
    def __init__(self, data, classes):
        self.classes = classes
        assert data[:4] == b'XUIB', 'not an XUIB scene'
        ver, flags = struct.unpack('>II', data[4:12])
        assert ver == 5, 'XUIB version %d' % ver
        nsec = struct.unpack('>H', data[0x12:0x14])[0]
        table = 0x14 + (0x28 if flags & 1 else 0)
        self.strs, self.vects, self.quats, self.cust, self.data = [], [], [], b'', b''
        for i in range(nsec):
            tag, off, size = struct.unpack_from('>4sII', data, table + i * 12)
            sec = data[off:off + size]
            if tag == b'STRN':
                q = 0
                while q + 2 <= len(sec):
                    n = struct.unpack_from('>H', sec, q)[0]; q += 2
                    self.strs.append(sec[q:q + n * 2].decode('utf-16-be', 'replace')); q += n * 2
            elif tag == b'VECT':
                self.vects = [struct.unpack_from('>3f', sec, q) for q in range(0, len(sec) - 11, 12)]
            elif tag == b'QUAT':
                self.quats = [struct.unpack_from('>4f', sec, q) for q in range(0, len(sec) - 15, 16)]
            elif tag == b'CUST':
                self.cust = sec
            elif tag == b'DATA':
                self.data = sec
        self.p = 0
        self.dropped = []

    def u8(self):
        v = self.data[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('>H', self.data, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('>I', self.data, self.p)[0]; self.p += 4; return v

    def s(self, i):
        return self.strs[i - 1] if 0 < i <= len(self.strs) else ''

    def chain(self, cls):
        return self.classes.get(cls)

    def element(self):
        cls = self.s(self.u16())
        flags = self.u8()
        assert cls and not flags & ~7, 'bad element header at %d' % self.p
        e = {'class': cls, 'props': [], 'children': [], 'frames': [], 'timelines': []}
        chain = self.chain(cls)
        if flags & 1:
            assert chain is not None, 'unknown class %s with properties' % cls
            self.u16()
            for level in chain:
                self.level(level, e['props'])
        if flags & 2:
            n = self.u32()
            assert n < 1 << 14, 'child count'
            for i in range(n):
                e['children'].append(self.element())
        if flags & 4:
            self.timelines(e)
        return e

    def level(self, props, out):
        hdr = self.u8()
        mask = 0
        for i in range(hdr & 7):
            mask = mask << 8 | self.u8()
        for i, pd in enumerate(props):
            if not mask >> i & 1:
                continue
            if pd['f'] & 1:
                for k in range(self.count()):
                    out.append((pd, self.value(pd)))
            else:
                out.append((pd, self.value(pd)))

    def count(self):
        b = self.u8()
        if b < 0x80:
            return b
        v = 0
        for i in range(b & 0x7f):
            v = v << 8 | self.u8()
        return v

    def value(self, pd):
        t = pd['t']
        if t == TBool: return self.u8() != 0
        if t == TInt: return struct.unpack('>i', struct.pack('>I', self.u32()))[0]
        if t == TUint: return self.u32()
        if t == TFloat: return struct.unpack('>f', struct.pack('>I', self.u32()))[0]
        if t == TString: return self.s(self.u16())
        if t == TColor: return self.u32()
        if t == TVector:
            i = self.u32(); return self.vects[i] if i < len(self.vects) else (0.0, 0.0, 0.0)
        if t == TQuat:
            i = self.u32(); return self.quats[i] if i < len(self.quats) else (0.0, 0.0, 0.0, 1.0)
        if t == TObject:
            self.u16()
            sub = []
            self.level(pd.get('s', []), sub)
            return sub
        if t == TCustom:
            return self.custom(self.u32(), pd['n'])
        raise ValueError('property %s has type %d' % (pd['n'], t))

    def custom(self, off, name):
        c = self.cust
        if off + 4 > len(c):
            return None
        size = struct.unpack_from('>I', c, off)[0]
        b = c[off + 4:off + 4 + size]
        if name != 'Points' or len(b) < 12:
            return b
        w, h, n = struct.unpack_from('>ffI', b)
        pts = []
        for i in range(n):
            q = 12 + i * 24
            if q + 24 > len(b):
                break
            pts.append(struct.unpack_from('>6f', b, q))
        return ('points', w, h, pts)

    def timelines(self, e):
        for i in range(self.u32()):
            name = self.s(self.u16()); time = self.u32(); cmd = self.u8(); target = self.s(self.u16())
            e['frames'].append((name, time, cmd, target))
        if not e['children']:
            return
        for i in range(self.u32()):
            target = self.s(self.u16())
            child = next((c for c in e['children'] if elem_id(c) == target), None)
            np = self.u32()
            paths = [self.prop_path(child) for k in range(np)]
            keys = []
            for k in range(self.u32()):
                time = self.u32(); interp = self.u8()
                ease = struct.unpack('>3b', bytes([self.u8(), self.u8(), self.u8()]))
                vals = []
                for path, pd in paths:
                    if pd is None or pd['t'] in (TObject, TCustom):
                        self.u32(); vals.append(None)
                    else:
                        vals.append(self.value(pd))
                keys.append((time, interp, ease, vals))
            e['timelines'].append((target, paths, keys))

    def prop_path(self, target):
        b = self.u8()
        depth, indexed = b & 0x7f, b & 0x80
        assert depth, 'empty property path'
        up, idx = self.u8(), self.u8()
        steps = [idx] + [self.u8() for i in range(depth - 1)]
        index = self.u32() if indexed else -1
        if target is None:
            return (None, None)
        chain = self.chain(target['class'])
        if chain is None or up >= len(chain):
            return (None, None)
        props = chain[len(chain) - 1 - up]
        names, pd = [], None
        for i, s in enumerate(steps):
            if s >= len(props):
                return (None, None)
            pd = props[s]
            names.append(pd['n'])
            if i < len(steps) - 1:
                props = pd.get('s', [])
        path = '.'.join(names) + ('[%d]' % index if index >= 0 else '')
        return (path, pd)


def elem_id(e):
    for pd, v in e['props']:
        if pd['n'] == 'Id':
            return v
    return ''


# ---- XML --------------------------------------------------------------------

def esc(s):
    return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def fmt(pd, v):
    t = pd['t']
    if t == TBool: return 'true' if v else 'false'
    if t in (TInt, TUint): return str(v)
    if t == TFloat: return num(v)
    if t == TString: return esc(v)
    if t == TColor: return '0x%08x' % v
    if t == TVector: return ','.join(num(x) for x in v)
    if t == TQuat: return ','.join(num(x) for x in v)
    if t == TCustom:
        if isinstance(v, tuple) and v[0] == 'points':
            _, w, h, pts = v
            return num(w) + ',' + num(h) + ';' + ';'.join(','.join(num(x) for x in p) for p in pts)
        return None
    return None


def num(x):
    s = '%.6g' % x
    return '0' if s == '-0' else s


def write_props(out, props, ind):
    for pd, v in props:
        if pd['t'] == TObject:
            out.append('%s<%s>' % (ind, pd['n']))
            write_props(out, v, ind + ' ')
            out.append('%s</%s>' % (ind, pd['n']))
            continue
        s = fmt(pd, v)
        if s is None:
            continue
        out.append('%s<%s>%s</%s>' % (ind, pd['n'], s, pd['n']))


CMD = {0: 'Play', 1: 'Stop', 2: 'GoTo', 3: 'GoToAndPlay', 4: 'GoToAndStop'}


def write_element(out, e, ind, notes):
    out.append('%s<%s>' % (ind, e['class']))
    if e['props']:
        out.append('%s<Properties>' % ind)
        write_props(out, e['props'], ind + ' ')
        out.append('%s</Properties>' % ind)
    for c in e['children']:
        write_element(out, c, ind, notes)
    if e['frames'] or e['timelines']:
        out.append('%s<Timelines>' % ind)
        if e['frames']:
            out.append('%s<NamedFrames>' % ind)
            for name, time, cmd, target in e['frames']:
                out.append('%s<NamedFrame><Name>%s</Name><Time>%d</Time><Command>%s</Command><CommandParams>%s</CommandParams></NamedFrame>'
                           % (ind, esc(name), time, CMD.get(cmd, 'Play'), esc(target)))
            out.append('%s</NamedFrames>' % ind)
        for target, paths, keys in e['timelines']:
            keep = [i for i, (path, pd) in enumerate(paths) if path is not None and pd['t'] not in (TObject, TCustom)]
            for i, (path, pd) in enumerate(paths):
                if i not in keep:
                    notes.append('%s: dropped track %s' % (target, path or '?'))
            if not keep:
                continue
            out.append('%s<Timeline>' % ind)
            out.append('%s <Id>%s</Id>' % (ind, esc(target)))
            for i in keep:
                out.append('%s <TimelineProp>%s</TimelineProp>' % (ind, paths[i][0]))
            for time, interp, ease, vals in keys:
                out.append('%s <KeyFrame><Time>%d</Time><Interpolation>%d</Interpolation><EaseIn>%d</EaseIn><EaseOut>%d</EaseOut><EaseScope>%d</EaseScope>'
                           % (ind, time, interp, ease[0], ease[1], ease[2]))
                for i in keep:
                    out.append('%s  <Prop>%s</Prop>' % (ind, fmt(paths[i][1], vals[i])))
                out.append('%s </KeyFrame>' % ind)
            out.append('%s</Timeline>' % ind)
        out.append('%s</Timelines>' % ind)
    out.append('%s</%s>' % (ind, e['class']))


def convert(data, classes):
    r = Reader(data, classes)
    root = r.element()
    assert r.p == len(r.data), '%d trailing bytes' % (len(r.data) - r.p)
    out, notes = [], []
    write_element(out, root, '', notes)
    return '\n'.join(out) + '\n', notes


def main():
    a = sys.argv[1:]
    if a and a[0] == '--list':
        for name, b in sorted(xzp_entries(open(a[1], 'rb').read()).items()):
            print('%8d  %s' % (len(b), name))
        return
    if len(a) == 3:
        data = xzp_entries(open(a[0], 'rb').read())[a[1]]
        dst = a[2]
    elif len(a) == 2:
        data, dst = open(a[0], 'rb').read(), a[1]
    else:
        sys.exit(__doc__)
    xml, notes = convert(data, load_classes())
    open(dst, 'w').write(xml)
    for n in notes:
        print('note:', n, file=sys.stderr)
    print('wrote', dst, len(xml), 'bytes')


if __name__ == '__main__':
    main()
