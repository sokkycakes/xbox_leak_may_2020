#!/usr/bin/env python3
"""Generate the hub scene of the Frolick dashboard for the xapp engine.

    genhub.py dashdir

Writes dashdir/default.xap (the hub), dashdir/backdrop.png (the screen
backdrop), dashdir/pads/overlay.xui (the screen overlay: the wordmark, the
A prompt, the footer) and dashdir/pads/settings.xui (the settings panel,
derived from catalog.xui the way Frolick's RML settings panel is a
relabelled catalog panel).

Everything in the world comes from Frolick's Godot scene,
frolick_assets/main.tscn, node for node: the nav group and its three pads
(discs, bumpers, the library's highlight ring), the billboard emblems and
labels over each pad, the rail ribbon, the atmo dome, the cameras
(Camera3D for the library, cam_angle_pad for the catalog and its mirror
for settings) and the footer; the backdrop from backdrop.tres.  The pad
panels (Frolick's own .xui scenes) face each camera at the size the
mockup shows.  Godot is right-handed with z toward the viewer; the engine
mirrors its world in z before the camera (CCamera::UpdateViewMatrix), so
Transforms and vertices take Godot's coordinates as they are and only the
camera's rotation is built in the mirrored frame.

A Godot Transform3D(a..i, ox, oy, oz) lists its basis by rows, so the
axes (columns) are X = (a, d, g), Y = (b, e, h), Z = (c, f, i); a camera
looks down its -Z.
"""
import math, os, re, sys

# ---- main.tscn -------------------------------------------------------------
NAV_SCALE, NAV_ORIGIN = 0.725065, (0.0, -0.0441807, 0.116777)
PAD_SCALE = 0.762073
S = NAV_SCALE * PAD_SCALE                      # pad-local units to world
PADS = {  # local origin under nav, disc colour, emblem modulate
    "library": ((-0.000382925, 0.00177303, 0.234483), (0.54002, 0.78, 0.2262), (0.537255, 0.772549, 0.223529)),
    "settings": ((1.07647, 0.002, -0.339248), (0.85, 0.543535, 0.2465), (0.796078, 0.498039, 0.227451)),
    "catalog": ((-1.076, 0.002, -0.339658), (0.2494, 0.503817, 0.86), (0.25098, 0.501961, 0.858824)),
}
PAD_ORDER = ["catalog", "library", "settings"]  # left to right; nPad in the script
DISC_RADIUS = 0.5 * S                          # CylinderMesh radius 0.5, height 0.001
RING_SCALE, RING_Y = 0.97003, -0.00541713      # highlight_ring (library only): top 0.525, bottom 0.54
BUMPER_SCALE, BUMPER_Y = (1.03758, 0.358507), 0.025  # the rail tube (r 0.5, h 0.02) around each pad
BUMPER_INNER = (0.785156, 0.774878, 0.69118)   # its opaque inner tube
RAIL_ORIGIN, RAIL_SCALE = (-0.20688, 2.13097, -4.21954), (11.8372, 90.7688)
RAIL_COLOR = (0.587, 0.569, 0.441)             # the gltf's base colour, as the Go app draws it
ATMO_ORIGIN, ATMO_SCALE = (0.0, -2.12831, -4.93449), 7.82174   # SphereMesh r 0.891 h 1.09 hemisphere
EMBLEM_SCALE = 0.216655                        # Sprite3D2 (library, catalog) / Sprite3D (settings)
EMBLEM_Y = {"library": -0.0289344, "settings": 0.207169, "catalog": 0.207169}
EMBLEM = [  # local offset in the parent sprite, pixel_size
    ((0.0, 0.0, 0.0), 0.0055),
    ((-0.0173802, 0.0480719, -0.0536249), 0.0037),
    ((-0.0173802, 0.0669544, -0.155033), 0.0027),
]
LABEL_Y, LABEL_PIXEL = 0.115168, 0.002         # Label3D, billboard, no depth test
LABEL_OVERSAMPLE = 2                           # mkart.py renders the labels at 2x
# In a pad's focus view the label is the page's title; the camera frames
# it cut by the screen edge, so it slides this far (720x480 output pixels,
# right and down) during the zoom to show the whole word (sokky 2026-10-20:
# move the label, not the camera); label_focus_offset turns that into the
# world offset the Sprite node takes.
LABEL_FOCUS_SHIFT = {"catalog": (75.0, -8.0), "settings": (-95.0, -8.0)}
RAW_W, RAW_H = 640, 480                        # the frame the engine renders (shown 720 wide)
CAMERAS = {  # eye, view direction (-Z column), up (Y column), vertical fov in degrees
    "library": ((0.0, 0.106559, 0.917581), (0.0, 0.286932, -0.708153), (0.0, 1.46646, 0.594187), 80.0),
    "catalog": ((-0.848216, 0.1074, 0.464691), (0.628121, 0.375529, -0.6815), (-0.254505, 0.926811, 0.276133), 80.1803),
    # cam_angle_padfocus: the close, near top-down shot of the catalog pad
    # (pitched 70 degrees down); A on the catalog or settings panel flies
    # there, B flies back.  Its up vector is its own, not +Y.
    "catalogfocus": ((-0.639461, 0.133682, 0.0213613), (0.0, -0.942063, -0.335435), (0.0, 0.335435, -0.942063), 80.1803),
}
ZOOM_DURATION = 0.75                           # the library carousel's zoom
# The padfocus pages (the Blades' third-level pages, tools/genpage.py)
# hang off a HUD XuiPanel; genpage.py's --script tables say which page
# each pad opens, what a press opens next and what the meta panel says.
# The carousel camera and the game belt, fitted to sokky's mockup
# screenshot of the belt view (2026-10-19, 2560x1440): the camera from the
# three pad labels (1.8 px rms), low behind the library pad and pitched up
# 22.0 degrees, 0.19 ahead and 0.07 above carousel.go's carouselEyeTarget;
# the belt (GameTap's, see art/gametap) from the cover corners, its ring
# centred on the camera as GameTap shoots it, the regular covers at 0.97 of
# the rail radius, the focused cover 4.7 ahead.
# The view is widened from the hub's 80 degrees so the three pad labels
# sit inside the NTSC title-safe area (the inner 80%) in this view, as
# sokky asked (2026-10-20): same position and angle, zoomed out.
CAROUSEL_EYE = (0.0, 0.1072, 0.6407)
CAROUSEL_LOOK = (0.0, 0.4043, -1.0)
CAROUSEL_FOV = 96.0
# sokky then (2026-10-20) wanted the belt back on the rail ring, which it
# follows nicely from this camera, with the focused tile kept at the size
# it had with the ring on the camera: the ring's axis is the rail's, the
# covers sit at 0.95 of the ribbon radius at GameTap's sizes, and the
# focused slot is pulled 5.6 off the wall toward the camera (sokky's pick),
# a cover sliding in or out following by its angle from the slot.
BELT_CENTRE = RAIL_ORIGIN
BELT_INSET = 0.95
BELT_COVER_SCALE = 1.0
BELT_FOCUS_PULL = 5.6
BELT_SKIP = 2              # slots either side of the focus hidden behind the big cover: they hold nothing
# The belt's list: two reserved tiles at the left end (sokky, 2026-10-20),
# "show all games" (grid mode, not built yet) and the game in the disc
# tray or game card, each with an icon centred on its tile, then the games
# (carousel.go's placeholder titles until a real list exists).  The icon
# images are not in the cloud yet; the names are placeholders for them.
BELT_RESERVED = [("All games", "icon_grid.png"), ("Disc", "icon_disc.png")]
CAROUSEL_GAMES = [n for n, _ in BELT_RESERVED] + [
    "Game Title", "Stiletto Swift", "Game Title 3", "Game Title 4", "Game Title 5",
    "Game Title 6", "Game Title 7", "Game Title 8", "Game Title 9"]
BELT_START = len(BELT_RESERVED)                # the first game is focused when the belt opens
FOOTER_H, FOOTER_COLOR, FOOTER_ALPHA = 50, (0.68, 0.665493, 0.5712), 0.592157
FOOTER_STOPS = [(0.0, 0.0), (0.387409, 0.647059), (0.939467, 1.0)]
WORDMARK_COLOR = (0.79, 0.773147, 0.6636)      # LabelSettings_1bvp3, 37 px
# The big faded word behind each view (the mockup's "discover"; playpanel.tscn's
# "play": size 111, white at 0.2157).
WORDS = {"catalog": "discover", "library": "play", "settings": ""}
# ---- backdrop.tres ---------------------------------------------------------
BACKDROP_FROM, BACKDROP_TO = (0.521368, 0.709402), (0.534188, 0.0)
BACKDROP_STOPS = [(0.0, (0.42, 0.41104, 0.3528)), (0.32809, (0.29, 0.283813, 0.2436)),
                  (0.665169, (0.201875, 0.198955, 0.179975)), (1.0, (0.13, 0.13, 0.13))]
# ---- the pad panels (Frolick's 0.9 x 0.45 canvas, 1024 x 512) ------------
# Centred on each camera's view axis, a little above it, tilted back a
# few degrees, close enough to fill the view as Frolick's own panels do.
PANEL_W, PANEL_H = 0.9, 0.45
PANEL_DIST, PANEL_RAISE, PANEL_TILT = 0.37, 0.06, 10.6
# The screen overlay canvas (the engine's HUD is 720 x 480).
HUD_W, HUD_H = 720, 480
UI = HUD_H / 720.0                             # Godot's 720-high UI to the HUD
PIN_TIP_Y = 378                                # the A pin's point, just above the pad label
# PointSize is in points (the em is 4/3 PointSize pixels, as in the XUI
# engine); Frolick's sizes were authored as the pixel height of DejaVu's
# ascent + descent (2384/2048 em), so they convert by this factor.
PT = 0.75 * 2048 / 2384
# The Blades pages are 1120 x 770 canvases; a 360 at 16:9 shows the window
# of rows 58.7..688.7 scaled to the full screen height, so the page panel
# is wider than the HUD (66 px cropped each side) and centred on that
# window.
PAGE_WINDOW = (58.7, 688.7)
# ... scaled in about the centre so the whole page, legends included, sits
# inside NTSC action safe (the inner 90%) with a little to spare (sokky,
# 2026-10-20).
PAGE_SAFE = 0.93

