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
CAMERAS = {  # eye, view direction (-Z column), up (Y column), vertical fov in degrees
    "library": ((0.0, 0.106559, 0.917581), (0.0, 0.286932, -0.708153), (0.0, 1.46646, 0.594187), 80.0),
    "catalog": ((-0.848216, 0.1074, 0.464691), (0.628121, 0.375529, -0.6815), (-0.254505, 0.926811, 0.276133), 80.1803),
    # cam_angle_padfocus: the close, near top-down shot of the catalog pad
    # (pitched 70 degrees down); A on the catalog or settings panel flies
    # there, B flies back.  Its up vector is its own, not +Y.
    "catalogfocus": ((-0.639461, 0.133682, 0.0213613), (0.0, -0.942063, -0.335435), (0.0, 0.335435, -0.942063), 80.1803),
}
ZOOM_DURATION = 0.75                           # the library carousel's zoom
# The carousel camera and the game belt, fitted to sokky's mockup
# screenshot of the belt view (2026-10-19, 2560x1440): the camera from the
# three pad labels (1.8 px rms), low behind the library pad and pitched up
# 22.0 degrees, 0.19 ahead and 0.07 above carousel.go's carouselEyeTarget;
# the belt (GameTap's, see art/gametap) from the cover corners, its ring
# centred on the camera as GameTap shoots it, the regular covers at 0.97 of
# the rail radius, the focused cover 4.7 ahead.
CAROUSEL_EYE = (0.0, 0.1072, 0.6407)
CAROUSEL_LOOK = (0.0, 0.4043, -1.0)
BELT_CENTRE = (-0.084, 1.581, 0.894)           # the belt ring's axis, at the regular covers' height
BELT_INSET = 0.97
BELT_GAMES = 9                                 # placeholder count: carousel.go's carouselGames
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


