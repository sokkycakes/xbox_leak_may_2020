#!/usr/bin/env python3
"""Generate the padfocus pages of the Frolick dashboard: the full-screen
pages the catalog and settings pads open, laid out like the Xbox 360
Blades dashboard's third-level pages (the ones that slide the blades away
and expand) and drawn with Frolick's art.

    genpage.py dashdir

Writes dashdir/pads/page_skin.xui (the visuals every page uses, in
Frolick's palette: the FrolickTile treatment on a list bar, the header,
the meta panel card, the A/B legends with Frolick's glyphs) and one
dashdir/pads/page_*.xui per page.

Each page's layout is the Blades scene itself, converted from the
dashboard's XUIB with tools/xur2xui.py and kept in art/blades/ as it came
out: the ids, sizes, positions and navigation of its controls are
Microsoft's, so that the pages keep the Blades' structure.  What this
script adds is Frolick's: the content card behind the list, the pad's
colour as the wing on the card's edge (the Blades' blade wing), and the
open and close motion, timed like dashmain.xur's 1OpenFull (30 frames at
30 fps: the wing slides out, the panel blinks in).  The Blades skin's
visuals are not used; page_skin.xui draws every control.

The scenes are 1120x770 canvases (the Blades' 720p design size).  The hub
shows a page on a HUD XuiPanel 720 wide, so the whole canvas lands on the
NTSC screen at 0.643 and the content (x 146..1004, y 96..686) sits inside
the title-safe area.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BLADES = os.path.join(HERE, "..", "art", "blades")

# Frolick's palette (the catalog and library panels, catalog.xui / frolick_skin.xui)
CARD_FILL, CARD_STROKE = "0xff55534e", "0xff898989"      # the panel card
TILE_STROKE, HALO_STROKE = "0xff404040", "0x70d9d9d9"    # FrolickTile's edge and focus halo
TEXT, TEXT_DIM = "0xffffffff", "0xffeeeeee"
# The pads' disc colours (main.tscn), the wing of each pad's pages.
PAD_COLOR = {"settings": "0xffd98b3f", "catalog": "0xff4080db"}

CANVAS_W, CANVAS_H = 1120, 770
CARD = (116, 76, 898, 624)          # x, y, w, h: 30 around the Blades' content
WING_W = 14

PAGES = [
    # file, Blades scene, pad, the first focused control
    ("page_console", "dashSysCslSet.xui", "settings", "btnDisplay"),
]


def rect(w, h):
    return "%g,%g;0,0;%g,0;%g,%g;0,%g" % (w, h, w, w, h, h)


def rounded(w, h, r):
    """A rounded rectangle the way the XUI Tool writes one (FrolickTile's bg)."""
    k = 0.4477 * r  # control points of a quarter circle
    return ("%g,%g;%g,0;%g,0,%g,0,%g,%g;%g,%g;%g,%g,%g,%g,%g,%g;%g,%g;%g,%g,%g,%g,0,%g;0,%g;0,%g,0,%g,%g,0"
            % (w, h, r, w - r, w - k, w, k, w, r, w, h - r, w, h - k, w - k, h, w - r, h, r, h, k, h, h - k,
               h - r, r, k, k))


def figure(id, x, y, w, h, fill, stroke=None, extra="", points=None, opacity=None, anchor=None):
    s = ["<XuiFigure>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h,
         " <Position>%g,%g,0</Position>" % (x, y)]
    if opacity is not None:
        s.append(" <Opacity>%g</Opacity>" % opacity)
    if anchor is not None:
        s.append(" <Anchor>%d</Anchor>" % anchor)
    if extra:
        s.append(extra)
    if stroke:
        s.append(" <Stroke>\n  <StrokeWidth>%g</StrokeWidth>\n  <StrokeColor>%s</StrokeColor>\n </Stroke>" % stroke)
    s.append(fill)
    s.append(" <Closed>true</Closed>")
    s.append(" <Points>%s</Points>" % (points or rect(w, h)))
    s += ["</Properties>", "</XuiFigure>"]
    return "\n".join(s)


def solid(color):
    return " <Fill>\n  <FillType>1</FillType>\n  <FillColor>%s</FillColor>\n </Fill>" % color


def nofill():
    return " <Fill>\n  <FillType>0</FillType>\n </Fill>"


def gradient(stops, rotation=90):
    return (" <Fill>\n  <FillType>2</FillType>\n  <Gradient>\n   <NumStops>%d</NumStops>\n%s\n%s\n  </Gradient>\n  <Rotation>%g</Rotation>\n </Fill>"
            % (len(stops), "\n".join("   <StopColor>%s</StopColor>" % c for _, c in stops),
               "\n".join("   <StopPos>%g</StopPos>" % p for p, _ in stops), rotation))


def text(id, x, y, w, h, size, style, color=TEXT, presenter=True, value=None, shadow="0"):
    tag = "XuiTextPresenter" if presenter else "XuiText"
    s = ["<%s>" % tag, "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h,
         " <Position>%g,%g,0</Position>" % (x, y)]
    if value is not None:
        s.append(" <Text>%s</Text>" % value)
    s += [" <TextColor>%s</TextColor>" % color, " <DropShadowColor>%s</DropShadowColor>" % shadow,
          " <PointSize>%d</PointSize>" % size, " <TextStyle>%d</TextStyle>" % style, "</Properties>", "</%s>" % tag]
    return "\n".join(s)


def image(id, x, y, w, h, path):
    return "\n".join(["<XuiImage>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h,
                      " <Position>%g,%g,0</Position>" % (x, y), " <ImagePath>%s</ImagePath>" % path, " <SizeMode>4</SizeMode>",
                      "</Properties>", "</XuiImage>"])


def frames(names):
    return "<NamedFrames>\n%s\n</NamedFrames>" % "\n".join(
        "<NamedFrame><Name>%s</Name><Time>%d</Time><Command>%s</Command><CommandParams>%s</CommandParams></NamedFrame>"
        % (n, t, c, p) for n, t, c, p in names)


def track(id, prop, keys):
    return "<Timeline>\n<Id>%s</Id>\n<TimelineProp>%s</TimelineProp>\n%s\n</Timeline>" % (id, prop, "\n".join(
        "<KeyFrame><Time>%d</Time><Interpolation>%d</Interpolation><Prop>%s</Prop></KeyFrame>" % (t, i, v) for t, i, v in keys))


ONE_LINE, RIGHT, CENTER_H, CENTER_V, SHADOW = 0x10, 0x200, 0x400, 0x1000, 0x4000


# ---- the skin ---------------------------------------------------------------

def bar_visual():
    """The list bar (XuiNavButton falls back to XuiButton): FrolickTile's
    treatment, a rounded box with the halo that comes with focus and the
    shine that sweeps it, on a 423x47 bar with the label at the left."""
    w, h = 423, 47
    body = [
        figure("halo", -4, -4, w + 8, h + 8, nofill(), (3, HALO_STROKE), points=rounded(w + 8, h + 8, 9), opacity=0, anchor=15),
        figure("bg", 0, 0, w, h, solid("0x2a000000"), (2, TILE_STROKE), points=rounded(w, h, 6), anchor=15),
        figure("gloss", 2, 2, w - 4, (h - 4) * 0.45, gradient([(0, "0x40ffffff"), (1, "0x00ffffff")]), anchor=7),
        figure("glow", 0, 0, w, h, gradient([(0, "0x00ffffff"), (0.5, "0x2affffff"), (1, "0x00ffffff")], 0), opacity=0, anchor=15,
               points=rounded(w, h, 6)),
        "\n".join(["<XuiGroup>", "<Properties>", " <Id>shineClip</Id>", " <Width>%d</Width>" % w, " <Height>%d</Height>" % h,
                   " <Anchor>15</Anchor>", " <ClipChildren>true</ClipChildren>", " <Opacity>0</Opacity>", "</Properties>",
                   figure("shine", -160, 0, 120, h, gradient([(0, "0x00ffffff"), (0.5, "0x30ffffff"), (1, "0x00ffffff")], 0), anchor=10),
                   "<Timelines>",
                   frames([("Sweep", 0, "play", ""), ("EndSweep", 100, "gotoandplay", "Sweep")]),
                   track("shine", "Position", [(0, 0, "-160,0,0"), (40, 2, "%d,0,0" % (w + 40)), (100, 0, "%d,0,0" % (w + 40))]),
                   "</Timelines>", "</XuiGroup>"]),
        text("text_Label", 16, 0, w - 32, h, 18, ONE_LINE | CENTER_V),
    ]
    tl = [
        frames([("KillFocus", 0, "play", ""), ("Normal", 8, "play", ""), ("EndNormal", 9, "stop", ""),
                ("Focus", 10, "play", ""), ("FocusLoop", 18, "play", ""), ("EndFocus", 120, "gotoandplay", "FocusLoop"),
                ("Press", 130, "play", ""), ("EndPress", 140, "gotoandplay", "FocusLoop")]),
        track("halo", "Opacity", [(0, 0, 1), (8, 0, 0), (10, 0, 0), (18, 0, 1), (69, 2, 0.55), (120, 2, 1), (130, 0, 1), (140, 0, 1)]),
        track("glow", "Opacity", [(0, 0, 1), (8, 0, 0), (10, 0, 0), (18, 0, 1), (120, 0, 1), (130, 0, 1), (133, 0, 0.4), (140, 0, 1)]),
        track("shineClip", "Opacity", [(0, 0, 0), (18, 0, 0), (20, 0, 1), (120, 0, 1), (130, 0, 1)]),
    ]
    return visual("XuiButton", w, h, body, tl)


def visual(id, w, h, body, timelines=None):
    s = ["<XuiVisual>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h, "</Properties>"]
    s += body
    if timelines:
        s += ["<Timelines>"] + timelines + ["</Timelines>"]
    s.append("</XuiVisual>")
    return "\n".join(s)


def legend_visual(button, glyph):
    """legend_A/B: the label, then the glyph at the right (the Blades put
    the glyph at x 386 of a 420 bar), with Frolick's glyph art."""
    w, h = 420, 47
    return visual("legend_" + button, w, h, [
        text("label", 6, 0, 378, 40, 18, ONE_LINE | RIGHT | CENTER_V),
        image("glyph", 388, 4, 32, 32, glyph),
    ])


def head_visual(id, size, color):
    return visual(id, 420, 47, [text("Text", 0, 0, 415, 47, size, ONE_LINE | CENTER_V, color)])


def meta_visual():
    """metaScene_1line: the meta panel at the right of a settings list,
    showing the focused item's current setting; a card inside the page's
    card, with the sheen Frolick's catalog panels carry."""
    w, h = 395, 426
    return visual("metaScene_1line", w, h, [
        figure("bg", 0, 0, w, h, solid("0x3a000000"), (2, TILE_STROKE), anchor=15),
        figure("sheen", 2, 2, w - 4, h - 4, gradient([(0, "0x26ffffff"), (0.35, "0x00ffffff"), (1, "0x00000000")]), anchor=15),
        text("Pane_txtCurrentSetting", 17.5, 50, w - 35, h - 60, 18, 0),
    ])


def skin():
    body = [
        bar_visual(),
        head_visual("Label_Head", 22, TEXT),
        head_visual("Label_MetaHead_1line", 20, TEXT_DIM),
        legend_visual("A", "glyph_a_button.png"),
        legend_visual("B", "glyph_b.png"),
        meta_visual(),
    ]
    return ('<XuiCanvas version="000c">\n<XuiScene>\n<Properties>\n <Id>FrolickPageSkin</Id>\n <Width>1280</Width>\n <Height>720</Height>\n'
            '</Properties>\n%s\n</XuiScene>\n</XuiCanvas>\n' % "\n".join(body))


# ---- the pages --------------------------------------------------------------

def element_blocks(scene_xml):
    """The top-level child elements of the Blades scene's root XuiScene, as
    text blocks, and the root's Properties."""
    m = re.search(r"<XuiScene>\s*<Properties>(.*?)</Properties>(.*)</XuiScene>\s*</XuiCanvas>", scene_xml, re.S)
    props, body = m.group(1), m.group(2)
    blocks, depth, start = [], 0, None
    for t in re.finditer(r"<(/?)(Xui\w+)>", body):
        if t.group(1) == "":
            if depth == 0:
                start = t.start()
            depth += 1
        else:
            depth -= 1
            if depth == 0:
                blocks.append(body[start:t.end()])
    return props, blocks


def page(file, scene, pad, first):
    src = open(os.path.join(BLADES, scene)).read()
    props, blocks = element_blocks(src)
    # The scene (the Blades' scCslSettings and so on) is "page" here, the
    # id the hub plays Open and Close on.
    kept = []
    for b in blocks:
        id = re.search(r"<Id>(\w+)</Id>", b).group(1)
        # The Blades' disabled legend placeholders (legend_x, legend_y:
        # Enabled false, no text) show nothing on this page.
        if "<Enabled>false</Enabled>" in b:
            continue
        # The meta panel is a nested scene with a Visual on the 360, which
        # the runtime does not skin; a label carries the same visual and
        # its Text is the current setting.
        if b.startswith("<XuiScene>") and "<Visual>" in b:
            b = re.sub(r"<ClassOverride>\w+</ClassOverride>\s*", "", b)
            b = re.sub(r"<Timelines>.*</Timelines>\s*", "", b, flags=re.S)
            b = b.replace("<XuiScene>", "<XuiLabel>").replace("</XuiScene>", "</XuiLabel>")
            b = b.replace("</Properties>", " <Text></Text>\n</Properties>", 1)
        kept.append(b)

    cx, cy, cw, ch = CARD
    frolick = [
        # the content card, Frolick's panel card, scaling in from its centre
        "\n".join(["<XuiGroup>", "<Properties>", " <Id>card</Id>", " <Width>%d</Width>" % cw, " <Height>%d</Height>" % ch,
                   " <Position>%d,%d,0</Position>" % (cx, cy), " <Pivot>%g,%g,0</Pivot>" % (cw / 2, ch / 2),
                   " <Opacity>0</Opacity>", "</Properties>",
                   figure("cardBg", 0, 0, cw, ch, solid(CARD_FILL), (6, CARD_STROKE)),
                   figure("cardSheen", 3, 3, cw - 6, ch - 6, gradient([(0, "0x2effffff"), (0.4, "0x00ffffff"), (1, "0x33000000")])),
                   "</XuiGroup>"]),
        # the wing: the pad's colour down the card's left edge, sliding out
        # as the Blades' wing does
        figure("wing", cx + 3, cy + 3, WING_W, ch - 6, solid(PAD_COLOR[pad]), opacity=0),
    ]
    content = "\n".join(["<XuiGroup>", "<Properties>", " <Id>content</Id>", " <Width>%d</Width>" % CANVAS_W,
                         " <Height>%d</Height>" % CANVAS_H, " <Position>0,0,0</Position>", " <Opacity>0</Opacity>",
                         "</Properties>"] + kept + ["</XuiGroup>"])
    # Open and close, 30 frames each as dashmain's 1OpenFull / 1CloseFull:
    # the card fades up over the first third while it grows from 0.92 and
    # the wing slides out from under it; the Blades' content panel blinks
    # in at frames 10..26, which here is the content's fade.  (Tracks
    # address the root's direct children, so the three fade separately.)
    fade = [(0, 2, 0), (10, 0, 1), (31, 0, 1), (51, 2, 0), (61, 0, 0), (62, 0, 0), (63, 0, 0)]
    tl = [
        frames([("Open", 0, "play", ""), ("OpenEnd", 30, "stop", ""), ("Close", 31, "play", ""), ("CloseEnd", 61, "stop", ""),
                ("Normal", 62, "play", ""), ("EndNormal", 63, "stop", "")]),
        track("card", "Opacity", fade),
        track("wing", "Opacity", fade),
        track("card", "Scale", [(0, 2, "0.92,0.92,1"), (30, 0, "1,1,1"), (31, 2, "1,1,1"), (61, 0, "0.92,0.92,1"),
                                (62, 0, "0.92,0.92,1"), (63, 0, "0.92,0.92,1")]),
        track("wing", "Position", [(0, 2, "%d,%d,0" % (cx + 3, cy + 3)), (30, 0, "%d,%d,0" % (cx - WING_W - 6, cy + 3)),
                                   (31, 2, "%d,%d,0" % (cx - WING_W - 6, cy + 3)), (61, 0, "%d,%d,0" % (cx + 3, cy + 3)),
                                   (62, 0, "%d,%d,0" % (cx + 3, cy + 3)), (63, 0, "%d,%d,0" % (cx + 3, cy + 3))]),
        track("content", "Opacity", [(0, 0, 0), (10, 0, 0), (26, 0, 1), (31, 0, 1), (41, 0, 0), (61, 0, 0), (62, 0, 0), (63, 0, 0)]),
        track("content", "Position", [(0, 2, "24,0,0"), (30, 0, "0,0,0"), (31, 2, "0,0,0"), (61, 0, "24,0,0"),
                                      (62, 0, "24,0,0"), (63, 0, "24,0,0")]),
    ]
    body = "\n".join(frolick + [content])
    return ('<XuiCanvas version="000c">\n<Properties>\n <Id>canvas</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n</Properties>\n'
            '<XuiScene>\n<Properties>\n <Id>page</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n</Properties>\n%s\n<Timelines>\n%s\n</Timelines>\n'
            '</XuiScene>\n</XuiCanvas>\n' % (CANVAS_W, CANVAS_H, CANVAS_W, CANVAS_H, body, "\n".join(tl)))


def main():
    out = sys.argv[1]
    pads = os.path.join(out, "pads")
    os.makedirs(pads, exist_ok=True)
    open(os.path.join(pads, "page_skin.xui"), "w").write(skin())
    for file, scene, pad, first in PAGES:
        open(os.path.join(pads, file + ".xui"), "w").write(page(file, scene, pad, first))


if __name__ == "__main__":
    main()