# The hub's A pin (sokky, 2026-10-20): it fades out as a pad flight or a
# zoom starts and back in once the camera has settled on a pad; idle, it
# dips a few pixels every two seconds, eased out.
PIN_FADE_OUT = 0.15     # s
PIN_FADE_IN = 0.2       # s
PIN_PULSE_DY = 5        # HUD px (of 480), down
PIN_PULSE_PERIOD = 2.0  # s
PIN_PULSE_DUR = 0.6     # s, the dip and the return
PAGE_W = 1120 * HUD_H / (PAGE_WINDOW[1] - PAGE_WINDOW[0]) * PAGE_SAFE
PAGE_VIEW_Y = (PAGE_WINDOW[0] + PAGE_WINDOW[1]) / 2


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a): l = math.sqrt(dot(a, a)); return mul(a, 1 / l)
def mirror(a): return (a[0], a[1], -a[2])
def mirrorx(a): return (-a[0], a[1], a[2])
def v3(a): return "%.5f %.5f %.5f" % a


def pad_world(name):
    """World centre of a pad disc."""
    return add(NAV_ORIGIN, mul(PADS[name][0], NAV_SCALE))


def label_focus_offset(name):
    """World offset that moves a pad's label by LABEL_FOCUS_SHIFT on the
    screen in the pad's focus view: along the view's right and up at the
    label's depth (vertical fov over 480 lines, 4:3 frame shown 720 wide)."""
    eye, d, up, fov = camera(name + "focus")
    r = norm(cross(d, up))
    p = add(pad_world(name), (0, LABEL_Y * S, 0))
    depth = dot(sub(p, eye), d)
    half = depth * math.tan(math.radians(fov) / 2)      # world half-height at the label
    dx, dy = LABEL_FOCUS_SHIFT[name]
    ax = dx * (RAW_W / float(HUD_W)) * half * (RAW_W / float(RAW_H)) / (RAW_W / 2.0)
    ay = -dy * half / (RAW_H / 2.0)
    return add(mul(r, ax), mul(up, ay))


def camera(name):
    """eye, direction, up, fov for a pad's view."""
    if name == "carousel":
        return CAROUSEL_EYE, norm(CAROUSEL_LOOK), (0.0, 1.0, 0.0), CAROUSEL_FOV
    if name.startswith("settings"):
        eye, d, up, fov = CAMERAS["catalog" + name[len("settings"):]]
        return mirrorx(eye), norm(mirrorx(d)), norm(mirrorx(up)), fov
    eye, d, up, fov = CAMERAS[name]
    return eye, norm(d), norm(up), fov


def quat_from_rows(r, u, b):
    """Quaternion q with D3DXMatrixRotationQuaternion(q) rows r, u, b
    (the images of the x, y and z axes for row vectors)."""
    m = [r, u, b]
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1) * 2
        w = 0.25 * s
        x = (m[1][2] - m[2][1]) / s
        y = (m[2][0] - m[0][2]) / s
        z = (m[0][1] - m[1][0]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2
        w = (m[1][2] - m[2][1]) / s
        x = 0.25 * s
        y = (m[1][0] + m[0][1]) / s
        z = (m[2][0] + m[0][2]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2
        w = (m[2][0] - m[0][2]) / s
        x = (m[1][0] + m[0][1]) / s
        y = 0.25 * s
        z = (m[2][1] + m[1][2]) / s
    else:
        s = math.sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2
        w = (m[0][1] - m[1][0]) / s
        x = (m[2][0] + m[0][2]) / s
        y = (m[2][1] + m[1][2]) / s
        z = 0.25 * s
    rows = [(1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)),
            (2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)),
            (2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y))]
    for a, c in zip(rows, m):
        assert max(abs(a[i] - c[i]) for i in range(3)) < 1e-3, (rows, m)
    return x, y, z, w


def axis_angle(q):
    x, y, z, w = q
    w = max(-1.0, min(1.0, w))
    angle = 2 * math.acos(w)
    s = math.sqrt(max(0.0, 1 - w * w))
    if s < 1e-6:
        return (1.0, 0.0, 0.0, 0.0)
    return (x / s, y / s, z / s, angle)


def viewpoint(name):
    """Viewpoint text for a camera.

    The engine mirrors the world in z before applying the camera, so the
    camera's rotation is built in the mirrored (left-handed) frame with
    D3DXMatrixLookAtLH's axes from the view direction and the camera's up,
    and the position is Godot's as given (CViewpoint::Bind negates it)."""
    eye, d, up, fov = camera(name)
    zaxis = mirror(d)
    xaxis = norm(cross(mirror(up), zaxis))
    yaxis = cross(zaxis, xaxis)
    ax, ay, az, angle = axis_angle(quat_from_rows(xaxis, yaxis, zaxis))
    # CViewpoint::Bind rotates by -orientation.w, so hand it the negative
    return "fieldOfView %.5f\n    position %s\n    orientation %.4f %.4f %.4f %.4f" % (
        math.radians(fov), v3(eye), ax, ay, az, -angle)


def panel(name):
    """XuiPanel origin/axisX/axisY: the canvas's top left corner, its width
    vector and its height vector, for a quad facing the pad's camera."""
    eye, d, up, fov = camera(name)
    right = norm(cross(d, up))
    t = math.radians(PANEL_TILT)
    pup = norm(add(mul(up, math.cos(t)), mul(d, math.sin(t))))  # top edge leans away
    centre = add(add(eye, mul(d, PANEL_DIST)), mul(up, PANEL_RAISE))
    tl = add(sub(centre, mul(right, PANEL_W / 2)), mul(pup, PANEL_H / 2))
    return "origin %s\n                    axisX %s\n                    axisY %s" % (
        v3(tl), v3(mul(right, PANEL_W)), v3(mul(pup, -PANEL_H)))


def backdrop(path, size=512):
    """Frolick's backdrop texture: the radial gradient of backdrop.tres,
    rasterised the way the Go app does it (linear between stops)."""
    from PIL import Image
    (fx, fy), (tx, ty) = BACKDROP_FROM, BACKDROP_TO
    radius = math.hypot(tx - fx, ty - fy)
    im = Image.new("RGB", (size, size))
    px = im.load()
    for y in range(size):
        v = 1 - y / size
        for x in range(size):
            u = x / size
            t = min(1.0, math.hypot(u - fx, v - fy) / radius)
            for i in range(len(BACKDROP_STOPS) - 1):
                p0, c0 = BACKDROP_STOPS[i]
                p1, c1 = BACKDROP_STOPS[i + 1]
                if t <= p1:
                    f = (t - p0) / (p1 - p0)
                    px[x, y] = tuple(int((c0[k] + (c1[k] - c0[k]) * f) * 255 + 0.5) for k in range(3))
                    break
    im.save(path)


# ---- the pads: discs, bumpers, emblems, labels ----------------------------

def pad_nodes(name):
    centre = pad_world(name)
    colour, tint = PADS[name][1], PADS[name][2]
    out = []
    out.append('''        // %s: the pad disc, its bumper and what floats over it
        Transform
        {
            translation %s
            children [ Disc { radius %.5f color %s } ]
        }''' % (name, v3(centre), DISC_RADIUS, v3(colour)))
    if name == "library":
        out.append('''        Transform
        {
            translation %s
            children [ Disc { radius %.5f innerRadius %.5f color %s } ]
        }''' % (v3(add(centre, (0, RING_Y * S, 0))), 0.54 * RING_SCALE * S, 0.525 * RING_SCALE * S, v3(colour)))
    out.append('''        Transform
        {
            translation %s
            children [ Ribbon { radius %.5f height %.5f color %s inner true innerColor %s } ]
        }''' % (v3(add(centre, (0, BUMPER_Y * S, 0))), 0.5 * BUMPER_SCALE[0] * S, 0.02 * BUMPER_SCALE[1] * S,
                v3(RAIL_COLOR), v3(BUMPER_INNER)))
    base = add(centre, (0, EMBLEM_Y[name] * S, 0))
    for off, px in EMBLEM:
        out.append('''        Transform
        {
            translation %s
            children [ Sprite { url "circle.xtx" pixelSize %.6f color %s } ]
        }''' % (v3(add(base, mul(off, EMBLEM_SCALE * S))), px * EMBLEM_SCALE * S, v3(tint)))
    out.append('''        Transform
        {
            translation %s
            children [ DEF label_%s Sprite { url "label_%s.xtx" pixelSize %.6f depthTest false } ]
        }''' % (v3(add(centre, (0, LABEL_Y * S, 0))), name, name, LABEL_PIXEL * S / LABEL_OVERSAMPLE))
    return "\n".join(out)


# ---- scenes ----------------------------------------------------------------

def hexcol(c, a=1.0):
    return "0x%02x%02x%02x%02x" % (int(a * 255 + 0.5), int(c[0] * 255 + 0.5), int(c[1] * 255 + 0.5), int(c[2] * 255 + 0.5))


def rect_points(w, h):
    return "%g,%g;0,0;%g,0;%g,%g;0,%g" % (w, h, w, w, h, h)


