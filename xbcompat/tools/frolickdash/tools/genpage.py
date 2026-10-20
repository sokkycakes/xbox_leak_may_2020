#!/usr/bin/env python3
"""Generate the padfocus pages of the Frolick dashboard: the full-screen
pages the catalog and settings pads open, laid out like the Xbox 360
Blades dashboard's third-level pages (the ones that slide the blades away
and expand) and drawn with Frolick's art.

    genpage.py dashdir           writes dashdir/pads/page_*.xui and page_skin.xui
    genpage.py --script          prints the hub script's page tables

Each page's layout is the Blades scene itself, converted from the
dashboard's XUIB with tools/xur2xui.py and kept in art/blades/ as it came
out: the ids, sizes, positions and navigation of its controls are
Microsoft's, so that the pages keep the Blades' structure.  What this
script adds is the page's dress, after sokky's catalog padfocus mockup
(2026-10-20) and the 360's own sub-pages: the page spans the whole screen
edge to edge like the 360's, a darker header strip over a translucent
slate body (the mockup's panel: no border, square edges, the pad showing
through), the legends below it, and the list bars in the Blades' button
style (rounded, a thin light border, a dark glassy fill with a top
highlight, white text at the left, the focused bar lit) in the panel's
slate tones, with Frolick's glyphs on the legends.  The open and close
motion is timed like dashmain.xur's 1OpenFull (30 frames at 30 fps).  The
Blades skin's visuals are not used; page_skin.xui draws every control.

The Blades' lists (XuiList, XuiCommonList) are filled by the dashboard at
run time; the runtime draws no lists, so a list becomes its items as
buttons stacked the way the list would stack them, with the skin's item
visual (list_twoline-icon -> btn_twoline-icon and so on) and the item
texts from PAGES below.  Icons the items would carry are left empty until
Frolick's art for them exists.

The scenes are 1120x770 canvases (the Blades' 720p design size).  The hub
shows a page on a HUD XuiPanel 720 wide, so the whole canvas lands on the
NTSC screen at 0.643 and the content (x 146..1004, y 96..686) sits inside
the title-safe area.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BLADES = os.path.join(HERE, "..", "art", "blades")
sys.path.insert(0, HERE)
from genhub import CAROUSEL_GAMES, BELT_START

# The panel, sampled from sokky's mockup: slate, about 85% opaque, a
# little darker at the bottom; the header strip darker still.
PANEL_TOP, PANEL_BOTTOM, HEADER_FILL = "0xd9424c5a", "0xd9313b49", "0xe6283140"
# The Blades' buttons in the panel's tones: the border, the glassy fill
# (top to bottom), the focused bar's fill (top to bottom).
BAR_STROKE, BAR_TOP, BAR_BOTTOM = "0xff8494a8", "0xc0384456", "0xc0222b38"
FOCUS_TOP, FOCUS_BOTTOM = "0xff8d9db5", "0xff5a6a80"
TEXT, TEXT_DIM = "0xffffffff", "0xffe4e8ee"

CANVAS_W, CANVAS_H = 1120, 770
# The screen on the canvas: 720x480 at 0.643, so x 0..1120 and y 12..758.
# The header strip holds the Blades' header (y 96); the body ends where
# their lists end (603) and the legends (611..686) sit below it.
HEADER_Y, BODY_Y, BODY_END, LEGEND_END = 60, 150, 603, 707

# The Blades' list visuals: the item visual and its height (the skin's
# control_ListItem), and the item's second line and icon slots, if any.
LIST_ITEMS = {
    "": ("XuiButton", 45, None, None),                                   # list_Common
    "list_twoline-icon": ("btn_twoline-icon", 72, (83, 35, 328, 40), (11, 6, 60, 60)),
    "List_3Line_Icon_prgbr": ("btn_3Line_Icon_prgbr", 105, (83, 60, 574, 47), (6, 5, 60, 60)),
    "List_Fullwidth_3Icon_4string": ("ListItem_Fullwidth_3Icon_4String", 47, None, (11, 8, 32, 32)),
    "List_oneline_icon": ("btn_oneline_icon", 72, None, (11, 4, 64, 64)),
}

# The pages.  scene: the Blades scene in art/blades/ (None: the System
# list, built here in the Console Settings layout); pad; first: the
# control focused on entry; parent: the page B returns to ("" closes the
# page and flies the camera back); header: the title (the scene's own if
# None); items: the buttons of the System list; lists: {list id: [(line
# 1, line 2, fraction), ...]}; press: {control id: page}; meta: {control
# id: the meta panel's text}; labels: {id: text}; hide: ids shown nothing
# here.  The list texts are placeholders until the pages read the console.
GAMES = [(g, "", 0) for g in CAROUSEL_GAMES[BELT_START:]]   # the belt's games, not its All games / Disc tiles
PAGES = [
    ("page_system", dict(scene=None, pad="settings", first="btnConsole", parent="", header="Settings",
                         items=[("btnConsole", "Console Settings", "page_console"), ("btnMemory", "Memory", "page_memory"),
                                ("btnNetwork", "Network Settings", "page_network")])),
    ("page_console", dict(scene="dashSysCslSet.xui", pad="settings", first="btnDisplay", parent="page_system",
                          meta={"btnDisplay": "480i NTSC, 4:3", "btnAudio": "Stereo", "btnLanguage": "English",
                                "btnClock": "Set from the network", "btnCountry": "United States", "btnAutoOff": "Off",
                                "btnScreensaver": "Off", "btnRemote": "Not set up", "btnPolicy": "Frolick on the Sion"})),
    ("page_memory", dict(scene="DeviceSelector.xui", pad="settings", first="list_devices_0", parent="page_system",
                         lists={"list_devices": [("Hard Drive", "7.2 GB free of 8.0 GB", 0.1)]},
                         press={"list_devices_0": "page_memory_items"}, labels={"labTotal": "1 of 1"})),
    ("page_memory_items", dict(scene="Items.xui", pad="settings", first="list_items_0", parent="page_memory",
                               header="Hard Drive",
                               lists={"list_items": [("Games", "", 0), ("Saved Games", "", 0), ("Music", "", 0),
                                                     ("Pictures", "", 0), ("Videos", "", 0)]},
                               labels={"labTotal": "1 of 5"})),
    ("page_network", dict(scene="2000_NetworkConfigList.xui", pad="settings", first="list_items_0", parent="page_system",
                          lists={"list_items": [("Wireless Network", "Connected", 0)]},
                          meta={"list_items_0": "Wireless Network\nConnected\n\nIP address 192.168.1.247\nSubnet mask 255.255.255.0\nGateway 192.168.1.1"})),
    ("page_store", dict(scene="1400_StoreFrontScene.xui", pad="catalog", first="btnGameDownloads", parent="", header="Marketplace",
                        press={"btnGameDownloads": "page_store_games", "btnHotList": "page_store_hot",
                               "btnDemosAndTrailers": "page_store_demos", "btnThemes": "page_store_themes",
                               "btnDownloadStatusAndHistory": "page_store_history"},
                        hide=["btnX"])),
    ("page_store_games", dict(scene="1412_GameListScene.xui", pad="catalog", first="lstGames_0", parent="page_store",
                              header="Game Downloads", lists={"lstGames": GAMES}, hide=["btnX"],
                              labels={"labItemCount": "%d items" % min(len(GAMES), 8)})),
    ("page_store_hot", dict(scene="1405_HotListScene.xui", pad="catalog", first="lstHot_0", parent="page_store",
                            lists={"lstHot": GAMES[:7]},
                            hide=["btnX", "labMembershipTitle", "labMembership", "labCurrentBalanceTitle", "labCurrentBalance"],
                            labels={"labItemCount": "7 items"})),
    ("page_store_demos", dict(scene="1412_GameListScene.xui", pad="catalog", first="lstGames_0", parent="page_store",
                              header="Demos and Trailers", lists={"lstGames": GAMES[2:8]}, hide=["btnX"],
                              labels={"labItemCount": "6 items"})),
    ("page_store_themes", dict(scene="1412_GameListScene.xui", pad="catalog", first="lstGames_0", parent="page_store",
                               header="Themes and Gamer Pictures", lists={"lstGames": [("Frolick", "", 0), ("Sion", "", 0)]},
                               hide=["btnX"], labels={"labItemCount": "2 items"})),
    ("page_store_history", dict(scene="1414_DownloadHistoryScene.xui", pad="catalog", first="lstDownloadHistory_0", parent="page_store",
                                lists={"lstDownloadHistory": GAMES[:3]}, hide=["btnX"], labels={"labItemCount": "3 items"})),
]
ROOT_PAGE = {"settings": "page_system", "catalog": "page_store"}


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


def text(id, x, y, w, h, size, style, color=TEXT, presenter=True, value=None, shadow="0", opacity=None):
    tag = "XuiTextPresenter" if presenter else "XuiText"
    s = ["<%s>" % tag, "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h,
         " <Position>%g,%g,0</Position>" % (x, y)]
    if opacity is not None:
        s.append(" <Opacity>%g</Opacity>" % opacity)
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

def visual(id, w, h, body, timelines=None):
    s = ["<XuiVisual>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h, "</Properties>"]
    s += body
    if timelines:
        s += ["<Timelines>"] + timelines + ["</Timelines>"]
    s.append("</XuiVisual>")
    return "\n".join(s)


def bar_visual(id, w, h, label):
    """A list bar in the Blades' button style: a rounded box with a thin
    light border, a dark glassy fill with a highlight along its top, the
    label (the control's Text) where the Blades' item visual puts its
    first line; focus lights the box up, a press dips it.  The box sits a
    pixel inside the control so bars on the Blades' 45 pitch keep a gap."""
    tx, ty, tw, th = label
    bw, bh, r = w - 2, h - 3, 9
    body = [
        figure("bg", 1, 1, bw, bh, gradient([(0, BAR_TOP), (1, BAR_BOTTOM)]), (2, BAR_STROKE), points=rounded(bw, bh, r), anchor=15),
        figure("lit", 1, 1, bw, bh, gradient([(0, FOCUS_TOP), (1, FOCUS_BOTTOM)]), (2, "0xffb8c4d4"), points=rounded(bw, bh, r),
               opacity=0, anchor=15),
        figure("hi", 3, 3, bw - 4, (bh - 4) * 0.42, gradient([(0, "0x5affffff"), (1, "0x00ffffff")]), points=rounded(bw - 4, (bh - 4) * 0.42, 6), anchor=7),
        text("text_Label", tx, ty, tw, th, 18, ONE_LINE | CENTER_V),
    ]
    tl = [
        frames([("KillFocus", 0, "play", ""), ("Normal", 8, "play", ""), ("EndNormal", 9, "stop", ""),
                ("Focus", 10, "play", ""), ("EndFocus", 18, "stop", ""),
                ("Press", 20, "play", ""), ("EndPress", 30, "stop", "")]),
        track("lit", "Opacity", [(0, 0, 1), (8, 0, 0), (10, 0, 0), (18, 0, 1), (20, 0, 1), (23, 0, 0.5), (30, 0, 1)]),
    ]
    return visual(id, w, h, body, tl)


def legend_visual(button, glyph, right):
    """legend_A/B: the label, then the glyph at the right (the Blades put
    the glyph at x 386 of a 420 bar); legend_X/Y: the glyph at the left,
    then the label.  Frolick's glyph art."""
    w, h = 420, 47
    if right:
        body = [text("label", 6, 0, 378, 40, 18, ONE_LINE | RIGHT | CENTER_V), image("glyph", 388, 4, 32, 32, glyph)]
    else:
        body = [image("glyph", 0, 4, 32, 32, glyph), text("label", 38, 0, 378, 40, 18, ONE_LINE | CENTER_V)]
    return visual("legend_" + button, w, h, body)


def label_visual(id, size, style, color=TEXT, x=0, w=415, opacity=None):
    return visual(id, 420, 47, [text("Text", x, 0, w, 47, size, style, color, opacity=opacity)])


def card_visual(id, w, h, pane=False):
    """The meta panes: the Blades' box without the focus."""
    return visual(id, w, h, [
        figure("bg", 0, 0, w, h, gradient([(0, BAR_TOP), (1, BAR_BOTTOM)]), (2, BAR_STROKE), points=rounded(w, h, 9), anchor=15),
        figure("hi", 2, 2, w - 4, 40, gradient([(0, "0x3cffffff"), (1, "0x00ffffff")]), points=rounded(w - 4, 40, 6), anchor=7),
    ] + ([text("Pane_txtCurrentSetting", 17.5, 50, w - 35, h - 60, 18, 0)] if pane else []))


def skin():
    body = [
        bar_visual("XuiButton", 423, 47, (16, 0, 391, 47)),
        bar_visual("btn_oneline_icon", 420, 72, (83, 0, 328, 72)),
        bar_visual("btn_twoline-icon", 420, 72, (83, 5, 328, 40)),
        bar_visual("btn_3Line_Icon_prgbr", 855, 105, (83, 0, 574, 72)),
        bar_visual("ListItem_Fullwidth_3Icon_4String", 855, 47, (9, 4, 385, 42)),
        label_visual("Label_Head", 22, ONE_LINE | CENTER_V),
        label_visual("Label_MetaHead_1line", 20, ONE_LINE | CENTER_V, TEXT_DIM),
        label_visual("Label_TabTitle", 20, ONE_LINE | CENTER_V, x=5),
        label_visual("Label_Body", 18, 0, x=11, w=400),
        label_visual("Label_Body_1line", 18, ONE_LINE | CENTER_V, x=11, w=400),
        label_visual("Label_BodyRight", 18, ONE_LINE | RIGHT | CENTER_V, x=5),
        label_visual("Label_BodyCentered", 18, CENTER_H | CENTER_V, x=10, w=400),
        label_visual("XuiLabelCenterJustify", 18, CENTER_H | CENTER_V, w=420),
        label_visual("Label_Sub", 16, ONE_LINE | CENTER_V, TEXT_DIM, x=5, opacity=0.75),
        label_visual("Label_SubRight", 16, ONE_LINE | RIGHT | CENTER_V, TEXT_DIM, x=5, opacity=0.75),
        legend_visual("A", "glyph_a_button.png", True),
        legend_visual("B", "glyph_b.png", True),
        legend_visual("X", "glyph_x.png", False),
        legend_visual("Y", "glyph_y.png", False),
        card_visual("metaScene_1line", 395, 426, pane=True),
        card_visual("Metapane", 420, 446),
        # the Blades' content back panel, loading dots and banner slot: the
        # page's card is the panel, nothing loads, and the banner art is
        # Frolick's to supply
        visual("back panel", 855, 570, []),
        visual("Loading_Large", 121, 104, []),
        visual("XuiScene", 420, 358, []),
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


def prop(block, name):
    m = re.search(r"<%s>(.*?)</%s>" % (name, name), block, re.S)
    return m.group(1) if m else None


def set_prop(block, name, value):
    if re.search(r"<%s>.*?</%s>" % (name, name), block, re.S):
        return re.sub(r"<%s>.*?</%s>" % (name, name), "<%s>%s</%s>" % (name, value, name), block, count=1, flags=re.S)
    return block.replace("</Properties>", " <%s>%s</%s>\n</Properties>" % (name, value, name), 1)


def nav_button(id, x, y, w, h, label, visual=None, nav=None, children=""):
    s = ["<XuiNavButton>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h,
         " <Position>%g,%g,0</Position>" % (x, y)]
    if visual:
        s.append(" <Visual>%s</Visual>" % visual)
    for k, v in (nav or {}).items():
        s.append(" <%s>%s</%s>" % (k, v, k))
    s += [" <Text>%s</Text>" % label, "</Properties>", children, "</XuiNavButton>"]
    return "\n".join(s)


def list_items(block, items):
    """A Blades list as its items: buttons with the skin's item visual,
    stacked down the list's box, chained up and down, the list's own
    sideways navigation on each."""
    id = prop(block, "Id")
    x, y = [float(v) for v in prop(block, "Position").split(",")[:2]]
    w = float(prop(block, "Width"))
    vis, h, line2, icon = LIST_ITEMS[prop(block, "Visual") or ""]
    pitch = h if h == 45 else h + 2
    # the list's box holds so many; the rest would scroll on the 360
    items = items[:int(float(prop(block, "Height")) // pitch)]
    nav = {k: prop(block, k) for k in ("NavLeft", "NavRight") if prop(block, k)}
    out = []
    for i, (l1, l2, frac) in enumerate(items):
        n = dict(nav)
        if i > 0:
            n["NavUp"] = "%s_%d" % (id, i - 1)
        if i < len(items) - 1:
            n["NavDown"] = "%s_%d" % (id, i + 1)
        kids = []
        if line2 and l2:
            lx, ly, lw, lh = line2
            kids.append(text("line2", lx, ly, lw, lh, 18, ONE_LINE | CENTER_V, TEXT_DIM, presenter=False, value=l2))
        if frac:
            # the Blades' device bar (prgbr_Memory): the used share, drawn as
            # Frolick's download bar
            kids.append(figure("barTrack", 5, 82, 65, 9, solid("0x66000000"), (1, BAR_STROKE)))
            kids.append(figure("barFill", 6, 83, 63 * frac, 7, solid("0xff7ac142")))
        out.append(nav_button("%s_%d" % (id, i), x, y + i * pitch, w, h, l1, vis if vis != "XuiButton" else None, n, "\n".join(kids)))
    return out


def page(file, spec):
    pad, first = spec["pad"], spec["first"]
    if spec["scene"] is None:
        # The System list in the Console Settings layout: the bars at
        # 146,153 on a 45 pitch, the header at 156,96.
        kept = []
        for i, (id, label, _) in enumerate(spec["items"]):
            nav = {}
            if i > 0:
                nav["NavUp"] = spec["items"][i - 1][0]
            if i < len(spec["items"]) - 1:
                nav["NavDown"] = spec["items"][i + 1][0]
            kept.append(nav_button(id, 146, 153 + 45 * i, 423, 47, label, nav=nav))
        kept.append(nav_button("legend_a", 573, 639, 420, 47, "Select", "legend_A"))
        kept.append(nav_button("legend_b", 555, 611, 420, 47, "Back", "legend_B").replace("XuiNavButton", "XuiBackButton"))
        kept.append("\n".join(["<XuiLabel>", "<Properties>", " <Id>labHeader</Id>", " <Width>855</Width>", " <Height>47</Height>",
                               " <Position>156,96,0</Position>", " <Visual>Label_Head</Visual>", " <Text>%s</Text>" % spec["header"],
                               "</Properties>", "</XuiLabel>"]))
    else:
        src = open(os.path.join(BLADES, spec["scene"])).read()
        props, blocks = element_blocks(src)
        kept = []
        for b in blocks:
            id = prop(b, "Id") or ""
            # The Blades' disabled legend placeholders (Enabled false) show
            # nothing on these pages, nor do the loading dots, the "no
            # items" labels of a filled list, or what the page hides.
            if "<Enabled>false</Enabled>" in b or id in spec.get("hide", []) or id in ("labDots", "ctlLoading", "txt_SelectProfile"):
                continue
            if ("Empty" in id or id == "txt_Directions") and spec.get("lists"):
                continue
            if b.startswith("<XuiList>") or b.startswith("<XuiCommonList>"):
                kept += list_items(b, spec.get("lists", {}).get(id, []))
                continue
            # The meta panel is a nested scene with a Visual on the 360,
            # which the runtime does not skin; a label carries the same
            # visual and its Text is the current setting.
            if b.startswith("<XuiScene>") and "<Visual>" in b and "metaScene" in b:
                b = re.sub(r"<ClassOverride>\w+</ClassOverride>\s*", "", b)
                b = re.sub(r"<Timelines>.*</Timelines>\s*", "", b, flags=re.S)
                b = b.replace("<XuiScene>", "<XuiLabel>").replace("</XuiScene>", "</XuiLabel>")
                b = b.replace("</Properties>", " <Text></Text>\n</Properties>", 1)
            # Navigation into a nested scene names the path; the runtime
            # finds ids anywhere in the tree.
            b = re.sub(r"(<Nav\w+>)[\w ]+\\", r"\1", b)
            # the title, the counts, the meta text
            if spec.get("header") and id in ("labSceneTitle", "txt_Header", "labHeader", "labelHeading"):
                b = set_prop(b, "Text", spec["header"])
            if id in spec.get("labels", {}):
                b = set_prop(b, "Text", spec["labels"][id])
            if id == "txt_Meta":
                b = set_prop(b, "Text", "")
            kept.append(b)

    panel = "\n".join(["<XuiGroup>", "<Properties>", " <Id>panel</Id>", " <Width>%d</Width>" % CANVAS_W, " <Height>%d</Height>" % CANVAS_H,
                        " <Position>0,0,0</Position>", " <Opacity>0</Opacity>", "</Properties>",
                        # the header strip and the body, edge to edge
                        figure("header", 0, HEADER_Y, CANVAS_W, BODY_Y - HEADER_Y, solid(HEADER_FILL)),
                        figure("body", 0, BODY_Y, CANVAS_W, BODY_END - BODY_Y, gradient([(0, PANEL_TOP), (1, PANEL_BOTTOM)])),
                        # the legend bar under the body, down to Frolick's footer
                        figure("legendBar", 0, BODY_END, CANVAS_W, LEGEND_END - BODY_END, solid(HEADER_FILL)),
                        "</XuiGroup>"])
    content = "\n".join(["<XuiGroup>", "<Properties>", " <Id>content</Id>", " <Width>%d</Width>" % CANVAS_W,
                         " <Height>%d</Height>" % CANVAS_H, " <Position>0,0,0</Position>", " <Opacity>0</Opacity>",
                         "</Properties>"] + kept + ["</XuiGroup>"])
    # Open and close, 30 frames each as dashmain's 1OpenFull / 1CloseFull:
    # the panel fades up over the first third, the Blades' content panel
    # blinks in at frames 10..26, which here is the content's fade and
    # slide.  Fade (10 frames) brings a page in from another page, the
    # Blades' FadeIn between third-level scenes.  (Tracks address the
    # root's direct children, so the two fade separately.)
    fade = [(0, 2, 0), (10, 0, 1), (31, 0, 1), (51, 2, 0), (61, 0, 0), (62, 0, 0), (63, 0, 0), (64, 0, 0), (74, 0, 1)]
    tl = [
        frames([("Open", 0, "play", ""), ("OpenEnd", 30, "stop", ""), ("Close", 31, "play", ""), ("CloseEnd", 61, "stop", ""),
                ("Normal", 62, "play", ""), ("EndNormal", 63, "stop", ""), ("Fade", 64, "play", ""), ("FadeEnd", 74, "stop", "")]),
        track("panel", "Opacity", fade),
        track("content", "Opacity", [(0, 0, 0), (10, 0, 0), (26, 0, 1), (31, 0, 1), (41, 0, 0), (61, 0, 0), (62, 0, 0), (63, 0, 0),
                                     (64, 0, 0), (74, 0, 1)]),
        track("content", "Position", [(0, 2, "24,0,0"), (30, 0, "0,0,0"), (31, 2, "0,0,0"), (61, 0, "24,0,0"),
                                      (62, 0, "24,0,0"), (63, 0, "24,0,0"), (64, 0, "0,0,0"), (74, 0, "0,0,0")]),
    ]
    body = "\n".join([panel, content])
    # The scene (the Blades' scCslSettings and so on) is "page" here, the
    # id the hub plays Open, Close and Fade on.
    return ('<XuiCanvas version="000c">\n<Properties>\n <Id>canvas</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n</Properties>\n'
            '<XuiScene>\n<Properties>\n <Id>page</Id>\n <Width>%d</Width>\n <Height>%d</Height>\n</Properties>\n%s\n<Timelines>\n%s\n</Timelines>\n'
            '</XuiScene>\n</XuiCanvas>\n' % (CANVAS_W, CANVAS_H, CANVAS_W, CANVAS_H, body, "\n".join(tl)))


# ---- the hub script's tables -------------------------------------------------

def script():
    """PageRoot, PageUrl, PageFirst, PageParent, PageTarget and PageMeta
    for default.xap: the pages as the hub walks them."""
    q = lambda s: '"%s"' % s.replace('"', "").replace("\n", "\\n")
    out = ["// The padfocus pages (tools/genpage.py PAGES): each page's scene, its",
           "// first control, the page B returns to, where a press goes, and what",
           "// the meta panel says for the focused control.",
           "function PageRoot(pad)", "{", "    if (pad == 0)", "        return %s;" % q(ROOT_PAGE["catalog"]),
           "    return %s;" % q(ROOT_PAGE["settings"]), "}", ""]
    for name, key in [("PageUrl", None), ("PageFirst", "first"), ("PageParent", "parent")]:
        out += ["function %s(page)" % name, "{"]
        for file, spec in PAGES:
            out.append("    if (page == %s)" % q(file))
            out.append("        return %s;" % q("pads/%s.xui" % file if key is None else spec[key]))
        out += ['    return "";', "}", ""]
    for name, key in [("PageTarget", "press"), ("PageMeta", "meta")]:
        out += ["function %s(page, id)" % name, "{"]
        for file, spec in PAGES:
            table = dict(spec.get(key, {}))
            if key == "press" and spec["scene"] is None:
                table.update((id, target) for id, _, target in spec["items"])
            if not table:
                continue
            out += ["    if (page == %s)" % q(file), "    {"]
            for id, v in table.items():
                out.append("        if (id == %s)" % q(id))
                out.append("            return %s;" % q(v))
            out.append("    }")
        out += ['    return "";', "}", ""]
    return "\n".join(out)


def main():
    if sys.argv[1] == "--script":
        print(script())
        return
    out = sys.argv[1]
    pads = os.path.join(out, "pads")
    os.makedirs(pads, exist_ok=True)
    open(os.path.join(pads, "page_skin.xui"), "w").write(skin())
    for file, spec in PAGES:
        open(os.path.join(pads, file + ".xui"), "w").write(page(file, spec))


if __name__ == "__main__":
    main()
