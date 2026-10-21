#!/usr/bin/env python3
"""Write the library pad's panel, dash/pads/library.xui, from sokky's Figma
frame (file fL710Dg0cpzZzYe0OZSBVu, node 73:88 "Group 16", 586x402), and the
three focus visuals its tiles use into dash/pads/frolick_skin.xui.

    genlibrary.py [frolickdash dir]

Everything below is the Figma frame in XUI terms, so the panel stays an
ordinary scene the XUI runtime (and the 360's XuiScene classes) understand:

* boxes, corner radii and strokes are XuiFigures with Bezier corners, one
  figure per Figma fill layer (a Figma node with three fills is three
  figures stacked at the same box);
* Figma's vertical gradients are linear fills at Rotation -90 (top-down);
* Figma's radial fills are affine ellipses.  An ellipse is a rotated,
  axis-scaled circle whatever the skew of the matrix Figma stores, so each
  one becomes the brush Fill of a radial gradient: Rotation, Scale and
  Translation, computed in radial() for the runtime's brush (xui_draw.cpp:
  the figure box normalised to -0.5..0.5, rotated, scaled, translated; the
  ramp texture reaches its last stop at radius 0.5);
* text is Fira Sans (the <Font> name picks FiraSans.ttf from the XIP) at
  PointSize = Figma px * 0.75 (the engine's em is 4/3 PointSize) with the
  text box's top at Figma's box top, which puts the baseline where Figma
  has it (both put it one font ascent below the top).

The tiles are XuiButtons whose Visual (FrolickFrameL / TR / BR, generated
here from the skin's FrolickTile) carries only the focus halo, glow and
shine, cut to the tile's corners; the tile's own border, gloss and shade
are the Figma layers.
"""
import math, os, re, sys

K = 0.5523  # Bezier circle constant


def num(v):
    s = "%.4f" % v
    s = s.rstrip("0").rstrip(".")
    return s if s not in ("", "-0") else "0"


def rrect(w, h, tl=0, tr=0, br=0, bl=0):
    """XUI Points for a w x h box with the given corner radii, clockwise
    from the top-left corner: 'w,h;' then points, each 'x,y' or
    'x,y,c1x,c1y,c2x,c2y' (the controls of the curve to the next point)."""
    pts = []

    def corner(sx, sy, ex, ey, cx, cy, r):
        # arc from (sx,sy) to (ex,ey) around the corner (cx,cy)
        if r <= 0:
            pts.append((cx, cy))
            return
        c1 = (sx + (cx - sx) * K, sy + (cy - sy) * K)
        c2 = (ex + (cx - ex) * K, ey + (cy - ey) * K)
        pts.append((sx, sy, c1[0], c1[1], c2[0], c2[1]))
        pts.append((ex, ey))

    # top-right
    corner(w - tr, 0, w, tr, w, 0, tr)
    corner(w, h - br, w - br, h, w, h, br)
    corner(bl, h, 0, h - bl, 0, h, bl)
    corner(0, tl, tl, 0, 0, 0, tl)
    # start the list at the top-left arc's end so the first point is on the top edge
    pts = pts[-1:] + pts[:-1] if tl > 0 else pts
    out = ["%s,%s" % (num(w), num(h))]
    seen = []
    for p in pts:
        if seen and seen[-1][:2] == p[:2] and len(p) == 2:
            continue
        seen.append(p)
        out.append(",".join(num(v) for v in p))
    return ";".join(out)


def hexc(r, g, b, a=1.0):
    return "0x%02x%02x%02x%02x" % (int(round(a * 255)), r, g, b)


def gradient(stops):
    """stops: list of (pos, colour) with colour '0xaarrggbb'."""
    s = ["  <Gradient>", "   <NumStops>%d</NumStops>" % len(stops)]
    s += ["   <StopColor>%s</StopColor>" % c for _, c in stops]
    s += ["   <StopPos>%s</StopPos>" % num(p) for p, _ in stops]
    s.append("  </Gradient>")
    return s