def camera(name):
    """eye, direction, up, fov for a pad's view."""
    if name == "carousel":
        return CAROUSEL_EYE, norm(CAROUSEL_LOOK), (0.0, 1.0, 0.0), CAMERAS["library"][3]
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
            children [ Sprite { url "label_%s.xtx" pixelSize %.6f depthTest false } ]
        }''' % (v3(add(centre, (0, LABEL_Y * S, 0))), name, LABEL_PIXEL * S / LABEL_OVERSAMPLE))
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
                   " <PointSize>%d</PointSize>" % round(111 * UI), " <TextStyle>16</TextStyle>", "</Properties>", "</XuiText>"]),
        # A + chevron under the panel
        "\n".join(["<XuiImage>", "<Properties>", " <Id>promptA</Id>", " <Width>26</Width>", " <Height>26</Height>",
                   " <Position>347,336,0</Position>", " <ImagePath>glyph_a_button.png</ImagePath>", " <SizeMode>4</SizeMode>",
                   "</Properties>", "</XuiImage>"]),
        "\n".join(["<XuiFigure>", "<Properties>", " <Id>chevron</Id>", " <Width>22</Width>", " <Height>12</Height>",
                   " <Position>349,366,0</Position>",
                   " <Stroke>\n  <StrokeWidth>3</StrokeWidth>\n  <StrokeColor>0xffffffff</StrokeColor>\n </Stroke>",
                   " <Fill>\n  <FillType>0</FillType>\n </Fill>", " <Closed>false</Closed>",
                   " <Points>22,12;0,0;11,12;22,0</Points>", "</Properties>", "</XuiFigure>"]),
        # the footer strip
        "\n".join(["<XuiFigure>", "<Properties>", " <Id>footer</Id>", " <Width>%d</Width>" % w, " <Height>%g</Height>" % fh,
                   " <Position>0,%g,0</Position>" % (h - fh),
                   " <Fill>\n  <FillType>2</FillType>\n  <Gradient>\n   <NumStops>%d</NumStops>\n%s\n%s\n  </Gradient>\n  <Rotation>90</Rotation>\n </Fill>" % (len(FOOTER_STOPS), stops, pos),
                   " <Closed>true</Closed>", " <Points>%s</Points>" % rect_points(w, fh), "</Properties>", "</XuiFigure>"]),
        "\n".join(["<XuiText>", "<Properties>", " <Id>frolick</Id>", " <Width>%g</Width>" % (136 * UI), " <Height>%g</Height>" % (42 * UI),
                   " <Position>%g,%g,0</Position>" % (w - 136 * UI, h - 42 * UI), " <Text>frolick</Text>",
                   " <TextColor>%s</TextColor>" % hexcol(WORDMARK_COLOR), " <PointSize>%d</PointSize>" % round(37 * UI),
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

DEF theMover CameraMover
{
    duration 0.55
    function OnArrive()
    {
        bMoving = false;
    }
}

// The zoom onto a pad takes longer than a pad switch, as Frolick's carousel does.
DEF theZoomer CameraMover
{
    duration %(zoom_duration)s
    function OnArrive()
    {
        bMoving = false;
    }
}

var nPad;       // 0 catalog, 1 library, 2 settings
var bMoving;
var bZoomed;    // the camera is on the pad's focus view (catalog, settings)
var bLibraryOpen;   // the camera is in the game belt

DEF theHub Level
{
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
            centre %(belt_centre)s
            radius %(belt_radius).5f
            fadeDuration %(zoom_duration)s
            count %(belt_count)d
            function OnStep() { }
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
    }
}

// Focus moves inside the panel first; at its edge, left and right fly to
// the next pad, as in Frolick.
function PadMove(dir, step)
{
    if (bMoving)
        return;
    if (bZoomed)
        return;
    if (bLibraryOpen)
    {
        // the belt: left scrolls it right (Belt_Right), right scrolls it left
        if (step < 0)
            theBelt.Step(0 - 1);
        else if (step > 0)
            theBelt.Step(1);
        return;
    }

    var moved = 0;
    if (nPad == 0)
        moved = padCatalog.Navigate(dir);
    else if (nPad == 1)
        moved = padLibrary.Navigate(dir);
    else
        moved = padSettings.Navigate(dir);

    if (moved == 0)
    {
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
}

function PadPress()
{
    if (bMoving)
        return;
    if (bZoomed)
        return;
    if (bLibraryOpen)
        return;     // launching a game: nothing to launch yet

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
    theBelt.Open();
}

function CloseLibrary()
{
    bMoving = true;
    bLibraryOpen = false;
    SetPadAlpha(1, 1);
    theZoomer.GoTo(vpLibrary);
    theBelt.Close();
}


// A on the catalog or settings panel: the panel fades out and the camera
// flies to cam_angle_padfocus.  B flies back up to the pad's view.
function ZoomIn()
{
    bMoving = true;
    bZoomed = true;
    SetPadAlpha(nPad, 0);
    if (nPad == 0)
        theZoomer.GoTo(vpCatalogFocus);
    else
        theZoomer.GoTo(vpSettingsFocus);
}

function PadBack()
{
    if (bMoving)
        return;
    if (bLibraryOpen)
    {
        if (theBelt.IsBusy() == 0)
            CloseLibrary();
        return;
    }
    if (bZoomed == false)
        return;
    bMoving = true;
    bZoomed = false;
    SetPadAlpha(nPad, 1);
    if (nPad == 0)
        theZoomer.GoTo(vpCatalog);
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

function GoPad(n)
{
    bMoving = true;
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
    EnableInput(true);
    theHub.GoTo();
    padLibrary.Play("", "Intro");
    theOverlay.SetText("word", "%(word_library)s");
}
'''


def main():
    out = sys.argv[1]
    pads = os.path.join(out, "pads")
    os.makedirs(pads, exist_ok=True)
    vals = dict(
        atmo_origin=v3(ATMO_ORIGIN), atmo_radius=0.891 * ATMO_SCALE, atmo_height=0.545 * ATMO_SCALE,
        rail_origin=v3(RAIL_ORIGIN), rail_radius=0.5 * RAIL_SCALE[0], rail_height=0.02 * RAIL_SCALE[1],
        rail_color=v3(RAIL_COLOR), hud_w=HUD_W)
    for name in PAD_ORDER:
        vals["vp_" + name] = viewpoint(name)
        vals["panel_" + name] = panel(name)
        vals["pad_" + name] = pad_nodes(name)
        vals["word_" + name] = WORDS[name]
    for name in ("catalogfocus", "settingsfocus", "carousel"):
        vals["vp_" + name] = viewpoint(name)
    vals["zoom_duration"] = ZOOM_DURATION
    vals["belt_centre"] = v3(BELT_CENTRE)
    vals["belt_radius"] = 0.5 * RAIL_SCALE[0] * BELT_INSET
    vals["belt_count"] = BELT_GAMES
    open(os.path.join(out, "default.xap"), "w").write(HUB % vals)
    open(os.path.join(pads, "overlay.xui"), "w").write(overlay_scene())
    catalog = open(os.path.join(pads, "catalog.xui")).read()
    open(os.path.join(pads, "settings.xui"), "w").write(settings_scene(catalog))
    backdrop(os.path.join(out, "backdrop.png"))
    print("wrote", out)


if __name__ == "__main__":
    main()