def overlay_scene():
    """The screen overlay: the faded word behind the view, the A prompt
    under the panel and the footer with the wordmark (appUILayer/footer)."""
    w, h = HUD_W, HUD_H
    fh = FOOTER_H * UI
    stops = "\n".join("   <StopColor>%s</StopColor>" % hexcol(FOOTER_COLOR, a * FOOTER_ALPHA) for _, a in FOOTER_STOPS)
    pos = "\n".join("   <StopPos>%g</StopPos>" % p for p, _ in FOOTER_STOPS)
    body = "\n".join([
        # the big faded word, top left, cropped by the top edge like the mockup
        "\n".join(["<XuiText>", "<Properties>", " <Id>word</Id>", " <Width>640</Width>", " <Height>120</Height>",
                   " <Position>-4,-26,0</Position>", " <Text>discover</Text>", " <TextColor>0x37ffffff</TextColor>",
                   " <PointSize>%.2f</PointSize>" % (111 * UI * PT), " <TextStyle>16</TextStyle>", "</Properties>", "</XuiText>"]),
        # the A pin over the focused pad: sokky's Figma A_Focus (node
        # 143:146 of fL710Dg0cpzZzYe0OZSBVu), a 128 px frame holding the lime
        # pin (its Material teardrop, point down, as a figure in the Figma's
        # own points) with the Xelu A in its round head, at the mockup's
        # 720-high UI scale, centred on the pad, the tip just above the label
        "\n".join(["<XuiGroup>", "<Properties>", " <Id>promptA</Id>",
                   " <Width>%.3f</Width>" % (128 * UI), " <Height>%.3f</Height>" % (128 * UI),
                   " <Position>%.3f,%.3f,0</Position>" % (360 - 64 * UI, PIN_TIP_Y - 114.667 * UI),
                   "</Properties>",
                   "<XuiFigure>", "<Properties>", " <Id>pin</Id>",
                   " <Width>%.3f</Width>" % (85.334 * UI), " <Height>%.3f</Height>" % (101.334 * UI),
                   " <Position>%.3f,%.3f,0</Position>" % (21.3333 * UI, 13.3333 * UI),
                   " <Fill>\n  <FillType>1</FillType>\n  <FillColor>0xffc4ff46</FillColor>\n </Fill>", " <Closed>true</Closed>",
                   " <Points>85.334,101.334;42.667,0,54.489,0,64.556,4.089;72.867,12.267,81.178,20.444,85.333,30.4;"
                   "85.333,42.133,85.333,47.733,84.244,53.089;82.067,58.2,79.889,63.311,76.8,67.822;72.8,71.733;"
                   "42.667,101.333;12.533,71.733,8.533,67.822,5.445,63.311;3.267,58.2,1.089,53.089,0,47.733;"
                   "0,42.133,0,30.4,4.156,20.444;12.467,12.267,20.778,4.089,30.845,0</Points>",
                   "</Properties>", "</XuiFigure>",
                   "<XuiImage>", "<Properties>", " <Id>pinA</Id>",
                   " <Width>%.3f</Width>" % (61 * UI), " <Height>%.3f</Height>" % (61 * UI),
                   " <Position>%.3f,%.3f,0</Position>" % (33 * UI, 24 * UI),
                   " <ImagePath>glyph_a_button.png</ImagePath>", " <SizeMode>4</SizeMode>",
                   "</Properties>", "</XuiImage>", "</XuiGroup>"]),
        # the game title of the belt view: screen space, facing the camera,
        # centred above the focused cover (the cover's top is at about y 165
        # in the 96-degree view), 1.25x the size it had on the cover
        "\n".join(["<XuiText>", "<Properties>", " <Id>title</Id>", " <Width>%d</Width>" % w, " <Height>30</Height>",
                   " <Position>0,122,0</Position>", " <Opacity>0</Opacity>", " <Text></Text>", " <TextColor>0xffffffff</TextColor>",
                   " <DropShadowColor>0</DropShadowColor>", " <PointSize>%.2f</PointSize>" % (22 * PT), " <TextStyle>1040</TextStyle>",
                   "</Properties>", "</XuiText>"]),
        # B + return arrow of the belt view: screen space, centred between
        # the focused cover and the library label as in sokky's mockup (B at
        # 0.506 W, the arrow box 0.482-0.519 W; 52 px above the mockup's
        # 0.828 H because the wider view lifts the label), shown only while
        # the belt is open
        "\n".join(["<XuiImage>", "<Properties>", " <Id>bGlyph</Id>", " <Width>14.4</Width>", " <Height>14.4</Height>",
                   " <Position>357.1,338.2,0</Position>", " <Opacity>0</Opacity>", " <ImagePath>glyph_b.png</ImagePath>", " <SizeMode>4</SizeMode>",
                   "</Properties>", "</XuiImage>"]),
        "\n".join(["<XuiFigure>", "<Properties>", " <Id>backStem</Id>", " <Width>26.6</Width>", " <Height>9.6</Height>",
                   " <Position>347.0,353.6,0</Position>", " <Opacity>0</Opacity>",
                   " <Stroke>\n  <StrokeWidth>3</StrokeWidth>\n  <StrokeColor>0xffd8d8d8</StrokeColor>\n </Stroke>",
                   " <Fill>\n  <FillType>0</FillType>\n </Fill>", " <Closed>false</Closed>", " <Points>26.6,9.6;25.1,0.0;25.1,9.6;1.0,9.6</Points>", "</Properties>", "</XuiFigure>"]),
        "\n".join(["<XuiFigure>", "<Properties>", " <Id>backHead</Id>", " <Width>9.4</Width>", " <Height>20.2</Height>",
                   " <Position>347.0,353.6,0</Position>", " <Opacity>0</Opacity>",
                   " <Stroke>\n  <StrokeWidth>3</StrokeWidth>\n  <StrokeColor>0xffd8d8d8</StrokeColor>\n </Stroke>",
                   " <Fill>\n  <FillType>0</FillType>\n </Fill>", " <Closed>false</Closed>", " <Points>9.4,20.2;9.4,0.0;0.0,9.6;9.4,20.2</Points>", "</Properties>", "</XuiFigure>"]),
        # the footer strip
        "\n".join(["<XuiFigure>", "<Properties>", " <Id>footer</Id>", " <Width>%d</Width>" % w, " <Height>%g</Height>" % fh,
                   " <Position>0,%g,0</Position>" % (h - fh),
                   " <Fill>\n  <FillType>2</FillType>\n  <Gradient>\n   <NumStops>%d</NumStops>\n%s\n%s\n  </Gradient>\n  <Rotation>-90</Rotation>\n </Fill>" % (len(FOOTER_STOPS), stops, pos),
                   " <Closed>true</Closed>", " <Points>%s</Points>" % rect_points(w, fh), "</Properties>", "</XuiFigure>"]),
        "\n".join(["<XuiText>", "<Properties>", " <Id>frolick</Id>", " <Width>%g</Width>" % (136 * UI), " <Height>%g</Height>" % (42 * UI),
                   " <Position>%g,%g,0</Position>" % (w - 136 * UI, h - 42 * UI), " <Text>frolick</Text>",
                   " <TextColor>%s</TextColor>" % hexcol(WORDMARK_COLOR), " <PointSize>%.2f</PointSize>" % (37 * UI * PT),
                   " <TextStyle>%d</TextStyle>" % (0x10 | 0x400 | 0x1000), "</Properties>", "</XuiText>"]),
    ])
    return ('<XuiCanvas version="000c">\n<Properties>\n <Id>canvas</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n'
            '</Properties>\n<XuiScene>\n<Properties>\n <Id>Overlay</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n'
            '</Properties>\n%s\n</XuiScene>\n</XuiCanvas>\n' % (w, h, w, h, body))


def settings_scene(catalog):
    """Frolick's settings panel is the catalog layout relabelled."""
    s = catalog.replace("CatalogPanel", "SettingsPanel")
    for old, new in [("explore", "settings"), ("spotlight +", "settings"), ("featured", "quick panel"),
                     ("downloads", "video"), ("Stiletto Swift", "Config Core"), ("70%", "12%"),
                     ("STILETTO", "SETTINGS"), ("SWIFT", "")]:
        s = s.replace("<Text>%s</Text>" % old, "<Text>%s</Text>" % new)
    # the progress bar: 12% of the track instead of 70%
    m = re.search(r"<Id>dlFill</Id>\s*<Width>([\d.]+)</Width>", s)
    if m:
        w = float(m.group(1))
        s = s[:m.start(1)] + "%g" % round(w / 0.7 * 0.12) + s[m.end(1):]
    s = s.replace("<ImagePath>hero.png</ImagePath>", "<ImagePath>settings_hero.png</ImagePath>")
    return s


# ---- the hub ---------------------------------------------------------------

