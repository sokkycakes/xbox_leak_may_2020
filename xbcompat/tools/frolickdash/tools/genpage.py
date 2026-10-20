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
through) and the legend bar below it.  Every control on it is drawn by
the Blades dashboard's own skin: art/blades/skin.xui holds the visuals
the pages use (buttons, list items, labels, the legends with the skin's
A/B/X/Y button images, the meta panes), pulled byte for byte out of
dash_dashuisk.xzp's skin.xur by tools/skinsubset.py, and page_skin.xui is
that file.  The open and close motion is timed like dashmain.xur's
1OpenFull (30 frames at 30 fps).

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
PANEL_TOP, PANEL_BOTTOM = "0xd9424c5a", "0xd9313b49"
# The header strip and the legend bar have no fill (sokky, 2026-10-20):
# the pad's big label shows through above the body as the page's title,
# the page's own header is its sub-title, and the legends sit on the pad.
# The Blades skin's body text (its presenters' TextColor / DropShadowColor).
TEXT, SHADOW = "0xe60f0f0f", "0x5aebebeb"
# Item text on the pages: white at rest, the Blades' dark only on the
# highlighted row (sokky, 2026-10-20); skin() keys the visuals so.
ITEM_REST, ITEM_FOCUS = "0xffffffff", "0xff0a0a0a"   # the focus a touch blacker than the Blades' (sokky)

CANVAS_W, CANVAS_H = 1120, 770
# The screen on the canvas: 720x480 at 0.643, so x 0..1120 and y 12..758.
# The header strip holds the Blades' header (y 96); the body ends where
# their lists end (603) and the legends (611..686) sit below it.
# The body starts 8 canvas rows (6 output lines) above the Blades' lists
# and meta text and ends 8 above their legends, the padding sokky asked
# for (2026-10-20); the sub-title sits with its text 4 rows above the body.
HEADER_Y, BODY_Y, BODY_END, LEGEND_END = 60, 142, 595, 707
HEADER_LABEL_Y = 107
# The page's header (the Blades' scene title at 156,96) is the sub-title
# under the pad's big label.  The catalog's label sits top left in its
# mirrored view, so its pages' header moves right of it.
HEADER_IDS = ("labSceneTitle", "txt_Header", "labHeader", "labelHeading")
CATALOG_HEADER_X = 500

# The Blades' list visuals: the item visual, its height (the skin's
# control_ListItem) and, per line of an item, the data slot its presenter
# is bound to (DataAssociation k shows the item's Data<k>, which the
# dashboard's lists feed and the pages write); None: the unbound
# presenter, which shows the item's Text.
LIST_ITEMS = {
    "": ("XuiButton", 45, [], None),                                        # list_Common
    "list_twoline-icon": ("btn_twoline-icon", 72, [None, 1], (11, 6, 60, 60)),
    "List_3Line_Icon_prgbr": ("btn_3Line_Icon_prgbr", 105, [2, 1], (6, 5, 60, 60)),
    "List_Fullwidth_3Icon_4string": ("ListItem_Fullwidth_3Icon_4String", 47, [], (11, 8, 32, 32)),
    "List_oneline_icon": ("btn_oneline_icon", 72, [], (11, 4.25, 64, 64)),
}
# The last tuple is the item visual's image presenter (x, y, w, h), where
# an item's icon (the 360's ico_64x_*.png, kept in art/blades/) goes.

