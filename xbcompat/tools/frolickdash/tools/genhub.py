#!/usr/bin/env python3
"""Generate the hub scene of the Frolick dashboard for the xapp engine.

    genhub.py dashdir

Writes dashdir/default.xap (the XAP hub: three pads, their panels, the
camera moves and the joystick handling), dashdir/pads/settings.xui (the
settings panel, derived from catalog.xui the way Frolick's RML settings
panel is a relabelled catalog panel) and dashdir/pads/pad_*.xui (the floor
pads, drawn as flat XUI scenes).

The numbers come from Frolick's cmd/demo/pads.go: pad positions, the
per-pad cameras, the panel placement (0.9 x 0.45 world units, between the
camera and its target, shifted 0.256 toward the camera and 0.08 up) and the
panels' right/up axes.  Frolick works in OpenGL's right-handed frame; the
engine mirrors its world in z before the camera (CCamera::UpdateViewMatrix),
so Transforms take Frolick's coordinates as they are and only the camera's
rotation is built in the mirrored frame.
"""
import math, os, re, sys

# ---- Frolick's numbers (right-handed, z toward the viewer) ----------------
PADS = [  # name, x, z, colour (pad top), accent
    ("catalog", -1.076, -0.3397, (0.051, 0.218, 0.711), (0.251, 0.502, 0.859)),
    ("library", -0.0004, 0.2345, (0.253, 0.57, 0.042), (0.537, 0.773, 0.224)),
    ("settings", 1.0765, -0.3392, (0.692, 0.257, 0.049), (0.796, 0.498, 0.227)),
]
LOOKAT_Y = 0.3758
CAM_CATALOG = ((-1.0784, 0.0988, 0.4766), (-0.5223, 0.4158, 0.0571))
CAM_LIBRARY_OFFSET = (-0.0052, -0.2704, 0.7159)
FOV = 1.3962634  # 80 degrees
PANEL_W, PANEL_H = 0.9, 0.45
PANEL_T, PANEL_TOWARD, PANEL_UP = 0.2, 0.256, 0.08
# right and up axes of each panel (pads.go hubTargetR*/U*).  Those literals
# have z pointing into the screen; here z is toward the viewer like the
# cameras above, so their z is negated (the catalog panel, yawed -49.5
# degrees, has its right axis at (0.65, 0, 0.76) and faces its camera).
PANEL_AXES = {
    "library": ((1.000, -0.000, 0.007), (-0.001, 0.997, 0.080)),
    "catalog": ((0.641, -0.108, 0.760), (0.011, 0.991, 0.132)),
    "settings": ((-0.645, 0.105, 0.757), (0.014, 0.992, -0.126)),
}
PAD_RADIUS = 0.42


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a): l = math.sqrt(dot(a, a)); return mul(a, 1 / l)
def mirror(a): return (a[0], a[1], -a[2])


def camera(name):
    """eye, target in Frolick's frame."""
    x, z = [(p[1], p[2]) for p in PADS if p[0] == name][0]
    if name == "library":
        target = (x, LOOKAT_Y, z)
        return add(target, CAM_LIBRARY_OFFSET), target
    eye, target = CAM_CATALOG
    if name == "settings":
        eye, target = (-eye[0], eye[1], eye[2]), (-target[0], target[1], target[2])
    return eye, target


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
    # check against D3DX's matrix layout
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
    """Viewpoint position/orientation text for a camera at eye looking at target.

    The engine mirrors the world in z before applying the camera, so the
    camera's rotation is built in the mirrored (left-handed) frame with
    D3DXMatrixLookAtLH's axes, and the position is Frolick's as given
    (CViewpoint::Bind negates it to the engine's frame)."""
    eye, target = camera(name)
    zaxis = mirror(norm(sub(target, eye)))
    xaxis = norm(cross((0, 1, 0), zaxis))
    yaxis = cross(zaxis, xaxis)
    ax, ay, az, angle = axis_angle(quat_from_rows(xaxis, yaxis, zaxis))
    # CViewpoint::Bind rotates by -orientation.w, so hand it the negative
    return "position %.4f %.4f %.4f\n    orientation %.4f %.4f %.4f %.4f" % (
        eye[0], eye[1], eye[2], ax, ay, az, -angle)


