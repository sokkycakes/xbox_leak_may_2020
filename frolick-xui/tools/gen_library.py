#!/usr/bin/env python3
"""Generate assets/xui/library.xui and assets/xui/frolick_skin.xui.

The scenes are plain .xui XML and can be edited by hand afterwards; this
script exists so the rounded shapes and their Bezier points stay exact.
"""
import os, sys

K = 0.5523  # circle Bezier constant


def rrect(w, h, r=(0, 0, 0, 0)):
    """Points string for a rounded rectangle; r = (tl, tr, br, bl)."""
    tl, tr, br, bl = r
    pts = []
    def corner(px, py, c1, c2, nx, ny):
        pts.append('%g,%g,%g,%g,%g,%g' % (px, py, c1[0], c1[1], c2[0], c2[1]))
    # top edge start
    pts.append('%g,%g' % (tl, 0))
    if tr:
        corner(w - tr, 0, (w - tr + K * tr, 0), (w, tr - K * tr), w, tr)
    else:
        pts.append('%g,%g' % (w, 0))
    if tr:
        pts.append('%g,%g' % (w, tr))
    if br:
        corner(w, h - br, (w, h - br + K * br), (w - br + K * br, h), w - br, h)
        pts.append('%g,%g' % (w - br, h))
    else:
        pts.append('%g,%g' % (w, h))
    if bl:
        corner(bl, h, (bl - K * bl, h), (0, h - bl + K * bl), 0, h - bl)
        pts.append('%g,%g' % (0, h - bl))
    else:
        pts.append('%g,%g' % (0, h))
    if tl:
        corner(0, tl, (0, tl - K * tl), (tl - K * tl, 0), tl, 0)
    else:
        pts.append('%g,%g' % (0, 0))
    # drop duplicate first point when tl == 0
    if not tl and pts[0] == '0,0':
        pts = pts[1:]
    return '%g,%g;' % (w, h) + ';'.join(pts)


def props(d, ind):
    out = []
    for k, v in d.items():
        if isinstance(v, dict):
            out.append('%s<%s>\n%s%s</%s>' % (ind, k, props(v, ind + ' '), ind, k))
        elif isinstance(v, list):
            out += ['%s<%s>%s</%s>' % (ind, k, x, k) for x in v]
        else:
            out.append('%s<%s>%s</%s>' % (ind, k, v, k))
    return '\n'.join(out) + '\n'


def el(cls, p, kids=(), timelines=''):
    body = '<Properties>\n%s</Properties>\n' % props(p, ' ')
    return '<%s>\n%s%s%s</%s>\n' % (cls, body, ''.join(kids), timelines, cls)


def fig(id_, x, y, w, h, fill, radius=(0, 0, 0, 0), stroke=None, **extra):
    p = {'Id': id_, 'Width': w, 'Height': h, 'Position': '%g,%g,0' % (x, y)}
    p.update(extra)
    if stroke:
        p['Stroke'] = {'StrokeWidth': stroke[0], 'StrokeColor': stroke[1]}
    p['Fill'] = fill
    p['Closed'] = 'true'
    p['Points'] = rrect(w, h, radius)
    return el('XuiFigure', p)


def solid(c):
    return {'FillType': 1, 'FillColor': c}


def linear(stops, rot=0):
    g = {'NumStops': len(stops), 'StopColor': [c for _, c in stops], 'StopPos': [p for p, _ in stops]}
    f = {'FillType': 2, 'Gradient': g}
    if rot:
        f['Rotation'] = rot
    return f


def radial(stops):
    g = {'Radial': 'true', 'NumStops': len(stops), 'StopColor': [c for _, c in stops], 'StopPos': [p for p, _ in stops]}
    return {'FillType': 3, 'Gradient': g}


def text(id_, x, y, w, h, s, size, color='0xffffffff', style=0x4000, shadow='0x80000000'):
    return el('XuiText', {'Id': id_, 'Width': w, 'Height': h, 'Position': '%g,%g,0' % (x, y),
                          'Text': s, 'TextColor': color, 'DropShadowColor': shadow,
                          'PointSize': size, 'Font': 'Convection', 'TextStyle': style})


def image(id_, x, y, w, h, path, mode=4):
    return el('XuiImage', {'Id': id_, 'Width': w, 'Height': h, 'Position': '%g,%g,0' % (x, y),
                           'ImagePath': path, 'SizeMode': mode})


def timelines(frames, tracks):
    nf = ''.join('<NamedFrame><Name>%s</Name><Time>%d</Time><Command>%s</Command><CommandParams>%s</CommandParams></NamedFrame>\n'
                 % (n, t, c, a) for n, t, c, a in frames)
    tl = ''
    for target, propnames, keys in tracks:
        tl += '<Timeline>\n<Id>%s</Id>\n' % target
        tl += ''.join('<TimelineProp>%s</TimelineProp>\n' % p for p in propnames)
        for t, interp, vals in keys:
            tl += '<KeyFrame><Time>%d</Time><Interpolation>%d</Interpolation>%s</KeyFrame>\n' % (
                t, interp, ''.join('<Prop>%s</Prop>' % v for v in vals))
        tl += '</Timeline>\n'
    return '<Timelines>\n<NamedFrames>\n%s</NamedFrames>\n%s</Timelines>\n' % (nf, tl)