def radial(w, h, cx, cy, ax, ay, bx, by):
    """Brush Rotation/Scale/Translation for a radial fill on a w x h box
    whose unit circle Figma maps by the matrix with columns (ax,ay) and
    (bx,by) to an ellipse centred at (cx,cy) (box pixels).

    The gradient value is t = |M^-1 (p - c)|, so its contours depend on
    (M M^T)^-1 only; in the brush's normalised box coordinates q = N p - 0.5
    (N = diag(1/w, 1/h)) that quadratic form is B = N^-1 (M M^T)^-1 N^-1,
    and the brush's own contour is |2 (S R q + T)| = 1 with S = diag(sx, sy)
    (its ramp ends at radius 0.5).  So R and the scales come from the
    eigenvectors and eigenvalues of B, and T = -S R qc."""
    # M M^T
    m00 = ax * ax + bx * bx
    m01 = ax * ay + bx * by
    m11 = ay * ay + by * by
    det = m00 * m11 - m01 * m01
    a00, a01, a11 = m11 / det, -m01 / det, m00 / det          # A = (M M^T)^-1
    b00, b01, b11 = a00 * w * w, a01 * w * h, a11 * h * h      # B = N^-1 A N^-1
    # eigen decomposition of the symmetric B
    tr, dt = b00 + b11, b00 * b11 - b01 * b01
    disc = math.sqrt(max(tr * tr / 4 - dt, 0))
    l1, l2 = tr / 2 + disc, tr / 2 - disc
    if abs(b01) > 1e-12:
        v1 = (l1 - b11, b01)
    elif b00 >= b11:
        v1 = (1.0, 0.0)
    else:
        v1 = (0.0, 1.0)
    n = math.hypot(*v1)
    v1 = (v1[0] / n, v1[1] / n)
    v2 = (-v1[1], v1[0])
    # B = Q diag(l1,l2) Q^T with Q = [v1 v2]; brush R = Q^T
    r00, r01, r10, r11 = v1[0], v1[1], v2[0], v2[1]
    rot = math.degrees(math.atan2(r10, r00))
    sx, sy = math.sqrt(l1) / 2, math.sqrt(l2) / 2
    qx, qy = cx / w - 0.5, cy / h - 0.5
    tx, ty = -(r00 * qx + r01 * qy) * sx, -(r10 * qx + r11 * qy) * sy
    # self-check against the runtime's uv code: the image of the unit circle
    # must be the brush's t = 1 contour
    cs, sn = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    for k in range(12):
        th = k * math.pi / 6
        px, py = cx + ax * math.cos(th) + bx * math.sin(th), cy + ay * math.cos(th) + by * math.sin(th)
        a, b = px / w - 0.5, py / h - 0.5
        a2, b2 = a * cs - b * sn, a * sn + b * cs
        t = 2 * math.hypot(a2 * sx + tx, b2 * sy + ty)
        assert abs(t - 1) < 1e-6, (t, rot, sx, sy, tx, ty)
    return rot, sx, sy, tx, ty


def figure(id_, x, y, w, h, points, fill=None, stroke=None, opacity=None, anchor=None):
    s = ["<XuiFigure>", "<Properties>", " <Id>%s</Id>" % id_,
         " <Width>%s</Width>" % num(w), " <Height>%s</Height>" % num(h),
         " <Position>%s,%s,0</Position>" % (num(x), num(y))]
    if anchor is not None:
        s.append(" <Anchor>%d</Anchor>" % anchor)
    if opacity is not None:
        s.append(" <Opacity>%s</Opacity>" % num(opacity))
    if stroke:
        s += [" <Stroke>", "  <StrokeWidth>%s</StrokeWidth>" % num(stroke[0]),
              "  <StrokeColor>%s</StrokeColor>" % stroke[1], " </Stroke>"]
    s.append(" <Fill>")
    if fill is None:
        s.append("  <FillType>0</FillType>")
    elif fill[0] == "solid":
        s += ["  <FillType>1</FillType>", "  <FillColor>%s</FillColor>" % fill[1]]
    elif fill[0] == "linear":
        s.append("  <FillType>2</FillType>")
        s += gradient(fill[1])
        s.append("  <Rotation>%s</Rotation>" % num(fill[2]))
    elif fill[0] == "radial":
        rot, sx, sy, tx, ty = fill[2]
        s.append("  <FillType>3</FillType>")
        s += gradient(fill[1])
        s[-1:-1] = ["   <Radial>true</Radial>"]
        s += ["  <Translation>%s,%s,0</Translation>" % (num(tx), num(ty)),
              "  <Scale>%s,%s,1</Scale>" % (num(sx), num(sy)),
              "  <Rotation>%s</Rotation>" % num(rot)]
    s += [" </Fill>", " <Closed>true</Closed>", " <Points>%s</Points>" % points,
          "</Properties>", "</XuiFigure>"]
    return s