# The pages.  scene: the Blades scene in art/blades/ (None: the System
# list, built here in the Console Settings layout); pad; first: the
# control focused on entry; parent: the page B returns to ("" closes the
# page and flies the camera back); header: the title (the scene's own if
# None); items: the buttons of the System list; lists: {list id: [(line
# 1, line 2, fraction), ...]}; press: {control id: page}; meta: {control
# id: the meta panel's text}; labels: {id: text}; positions: {id: "x,y,0"};
# clone: {id: (new id, "x,y,0")}, a copy of a control; hide: ids shown nothing
# here.  The list texts are placeholders until the pages read the console.
GAMES = [(g, "", 0) for g in CAROUSEL_GAMES[BELT_START:]]   # the belt's games, not its All games / Disc tiles
PAGES = [
    ("page_system", dict(scene=None, pad="settings", first="btnConsole", parent="", header="Settings",
                         items=[("btnConsole", "Console Settings", "page_console"), ("btnMemory", "Memory", "page_memory"),
                                ("btnNetwork", "Network Settings", "page_network")])),
    # The game's page in the library belt (A on a cover): the Blades' Game
    # Summary (dash_games.xzp 401_GameDetails) as the Xbox dashboard's
    # Games pod shows a game (sokky, 2026-10-20): Play Game, Game Storage
    # (the title's saved games in Settings > Memory), Install Game for a
    # disc or card game not yet on the hard disk (hidden otherwise), no
    # achievements; the meta pane carries the platform and runtime (as a
    # Kazeta cart names them: Xbox on xbcompat, Windows on Proton...), the
    # saves and where the game is.  Filled by the hub script (kind "game").
    ("page_game", dict(scene="401_GameDetails.xui", pad="library", first="btnViewAchievements", parent="",
                       header="Game Summary", dynamic="game",
                       labels={"btnViewAchievements": "Play Game", "btnManageGameFiles": "Game Storage",
                               "btnBrowseDownloads": "Install Game", "labGameTitle": "", "labGameSummaryTitle": "Details",
                               "labRecentAchievementsText": "", "labAchievementsText": "Saved games",
                               "labAchievementsNum": "", "labGamerPointsText": "Location", "labGamerPointsNum": ""},
                       hide=["img_frame1", "img_frame2", "img_frame3", "img_frame4", "imgAchievements1", "imgAchievements2",
                             "imgAchievements3", "imgAchievements4", "icoGamerScore"],
                       # four label/value rows on a 40 pitch where the achievement tiles were;
                       # the Runtime row is a copy of the Achievements pair
                       positions={"labRecentAchievementsText": "581,286,0", "labAchievementsText": "581,366,0",
                                  "labAchievementsNum": "782,366,0", "labGamerPointsText": "581,406,0",
                                  "labGamerPointsNum": "782,406,0"},
                       clone={"labAchievementsNum": ("labRecentAchievementsNum", "782,286,0"),
                              "labAchievementsText": ("labRuntimeText", "581,326,0"),
                              "labAchievementsNum ": ("labRuntimeNum", "782,326,0")})),
    ("page_console", dict(scene="dashSysCslSet.xui", pad="settings", first="btnDisplay", parent="page_system",
                          meta={"btnDisplay": "480i NTSC, 4:3", "btnAudio": "Stereo", "btnLanguage": "English",
                                "btnClock": "Set from the network", "btnCountry": "United States", "btnAutoOff": "Off",
                                "btnScreensaver": "Off", "btnRemote": "Not set up", "btnPolicy": "Frolick on the Sion"})),
    # The Memory pages read the console (hub script: theStorage,
    # theMemoryMonitor, the dashboard's own saved-game code): a dynamic page
    # is a list the script fills a row at a time (dynamic: what it lists,
    # rows: as many as the Blades' list box holds, scrolled by the script).
    ("page_memory", dict(scene="DeviceSelector.xui", pad="settings", first="list_devices_0", parent="page_system",
                         header="Memory", dynamic="devices", hide=["txt_Directions"],
                         labels={"legend_y": "", "txt_EmptyList": "No storage devices found."})),
    ("page_memory_titles", dict(scene="Items.xui", pad="settings", first="list_items_0", parent="page_memory",
                                header="Hard Drive", visual="list_twoline-icon", dynamic="titles",
                                labels={"legend_y": "Delete All", "txt_EmptyList": "No game data on this device."})),
    ("page_memory_saves", dict(scene="Items.xui", pad="settings", first="list_items_0", parent="page_memory_titles",
                               header="Saved Games", visual="list_twoline-icon", dynamic="saves",
                               labels={"legend_y": "Delete", "txt_EmptyList": "No saved games."})),
    # The library pad's X and Y on the hub (sokky 2026-10-20): recent games
    # = the games launched most recently (GameCollection records each
    # launch in T:\recent.txt), favorites not built yet, so an empty Blades
    # list stands in.  The hub fills them (kind "recent" / "favorites").
    ("page_recent", dict(scene="Items.xui", pad="library", first="list_items_0", parent="",
                         header="Recent Games", visual="list_twoline-icon", dynamic="recent",
                         labels={"legend_y": "", "legend_a": "Play", "txt_EmptyList": "No games played yet."})),
    ("page_favorites", dict(scene="Items.xui", pad="library", first="list_items_0", parent="",
                            header="Favorite Games", visual="list_twoline-icon", dynamic="favorites",
                            labels={"legend_y": "", "txt_EmptyList": "Favorites are not available yet."})),
    # The Blades' download screen (marketplace_media.xzp 1443_DownloadScene):
    # a look at it for Frolick's download UI (sokky 2026-10-20), not wired
    # to anything yet.
    ("page_download", dict(scene="1443_DownloadScene.xui", pad="catalog", first="btnContinue", parent="",
                           header="Download",
                           labels={"labSceneTitle": "Stiletto Swift", "labContentTitle": "Stiletto Swift",
                                   "labContentSize": "412 MB", "edtContentMetaText": "Frolick game card image",
                                   "btnContinue": "Pause", "labDownloadProgress": "70% (288 MB of 412 MB)"})),
    ("page_memory_actions", dict(scene=None, pad="settings", first="btnCopy", parent="page_memory_saves", header="Saved Game",
                                 items=[("btnCopy", "Copy", ""), ("btnMove", "Move", ""), ("btnDelete", "Delete", "")])),
    ("page_memory_dest", dict(scene="DeviceSelector.xui", pad="settings", first="list_devices_0", parent="page_memory_actions",
                              header="Copy To", dynamic="dest", hide=["txt_Directions"],
                              labels={"legend_y": "", "txt_EmptyList": "Insert a memory unit to copy to."})),
    ("page_confirm", dict(scene=None, pad="settings", first="btnNo", parent="page_memory_saves", header="Are you sure?",
                          items=[("btnYes", "Yes", ""), ("btnNo", "No", "")])),
    ("page_notice", dict(scene=None, pad="settings", first="btnOK", parent="page_memory_saves", header="",
                         items=[("btnOK", "OK", "")])),
    ("page_network", dict(scene="2000_NetworkConfigList.xui", pad="settings", first="list_items_0", parent="page_system",
                          lists={"list_items": [("Wireless Network", "Connected", 0, "ico_64x_Infrastructure.png")]},
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


def gradient(stops, rotation=-90):   # -90: from the top down
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


def frames(names):
    return "<NamedFrames>\n%s\n</NamedFrames>" % "\n".join(
        "<NamedFrame><Name>%s</Name><Time>%d</Time><Command>%s</Command><CommandParams>%s</CommandParams></NamedFrame>"
        % (n, t, c, p) for n, t, c, p in names)


def track(id, prop, keys):
    return "<Timeline>\n<Id>%s</Id>\n<TimelineProp>%s</TimelineProp>\n%s\n</Timeline>" % (id, prop, "\n".join(
        "<KeyFrame><Time>%d</Time><Interpolation>%d</Interpolation><Prop>%s</Prop></KeyFrame>" % (t, i, v) for t, i, v in keys))




# ---- the skin ---------------------------------------------------------------

# The visuals whose text follows focus: the buttons and list items.
ITEM_VISUALS = ("XuiButton", "btn_oneline_icon", "btn_twoline-icon", "btn_3Line_Icon_prgbr", "ListItem_Fullwidth_3Icon_4String")
REST_STATES = ("Normal", "EndNormal", "NormalDisable", "EndNormalDisable", "NormalSelDisable", "EndNormalSelDisable")


def item_text_colours(visual):
    """Keys every text presenter's TextColor in a button visual: white in
    the rest states, the Blades' dark in the focus, press and init-focus
    states, fading with the highlight over the KillFocus and Focus runs
    (the named frames give each visual's own times)."""
    frames = [(m.group(1), int(m.group(2))) for m in re.finditer(r"<NamedFrame><Name>([^<]*)</Name><Time>(\d+)</Time>", visual)]
    if not frames:
        return visual
    keys = []
    for i, (name, t) in enumerate(frames):
        nxt = frames[i + 1][1] if i + 1 < len(frames) else t
        if name == "KillFocus":
            keys += [(t, ITEM_FOCUS), (nxt, ITEM_REST)]
        elif name == "Focus":
            keys += [(t, ITEM_REST), (nxt, ITEM_FOCUS)]
        elif name in REST_STATES:
            keys.append((t, ITEM_REST))
        else:
            keys.append((t, ITEM_FOCUS))
    keys = sorted(dict(keys).items())
    ids = re.findall(r"<XuiTextPresenter>\n<Properties>\n <Id>([^<]*)</Id>", visual)
    tracks = []
    for id in ids:
        tracks.append("<Timeline>\n <Id>%s</Id>\n <TimelineProp>TextColor</TimelineProp>\n%s</Timeline>\n" % (
            id, "".join("<KeyFrame><Time>%d</Time><Interpolation>0</Interpolation><Prop>%s</Prop></KeyFrame>\n" % k for k in keys)))
        visual = re.sub(r"(<Id>%s</Id>\n(?:[^\n]*\n){0,12}? <TextColor>)%s(</TextColor>)" % (re.escape(id), TEXT),
                        r"\g<1>%s\g<2>" % ITEM_REST, visual, count=1)
    k = visual.rfind("</Timelines>")
    return visual[:k] + "".join(tracks) + visual[k:]


def skin():
    """The Blades skin's visuals, as tools/skinsubset.py pulled them, the
    buttons' and list items' text keyed white at rest and dark on focus."""
    s = open(os.path.join(BLADES, "skin.xui")).read()
    def fix(m):
        if m.group(1) in ITEM_VISUALS:
            return item_text_colours(m.group(0))
        # labels and the meta pane: white body text
        return m.group(0).replace("<TextColor>%s</TextColor>" % TEXT, "<TextColor>%s</TextColor>" % ITEM_REST)
    return re.sub(r"<XuiVisual>\n<Properties>\n <Id>([^<]*)</Id>.*?</XuiVisual>", fix, s, flags=re.S)


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


def list_capacity(block, visual=None):
    """How many items the list's box holds (the rows of a dynamic page)."""
    vis, h, lines, icon = LIST_ITEMS[visual or prop(block, "Visual") or ""]
    pitch = h if h == 45 else h + 2
    return int(float(prop(block, "Height")) // pitch)


def list_items(block, items, visual=None, dynamic=False):
    """A Blades list as its items: buttons with the skin's item visual,
    stacked down the list's box, chained up and down, the list's own
    sideways navigation on each.  The lines the dashboard would feed the
    item's bound presenters are drawn in their place."""
    id = prop(block, "Id")
    x, y = [float(v) for v in prop(block, "Position").split(",")[:2]]
    w = float(prop(block, "Width"))
    vis, h, lines, icon = LIST_ITEMS[visual or prop(block, "Visual") or ""]
    # btn_3Line_Icon_prgbr's bar (the 360 feeds it the device's fill): the
    # same visual over it, held at the item's fraction
    bar = (5, 76, 65, 21, "prgbr_Memory") if vis == "btn_3Line_Icon_prgbr" else None
    pitch = h if h == 45 else h + 2
    # the list's box holds so many; the rest would scroll on the 360
    items = items[:int(float(prop(block, "Height")) // pitch)]
    if dynamic:
        # every row, blank, with an icon slot and the gauge: the script
        # fills them (SetText, SetData, SetImage, SetValue, SetVisible)
        items = [("", "", 0, "blank")] * int(float(prop(block, "Height")) // pitch)
    nav = {k: prop(block, k) for k in ("NavLeft", "NavRight") if prop(block, k)}
    out = []
    for i, item in enumerate(items):
        l1, l2, frac = item[:3]
        ico = item[3] if len(item) > 3 else ""
        n = dict(nav)
        if i > 0:
            n["NavUp"] = "%s_%d" % (id, i - 1)
        if i < len(items) - 1:
            n["NavDown"] = "%s_%d" % (id, i + 1)
        kids, label, data = [], l1, {}
        for j, slot in enumerate(lines):
            if slot is None:
                continue
            data["Data%d" % slot] = (l1, l2)[j]
            if j == 0:
                label = ""      # the first line has its own slot; the Text would double it
        if ico and icon:
            ix, iy, iw, ih = icon
            kids.append("\n".join(["<XuiImage>", "<Properties>", " <Id>%s_%d_icon</Id>" % (id, i), " <Width>%g</Width>" % iw, " <Height>%g</Height>" % ih,
                                   " <Position>%g,%g,0</Position>" % (ix, iy), " <ImagePath>%s</ImagePath>" % ("" if ico == "blank" else ico),
                                   "</Properties>", "</XuiImage>"]))
        if bar:
            bx, by, bw, bh, bvis = bar
            kids.append("\n".join(["<XuiProgressBar>", "<Properties>", " <Id>%s_%d_bar</Id>" % (id, i), " <Width>%g</Width>" % bw, " <Height>%g</Height>" % bh,
                                   " <Position>%g,%g,0</Position>" % (bx, by), " <Visual>%s</Visual>" % bvis,
                                   " <RangeMin>0</RangeMin>", " <RangeMax>1</RangeMax>", " <Value>%g</Value>" % frac, "</Properties>", "</XuiProgressBar>"]))
        n.update(data)
        out.append(nav_button("%s_%d" % (id, i), x, y + i * pitch, w, h, label, vis if vis != "XuiButton" else None, n, "\n".join(kids)))
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
            if id in spec.get("hide", []) or id in ("labDots", "ctlLoading", "txt_SelectProfile"):
                continue
            if ("Empty" in id or id == "txt_Directions") and spec.get("lists"):
                continue
            if b.startswith("<XuiList>") or b.startswith("<XuiCommonList>"):
                kept += list_items(b, spec.get("lists", {}).get(id, []), spec.get("visual"), bool(spec.get("dynamic")))
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
                # a legend reads as live only when enabled (the skin's
                # NormalDisable frame shows a grey button and no text); an
                # empty label makes it the Blades' blank placeholder
                if id.startswith("legend_"):
                    b = set_prop(b, "Enabled", "true" if spec["labels"][id] else "false")
            if id in spec.get("positions", {}):
                b = set_prop(b, "Position", spec["positions"][id])
            # clone: {source id (a trailing space makes a second copy): (new id, position)}
            for src, (nid, pos) in spec.get("clone", {}).items():
                if src.strip() == id:
                    kept.append(set_prop(set_prop(set_prop(b, "Id", nid), "Position", pos), "Text", ""))
            if id == "txt_Meta":
                b = set_prop(b, "Text", "")
            kept.append(b)

    hx = CATALOG_HEADER_X if pad == "catalog" else 156
    kept = [set_prop(b, "Position", "%d,%d,0" % (hx, HEADER_LABEL_Y)) if prop(b, "Id") in HEADER_IDS else b for b in kept]
    # body text is white on the slate (sokky, 2026-10-20), the Blades' dark
    # stays only on highlighted items (the skin's own tracks)
    kept = [b.replace("<TextColor>0xe60f0f0f</TextColor>", "<TextColor>0xffffffff</TextColor>")
             .replace("<TextColor>0xff0f0f0f</TextColor>", "<TextColor>0xffffffff</TextColor>") for b in kept]

    panel = "\n".join(["<XuiGroup>", "<Properties>", " <Id>panel</Id>", " <Width>%d</Width>" % CANVAS_W, " <Height>%d</Height>" % CANVAS_H,
                        " <Position>0,0,0</Position>", " <Opacity>0</Opacity>", "</Properties>",
                        # the body, edge to edge; the header above it and the
                        # legend bar below have no fill
                        figure("body", 0, BODY_Y, CANVAS_W, BODY_END - BODY_Y, gradient([(0, PANEL_TOP), (1, PANEL_BOTTOM)])),
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

def dynamic_info(spec):
    """A dynamic page's list id and its rows."""
    src = open(os.path.join(BLADES, spec["scene"])).read()
    for b in element_blocks(src)[1]:
        if b.startswith("<XuiList>") or b.startswith("<XuiCommonList>"):
            return prop(b, "Id"), list_capacity(b, spec.get("visual"))
    return "", 0


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
    for name, key, default in [("PageKind", "dynamic", '""'), ("PageList", "list", '""'), ("PageRows", "rows", "0")]:
        out += ["function %s(page)" % name, "{"]
        for file, spec in PAGES:
            if not spec.get("dynamic"):
                continue
            info = dynamic_info(spec)
            v = {"dynamic": q(spec["dynamic"]), "list": q(info[0]), "rows": str(info[1])}[key]
            out.append("    if (page == %s)" % q(file))
            out.append("        return %s;" % v)
        out += ["    return %s;" % default, "}", ""]
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