HUB = r'''//
// Frolick for the Xbox dashboard engine: the hub.  Generated by
// tools/genhub.py from frolick_assets/main.tscn.
//

DEF theScreen Screen
{
    width 640
    height 480
}

NavigationInfo
{
    type "sample"
    headlight false
    avatarSize 0.1 1.6 0.75
}

DEF theBackground Background
{
    skyColor 0.039 0.039 0.059
    backdrop XtxTexture { url "backdrop.xtx" }
}

// The library pad's camera loads first, so the dashboard opens on it.
DEF vpLibrary Viewpoint
{
    %(vp_library)s
}

DEF vpCatalog Viewpoint
{
    %(vp_catalog)s
}

DEF vpSettings Viewpoint
{
    %(vp_settings)s
}

// main.tscn's cam_angle_padfocus, and its mirror for the settings pad.
DEF vpCatalogFocus Viewpoint
{
    %(vp_catalogfocus)s
}

DEF vpSettingsFocus Viewpoint
{
    %(vp_settingsfocus)s
}

// The library carousel's camera (carousel.go carouselEyeTarget).
DEF vpCarousel Viewpoint
{
    %(vp_carousel)s
}

// Launching a game, as the dashboard does it (default.xap theLauncherLevel):
// the launcher level's black sky and Xbox logo backdrop bind as the
// transition starts, the hub cross-fades out over the Level's 0.75 s,
// input is off, the logo holds 1.1 s, then the frame is persisted and
// the console reboots into the game (GameCollection.LaunchGame).
DEF theXboxLogoBackground Background
{
    skyColor 0 0 0
    backdrop ImageTexture { url "xboxlogo.bmp" }
}

// The launch sound (art/sounds/launch.wav, sokky's pick, a loose file beside
// the XBE as the dashboard keeps its own sounds) starts with the launch;
// the logo holds until it has finished playing.
DEF theLaunchSound AudioClip
{
    url "launch.wav"
    volume 1
    function OnEndOfAudio() { bLaunchSoundDone = true; }
}

// The UI sounds (sokky 2026-10-21): Frolick's own woosh (frolick_assets
// woosh111.ogg) for moving pad to pad, the Xbox 360 Blades dashboard's
// sounds for everything else (xam shrdres btn_Focus / btn_Select /
// btn_Back; dashmain's 2ndLevelOpen/Close for the pad focus and the belt,
// 3rdLevelOpen/Close for a page over a page), decoded from XMA to WAV in
// art/sounds/, loose files beside the XBE like launch.wav.  Stop() before
// Play() so a sound retriggers from its start.
DEF sndWoosh AudioClip { url "woosh.wav" volume 1 }
DEF sndFocus AudioClip { url "blades_focus.wav" volume 1 }
DEF sndSelect AudioClip { url "blades_select.wav" volume 1 }
DEF sndBack AudioClip { url "blades_back.wav" volume 1 }
DEF sndOpen AudioClip { url "blades_open.wav" volume 1 }
DEF sndClose AudioClip { url "blades_close.wav" volume 1 }
DEF sndPageOpen AudioClip { url "blades_page_open.wav" volume 1 }
DEF sndPageClose AudioClip { url "blades_page_close.wav" volume 1 }

// The menu ambience (sokky 2026-10-21, menu_music.mp3, 11.5 minutes):
// looped under the menus from start-up, at the track's own (quiet)
// level.  It is an Xbox ADPCM WAV (tools/xbadpcm.c, 34 MB), which the
// engine streams from the file (CFilePump, anything over 64 KB) and wraps
// to the start at the end.  A launch fades it out over the hub's fade
// (volume is attenuation in dB here: 1 - volume = hundreds of dB) and
// stops it; Frolick comes back as a fresh start, music and all.
DEF sndMusic AudioClip { url "menu_music.wav" loop true pause_on_moving false volume 1 }

DEF theLaunchLevel Level
{
    function OnActivate()
    {
        EnableInput(false);
        theXboxLogoBackground.isBound = true;
    }
    function OnArrival()
    {
        theXboxLogoBackground.isBound = true;
        // the cross-fade has landed on the logo
        bLaunching = true;
    }
    behavior
    {
        // (a level's behavior runs from start-up, before any GoTo, so it
        // polls for the arrival flag, as the dashboard's g_bAboutToReboot;
        // the launch waits for the sound to end as well, or for 10 s if
        // the sound never reports its end)
        sleep 0.05;
        if (bMusicFade)
        {
            nMusicVol = nMusicVol - 0.07;
            if (nMusicVol <= 0)
            {
                bMusicFade = false;
                sndMusic.Stop();
            }
            else
                sndMusic.volume = nMusicVol;
        }
        if (bLaunching)
        {
            nLaunchWait = nLaunchWait + 1;
            if (nLaunchWait > 200)
                bLaunchSoundDone = true;
            if (bLaunchSoundDone)
                theGameCollection.LaunchGame(nLaunchGame);
        }
        // the Downloads page follows its active download (twice a second)
        if (bPageOpen)
        {
            if (sPage == "page_downloads")
            {
                nDlTick = nDlTick + 1;
                if (nDlTick >= 10)
                {
                    nDlTick = 0;
                    DlTick();
                }
            }
        }
    }
}

DEF theMover CameraMover
{
    duration 0.55
    function OnArrive()
    {
        bMoving = false;
        // settled on the pad: the A pin fades back in
        if (bZoomed == false)
        {
            if (bLibraryOpen == false)
                theOverlay.Fade("promptA", 1, %(pin_fade_in)s);
        }
    }
}

// The zoom onto a pad takes longer than a pad switch, as Frolick's carousel does.
DEF theZoomer CameraMover
{
    duration %(zoom_duration)s
    function OnArrive()
    {
        bMoving = false;
        if (bZoomed)
        {
            if (sPendingPage == "")
                OpenPage();
            else
                OpenPageAt(sPendingPage);
            sPendingPage = "";
        }
        else if (bLibraryOpen == false)
        {
            // back on the pad's view: the hub's A prompt returns
            theOverlay.Fade("promptA", 1, %(pin_fade_in)s);
        }
    }
}

// The console behind the views, the dashboard's own code: the games on
// the disc, the game card and the hard disk (GameCollection, which also
// launches them), the saved games and their devices (Storage over the
// Memory screen's title lists and copier), the memory units coming and
// going (MemoryMonitor).
DEF theGameCollection GameCollection
{
    // a disc or game card came or went: the belt's tiles change
    function OnMediaChanged()
    {
        UpdateLibraryTile();
        if (bLibraryOpen)
            RefreshBelt();
    }
    function OnInstallProgress() { InstallProgress(); }
    function OnInstallComplete() { InstallDone(); }
    function OnInstallError() { Notice(theGameCollection.GetInstallError(), "page_game"); bBusy = false; }
}

DEF theMemoryMonitor MemoryMonitor
{
    enumerationOn true
    function OnDeviceChange()
    {
        if (bPageOpen)
        {
            if (PageKind(sPage) == "devices")
                PageEnter();
            if (PageKind(sPage) == "dest")
                PageEnter();
        }
    }
    function OnCurDevUnitChange() { }
    function OnTotalFreeChanged() { }
    function OnInvalidMU(a, b) { }
    function OnInvalidMURemoved() { }
}

DEF theDownloads Downloads
{
}

DEF theStorage Storage
{
    function OnCopyProgress() { StorageProgress(); }
    function OnCopyComplete() { StorageCopied(); }
    function OnCopyError() { StorageFailed("The copy failed."); }
    function OnDeleteEnd() { StorageDeleted(); }
}

var nPad;       // 0 catalog, 1 library, 2 settings
var bMoving;
var bZoomed;    // the camera is on the pad's focus view (catalog, settings)
var bLibraryOpen;   // the camera is in the game belt
var nGame;          // the belt game whose page is open (GameCollection index)
var nLaunchGame;    // the game the launch level starts
var bLaunching;     // set when the launch level has faded in
var bLaunchPending; // a launch is under way (ignore further presses)
var bLaunchSoundDone; // the launch sound has finished (OnEndOfAudio)
var nLaunchWait;    // polls since arrival, the 10 s fallback
var bPageOpen;      // the padfocus page is up (after the zoom)
var bBusy;          // a copy or delete runs: input waits

DEF theHub Level
{
    unloadable false    // no archive to unload when the launch level takes over
    children
    [
        // world3D/bg: the atmo dome, multiplied over the backdrop, and the
        // rail ribbon that circles the scene above and behind the pads.
        Transform
        {
            translation %(atmo_origin)s
            children [ Dome { radius %(atmo_radius).5f height %(atmo_height).5f } ]
        }
        Transform
        {
            translation %(rail_origin)s
            children [ Ribbon { radius %(rail_radius).5f height %(rail_height).5f color %(rail_color)s } ]
        }

        // world3D/nav: the pads
%(pad_catalog)s
%(pad_library)s
%(pad_settings)s

        // Each pad's panel faces its camera, faded in and out by its Transform.
        DEF xfCatalog Transform
        {
            fade 0.55
            alpha 0
            children
            [
                DEF padCatalog XuiPanel
                {
                    url "pads/catalog.xui"
                    skin "pads/frolick_skin.xui"
                    %(panel_catalog)s
                    function OnPress(id) { OnPadPress(0, id); }
                }
            ]
        }
        DEF xfLibrary Transform
        {
            fade 0.55
            alpha 1
            children
            [
                DEF padLibrary XuiPanel
                {
                    url "pads/library.xui"
                    skin "pads/frolick_skin.xui"
                    %(panel_library)s
                    function OnPress(id) { OnPadPress(1, id); }
                }
            ]
        }
        // The library view: GameTap's game belt (art/gametap) around the
        // carousel camera, entered from the library pad with A, left and
        // right step it, B leaves.
        DEF theBelt Belt
        {
            url "belt.bin"
            card "pads/carousel_card.xui"
            focus "pads/carousel_focus.xui"
            skin "pads/frolick_skin.xui"
            icon "pads/carousel_icon.xui"
            cover "pads/carousel_cover.xui"
            icons "%(belt_icons)s"
            start %(belt_start)d
            centre %(belt_centre)s
            radius %(belt_radius).5f
            coverScale %(belt_cover_scale).2f
            focusPull %(belt_focus_pull).2f
            skip %(belt_skip)d
            fadeDuration %(zoom_duration)s
            count %(belt_count)d
            function OnStep() { theOverlay.SetText("title", GameName(theBelt.GetSelected())); }
        }

        DEF xfSettings Transform
        {
            fade 0.55
            alpha 0
            children
            [
                DEF padSettings XuiPanel
                {
                    url "pads/settings.xui"
                    skin "pads/frolick_skin.xui"
                    %(panel_settings)s
                    function OnPress(id) { OnPadPress(2, id); }
                }
            ]
        }

        // appUILayer: the word behind the view, the A prompt, the footer.
        HUD
        {
            children
            [
                // The padfocus page: a Blades-style full-screen page
                // (tools/genpage.py) opened when the camera lands on the
                // catalog or settings focus view.
                DEF thePage XuiPanel
                {
                    url "pads/page_console.xui"
                    skin "pads/page_skin.xui"
                    width %(page_w).1f
                    viewY %(page_vy).1f
                    function OnFocus(id) { PageFocus(id); }
                    function OnPress(id) { PagePress(id); }
                }
                DEF theOverlay XuiPanel
                {
                    url "pads/overlay.xui"
                    width %(hud_w)d
                }
            ]
        }
    ]

    control DEF theJoystick Joystick
    {
        function OnMoveLeft() { PadMove("left", -1); }
        function OnMoveRight() { PadMove("right", 1); }
        function OnMoveUp() { PadMove("up", 0); }
        function OnMoveDown() { PadMove("down", 0); }
        function OnADown() { PadPress(); }
        function OnBDown() { PadBack(); }
        function OnXDown() { PadX(); }
        function OnYDown() { PadY(); }
        function OnStartDown() { PadStart(); }
    }
}

function SndWoosh() { sndWoosh.Stop(); sndWoosh.Play(); }
function SndFocus() { sndFocus.Stop(); sndFocus.Play(); }
function SndSelect() { sndSelect.Stop(); sndSelect.Play(); }
function SndBack() { sndBack.Stop(); sndBack.Play(); }
function SndOpen() { sndOpen.Stop(); sndOpen.Play(); }
function SndClose() { sndClose.Stop(); sndClose.Play(); }
function SndPageOpen() { sndPageOpen.Stop(); sndPageOpen.Play(); }
function SndPageClose() { sndPageClose.Stop(); sndPageClose.Play(); }

// Left and right fly to the next pad; the tiles inside a panel are not
// navigated (sokky: the pads are the choices, not the tiles), so up and
// down do nothing on the hub.
function PadMove(dir, step)
{
    if (bMoving)
        return;
    if (bZoomed)
    {
        // the page: up and down walk its list (its own NavUp/NavDown);
        // a list the script fills scrolls at its ends
        if (bBusy)
            return;
        if (bPageOpen)
        {
            if (thePage.Navigate(dir) == 0)
                PageScroll(dir);
            else
                SndFocus();
        }
        return;
    }
    if (bLibraryOpen)
    {
        if (bPageOpen)
        {
            if (bBusy)
                return;
            if (thePage.Navigate(dir) == 0)
                PageScroll(dir);
            else
                SndFocus();
            return;
        }
        // the belt: left scrolls it right (Belt_Right), right scrolls it left
        if (step < 0)
            theBelt.Step(0 - 1);
        else if (step > 0)
            theBelt.Step(1);
        if (step != 0)
            SndFocus();
        return;
    }

    var n = nPad + step;
    if (n >= 0)
    {
        if (n <= 2)
        {
            if (n != nPad)
                GoPad(n);
        }
    }
}

function PadPress()
{
    if (bMoving)
        return;
    if (bZoomed)
    {
        if (bBusy)
            return;
        if (bPageOpen)
        {
            SndSelect();
            thePage.Press();
        }
        return;
    }
    if (bLibraryOpen)
    {
        // A on a cover opens the game's page; Start launches it (PadStart)
        if (bPageOpen)
        {
            if (bBusy)
                return;
            SndSelect();
            thePage.Press();
            return;
        }
        if (theBelt.IsBusy() == 0)
            OpenGamePage();
        return;
    }

    // the A pin goes as A lands, not when the pad's press animation
    // ends and the flight starts
    theOverlay.Fade("promptA", 0, %(pin_fade_out)s);
    if (nPad == 0)
        padCatalog.Press();
    else if (nPad == 1)
        padLibrary.Press();
    else
        padSettings.Press();
}

function OnPadPress(pad, id)
{
    // Frolick: A on the library pad opens the game carousel.  On the
    // catalog and settings pads it flies down to the pad's focus view.
    if (pad == 1)
    {
        OpenLibrary();
        return;
    }
    ZoomIn();
}

// A on the library panel: the panel fades out and the camera flies in
// 750 ms (Frolick's carousel zoom) while the belt's covers fade in.
function OpenLibrary()
{
    bMoving = true;
    bLibraryOpen = true;
    SetPadAlpha(1, 0);
    theZoomer.GoTo(vpCarousel);
    RefreshBelt();
    theBelt.Open();
    theOverlay.SetText("title", GameName(theBelt.GetSelected()));
    ShowBeltHints(1);
}

// The belt's tiles: All games (the grid, not built yet), then the
// console's games as GameCollection lists them (the disc tray or game
// card slots first, then E:\Games), each cover with the game's title image.
// The library pad's game tile: the disc or game card that is in (its
// cover, name and publisher, Start to launch), or sokky's no-media variant
// of the panel (Figma 143:47: "Insert a Game Disc or Game Card." and the
// disc and card icons) when neither is present.  Kept current by
// OnMediaChanged.
function FindMedia()
{
    var n = theGameCollection.Scan();
    var i;
    var kind;
    var found = -1;
    for (i = 0; i < n; i = i + 1)
    {
        kind = theGameCollection.GetGameKind(i);
        if (found < 0)
        {
            if (kind == 1)
                found = i;
            if (kind == 2)
                found = i;
        }
    }
    return found;
}

function UpdateLibraryTile()
{
    var found = FindMedia();
    var on = 0;
    if (found >= 0)
    {
        on = 1;
        padLibrary.SetText("title", theGameCollection.GetGameName(found));
        padLibrary.SetText("subtitle", theGameCollection.GetGamePublisher(found));
        padLibrary.SetImage("cover", theGameCollection.GetGameImage(found));
    }
    padLibrary.SetVisible("coverFrame", on);
    padLibrary.SetVisible("cover", on);
    padLibrary.SetVisible("title", on);
    padLibrary.SetVisible("subtitle", on);
    padLibrary.SetVisible("glyphA", on);
    padLibrary.SetVisible("insertLine1", 1 - on);
    padLibrary.SetVisible("insertLine2", 1 - on);
    padLibrary.SetVisible("icoDisc", 1 - on);
    padLibrary.SetVisible("icoCard", 1 - on);
}

function RefreshBelt()
{
    var n = theGameCollection.Scan();
    var i;
    theBelt.count = n + 1;
    for (i = 0; i < n; i = i + 1)
        theBelt.SetGameImage(i + 1, theGameCollection.GetGameImage(i));
    theOverlay.SetText("title", GameName(theBelt.GetSelected()));
}

// Start on a cover: the game starts at once (D: becomes its directory
// and the console reboots into its XBE).  The empty tray and the grid
// tile do nothing yet.
function LaunchSelected()
{
    var sel = theBelt.GetSelected();
    if (sel == 0)
        return;
    if (theGameCollection.IsPlayable(sel - 1) == 0)
        return;
    LaunchGameWithLogo(sel - 1);
}

// The launch itself: the Xbox logo level, which launches when it has held
function LaunchGameWithLogo(n)
{
    if (bLaunchPending)
        return;
    bLaunchPending = true;
    nLaunchGame = n;
    bLaunchSoundDone = false;
    nLaunchWait = 0;
    nMusicVol = 1;
    bMusicFade = true;
    theLaunchSound.Play();
    if (theLaunchSound.transportMode != 1)   // no sound: don't wait for it
        bLaunchSoundDone = true;
    theLaunchLevel.GoTo();
}

function PadStart()
{
    if (bMoving)
        return;
    if (bBusy)
        return;
    if (bLibraryOpen == false)
    {
        if (bZoomed)
        {
            // Start on a recent game plays it, as A does
            if (bPageOpen)
                if (PageKind(sPage) == "recent")
                    PlayRecent();
            return;
        }
        HubButton("start");
        return;
    }
    if (bPageOpen)
        return;
    if (theBelt.IsBusy() == 0)
        LaunchSelected();
}

// The tiles' buttons on the hub (sokky 2026-10-20: the glyph on a tile is
// the button that opens it, and a press first flies to the pad's focus
// view, then opens the menu).  Library: Start launches the disc or game
// card at once, X the recent games, Y the favorites.  Catalog: Start the
// Marketplace, X (spotlight) its hot list, Y (downloads) the Downloads
// page.  Settings: Start the settings menu, X (quick panel) and Y
// (video) the console settings, Display first; the Blades scenes stand in
// until the real pages exist.
function HubButton(btn)
{
    var m;
    if (nPad == 1)
    {
        if (btn == "start")
        {
            m = FindMedia();
            if (m >= 0)
                LaunchGameWithLogo(m);
        }
        else if (btn == "x")
            FocusPad("page_recent");
        else
            FocusPad("page_favorites");
        return;
    }
    if (btn == "start")
    {
        PadPress();
        return;
    }
    if (nPad == 0)
    {
        if (btn == "x")
            FocusPad("page_store_hot");
        else
            FocusPad("page_downloads");
    }
    else
        FocusPad("page_console");
}

function FocusPad(page)
{
    sPendingPage = page;
    theOverlay.Fade("promptA", 0, %(pin_fade_out)s);
    ZoomIn();
}

function PadX()
{
    if (bMoving)
        return;
    if (bBusy)
        return;
    if (bPageOpen)
    {
        if (PageKind(sPage) == "downloads")
        {
            SndSelect();
            DlRemove();
        }
        return;
    }
    if (bZoomed)
        return;
    if (bLibraryOpen)
        return;
    HubButton("x");
}

// A or Start on a recent game
function PlayRecent()
{
    if (nListCount == 0)
        return;
    LaunchGameWithLogo(theGameCollection.GetRecentGame(nSel));
}

// A on a cover: the game's page (the Blades' Game Summary) over the
// belt: Play Game, Game Storage (its saved games in Settings > Memory),
// Install Game for a disc or card game not on the hard disk.
function OpenGamePage()
{
    var sel = theBelt.GetSelected();
    if (sel == 0)
        return;
    if (theGameCollection.IsPlayable(sel - 1) == 0)
        return;
    nGame = sel - 1;
    SndSelect();
    SndPageOpen();
    bPageOpen = true;
    ShowBeltHints(0);
    theOverlay.Fade("promptA", 0, %(pin_fade_out)s);
    ShowPage("page_game", "Open");
    PageChrome(0);
}

// The page's facts about the game, and whether Install Game applies.
function FillGamePage()
{
    var kind = theGameCollection.GetGameKind(nGame);
    var where = "Hard drive";
    var bInstall = 0;
    if (kind == 1)
        where = "Disc";
    if (kind == 2)
        where = "Game card";
    if (kind != 3)
    {
        if (theGameCollection.IsInstalled(nGame) == 1)
            where = where + ", installed";
        else
            bInstall = theGameCollection.CanInstall(nGame);
    }
    thePage.SetText("labGameTitle", theGameCollection.GetGameName(nGame));
    thePage.SetImage("imgGameIcon", theGameCollection.GetGameImage(nGame));
    thePage.SetText("labRecentAchievementsText", "Platform");
    thePage.SetText("labRecentAchievementsNum", theGameCollection.GetGamePlatform(nGame));
    thePage.SetText("labRuntimeText", "Runtime");
    thePage.SetText("labRuntimeNum", theGameCollection.GetGameRuntime(nGame));
    thePage.SetText("labAchievementsNum", "" + theGameCollection.GetSavedGameCount(nGame));
    thePage.SetText("labGamerPointsNum", where);
    thePage.SetVisible("btnBrowseDownloads", bInstall);
}

// Game Storage: the title's saved games page, as Settings > Memory shows
// it (B climbs out through the titles and devices pages).
function OpenGameStorage()
{
    var t;
    nDev = 8;
    theStorage.device = nDev;
    t = theStorage.FindTitle(theGameCollection.GetGameTitleID(nGame));
    if (t < 0)
    {
        Notice("This game has no saved games or stored items yet.", "page_game");
        return;
    }
    nTitle = t;
    ShowPage("page_memory_saves", "Fade");
}

function StartInstall()
{
    if (theGameCollection.StartInstall(nGame) == 0)
    {
        Notice("This game can't be installed.", "page_game");
        return;
    }
    bBusy = true;
    SetHeader("Installing...");
}

function InstallProgress()
{
    if (bBusy)
        SetHeader("Installing... " + theGameCollection.GetInstallStatus());
}

function InstallDone()
{
    bBusy = false;
    RefreshBelt();
    Notice("The game is installed on the hard drive.", "page_game");
}

// In the belt view the hub's A prompt goes and the B/back hint
// comes, in the screen overlay (not on the cover).
function ShowBeltHints(on)
{
    // (the pin comes back with the camera's arrival on the hub)
    if (on)
        theOverlay.Fade("promptA", 0, %(pin_fade_out)s);
    theOverlay.SetOpacity("title", on);
    theOverlay.SetOpacity("bGlyph", on);
    theOverlay.SetOpacity("backStem", on);
    theOverlay.SetOpacity("backHead", on);
}

function CloseLibrary()
{
    bMoving = true;
    SndClose();
    bLibraryOpen = false;
    SetPadAlpha(1, 1);
    theZoomer.GoTo(vpLibrary);
    theBelt.Close();
    ShowBeltHints(0);
}

// The belt's titles: tile 0 is the grid, the rest GameCollection's games
// (an empty tray says so).
function GameName(i)
{
    if (i == 0)
        return "All games";
    if (theGameCollection.GetGameKind(i - 1) == 0)
        return "No Disc";
    return theGameCollection.GetGameName(i - 1);
}

%(memory_script)s


// A on the catalog or settings panel: the panel fades out and the camera
// flies to cam_angle_padfocus.  B flies back up to the pad's view.
function ZoomIn()
{
    bMoving = true;
    bZoomed = true;
    SndOpen();
    SetPadAlpha(nPad, 0);
    LabelFocus(1);
    if (nPad == 0)
        theZoomer.GoTo(vpCatalogFocus);
    else if (nPad == 1)
        theZoomer.GoTo(vpCarousel);
    else
        theZoomer.GoTo(vpSettingsFocus);
}

function PadBack()
{
    if (bMoving)
        return;
    if (bBusy)
        return;
    if (bLibraryOpen)
    {
        if (bPageOpen)
        {
            // the game's page, or a Memory page reached from it
            if (PageBack() == 1)
                return;
            SndBack();
            SndPageClose();
            ClosePage();
            ShowBeltHints(1);
            return;
        }
        if (theBelt.IsBusy() == 0)
            CloseLibrary();
        return;
    }
    if (bZoomed == false)
        return;
    if (bPageOpen)
    {
        if (PageBack() == 1)
            return;
        ClosePage();
    }
    bMoving = true;
    bZoomed = false;
    SndClose();
    SetPadAlpha(nPad, 1);
    LabelFocus(0);
    if (nPad == 0)
        theZoomer.GoTo(vpCatalog);
    else if (nPad == 1)
        theZoomer.GoTo(vpLibrary);
    else
        theZoomer.GoTo(vpSettings);
}

function SetPadAlpha(pad, a)
{
    if (pad == 0)
        xfCatalog.SetAlpha(a);
    else if (pad == 1)
        xfLibrary.SetAlpha(a);
    else
        xfSettings.SetAlpha(a);
}

// The padfocus pages.  The camera lands on the pad's focus view and the
// pad's root page opens over it (30 frames, the Blades' 1OpenFull); A on
// an item fades its page in (the Blades' FadeIn between third-level
// scenes), B fades the parent back, and B on the root page closes it (the
// open backwards) while the camera flies back up.  While a page is open
// the hub's own A prompt gives way to the page's legend.
var sPage;      // the page on thePage
var sPendingPage; // the page a hub button opens once the focus flight lands
var bMusicFade;     // the menu music is fading out for a launch
var nMusicVol;      // its volume while it fades

function OpenPage()
{
    OpenPageAt(PageRoot(nPad));
}

function OpenPageAt(page)
{
    bPageOpen = true;
    ShowPage(page, "Open");
    theOverlay.Fade("promptA", 0, %(pin_fade_out)s);
    PageChrome(0);
}

// While a page is open the pad's big label stays, as the page's title
// (sokky: the page's own header is a sub-title under it); the overlay's
// faded word and the wordmark under the legend go.
function PageChrome(a)
{
    theOverlay.SetOpacity("word", a);
    theOverlay.SetOpacity("frolick", a);
}

// The pad's label slides into the frame with the zoom (it is the page's
// title there) and back with the flight out.
function LabelFocus(on)
{
    label_catalog.moveTime = %(zoom_duration)s;
    label_settings.moveTime = %(zoom_duration)s;
    if (nPad == 0)
    {
        label_catalog.offsetX = on * %(lofs_catalog_x)s;
        label_catalog.offsetY = on * %(lofs_catalog_y)s;
        label_catalog.offsetZ = on * %(lofs_catalog_z)s;
    }
    else if (nPad == 1)
    {
        // the library's focus view is the belt camera, where the label sits
        // small and far off; its pages carry their own header instead
        label_library.alpha = 1 - on;
    }
    if (nPad == 2)
    {
        label_settings.offsetX = on * %(lofs_settings_x)s;
        label_settings.offsetY = on * %(lofs_settings_y)s;
        label_settings.offsetZ = on * %(lofs_settings_z)s;
    }
}

function PadLabel(a)
{
    if (nPad == 0) label_catalog.alpha = a;
    if (nPad == 1) label_library.alpha = a;
    if (nPad == 2) label_settings.alpha = a;
    PageChrome(a);
}

function ShowPage(page, how)
{
    var first;
    sPage = page;
    thePage.url = PageUrl(page);
    thePage.Play("page", how);
    first = PageFirst(page);
    thePage.SetFocus(first);
    PageFocus(first);
    PageEnter();
    PageHeader();
}

function ClosePage()
{
    bPageOpen = false;
    thePage.Play("page", "Close");
    PageChrome(1);
}

// B on a page: the parent page, or the end of the page
function PageBack()
{
    var parent;
    if (sPage == "page_confirm")
        parent = sConfirmBack;
    else if (sPage == "page_notice")
        parent = sConfirmBack;
    else
        parent = PageParent(sPage);
    if (parent == "")
        return 0;
    SndBack();
    SndPageClose();
    ShowPage(parent, "Fade");
    return 1;
}

// The focused item's current setting, in the meta panel (the Blades'
// "Current Setting" pane or the network list's meta text).
function PageFocus(id)
{
    var s;
    var r;
    s = PageMeta(sPage, id);
    thePage.SetData("metaPanelScene", 4, s);   // metaScene_1line's Pane_txtCurrentSetting
    thePage.SetText("txt_Meta", s);
    r = RowOf(id);
    if (r >= 0)
    {
        nSel = nScroll + r;
        UpdateTotal();
        if (PageKind(sPage) == "downloads")
            DlLegend();
    }
}

function PagePress(id)
{
    var target;
    if (PageSpecial(id) == 1)
        return;
    target = PageTarget(sPage, id);
    if (target == "")
        return;     // no page behind this item yet: the bar's Press frame plays and that is all
    SndPageOpen();
    ShowPage(target, "Fade");
}

%(page_tables)s

function GoPad(n)
{
    bMoving = true;
    SndWoosh();
    theOverlay.Fade("promptA", 0, %(pin_fade_out)s);
    SetPadAlpha(nPad, 0);
    nPad = n;
    SetPadAlpha(nPad, 1);

    if (n == 0)
    {
        theMover.GoTo(vpCatalog);
        padCatalog.Play("", "Intro");
        theOverlay.SetText("word", "%(word_catalog)s");
    }
    else if (n == 1)
    {
        theMover.GoTo(vpLibrary);
        padLibrary.Play("", "Intro");
        theOverlay.SetText("word", "%(word_library)s");
    }
    else
    {
        theMover.GoTo(vpSettings);
        padSettings.Play("", "Intro");
        theOverlay.SetText("word", "%(word_settings)s");
    }
}

function initialize()
{
    nPad = 1;
    bMoving = false;
    bZoomed = false;
    bLibraryOpen = false;
    bPageOpen = false;
    bBusy = false;
    sPage = "";
    sPendingPage = "";
    sConfirmBack = "";
    sAction = "";
    nRows = 0;
    nScroll = 0;
    nSel = 0;
    nListCount = 0;
    nDev = 8;
    nTitle = 0;
    nSave = 0;
    nDest = 8;
    nGame = 0;
    nLaunchGame = 0;
    bLaunching = false;
    bLaunchPending = false;
    bLaunchSoundDone = false;
    nLaunchWait = 0;
    nDlTick = 0;
    nDlItem = 0;
    bMusicFade = false;
    nMusicVol = 1;
    sndMusic.Play();
    // the A pin's idle pulse: a small dip every PIN_PULSE_PERIOD seconds
    theOverlay.Pulse("promptA", %(pin_pulse_dy)s, %(pin_pulse_period)s, %(pin_pulse_dur)s);
    UpdateLibraryTile();
    EnableInput(true);
    theHub.GoTo();
    padLibrary.Play("", "Intro");
    theOverlay.SetText("word", "%(word_library)s");
}
'''