# ---------------------------------------------------------------- skin ----
# One visual, FrolickTile: a glossy 360-style tile that lights up green when
# focused, with the Blades-style shine sweeping across while it holds focus.
TW, TH = 200, 160  # design size; anchors stretch it to each tile
GREEN = '0xffd9d9d9'  # focus accent (light grey from the Figma panel)
tile_kids = [
    fig('halo', -4, -4, TW + 8, TH + 8, {'FillType': 0}, radius=(11, 11, 11, 11), stroke=(3, '0x70d9d9d9'),
        Anchor=15, Opacity=0),
    fig('bg', 0, 0, TW, TH, {'FillType': 0}, radius=(8, 8, 8, 8), stroke=(2, '0xff404040'), Anchor=15),
    fig('gloss', 2, 2, TW - 4, TH * 0.45, linear([(0, '0x57ffffff'), (1, '0x00ffffff')], rot=90), radius=(7, 7, 0, 0), Anchor=7),
    fig('glow', 0, 0, TW, TH, linear([(0, '0x30ffffff'), (0.5, '0x00ffffff'), (1, '0x20d9d9d9')], rot=90),
        radius=(8, 8, 8, 8), stroke=(3, GREEN), Anchor=15, Opacity=0),
    el('XuiGroup', {'Id': 'shineClip', 'Width': TW, 'Height': TH, 'Anchor': 15, 'ClipChildren': 'true'}, [
        fig('shine', -160, 0, 120, TH, linear([(0, '0x00ffffff'), (0.5, '0x70ffffff'), (1, '0x00ffffff')]),
            Anchor=10, Show='false'),
    ], timelines([('Normal', 0, 'stop', '')], [])),
]
tile_tl = timelines(
    [('KillFocus', 0, 'play', ''), ('Normal', 8, 'play', ''), ('EndNormal', 9, 'stop', ''),
     ('Focus', 10, 'play', ''), ('FocusLoop', 18, 'play', ''), ('EndFocus', 120, 'gotoandplay', 'FocusLoop'),
     ('Press', 130, 'play', ''), ('EndPress', 140, 'gotoandplay', 'FocusLoop')],
    [('halo', ['Opacity'], [(0, 0, [1]), (8, 0, [0]), (10, 0, [0]), (18, 0, [1]), (69, 2, [0.55]), (120, 2, [1]),
                             (130, 0, [1]), (140, 0, [1])]),
     ('glow', ['Opacity'], [(0, 0, [1]), (8, 0, [0]), (10, 0, [0]), (18, 0, [1]), (120, 0, [1]),
                             (130, 0, [1]), (133, 0, [0.4]), (140, 0, [1])]),
     ('shineClip', ['Opacity'], [(0, 0, [0]), (18, 0, [0]), (20, 0, [1]), (120, 0, [1]), (130, 0, [1])]),
     ])
# The shine moves inside its clip group; put that track on the group.
tile_kids[4] = el('XuiGroup', {'Id': 'shineClip', 'Width': TW, 'Height': TH, 'Anchor': 15, 'ClipChildren': 'true'}, [
    fig('shine', -160, 0, 120, TH, linear([(0, '0x00ffffff'), (0.5, '0x70ffffff'), (1, '0x00ffffff')]), Anchor=10),
], timelines([('Sweep', 0, 'play', ''), ('EndSweep', 100, 'gotoandplay', 'Sweep')],
             [('shine', ['Position'], [(0, 0, ['-160,0,0']), (40, 2, ['420,0,0']), (100, 0, ['420,0,0'])])]))
skin = '<XuiCanvas version="000c">\n' + el('XuiScene', {'Id': 'FrolickSkin', 'Width': 1280, 'Height': 720}, [
    el('XuiVisual', {'Id': 'FrolickTile', 'Width': TW, 'Height': TH}, tile_kids, tile_tl),
]) + '</XuiCanvas>\n'