def text(id_, x, y, w, h, txt, px, colour, style):
    return ["<XuiText>", "<Properties>", " <Id>%s</Id>" % id_,
            " <Width>%s</Width>" % num(w), " <Height>%s</Height>" % num(h),
            " <Position>%s,%s,0</Position>" % (num(x), num(y)),
            " <Text>%s</Text>" % txt, " <TextColor>%s</TextColor>" % colour,
            " <DropShadowColor>0</DropShadowColor>",
            " <PointSize>%s</PointSize>" % num(px * 0.75), " <Font>Fira Sans</Font>",
            " <TextStyle>%d</TextStyle>" % style, "</Properties>", "</XuiText>"]


def image(id_, x, y, w, h, path):
    return ["<XuiImage>", "<Properties>", " <Id>%s</Id>" % id_,
            " <Width>%s</Width>" % num(w), " <Height>%s</Height>" % num(h),
            " <Position>%s,%s,0</Position>" % (num(x), num(y)),
            " <ImagePath>%s</ImagePath>" % path, " <SizeMode>4</SizeMode>",
            "</Properties>", "</XuiImage>"]


CENTRE, LEFT_ONE_LINE = 1024, 16
# Every line on the tile is one line of its own (Figma lays each out as a
# single line); word wrap broke "Insert a Game Disc" in two over the line
# under it, the renderer's Fira being a little wider than the 270 px box.
CENTRE_ONE_LINE = CENTRE | LEFT_ONE_LINE
# a game's name or publisher too long for the tile shrinks to fit
# (styleShrink, Frolick's own TextStyle bit in xui/xui_draw.cpp)
CENTRE_FIT = CENTRE_ONE_LINE | 0x100000
GREY_TEXT, WHITE = "0xffd2d2d2", "0xffffffff"
BORDER = (2, "0xff404040")

# The gloss band both small tiles and the game tile share (white, strongest
# from 4.4% to 11.3% of the height, gone by 27%).
def gloss(alpha):
    return [(0.024194, hexc(255, 255, 255, 0)), (0.044342, hexc(255, 255, 255, alpha)),
            (0.11333, hexc(255, 255, 255, alpha)), (0.26815, hexc(255, 255, 255, 0))]