# The Memory pages' script: the lists the pages show are filled a row at
# a time from theStorage / theMemoryMonitor (the pages are the Blades'
# list scenes with as many rows as their box holds, tools/genpage.py
# PAGES dynamic), scrolled when the focus runs off an end, and the
# copy, move and delete of a save or a title's data run through
# theStorage with the dashboard's confirmations.  (The script language
# has no &&, || or unary minus, and a var declared in a block ends with it.)
MEMORY_SCRIPT = r"""
var nScroll;        // the first row's item
var nRows;          // rows on the page (0: not a list page)
var nListCount;     // items in the list
var nSel;           // the focused item
var nDev;           // the device the titles page shows (8 the hard disk)
var nTitle;         // the title the saves page shows
var nSave;          // the save the actions page is about
var nDest;          // the device a copy goes to
var sAction;        // "copy", "move", "delete", "deleteall"
var sConfirmBack;   // the page behind the confirm or notice page
var nDlTick;        // frames since the Downloads page last refreshed
var nDlItem;        // the download a confirm is about

// ---- the lists ----------------------------------------------------------

function RowId(r)
{
    return PageList(sPage) + "_" + r;
}

function RowOf(id)
{
    var r;
    for (r = 0; r < nRows; r = r + 1)
    {
        if (id == RowId(r))
            return r;
    }
    return 0 - 1;
}

// The devices in the order the pages list them: the hard disk, then the
// memory units that are in (units 0..7); skipDev left out (the source of
// a copy).
function DeviceAt(i, skipDev)
{
    var n = 0;
    var d;
    var k;
    for (k = 0; k < 9; k = k + 1)
    {
        if (k == 0)
            d = 8;
        else
            d = k - 1;
        if (d != skipDev)
        {
            if (theStorage.IsDeviceReady(d) == 1)
            {
                if (n == i)
                    return d;
                n = n + 1;
            }
        }
    }
    return 0 - 1;
}

function DeviceCount(skipDev)
{
    var n = 0;
    var d;
    var k;
    for (k = 0; k < 9; k = k + 1)
    {
        if (k == 0)
            d = 8;
        else
            d = k - 1;
        if (d != skipDev)
        {
            if (theStorage.IsDeviceReady(d) == 1)
                n = n + 1;
        }
    }
    return n;
}

function ListCount()
{
    var k = PageKind(sPage);
    if (k == "devices")
        return DeviceCount(0 - 1);
    if (k == "dest")
        return DeviceCount(nDev);
    if (k == "titles")
        return theStorage.GetTitleCount();
    if (k == "saves")
        return theStorage.GetSaveCount(nTitle);
    if (k == "recent")
        return theGameCollection.GetRecentCount();
    if (k == "downloads")
        return DlListCount();
    return 0;
}

// Row r shows item nScroll + r: its lines, its icon (the title's or
// save's own image, else the Blades' icon), a device's gauge.
function FillRow(r)
{
    var id = RowId(r);
    var i = nScroll + r;
    var k = PageKind(sPage);
    var d;
    var g;
    var img;
    var pub;
    if (i >= nListCount)
    {
        thePage.SetVisible(id, 0);
        return;
    }
    thePage.SetVisible(id, 1);
    if (k == "titles")
    {
        thePage.SetText(id, theStorage.GetTitleName(i));
        thePage.SetData(id, 1, theStorage.GetTitleInfo(i));
        img = theStorage.GetTitleImage(i);
        if (img == "")
            img = "ico_64x_games.png";
        thePage.SetImage(id + "_icon", img);
    }
    else if (k == "recent")
    {
        g = theGameCollection.GetRecentGame(i);
        thePage.SetText(id, theGameCollection.GetGameName(g));
        pub = theGameCollection.GetGamePublisher(g);
        if (pub == "")
            thePage.SetData(id, 1, theGameCollection.GetGamePlatform(g));
        else
            thePage.SetData(id, 1, theGameCollection.GetGamePlatform(g) + ", " + pub);
        img = theGameCollection.GetGameImage(g);
        if (img == "")
            img = "ico_64x_games.png";
        thePage.SetImage(id + "_icon", img);
    }
    else if (k == "downloads")
    {
        DlFillRow(id, i);
        return;
    }
    else if (k == "saves")
    {
        thePage.SetText(id, theStorage.GetSaveName(i));
        thePage.SetData(id, 1, theStorage.GetSaveInfo(i));
        img = theStorage.GetSaveImage(i);
        if (img == "")
            img = "ico_64x_games.png";
        thePage.SetImage(id + "_icon", img);
    }
    else
    {
        if (k == "devices")
            d = DeviceAt(i, 0 - 1);
        else
            d = DeviceAt(i, nDev);
        thePage.SetData(id, 2, theStorage.GetDeviceName(d));
        thePage.SetData(id, 1, theStorage.GetDeviceSpace(d));
        thePage.SetValue(id + "_bar", theStorage.GetDeviceFill(d));
        if (d == 8)
            img = "ico_64x_HD.png";
        else
            img = "ico_64x_MU.png";
        thePage.SetImage(id + "_icon", img);
    }
}

function UpdateTotal()
{
    if (nRows == 0)
        return;
    if (PageKind(sPage) == "downloads")
    {
        DlTotal();
        return;
    }
    if (nListCount == 0)
        thePage.SetText("labTotal", "");
    else
        thePage.SetText("labTotal", (nSel + 1) + " of " + nListCount);
}

function FillPage()
{
    var r;
    for (r = 0; r < nRows; r = r + 1)
        FillRow(r);
    if (nListCount == 0)
        thePage.SetVisible("txt_EmptyList", 1);
    else
        thePage.SetVisible("txt_EmptyList", 0);
    UpdateTotal();
}

// A list page just shown: its data from the console, the first row focused.
function PageEnter()
{
    var k = PageKind(sPage);
    if (k == "game")
    {
        FillGamePage();
        return;
    }
    nRows = PageRows(sPage);
    if (nRows == 0)
        return;
    if (k == "titles")
    {
        theStorage.device = nDev;
        theStorage.Refresh();
    }
    if (k == "saves")
    {
        theStorage.device = nDev;
        theStorage.title = nTitle;
    }
    if (k == "recent")
        theGameCollection.Scan();
    nScroll = 0;
    nSel = 0;
    nListCount = ListCount();
    FillPage();
    if (nListCount > 0)
        thePage.SetFocus(RowId(0));
    if (k == "downloads")
        DlEnter();
}

// The focus ran off an end of the rows: the list moves one item.
function PageScroll(dir)
{
    if (nRows == 0)
        return;
    if (PageKind(sPage) == "downloads")
        return;     // its sections are fixed to their rows (no scrolling yet)
    if (dir == "down")
    {
        if (nScroll + nRows < nListCount)
        {
            nScroll = nScroll + 1;
            nSel = nSel + 1;
            FillPage();
            SndFocus();
        }
    }
    else if (dir == "up")
    {
        if (nScroll > 0)
        {
            nScroll = nScroll - 1;
            nSel = nSel - 1;
            FillPage();
            SndFocus();
        }
    }
}

// The page's sub-title (the Blades scenes name it three ways).
function SetHeader(s)
{
    thePage.SetText("labHeader", s);
    thePage.SetText("txt_Header", s);
    thePage.SetText("labelHeading", s);
}

function PageHeader()
{
    if (sPage == "page_memory_titles")
        SetHeader(theStorage.GetDeviceName(nDev));
    if (sPage == "page_memory_saves")
        SetHeader(theStorage.GetTitleName(nTitle));
    if (sPage == "page_memory_actions")
        SetHeader(theStorage.GetSaveName(nSave));
    if (sPage == "page_game")
        SetHeader(theGameCollection.GetGameName(nGame));
}

// ---- what A does on the Memory pages ------------------------------------

function Confirm(question, back)
{
    sConfirmBack = back;
    ShowPage("page_confirm", "Fade");
    SetHeader(question);
}

function Notice(text, back)
{
    sConfirmBack = back;
    ShowPage("page_notice", "Fade");
    SetHeader(text);
}

function PageSpecial(id)
{
    var k = PageKind(sPage);
    if (k == "devices")
    {
        nDev = DeviceAt(nSel, 0 - 1);
        if (nDev < 0)
            return 1;
        ShowPage("page_memory_titles", "Fade");
        return 1;
    }
    if (k == "titles")
    {
        if (nListCount == 0)
            return 1;
        nTitle = nSel;
        ShowPage("page_memory_saves", "Fade");
        return 1;
    }
    if (k == "saves")
    {
        if (nListCount == 0)
            return 1;
        nSave = nSel;
        theStorage.save = nSave;
        ShowPage("page_memory_actions", "Fade");
        return 1;
    }
    if (k == "recent")
    {
        PlayRecent();
        return 1;
    }
    if (k == "favorites")
        return 1;
    if (k == "downloads")
    {
        DlPress();
        return 1;
    }
    if (k == "dest")
    {
        nDest = DeviceAt(nSel, nDev);
        if (nDest < 0)
            return 1;
        theStorage.save = nSave;
        if (theStorage.SaveExists(nDest) == 1)
            Confirm("Replace the copy on " + theStorage.GetDeviceName(nDest) + "?", "page_memory_dest");
        else
            StorageStart();
        return 1;
    }
    if (k == "game")
    {
        if (id == "btnViewAchievements")
            LaunchGameWithLogo(nGame);
        else if (id == "btnManageGameFiles")
            OpenGameStorage();
        else
            StartInstall();
        return 1;
    }
    if (sPage == "page_memory_actions")
    {
        if (id == "btnDelete")
        {
            sAction = "delete";
            Confirm("Delete " + theStorage.GetSaveName(nSave) + "?", "page_memory_actions");
            return 1;
        }
        if (theStorage.CanCopy(nSave) == 0)
        {
            Notice("This item can't be copied.", "page_memory_actions");
            return 1;
        }
        if (DeviceCount(nDev) == 0)
        {
            Notice("Insert a memory unit to copy to.", "page_memory_actions");
            return 1;
        }
        if (id == "btnCopy")
            sAction = "copy";
        else
            sAction = "move";
        ShowPage("page_memory_dest", "Fade");
        return 1;
    }
    if (sPage == "page_confirm")
    {
        if (id == "btnYes")
        {
            if (sAction == "dlremove")
                DlRemoveNow();
            else
                StorageStart();
        }
        else
            ShowPage(sConfirmBack, "Fade");
        return 1;
    }
    if (sPage == "page_notice")
    {
        ShowPage(sConfirmBack, "Fade");
        return 1;
    }
    return 0;
}

// Y: delete the focused title's data, or the focused save.
function PadY()
{
    if (bMoving)
        return;
    if (bBusy)
        return;
    if (bPageOpen == false)
    {
        if (bZoomed)
            return;
        if (bLibraryOpen)
            return;
        HubButton("y");
        return;
    }
    if (PageKind(sPage) == "titles")
    {
        if (nListCount == 0)
            return;
        nTitle = nSel;
        sAction = "deleteall";
        SndSelect();
        Confirm("Delete all data for " + theStorage.GetTitleName(nTitle) + "?", "page_memory_titles");
    }
    else if (PageKind(sPage) == "saves")
    {
        if (nListCount == 0)
            return;
        nSave = nSel;
        sAction = "delete";
        SndSelect();
        Confirm("Delete " + theStorage.GetSaveName(nSave) + "?", "page_memory_saves");
    }
}

// ---- the copy, move and delete ------------------------------------------

function StorageStart()
{
    bBusy = true;
    theStorage.device = nDev;
    theStorage.title = nTitle;
    if (sAction == "deleteall")
        theStorage.save = 0 - 1;
    else
        theStorage.save = nSave;
    if (sAction == "copy")
    {
        SetHeader("Copying...");
        theStorage.StartCopy(nDest);
    }
    else if (sAction == "move")
    {
        SetHeader("Moving...");
        theStorage.StartCopy(nDest);
    }
    else
    {
        SetHeader("Deleting...");
        theStorage.StartDelete();
    }
}

function StorageProgress()
{
    if (sAction == "move")
        SetHeader("Moving... " + Math.floor(theStorage.copyProgress * 100) + "%");
    else
        SetHeader("Copying... " + Math.floor(theStorage.copyProgress * 100) + "%");
}

// A move is a copy, then the source goes.
function StorageCopied()
{
    if (sAction == "move")
    {
        theStorage.save = nSave;
        theStorage.StartDelete();
        return;
    }
    bBusy = false;
    ShowPage("page_memory_saves", "Fade");
}

function StorageDeleted()
{
    var a = sAction;
    bBusy = false;
    sAction = "";
    if (a == "deleteall")
        ShowPage("page_memory_titles", "Fade");
    else
        ShowPage("page_memory_saves", "Fade");
}

function StorageFailed(text)
{
    bBusy = false;
    sAction = "";
    Notice(text, "page_memory_saves");
}

// ---- the Downloads page --------------------------------------------------
// The NXE's Active Downloads (sokky 2026-10-20): row 0 is the active
// download under "Active" (blank when none), the rows under "Pending"
// the queue.  theDownloads lists the active item first, so the list's
// virtual item v is download v - nDlOff (nDlOff 1 when nothing is
// active, the blank row).  A starts a pending download now or pauses /
// resumes the active one, X removes the focused one (asking first when
// it is under way), the legend's A names what it does.

function DlOff()
{
    return 1 - theDownloads.HasActive();
}

function DlListCount()
{
    var n = theDownloads.GetCount() + DlOff();
    if (n > nRows)
        n = nRows;      // no scrolling: the sections are fixed to their rows
    return n;
}

function DlFillRow(id, v)
{
    var d = v - DlOff();
    if (d < 0)
    {
        thePage.SetVisible(id, 0);
        return;
    }
    thePage.SetText(id, theDownloads.GetName(d));
    thePage.SetData(id, 1, theDownloads.GetKind(d) + "   " + theDownloads.GetDetail(d));
    thePage.SetData(id, 3, theDownloads.GetStatus(d));
    thePage.SetImage(id + "_icon", theDownloads.GetIcon(d));
    if (d == 0)
    {
        if (theDownloads.HasActive() == 1)
        {
            thePage.SetValue(id + "_bar", theDownloads.GetProgress(d));
            thePage.SetVisible(id + "_bar", 1);
        }
        else
            thePage.SetVisible(id + "_bar", 0);
    }
    else
        thePage.SetVisible(id + "_bar", 0);
}

function DlTotal()
{
    var n = theDownloads.GetCount();
    var d = nSel - DlOff();
    if (n == 0)
        thePage.SetText("labTotal", "");
    else if (d < 0)
        thePage.SetText("labTotal", n + " waiting");
    else
        thePage.SetText("labTotal", (d + 1) + " of " + n);
}

// what A does to the focused item
function DlLegend()
{
    var d = nSel - DlOff();
    if (d < 0)
        thePage.SetText("legend_a", "");
    else if (d > 0)
        thePage.SetText("legend_a", "Start Now");
    else if (theDownloads.HasActive() == 0)
        thePage.SetText("legend_a", "Start Now");
    else if (theDownloads.IsPaused() == 1)
        thePage.SetText("legend_a", "Resume");
    else
        thePage.SetText("legend_a", "Pause");
    if (theDownloads.GetCount() == 0)
        thePage.SetText("legend_x", "");
    else
        thePage.SetText("legend_x", "Remove");
}

// the page just opened: the first download focused (row 0 is blank when
// nothing is active)
function DlEnter()
{
    if (theDownloads.GetCount() == 0)
        return;
    nSel = DlOff();
    thePage.SetFocus(RowId(nSel));
    DlTotal();
    DlLegend();
}

// the list again after a change, the focus kept on the same row
function DlRefresh()
{
    var n = DlListCount();
    nListCount = n;
    if (nSel >= n)
        nSel = n - 1;
    if (nSel < DlOff())
        nSel = DlOff();
    FillPage();
    if (n > DlOff())
        thePage.SetFocus(RowId(nSel));
    DlLegend();
}

// twice a second while the page shows: the active row's progress, or
// the whole list when a download finished (the queue moved up)
function DlTick()
{
    if (nRows == 0)
        return;
    if (DlListCount() != nListCount)
        DlRefresh();
    else if (theDownloads.HasActive() == 1)
        DlFillRow(RowId(0), 0);
}

function DlPress()
{
    var d = nSel - DlOff();
    if (d < 0)
        return;
    if (d == 0)
    {
        if (theDownloads.HasActive() == 1)
            theDownloads.TogglePause();
        else
            theDownloads.StartNow(0);
    }
    else
    {
        theDownloads.StartNow(d);
        nSel = 0;
    }
    DlRefresh();
}

function DlRemove()
{
    var d = nSel - DlOff();
    if (d < 0)
        return;
    nDlItem = d;
    if (theDownloads.GetProgress(d) > 0)
    {
        sAction = "dlremove";
        Confirm("Remove " + theDownloads.GetName(d) + "?", "page_downloads");
        return;
    }
    DlRemoveNow();
}

function DlRemoveNow()
{
    theDownloads.Remove(nDlItem);
    if (sPage == "page_downloads")
        DlRefresh();
    else
        ShowPage("page_downloads", "Fade");
}
"""