def panel(name):
    """Transform translation/rotation text for the pad's panel.

    Transforms take Frolick's coordinates as they are: the engine's z mirror
    conjugates the rotation, so a proper rotation with rows (right, up,
    right x up) in Frolick's frame is what to give."""
    eye, target = camera(name)
    centre = add(target, mul(sub(eye, target), PANEL_T))
    centre = add(centre, mul(norm(sub(eye, centre)), PANEL_TOWARD))
    centre = (centre[0], centre[1] + PANEL_UP, centre[2])
    right, up = PANEL_AXES[name]
    right = norm(right)
    up = norm(sub(up, mul(right, dot(up, right))))  # make exactly perpendicular
    ax, ay, az, angle = axis_angle(quat_from_rows(right, up, cross(right, up)))
    return "translation %.4f %.4f %.4f\n            rotation %.4f %.4f %.4f %.4f" % (
        centre[0], centre[1], centre[2], ax, ay, az, angle)


def floor_pad(name, x, z):
    """A flat Transform for the pad disc; its scene's up points away from the viewer."""
    return "translation %.4f 0 %.4f\n            rotation 1 0 0 -1.5708" % (x, z)


# ---- scenes ----------------------------------------------------------------

def hexcol(c, a=1.0):
    return "0x%02x%02x%02x%02x" % (int(a * 255 + 0.5), int(c[0] * 255 + 0.5), int(c[1] * 255 + 0.5), int(c[2] * 255 + 0.5))


def circle_points(w, h):
    """XUI figure points for an ellipse filling w x h (four Bezier arcs)."""
    k = 0.5523
    rx, ry = w / 2, h / 2
    cx, cy = rx, ry
    pts = [
        (cx + rx, cy, cx + rx, cy + ry * k, cx + rx * k, cy + ry),
        (cx, cy + ry, cx - rx * k, cy + ry, cx - rx, cy + ry * k),
        (cx - rx, cy, cx - rx, cy - ry * k, cx - rx * k, cy - ry),
        (cx, cy - ry, cx + rx * k, cy - ry, cx + rx, cy - ry * k),
    ]
    return "%g,%g;" % (w, h) + ";".join("%g,%g,%g,%g,%g,%g" % p for p in pts)


def figure(id, x, y, w, h, fill, stroke=None, opacity=None):
    s = ["<XuiFigure>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h,
         " <Position>%g,%g,0</Position>" % (x, y)]
    if opacity is not None:
        s.append(" <Opacity>%g</Opacity>" % opacity)
    if stroke:
        s.append(" <Stroke>\n  <StrokeWidth>%g</StrokeWidth>\n  <StrokeColor>%s</StrokeColor>\n </Stroke>" % stroke)
    s.append(fill)
    s.append(" <Closed>true</Closed>")
    s.append(" <Points>%s</Points>" % circle_points(w, h))
    s += ["</Properties>", "</XuiFigure>"]
    return "\n".join(s)


def radial(stops, scale=1.0):
    cols = "\n".join("   <StopColor>%s</StopColor>" % c for c, _ in stops)
    pos = "\n".join("   <StopPos>%g</StopPos>" % p for _, p in stops)
    return (" <Fill>\n  <FillType>3</FillType>\n  <Gradient>\n   <Radial>true</Radial>\n   <NumStops>%d</NumStops>\n%s\n%s\n"
            "  </Gradient>\n  <Scale>%g,%g,1</Scale>\n </Fill>" % (len(stops), cols, pos, scale, scale))


def none_fill():
    return " <Fill>\n  <FillType>0</FillType>\n </Fill>"


def pad_scene(name, colour, accent):
    """The floor pad: a shaded disc in the pad's colour, a bumper ring, the
    highlight ring and the pad's name (Frolick's label_*.png, as text)."""
    S = 512
    body = "\n".join([
        # shadow under the pad
        figure("shadow", 6, 14, S - 12, S - 12, radial([("0x90000000", 0), ("0x60000000", 0.8), ("0x00000000", 1)])),
        # bumper (rail colour)
        figure("bumper", 0, 0, S, S, radial([("0xffa39f8a", 0), ("0xff8f8b76", 0.85), ("0xff6e6b5b", 1)])),
        # pad top
        figure("top", 28, 28, S - 56, S - 56,
               radial([(hexcol(mul(colour, 1.25)), 0), (hexcol(colour), 0.55), (hexcol(mul(colour, 0.7)), 1)]),
               stroke=(4, hexcol(mul(colour, 0.5)))),
        # highlight ring
        figure("ring", 52, 52, S - 104, S - 104, none_fill(), stroke=(10, hexcol(accent))),
        figure("ringInner", 150, 150, S - 300, S - 300, none_fill(), stroke=(6, hexcol(accent, 0.6))),
        figure("ringCore", 200, 200, S - 400, S - 400, radial([(hexcol(accent, 0.9), 0), (hexcol(accent, 0.0), 1)])),
        "\n".join(["<XuiText>", "<Properties>", " <Id>label</Id>", " <Width>400</Width>", " <Height>60</Height>",
                   " <Position>56,312,0</Position>", " <Text>%s</Text>" % name, " <TextColor>0xffffffff</TextColor>",
                   " <DropShadowColor>0xa0000000</DropShadowColor>", " <PointSize>34</PointSize>",
                   " <Font>Convection</Font>", " <TextStyle>%d</TextStyle>" % (0x400 | 0x4000), "</Properties>", "</XuiText>"]),
    ])
    return ('<XuiCanvas version="000c">\n<Properties>\n <Id>canvas</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n'
            '</Properties>\n<XuiScene>\n<Properties>\n <Id>Pad</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n'
            '</Properties>\n%s\n</XuiScene>\n</XuiCanvas>\n' % (S, S, S, S, body))


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
// tools/genhub.py from Frolick's pad, camera and panel numbers.
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
}