def library():
    s = ['<XuiCanvas version="000c">', "<Properties>", " <Id>canvas</Id>",
         " <Width>1024</Width>", " <Height>512</Height>", "</Properties>",
         "<XuiScene>", "<Properties>", " <Id>LibraryPanel</Id>", " <Width>586</Width>",
         " <Height>402</Height>", " <Position>219,55,0</Position>", " <Pivot>293,201,0</Pivot>",
         "</Properties>"]
    # 73:89 the card, 73:90 its sheen
    s += figure("card", 0, 0, 586, 402, rrect(586, 402, 36, 36, 36, 36),
                ("solid", "0xff55534e"), (6, "0xff898989"))
    s += figure("cardSheen", 3, 3, 580, 396, rrect(580, 396, 33, 33, 33, 33),
                ("linear", [(0, hexc(217, 217, 217, .3)), (0.97115, hexc(0, 0, 0, .3)), (1, hexc(0, 0, 0, 0))], -90))

    # 73:96 the game tile: 336x380 at 12,11, left corners 30
    gx, gy, gw, gh = 12, 11, 336, 380
    gpts = rrect(gw, gh, 30, 0, 0, 30)
    s += figure("gameBase", gx, gy, gw, gh, gpts, ("solid", "0xff55534e"), BORDER)
    s += ["<XuiButton>", "<Properties>", " <Id>tileGame</Id>",
          " <Width>%d</Width>" % gw, " <Height>%d</Height>" % gh,
          " <Position>%d,%d,0</Position>" % (gx, gy),
          " <Visual>FrolickFrameL</Visual>", " <NavRight>tileRecent</NavRight>", "</Properties>"]
    # 73:98 the cover box, 128 square at 104,62, r8, 1 px black border
    s += figure("coverFrame", 104, 62, 128, 128, rrect(128, 128, 8, 8, 8, 8),
                ("solid", "0xffd9d9d9"), (1, "0xff000000"))
    s += image("cover", 105, 63, 126, 126, "cover.png")
    # 73:99 "Game Title" 32 px and 73:100 "Developer" 24 px, centred on the tile
    s += text("title", 0, 200, gw, 38, "Game Title", 32, GREY_TEXT, CENTRE_FIT)
    s += text("subtitle", 0, 238, gw, 29, "Developer", 24, GREY_TEXT, CENTRE_FIT)
    # 73:101 Xelu 360_Start, 87 square at 125,283
    s += image("glyphA", 125, 283, 87, 87, "glyph_a.png")
    # The no-media variant (Figma 143:47 "Group 17", tile 143:55): the cover,
    # title, developer and Start glyph give way to 143:58 "Insert a Game
    # Disc / or Game Card." (32 px, two lines from y 144, centred) and the
    # Material Symbols disc and card icons 143:72 / 143:130, 45 square at
    # 121,237 and 170,237 (art/figma/nodisc_*.svg, rasterised by
    # tools/svgicon.py).  The hub shows one set or the other (SetVisible)
    # by whether a game disc or game card is in.
    s += text("insertLine1", 33, 144, 270, 38, "Insert a Game Disc", 32, GREY_TEXT, CENTRE_ONE_LINE)
    s += text("insertLine2", 33, 182, 270, 38, "or Game Card.", 32, GREY_TEXT, CENTRE_ONE_LINE)
    s += image("icoDisc", 121, 237, 45, 45, "nodisc_album.png")
    s += image("icoCard", 170, 237, 45, 45, "nodisc_sdcard.png")
    s.append("</XuiButton>")
    # 73:102 and 73:103, the game tile's overlays, above its contents
    s += figure("gameGloss", gx, gy, gw, gh, gpts,
                ("linear", [(0, hexc(255, 255, 255, .33)), (0.17426, hexc(255, 255, 255, .33)),
                            (0.30288, hexc(226, 226, 226, 0)), (0.91346, hexc(113, 113, 113, .041)),
                            (1, hexc(113, 113, 113, .33))], -90))
    s += figure("gameShade", gx, gy, gw, gh, gpts,
                ("linear", [(0, hexc(0, 0, 0, 0)), (0.33173, hexc(0, 0, 0, 0)), (1, hexc(0, 0, 0, .2))], -90))
    s += figure("gameFoot", gx, gy, gw, gh, gpts,
                ("linear", [(0, hexc(0, 0, 0, 0)), (0.85577, hexc(0, 0, 0, 0)), (1, hexc(0, 0, 0, .28))], -90),
                BORDER)

    # 73:92 recent games: 227x186 at 348,11, top-right corner 30
    rx, ry, rw, rh = 348, 11, 227, 186
    rpts = rrect(rw, rh, 0, 30, 0, 0)
    s += figure("recentBase", rx, ry, rw, rh, rpts, ("solid", "0xff737373"))
    s += figure("recentGlow", rx, ry, rw, rh, rpts,
                ("radial", [(0.072115, hexc(80, 88, 153)), (0.36058, hexc(74, 82, 114)), (0.875, hexc(85, 83, 78))],
                 radial(rw, rh, 19, 174.5, 190, -128.5, 302.37, 311.55)))
    s += figure("recentShade", rx, ry, rw, rh, rpts,
                ("linear", [(0, hexc(87, 87, 87, 0)), (0.59135, hexc(87, 87, 87, 0)), (0.69712, hexc(87, 87, 87, .327)),
                            (1, hexc(0, 0, 0, .4))], -90))
    s += figure("recentGloss", rx, ry, rw, rh, rpts, ("linear", gloss(.34), -90), BORDER)
    s += ["<XuiButton>", "<Properties>", " <Id>tileRecent</Id>",
          " <Width>%d</Width>" % rw, " <Height>%d</Height>" % rh,
          " <Position>%d,%d,0</Position>" % (rx, ry),
          " <Visual>FrolickFrameTR</Visual>", " <NavLeft>tileGame</NavLeft>",
          " <NavDown>tileFavorites</NavDown>", "</Properties>"]
    # 73:93 "recent" 40 px over "games" 24 px, box at 404,68 (the second line
    # 43 px down: its baseline in the Figma render)
    s += text("recentLabel", 404 - rx, 68 - ry, 160, 48, "recent", 40, WHITE, LEFT_ONE_LINE)
    s += text("recentSub", 404 - rx, 111 - ry, 160, 29, "games", 24, WHITE, LEFT_ONE_LINE)
    s += image("glyphX", 354 - rx, 138 - ry, 50, 50, "glyph_x.png")   # 73:104 360_X
    s.append("</XuiButton>")

    # 73:91 favorite games: 227x194 at 348,197, bottom-right corner 30
    fx, fy, fw, fh = 348, 197, 227, 194
    fpts = rrect(fw, fh, 0, 0, 30, 0)
    s += figure("favBase", fx, fy, fw, fh, fpts, ("solid", "0xff737373"))
    s += figure("favShade", fx, fy, fw, fh, fpts,
                ("linear", [(0, hexc(0, 0, 0, 0)), (0.70192, hexc(0, 0, 0, 0)), (1, hexc(0, 0, 0, .8))], -90))
    s += figure("favGlow", fx, fy, fw, fh, fpts,
                ("radial", [(0, hexc(187, 187, 0)), (0.18149, hexc(156, 157, 15)), (0.36298, hexc(126, 127, 31)),
                            (0.54447, hexc(95, 97, 46)), (0.72596, hexc(65, 67, 62))],
                 radial(fw, fh, 19, 37.5, 181.5, 133.5, -176.09, 394.9)), opacity=0.6)
    s += figure("favGloss", fx, fy, fw, fh, fpts, ("linear", gloss(.6), -90), BORDER)
    s += ["<XuiButton>", "<Properties>", " <Id>tileFavorites</Id>",
          " <Width>%d</Width>" % fw, " <Height>%d</Height>" % fh,
          " <Position>%d,%d,0</Position>" % (fx, fy),
          " <Visual>FrolickFrameBR</Visual>", " <NavLeft>tileGame</NavLeft>",
          " <NavUp>tileRecent</NavUp>", "</Properties>"]
    # 73:105 "favorite" over "games", box at 392,257
    s += text("favLabel", 392 - fx, 257 - fy, 180, 48, "favorite", 40, WHITE, LEFT_ONE_LINE)
    s += text("favSub", 392 - fx, 300 - fy, 180, 29, "games", 24, WHITE, LEFT_ONE_LINE)
    s += image("glyphY", 354 - fx, 209 - fy, 50, 50, "glyph_y.png")   # 73:106 360_Y
    s.append("</XuiButton>")

    s += ["</XuiScene>", "<Timelines>", "<NamedFrames>",
          "<NamedFrame><Name>Intro</Name><Time>0</Time><Command>play</Command><CommandParams></CommandParams></NamedFrame>",
          "<NamedFrame><Name>EndIntro</Name><Time>12</Time><Command>stop</Command><CommandParams></CommandParams></NamedFrame>",
          "</NamedFrames>", "<Timeline>", "<Id>LibraryPanel</Id>", "<TimelineProp>Opacity</TimelineProp>",
          "<TimelineProp>Scale</TimelineProp>",
          "<KeyFrame><Time>0</Time><Interpolation>2</Interpolation><Prop>0</Prop><Prop>0.94,0.94,1</Prop></KeyFrame>",
          "<KeyFrame><Time>12</Time><Interpolation>0</Interpolation><Prop>1</Prop><Prop>1,1,1</Prop></KeyFrame>",
          "</Timeline>", "</Timelines>", "</XuiCanvas>"]
    return "\n".join(s) + "\n"