def main():
    out = sys.argv[1]
    pads = os.path.join(out, "pads")
    os.makedirs(pads, exist_ok=True)
    vals = dict(
        atmo_origin=v3(ATMO_ORIGIN), atmo_radius=0.891 * ATMO_SCALE, atmo_height=0.545 * ATMO_SCALE,
        rail_origin=v3(RAIL_ORIGIN), rail_radius=0.5 * RAIL_SCALE[0], rail_height=0.02 * RAIL_SCALE[1],
        rail_color=v3(RAIL_COLOR), hud_w=HUD_W, page_w=PAGE_W, page_vy=PAGE_VIEW_Y)
    for name in PAD_ORDER:
        vals["vp_" + name] = viewpoint(name)
        vals["panel_" + name] = panel(name)
        vals["pad_" + name] = pad_nodes(name)
        vals["word_" + name] = WORDS[name]
    for name in ("catalogfocus", "settingsfocus", "carousel"):
        vals["vp_" + name] = viewpoint(name)
    vals["zoom_duration"] = ZOOM_DURATION
    for name in ("catalog", "settings"):
        o = label_focus_offset(name)
        for k, v in zip("xyz", o):
            vals["lofs_%s_%s" % (name, k)] = "%.5f" % v
    vals["belt_centre"] = v3(BELT_CENTRE)
    vals["belt_radius"] = 0.5 * RAIL_SCALE[0] * BELT_INSET
    vals["belt_cover_scale"] = BELT_COVER_SCALE
    vals["belt_focus_pull"] = BELT_FOCUS_PULL
    vals["belt_skip"] = BELT_SKIP
    vals["belt_count"] = len(CAROUSEL_GAMES)
    vals["belt_icons"] = ";".join(icon for _, icon in BELT_RESERVED)
    vals["belt_start"] = BELT_START
    vals["memory_script"] = MEMORY_SCRIPT
    vals["pin_fade_out"] = "%g" % PIN_FADE_OUT
    vals["pin_fade_in"] = "%g" % PIN_FADE_IN
    vals["pin_pulse_dy"] = "%g" % PIN_PULSE_DY
    vals["pin_pulse_period"] = "%g" % PIN_PULSE_PERIOD
    vals["pin_pulse_dur"] = "%g" % PIN_PULSE_DUR
    import genpage
    vals["page_tables"] = genpage.script()
    open(os.path.join(out, "default.xap"), "w").write(HUB % vals)
    open(os.path.join(pads, "overlay.xui"), "w").write(overlay_scene())
    catalog = open(os.path.join(pads, "catalog.xui")).read()
    open(os.path.join(pads, "settings.xui"), "w").write(settings_scene(catalog))
    backdrop(os.path.join(out, "backdrop.png"))
    print("wrote", out)


if __name__ == "__main__":
    main()