# --------------------------------------------------------------- scene ----
W, H = 586, 402
kids = [
    # Card: an Xbox-green blade with a dark rim, like the Blades "games" blade.
    fig('card', 0, 0, W, H, solid('0xff55534e'), radius=(36, 36, 36, 36), stroke=(6, '0xff898989')),
    fig('cardSheen', 3, 3, W - 6, H - 6, linear([(0, '0x4dd9d9d9'), (0.97, '0x4d000000'), (1, '0x00000000')], rot=90),
        radius=(33, 33, 33, 33)),
    # tile backgrounds sit under the buttons; the FrolickTile visual adds the stroke, gloss and focus
    fig('spotBg', 16, 16, 330, 370, linear([(0, '0xff6a6864'), (0.3, '0xff55534e'), (1, '0xff3e3d39')], rot=90),
        radius=(8, 8, 8, 8)),
    fig('exploreBg', 352, 16, 218, 180, solid('0xff737373'), radius=(8, 8, 8, 8)),
    fig('exploreGlow', 352, 16, 218, 180, dict(radial([(0, '0xff505899'), (0.55, '0xff4a5272'), (1, '0xff55534e')]), Translation='-94,79,0', Scale='2,2,1'),
        radius=(8, 8, 8, 8), Opacity=0.9),
    fig('exploreShade', 352, 16, 218, 180, linear([(0, '0x00575757'), (0.59, '0x00575757'), (0.7, '0x80575757'), (1, '0xcc000000')], rot=90),
        radius=(8, 8, 8, 8)),
    fig('dlBg', 352, 206, 218, 180, solid('0xff737373'), radius=(8, 8, 8, 8)),
    fig('dlGlow', 352, 206, 218, 180, dict(radial([(0, '0xffbbbb00'), (1, '0xff41433e')]), Translation='-94,-62,0', Scale='2,2,1'), radius=(8, 8, 8, 8), Opacity=0.6),
    fig('dlShade', 352, 206, 218, 180, linear([(0, '0x00000000'), (0.7, '0x00000000'), (1, '0xcc000000')], rot=90),
        radius=(8, 8, 8, 8)),
]
# Tiles are XuiButtons drawn with FrolickTile; their children sit on top.
def tile(id_, x, y, w, h, nav, children):
    p = {'Id': id_, 'Width': w, 'Height': h, 'Position': '%g,%g,0' % (x, y), 'Visual': 'FrolickTile'}
    p.update(nav)
    return el('XuiButton', p, children)

DARK = '0xffffffff'
kids.append(tile('tileSpotlight', 16, 16, 330, 370, {'NavRight': 'tileExplore'}, [
    fig('coverFrame', 95, 34, 140, 140, solid('0xffd9d9d9'), radius=(10, 10, 10, 10),
        stroke=(2, '0xff000000')),
    image('cover', 100, 39, 130, 130, 'cover.png'),
    text('title', 16, 196, 298, 36, 'Stiletto Swift', 26, DARK, 0x400, '0'),
    text('subtitle', 16, 234, 298, 28, 'spotlight + featured', 18, '0xffd2d2d2', 0x400, '0'),
    image('glyphA', 118, 292, 40, 40, 'glyph_a.png'),
    text('playLabel', 162, 297, 120, 32, 'play', 22, DARK, 0, '0'),
]))
kids.append(tile('tileExplore', 352, 16, 218, 180, {'NavLeft': 'tileSpotlight', 'NavDown': 'tileDownloads'}, [
    text('exploreLabel', 16, 18, 190, 40, 'explore', 30, DARK, 0, '0'),
    text('exploreSub', 16, 60, 190, 50, 'browse every system and core', 15, '0xffd2d2d2', 0, '0'),
    image('glyphX', 16, 132, 32, 32, 'glyph_x.png'),
    text('exploreHint', 54, 134, 150, 28, 'catalog', 17, '0xffd2d2d2', 0, '0'),
]))
kids.append(tile('tileDownloads', 352, 206, 218, 180, {'NavLeft': 'tileSpotlight', 'NavUp': 'tileExplore'}, [
    text('dlLabel', 16, 18, 190, 40, 'downloads', 30, DARK, 0, '0'),
    text('dlTitle', 16, 70, 150, 24, 'Stiletto Swift', 16, '0xffd2d2d2', 0x10, '0'),
    text('dlPct', 150, 70, 52, 24, '70%', 16, '0xffd2d2d2', 0x200 | 0x10, '0'),
    fig('dlTrack', 16, 98, 186, 14, solid('0xff727272'), radius=(7, 7, 7, 7),
        stroke=(2, '0xff5c5c5c')),
    fig('dlFill', 16, 98, 130, 14, linear([(0, '0xff6cf98d'), (0.4, '0xff2df65c'), (1, '0xff2df65c')], rot=90),
        radius=(7, 7, 7, 7)),
    image('glyphY', 16, 132, 32, 32, 'glyph_y.png'),
    text('dlHint', 54, 134, 150, 28, 'queue', 17, '0xffd2d2d2', 0, '0'),
]))
intro = timelines([('Intro', 0, 'play', ''), ('EndIntro', 12, 'stop', '')],
                  [('LibraryPanel', ['Opacity', 'Scale'], [(0, 2, [0, '0.94,0.94,1']), (12, 0, [1, '1,1,1'])])])
scene = '<XuiCanvas version="000c">\n' + el('XuiCanvas', {'Id': 'canvas', 'Width': 1024, 'Height': 512}, [
    el('XuiScene', {'Id': 'LibraryPanel', 'Width': W, 'Height': H, 'Position': '219,55,0',
                    'Pivot': '%g,%g,0' % (W / 2, H / 2)}, kids),
], intro)[len('<XuiCanvas>\n'):]

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'assets', 'xui')
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'library.xui'), 'w').write(scene)
open(os.path.join(out, 'frolick_skin.xui'), 'w').write(skin)
print('wrote', out)