def frames(skin):
    """The tiles' focus visuals: FrolickTile without its border and gloss
    (the Figma layers draw those), the halo and glow cut to each tile's
    corners.  Inserted into the skin before its closing tags, replacing
    earlier generated ones."""
    m = re.search(r"<XuiVisual>\n<Properties>\n <Id>FrolickTile</Id>.*?</XuiVisual>\n", skin, re.S)
    if not m:
        raise SystemExit("FrolickTile visual not found in the skin")
    tile = m.group(0)
    out = []
    for name, w, h, radii in (("FrolickFrameL", 336, 380, (30, 0, 0, 30)),
                              ("FrolickFrameTR", 227, 186, (0, 30, 0, 0)),
                              ("FrolickFrameBR", 227, 194, (0, 0, 30, 0))):
        v = tile.replace("<Id>FrolickTile</Id>\n <Width>200</Width>\n <Height>160</Height>",
                         "<Id>%s</Id>\n <Width>%d</Width>\n <Height>%d</Height>" % (name, w, h))
        v = re.sub(r"<XuiFigure>\n<Properties>\n <Id>(bg|gloss)</Id>.*?</XuiFigure>\n", "", v, flags=re.S)
        hr = tuple(r + 4 if r else 0 for r in radii)
        v = re.sub(r"(<Id>halo</Id>\n <Width>)208(</Width>\n <Height>)168(</Height>)",
                   r"\g<1>%d\g<2>%d\g<3>" % (w + 8, h + 8), v)
        v = re.sub(r"(<Id>halo</Id>.*?<Points>)[^<]*", lambda mm: mm.group(1) + rrect(w + 8, h + 8, *hr), v, flags=re.S)
        v = re.sub(r"(<Id>glow</Id>\n <Width>)200(</Width>\n <Height>)160(</Height>)",
                   r"\g<1>%d\g<2>%d\g<3>" % (w, h), v)
        v = re.sub(r"(<Id>glow</Id>.*?<Points>)[^<]*", lambda mm: mm.group(1) + rrect(w, h, *radii), v, flags=re.S)
        v = re.sub(r"(<Id>shineClip</Id>\n <Width>)200(</Width>\n <Height>)160(</Height>)",
                   r"\g<1>%d\g<2>%d\g<3>" % (w, h), v)
        v = v.replace("<Id>shine</Id>\n <Width>120</Width>\n <Height>160</Height>\n <Position>-160,0,0</Position>",
                      "<Id>shine</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n <Position>%d,0,0</Position>" % (w * 0.6, h, -w * 0.8))
        v = v.replace("<Prop>-160,0,0</Prop>", "<Prop>%d,0,0</Prop>" % (-w * 0.8))
        v = v.replace("<Prop>420,0,0</Prop>", "<Prop>%d,0,0</Prop>" % (w * 2.1))
        out.append(v)
    skin = re.sub(r"<XuiVisual>\n<Properties>\n <Id>FrolickFrame\w*</Id>.*?</XuiVisual>\n", "", skin, flags=re.S)
    return skin.replace("</XuiScene>\n</XuiCanvas>", "".join(out) + "</XuiScene>\n</XuiCanvas>")


if __name__ == "__main__":
    here = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    pads = os.path.join(here, "dash", "pads")
    open(os.path.join(pads, "library.xui"), "w").write(library())
    sk = os.path.join(pads, "frolick_skin.xui")
    new = frames(open(sk).read())
    open(sk, "w").write(new)
    print("wrote library.xui and the FrolickFrame visuals")