// The library pad's camera loads first, so the dashboard opens on it.
DEF vpLibrary Viewpoint
{
    fieldOfView %(fov)g
    %(vp_library)s
}

DEF vpCatalog Viewpoint
{
    fieldOfView %(fov)g
    %(vp_catalog)s
}

DEF vpSettings Viewpoint
{
    fieldOfView %(fov)g
    %(vp_settings)s
}

DEF theMover CameraMover
{
    duration 0.55
    function OnArrive()
    {
        bMoving = false;
    }
}

var nPad;       // 0 catalog, 1 library, 2 settings
var bMoving;

DEF theHub Level
{
    children
    [
        // The floor pads, in Frolick's arc.
        Transform
        {
            %(floor_catalog)s
            children [ XuiPanel { url "pads/pad_catalog.xui" width %(padw)g } ]
        }
        Transform
        {
            %(floor_library)s
            children [ XuiPanel { url "pads/pad_library.xui" width %(padw)g } ]
        }
        Transform
        {
            %(floor_settings)s
            children [ XuiPanel { url "pads/pad_settings.xui" width %(padw)g } ]
        }

        // Each pad's panel floats in front of its camera.
        DEF xfCatalog Transform
        {
            %(panel_catalog)s
            fade 0.55
            alpha 0
            children
            [
                DEF padCatalog XuiPanel
                {
                    url "pads/catalog.xui"
                    skin "pads/frolick_skin.xui"
                    width %(panelw)g
                    function OnPress(id) { OnPadPress(0, id); }
                }
            ]
        }
        DEF xfLibrary Transform
        {
            %(panel_library)s
            fade 0.55
            alpha 1
            children
            [
                DEF padLibrary XuiPanel
                {
                    url "pads/library.xui"
                    skin "pads/frolick_skin.xui"
                    width %(panelw)g
                    function OnPress(id) { OnPadPress(1, id); }
                }
            ]
        }
        DEF xfSettings Transform
        {
            %(panel_settings)s
            fade 0.55
            alpha 0
            children
            [
                DEF padSettings XuiPanel
                {
                    url "pads/settings.xui"
                    skin "pads/frolick_skin.xui"
                    width %(panelw)g
                    mirror true
                    function OnPress(id) { OnPadPress(2, id); }
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
    }
}

// Focus moves inside the panel first; at its edge, left and right fly to
// the next pad, as in Frolick.
function PadMove(dir, step)
{
    if (bMoving)
        return;

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

    if (nPad == 0)
        padCatalog.Press();
    else if (nPad == 1)
        padLibrary.Press();
    else
        padSettings.Press();
}

function OnPadPress(pad, id)
{
    // Frolick: A on the library pad opens the game carousel.
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
    }
    else if (n == 1)
    {
        theMover.GoTo(vpLibrary);
        padLibrary.Play("", "Intro");
    }
    else
    {
        theMover.GoTo(vpSettings);
        padSettings.Play("", "Intro");
    }
}

function initialize()
{
    nPad = 1;
    bMoving = false;
    EnableInput(true);
    theHub.GoTo();
    padLibrary.Play("", "Intro");
}
'''


def main():
    out = sys.argv[1]
    pads = os.path.join(out, "pads")
    os.makedirs(pads, exist_ok=True)
    vals = dict(fov=FOV, padw=PAD_RADIUS * 2, panelw=PANEL_W)
    for name, x, z, colour, accent in PADS:
        vals["vp_" + name] = viewpoint(name)
        vals["panel_" + name] = panel(name)
        vals["floor_" + name] = floor_pad(name, x, z)
        open(os.path.join(pads, "pad_%s.xui" % name), "w").write(pad_scene(name, colour, accent))
    open(os.path.join(out, "default.xap"), "w").write(HUB % vals)
    catalog = open(os.path.join(pads, "catalog.xui")).read()
    open(os.path.join(pads, "settings.xui"), "w").write(settings_scene(catalog))
    print("wrote", out)


if __name__ == "__main__":
    main()
