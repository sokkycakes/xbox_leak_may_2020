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
HEADER_IDS = ("labSceneTitle", "txt_Header", "labHeader", "labelHeading", "lblHelpTitle")
CATALOG_HEADER_X = 500

# The Blades' list visuals: the item visual, its height (the skin's
# control_ListItem) and, per line of an item, the data slot its presenter
# is bound to (DataAssociation k shows the item's Data<k>, which the
# dashboard's lists feed and the pages write); None: the unbound
# presenter, which shows the item's Text.
LIST_ITEMS = {
    "": ("XuiButton", 45, [], None),                                        # list_Common
    "list_twoline-icon": ("btn_twoline-icon", 72, [None, 1], (11, 6, 60, 60)),
    # btn_twoline-icon with a status at the right of line 2 (Data3), built in skin()
    "btn_twoline-status": ("btn_twoline-status", 72, [None, 1], (11, 6, 60, 60)),
    "List_3Line_Icon_prgbr": ("btn_3Line_Icon_prgbr", 105, [2, 1], (6, 5, 60, 60)),
    "List_Fullwidth_3Icon_4string": ("ListItem_Fullwidth_3Icon_4String", 47, [], (11, 8, 32, 32)),
    "List_oneline_icon": ("btn_oneline_icon", 72, [], (11, 4.25, 64, 64)),
}
# The last tuple is the item visual's image presenter (x, y, w, h), where
# an item's icon (the 360's ico_64x_*.png, kept in art/blades/) goes.
# A list's section headers (the NXE's Active / Pending over its downloads)
# are SECTION_H tall and sit over the row they name; a page's bar (the
# download's progress) runs under line 2 of each row.
SECTION_H = 36
ROW_BAR = (83, 63, 740, 6)

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
    # Console Settings (sokky, 2026-10-25): the Blades' list without
    # Locale, the rows under it moved up, Developer Settings added at the
    # end.  Auto-Off and Remote Control come later.  "@" meta: the hub
    # script says the current setting (SysMeta).
    ("page_console", dict(scene="dashSysCslSet.xui", pad="settings", first="btnDisplay", parent="page_system",
                          hide=["btnCountry"],
                          positions={"btnAutoOff": "146,333,0", "btnScreensaver": "146,378,0", "btnRemote": "146,423,0",
                                     "btnPolicy": "146,468,0"},
                          clone={"btnPolicy": ("btnDeveloper", "146,513,0")},
                          labels={"btnDeveloper": "Developer Settings"},
                          nav={"btnClock": {"NavDown": "btnAutoOff"}, "btnAutoOff": {"NavUp": "btnClock"},
                               "btnPolicy": {"NavDown": "btnDeveloper"}, "btnDeveloper": {"NavUp": "btnPolicy"}},
                          press={"btnDisplay": "page_display", "btnAudio": "page_audio", "btnLanguage": "page_language",
                                 "btnClock": "page_clock", "btnScreensaver": "page_screensaver", "btnPolicy": "page_sysinfo",
                                 "btnDeveloper": "page_developer"},
                          meta={k: "@" for k in ("btnDisplay", "btnAudio", "btnLanguage", "btnClock", "btnAutoOff",
                                                 "btnScreensaver", "btnRemote", "btnPolicy", "btnDeveloper")})),
    ("page_display", dict(scene="dashSysCslSetDisplay.xui", pad="settings", first="btnHD", parent="page_console",
                          hide=["btnPAL", "labAVPackInfo", "labCurrentSetting", "scnCurrentFormat"],
                          labels={"btnHD": "Resolution", "labDescription":
                                  "Set the resolution and screen format to match your TV. A new resolution shows "
                                  "at once; keep it within 15 seconds or the console goes back to the old one."},
                          nav={"btnScreenFormat": {"NavDown": ""}},
                          press={"btnHD": "page_display_hd", "btnScreenFormat": "page_display_format"},
                          meta={"btnHD": "@", "btnScreenFormat": "@"})),
    # The option lists below are filled by the hub script (kind "options":
    # OptCount / OptName / OptSelect), the current one in labCurrentSettings.
    ("page_display_hd", dict(scene="dashSysCslSetDisplayHiDef.xui", pad="settings", first="listOptions_0",
                             parent="page_display", header="Resolution", dynamic="options", hide=["labAVPackInfo"],
                             labels={"labMetaBody": "480i and 480p are standard definition; 720p and 1080p are "
                                     "high definition. Games that support HDTV modes use the ones up to the "
                                     "resolution set here."})),
    ("page_display_format", dict(scene="dashSysCslSetDisplayFormat.xui", pad="settings", first="btnNormal",
                                 parent="page_display", labels={"btnNormal": "Normal (4:3)", "btnWide": "Widescreen (16:9)"})),
    ("page_audio", dict(scene="dashSysCslSetAudioAnalog.xui", pad="settings", first="listOptions_0", parent="page_console",
                        header="Audio Settings", dynamic="options",
                        labels={"labMetaBody": "• Mono for a TV with one speaker.\n• Stereo for two speakers.\n"
                                "• Dolby Digital 5.1 for a surround receiver; games that support it play in 5.1."})),
    ("page_language", dict(scene="dashSysCslSetLanguage.xui", pad="settings", first="lstLanguages_0", parent="page_console",
                           dynamic="options", labels={"labMetaBody": "Games use this language when they have it."})),
    ("page_clock", dict(scene="dashSysCslSetClock.xui", pad="settings", first="btnOption1", parent="page_console",
                        hide=["btnOption4"],
                        labels={"btnOption1": "Sync to Internet Time", "btnOption2": "Time Zone",
                                "btnOption3": "Daylight Saving Time", "labMetaBody": ""},
                        nav={"btnOption3": {"NavDown": ""}},
                        press={"btnOption2": "page_timezone"})),
    ("page_timezone", dict(scene="dashSysCslSetClockTimeZone.xui", pad="settings", first="lstTimezone_0",
                           parent="page_clock", dynamic="options")),
    ("page_screensaver", dict(scene="dashSysCslSetScreensaver.xui", pad="settings", first="btnOn", parent="page_console",
                              labels={"labMetaBody": "The screen saver dims the picture after five minutes "
                                      "without a button pressed. More screen savers will come later."})),
    ("page_sysinfo", dict(scene="dashSysCslSetPolicyInfo_System.xui", pad="settings", first="btnB", parent="page_console",
                          hide=["btnA", "btnX", "btnY"], labels={"edInfo": ""})),
    ("page_developer", dict(scene="dashSysCslSetAudioAnalog.xui", pad="settings", first="listOptions_0",
                            parent="page_console", header="Developer Settings", dynamic="options",
                            labels={"labCurrentSettingsText": "Games Run On", "labMetaBody":
                                    "xbcompat replaces the Xbox's graphics library and is the fastest. xbcompat-gpu "
                                    "emulates the Xbox's GPU (xemu's NV2A) and draws some games more faithfully. "
                                    "The dashboards always run on xbcompat. The change applies to the next game."})),
    # A new resolution's question (the hub counts down in its header)
    ("page_keep", dict(scene=None, pad="settings", first="btnKeep", parent="page_display_hd", header="Keep this resolution?",
                       items=[("btnKeep", "Keep", ""), ("btnRevert", "Go Back", "")])),
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
    # All games (the belt's first tile, A; sokky 2026-10-22): its own scene
    # like Recent and Favorites, the games in the PS2 memory card browser's
    # grid (the hub's GameGrid node draws them and the blurred background
    # under this page), so the page is the header, the count and the
    # legends with no body panel and no list.
    ("page_allgames", dict(scene="Items.xui", pad="library", first="legend_b", parent="",
                           header="All Games", dynamic="allgames", list="", rows=0, body=False,
                           hide=["list_items", "txt_EmptyList"],
                           labels={"legend_y": "", "legend_x": "", "legend_a": "Play"})),
    # Downloads (catalog pad, Y; sokky 2026-10-20, from the NXE's Active
    # Downloads): the active download under "Active", the queue under
    # "Pending"; each row the title, its kind, the status at the right and,
    # on the active one, the progress.  A starts a pending item now or
    # pauses/resumes the active one, X removes an item.  Filled by the hub
    # (kind "downloads") from the Downloads node, a placeholder queue until
    # Frolick can download.
    ("page_downloads", dict(scene="Items.xui", pad="catalog", first="list_items_0", parent="",
                            header="Downloads", visual="btn_twoline-status", dynamic="downloads",
                            sections={0: "Active", 1: "Pending"}, bar=True,
                            labels={"legend_y": "", "legend_x": "Remove", "legend_a": "Start Now", "txt_EmptyList": "No downloads."})),
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
    # The catalog pad's focus view (A) is the later 360 dashboard's
    # Marketplace blade (sokky 2026-10-21, from dash.xex's 202 RootScene and
    # the skin's btn_1line_icon / metaScene_MarketPlace): a one-line list on
    # the left, the meta pane on the right with a highlight that follows the
    # focused row out of the list.  Built here (market_page).
    ("page_market", dict(scene="market", pad="catalog", first="btnSpotlight", parent="", header="Marketplace",
                         items=[("btnSpotlight", "Spotlight", "page_spotlight",
                                 "New and featured games, demos and extras."),
                                ("btnGameDownloads", "Game Downloads", "page_store_games",
                                 "Full games to download to the hard drive."),
                                ("btnDemosAndTrailers", "Demos and Trailers", "page_store_demos",
                                 "Try a game before you get it."),
                                ("btnThemes", "Themes and Gamer Pictures", "page_store_themes",
                                 "Themes and pictures for the dashboard."),
                                ("btnActiveDownloads", "Active Downloads", "page_downloads",
                                 "What is downloading now and what is queued."),
                                ("btnDownloadHistory", "Download History", "page_store_history",
                                 "Everything downloaded so far.")])),
    # Spotlight (catalog X, and the first item of the Marketplace): the
    # later dashboard's Spotlight screen (sokky's screenshot): the featured
    # banner, a row of category tabs (left and right switch, the list
    # wrapping round), four two-line items (btn_twoline_left, the List_twoline
    # counter under them) and the meta pane metaScene_2lineFocus_Long beside
    # them, its highlight following the focused row.  Y is Marketplace
    # Home.  Filled by the hub (kind "spotlight") from theStore, the
    # Internet Archive catalog frolick-store builds (SPOTLIGHT below).
    ("page_spotlight", dict(scene="spotlight", pad="catalog", first="lstSpot_0", parent="", header="Spotlight",
                            dynamic="spotlight", list="lstSpot", rows=4 if os.environ.get("FROLICK_SPOT_BANNER", "0") == "1" else 5,
                            legend_y="Marketplace Home")),
    ("page_store_games", dict(scene="1412_GameListScene.xui", pad="catalog", first="lstGames_0", parent="page_market",
                              header="Game Downloads", lists={"lstGames": GAMES}, hide=["btnX"],
                              labels={"labItemCount": "%d items" % min(len(GAMES), 8)})),
    ("page_store_demos", dict(scene="1412_GameListScene.xui", pad="catalog", first="lstGames_0", parent="page_market",
                              header="Demos and Trailers", lists={"lstGames": GAMES[2:8]}, hide=["btnX"],
                              labels={"labItemCount": "6 items"})),
    ("page_store_themes", dict(scene="1412_GameListScene.xui", pad="catalog", first="lstGames_0", parent="page_market",
                               header="Themes and Gamer Pictures", lists={"lstGames": [("Frolick", "", 0), ("Sion", "", 0)]},
                               hide=["btnX"], labels={"labItemCount": "2 items"})),
    ("page_store_history", dict(scene="1414_DownloadHistoryScene.xui", pad="catalog", first="lstDownloadHistory_0", parent="page_market",
                                lists={"lstDownloadHistory": GAMES[:3]}, hide=["btnX"], labels={"labItemCount": "3 items"})),
]
ROOT_PAGE = {"settings": "page_system", "catalog": "page_market"}

# Spotlight's tabs: the catalog frolick-store builds on the Sion from the
# Internet Archive (store/main.go defaultTabs, matched by number; the
# items come from theStore at run time).  Windows games first (sokky,
# 2026-10-22).
SPOTLIGHT = ["Featured", "Demos", "Shareware", "Freeware"]


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
ITEM_VISUALS = ("XuiButton", "btn_oneline_icon", "btn_twoline-icon", "btn_twoline-status", "btn_3Line_Icon_prgbr",
                "ListItem_Fullwidth_3Icon_4String", "btn_1line_icon", "btn_twoline_left", "btn_twoline_spot")
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
    # the later dashboard's Marketplace widgets (art/blades/skin_mp.xui,
    # pulled the same way out of the later dash.xex's skin)
    mp = open(os.path.join(BLADES, "skin_mp.xui")).read()
    mp = mp[mp.find("<XuiVisual>"):mp.rfind("</XuiScene>")]
    mp += spot_row_visual(mp) + spot_meta_visual(mp)
    s = s.replace("</XuiScene></XuiCanvas>", mp + "</XuiScene></XuiCanvas>")
    # btn_3Line_Icon_prgbr's logo spot is bound to Data1, the line our device
    # rows put their free space in, so the engine looked that text up as an
    # image file; Frolick has no logo for it
    s = s.replace(" <Id>msLogoSpace</Id>\n <Width>195</Width>\n <Height>32</Height>\n"
                  " <Position>653,70,0</Position>\n <Anchor>4</Anchor>\n <DataAssociation>1</DataAssociation>\n",
                  " <Id>msLogoSpace</Id>\n <Width>195</Width>\n <Height>32</Height>\n"
                  " <Position>653,70,0</Position>\n <Anchor>4</Anchor>\n", 1)
    def fix(m):
        if m.group(1) in ITEM_VISUALS:
            return item_text_colours(m.group(0))
        # labels and the meta pane: white body text
        return m.group(0).replace("<TextColor>%s</TextColor>" % TEXT, "<TextColor>%s</TextColor>" % ITEM_REST)
    s = re.sub(r"<XuiVisual>\n<Properties>\n <Id>([^<]*)</Id>.*?</XuiVisual>", fix, s, flags=re.S)
    return s.replace("</XuiScene></XuiCanvas>", status_visual(s) + bar_visual() + "</XuiScene></XuiCanvas>")


# Spotlight's row and meta pane, after the reference (sokky 2026-10-21):
# the title one line at the row's text inset, the item's kind under it at
# half opacity (Data3, so the visual's right-hand Data1 slot stays empty);
# the meta pane's description lower, below its title and image bands.
SPOT_TEXT_X = 82        # the reference's title inset (0.074 of its width over the list's 0.382)


def spot_row_visual(mp):
    v = re.search(r"<XuiVisual>\n<Properties>\n <Id>btn_twoline_left</Id>.*?</XuiVisual>", mp, flags=re.S).group(0)
    v = v.replace("<Id>btn_twoline_left</Id>", "<Id>btn_twoline_spot</Id>", 1)
    dx = SPOT_TEXT_X - 11.4615
    def shift(m, dy=0.0):
        x, y, z = [float(c) for c in m.group(1).split(",")]
        return "<Prop>%g,%g,%g</Prop>" % (x + dx, y + dy, z)
    pres = re.search(r"<XuiTextPresenter>\n<Properties>\n <Id>txt_line3</Id>.*?</XuiTextPresenter>", v, flags=re.S).group(0)
    title = pres.replace("<Width>310.538</Width>", "<Width>%g</Width>" % (410 - SPOT_TEXT_X)).replace("<Height>56</Height>", "<Height>30</Height>")
    title = title.replace("<Position>11.4615,6,0</Position>", "<Position>%g,6,0</Position>" % SPOT_TEXT_X)
    title = title.replace("<TextStyle>16385</TextStyle>", "<TextStyle>%d</TextStyle>" % (16385 | 0x10 | STYLE_ELLIPSIS))
    kind = title.replace("<Id>txt_line3</Id>", "<Id>txt_kind</Id>").replace("<Position>%g,6,0</Position>" % SPOT_TEXT_X,
                                                                         "<Position>%g,34,0</Position>" % SPOT_TEXT_X)
    kind = kind.replace("</Properties>", " <Opacity>0.5</Opacity>\n <DataAssociation>3</DataAssociation>\n</Properties>")
    v = v.replace(pres, title + "\n" + kind)
    tl = re.search(r"<Timeline>\s*<Id>txt_line3</Id>.*?</Timeline>", v, flags=re.S).group(0)
    t1 = re.sub(r"<Prop>([-\d.]+,[-\d.]+,[-\d.]+)</Prop>", shift, tl)
    # the kind's track: the same motion 28 rows lower, at half the opacity
    t2 = re.sub(r"<Prop>([-\d.]+,[-\d.]+,[-\d.]+)</Prop>", lambda m: shift(m, 28), tl)
    t2 = re.sub(r"(<KeyFrame>.*?\n  <Prop>)([-\d.]+)(</Prop>)", lambda m: m.group(1) + "%g" % (float(m.group(2)) * 0.5) + m.group(3), t2)
    t2 = t2.replace("<Id>txt_line3</Id>", "<Id>txt_kind</Id>", 1)
    return v.replace(tl, t1 + "\n" + t2)


def spot_meta_visual(mp):
    v = re.search(r"<XuiVisual>\n<Properties>\n <Id>metaScene_2lineFocus_Long</Id>.*?</XuiVisual>", mp, flags=re.S).group(0)
    v = v.replace("<Id>metaScene_2lineFocus_Long</Id>", "<Id>metaScene_spot</Id>", 1)
    v = re.sub(r"(<Id>Pane_txt1</Id>(?:\n[^\n]*){0,3}?\n <Position>)17.56,108,0", r"\g<1>%g,%g,0" % (17.56 + SPOT_BOX_INSET + 8, 2 * round((74 * 5 - 2) * SPOT_TITLE_BAND) + 8), v, count=1)
    v = re.sub(r"(<Id>Pane_txt</Id>(?:\n[^\n]*){0,3}?\n <Position>)17.56,0,0", r"\g<1>%g,%g,0" % (17.56 + SPOT_BOX_INSET + 8, SPOT_BAND_Y + 6), v, count=1)
    v = re.sub(r"(<Id>Pane_txt1?</Id>\n <Width>)383(</Width>)", r"\g<1>360\g<2>", v)
    # Catalog text can run long: the title stops at its band (two lines),
    # the description above the size line at the box's foot, each ending
    # in "..." (the heights are stretched with the pane, Anchor 15, so they
    # are set for the five-row pane's 368 of the visual's 450)
    ph, vh = 74 * 5 - 2, 450
    title_h = round(ph * SPOT_TITLE_BAND)
    desc_y = 2 * title_h + 8
    v = re.sub(r"(<Id>Pane_txt</Id>\n <Width>\d+</Width>\n <Height>)407(</Height>)", r"\g<1>%g\g<2>" % (title_h - 14 - SPOT_BAND_Y - ph + vh), v)
    v = re.sub(r"(<Id>Pane_txt1</Id>\n <Width>\d+</Width>\n <Height>)367(</Height>)", r"\g<1>%g\g<2>" % (ph - desc_y - 40 - ph + vh), v)
    v = re.sub(r"(<Id>Pane_txt1?</Id>(?:\n[^\n]*){0,8}?\n <TextStyle>)16385(</TextStyle>)", r"\g<1>%d\g<2>" % (16385 | STYLE_ELLIPSIS), v)
    return restep_pane(v, SPOT_PITCH)


# The pane's frames step its highlight and edge strips a row at a time on
# the 360's pitch (List_2Line_Focus: one-line rows ~42 apart, the focused
# one opening to two lines).  Spotlight's rows are all two-line, 74 apart,
# so every row-following key moves down by the extra pitch for its row:
# the row is read off highlight1 at the key's time, the pieces keep their
# own sizes, and the top strips (drawn upward from the row) grow by the
# same amount so they still reach the pane's top.
SPOT_PITCH = 74
# the XUI runtime's own text style: "..." where text runs out of its box
STYLE_ELLIPSIS = 0x1000000
# The details box: Metapane inset from the pane's edge strip, the title
# band the reference's 23% of the pane, the image band as tall again, the
# description under them; bands in Frolick's slate (a darker wash).
SPOT_BOX_INSET = 8
SPOT_BAND_X, SPOT_BAND_Y = 4, 4
SPOT_TITLE_BAND = 0.23
SPOT_BAND_FILL = "0x38000000"


def restep_pane(v, pitch):
    tracks = list(re.finditer(r"<Timeline>\s*<Id>([^<]*)</Id>(.*?)</Timeline>", v, flags=re.S))
    keyre = r"<KeyFrame><Time>(\d+)</Time>(.*?)</KeyFrame>"
    def keys(body):
        props = re.findall(r"<TimelineProp>([^<]*)</TimelineProp>", body)
        return props, [(int(m.group(1)), re.findall(r"<Prop>([^<]*)</Prop>", m.group(2))) for m in re.finditer(keyre, body, flags=re.S)]
    hl = [t for t in tracks if t.group(1) == "highlight1"][0]
    props, ks = keys(hl.group(2))
    hk = [(t, float(p[props.index("Position")].split(",")[1])) for t, p in ks]
    rows = sorted(set(y for _, y in hk))                      # the row tops on the 360's pitch
    def hy(t):
        for (t0, y0), (t1, y1) in zip(hk, hk[1:]):
            if t0 <= t <= t1:
                return y0 if t1 == t0 else y0 + (y1 - y0) * (t - t0) / (t1 - t0)
        return hk[-1][1]
    def shift(t):
        y = hy(t)
        for i in range(len(rows) - 1):
            if rows[i] - 0.01 <= y <= rows[i + 1] + 0.01:
                k = i + (y - rows[i]) / (rows[i + 1] - rows[i])
                return (rows[0] + k * pitch) - y
        return (rows[0] + (len(rows) - 1) * pitch) - rows[-1]
    out = v
    for tr in tracks:
        props, ks = keys(tr.group(2))
        if "Position" not in props:
            continue
        pi = props.index("Position")
        ys = set(p[pi].split(",")[1] for _, p in ks)
        if len(ys) < 2:
            continue        # a piece that stays put (the end caps)
        body = tr.group(2)
        def fix(m):
            t = int(m.group(1))
            vals = re.findall(r"<Prop>([^<]*)</Prop>", m.group(2))
            d = shift(t)
            x, y, z = vals[pi].split(",")
            vals[pi] = "%s,%g,%s" % (x, float(y) + d, z)
            if "Height" in props and "_top" in tr.group(1):
                sy = 1.0
                if "Scale" in props:
                    sy = abs(float(vals[props.index("Scale")].split(",")[1])) or 1.0
                hi = props.index("Height")
                vals[hi] = "%g" % (float(vals[hi]) + d / sy)
            it = iter(vals)
            return "<KeyFrame><Time>%s</Time>%s</KeyFrame>" % (m.group(1), re.sub(r"<Prop>[^<]*</Prop>", lambda _: "<Prop>%s</Prop>" % next(it), m.group(2)))
        out = out.replace(tr.group(2), re.sub(keyre, fix, body, flags=re.S), 1)
    return out


def status_visual(skin):
    """btn_twoline-status: the Blades' two-line list item with a third
    presenter, right-aligned at the end of line 2, showing the item's
    Data3 (a download's Downloading / Paused / Pending), keyed white and
    dark with the other two.  (Anchored right: the visual is 420 wide and
    the row 855, and a presenter anchored to both sides, as the Blades'
    are, would stretch past the row.)"""
    m = re.search(r"<XuiVisual>\n<Properties>\n <Id>btn_twoline-icon</Id>.*?</XuiVisual>", skin, flags=re.S)
    v = m.group(0).replace("<Id>btn_twoline-icon</Id>", "<Id>btn_twoline-status</Id>", 1)
    status = "\n".join(["<XuiTextPresenter>", "<Properties>", " <Id>txt_status</Id>", " <Width>395</Width>", " <Height>40</Height>",
                        " <Position>5,35,0</Position>", " <Anchor>4</Anchor>", " <TextColor>%s</TextColor>" % ITEM_REST,
                        " <DropShadowColor>%s</DropShadowColor>" % SHADOW, " <PointSize>18</PointSize>", " <TextStyle>16913</TextStyle>",
                        " <LineSpacingAdjust>-4</LineSpacingAdjust>", " <DataAssociation>3</DataAssociation>", "</Properties>",
                        "</XuiTextPresenter>"])
    k = v.find("</XuiTextPresenter>", v.find("<Id>txt_line4</Id>")) + len("</XuiTextPresenter>")
    v = v[:k] + "\n" + status + v[k:]
    # the colour track for the new presenter (the other two have theirs)
    track = re.search(r"<Timeline>\n <Id>txt_line4</Id>\n <TimelineProp>TextColor</TimelineProp>.*?</Timeline>\n", v, flags=re.S).group(0)
    k = v.rfind("</Timelines>")
    return v[:k] + track.replace("txt_line4", "txt_status") + v[k:]


def bar_visual():
    """prgbr_Download: a progress bar for a list row, a dark track with a
    white fill scaled from nothing (Normal) to the full width (EndNormal),
    the frame a XuiProgressBar's Value picks."""
    bx, by, bw, bh = ROW_BAR
    return "\n".join(["<XuiVisual>", "<Properties>", " <Id>prgbr_Download</Id>", " <Width>%g</Width>" % bw, " <Height>%g</Height>" % bh,
                      "</Properties>",
                      figure("track", 0, 0, bw, bh, solid("0x50000000")),
                      figure("fill", 0, 0, bw, bh, solid("0xffffffff")),
                      "<Timelines>",
                      frames([("Normal", 0, "Stop", ""), ("EndNormal", 100, "Stop", "")]),
                      track("fill", "Scale", [(0, 0, "0,1,1"), (100, 0, "1,1,1")]),
                      "</Timelines>", "</XuiVisual>"])


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


def list_capacity(block, visual=None, sections=None):
    """How many items the list's box holds (the rows of a dynamic page)."""
    vis, h, lines, icon = LIST_ITEMS[visual or prop(block, "Visual") or ""]
    pitch = h if h == 45 else h + 2
    return int((float(prop(block, "Height")) - len(sections or {}) * SECTION_H) // pitch)


def section_label(id, x, y, w, text):
    return "\n".join(["<XuiText>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % SECTION_H,
                      " <Position>%g,%g,0</Position>" % (x, y), " <TextColor>0xffb4c0c8</TextColor>", " <PointSize>16</PointSize>",
                      " <TextStyle>16400</TextStyle>", " <Text>%s</Text>" % text, "</Properties>", "</XuiText>"])


def list_items(block, items, visual=None, dynamic=False, sections=None, bar=False):
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
    if vis == "btn_3Line_Icon_prgbr":
        bar = (5, 76, 65, 21, "prgbr_Memory")
    pitch = h if h == 45 else h + 2
    # the list's box holds so many; the rest would scroll on the 360
    capacity = list_capacity(block, visual, sections)
    items = items[:capacity]
    if dynamic:
        # every row, blank, with an icon slot and the gauge: the script
        # fills them (SetText, SetData, SetImage, SetValue, SetVisible)
        items = [("", "", 0, "blank")] * capacity
    nav = {k: prop(block, k) for k in ("NavLeft", "NavRight") if prop(block, k)}
    out = []
    dy = 0
    for i, item in enumerate(items):
        if i in (sections or {}):
            out.append(section_label("%s_section_%d" % (id, i), x + 8, y + i * pitch + dy, w, sections[i]))
            dy += SECTION_H
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
        if bar is True:
            bx, by, bw, bh = ROW_BAR
            kids.append("\n".join(["<XuiProgressBar>", "<Properties>", " <Id>%s_%d_bar</Id>" % (id, i), " <Width>%g</Width>" % bw, " <Height>%g</Height>" % bh,
                                   " <Position>%g,%g,0</Position>" % (bx, by), " <Visual>prgbr_Download</Visual>",
                                   " <RangeMin>0</RangeMin>", " <RangeMax>1</RangeMax>", " <Value>%g</Value>" % frac, "</Properties>", "</XuiProgressBar>"]))
        elif bar:
            bx, by, bw, bh, bvis = bar
            kids.append("\n".join(["<XuiProgressBar>", "<Properties>", " <Id>%s_%d_bar</Id>" % (id, i), " <Width>%g</Width>" % bw, " <Height>%g</Height>" % bh,
                                   " <Position>%g,%g,0</Position>" % (bx, by), " <Visual>%s</Visual>" % bvis,
                                   " <RangeMin>0</RangeMin>", " <RangeMax>1</RangeMax>", " <Value>%g</Value>" % frac, "</Properties>", "</XuiProgressBar>"]))
        n.update(data)
        out.append(nav_button("%s_%d" % (id, i), x, y + i * pitch + dy, w, h, label, vis if vis != "XuiButton" else None, n, "\n".join(kids)))
    return out


# ---- the later dashboard's Marketplace pages --------------------------------

# The Blades' third-level layout: the list at 146, the meta pane 434 to its
# right (the pane's highlight1 sits at -434, over the focused row).
LIST_X, META_X = 146, 580


def label(id, x, y, w, h, visual, text_="", extra=""):
    return "\n".join(["<XuiLabel>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h,
                      " <Position>%g,%g,0</Position>" % (x, y), " <Visual>%s</Visual>" % visual, " <Text>%s</Text>" % text_]
                     + ([extra] if extra else []) + ["</Properties>", "</XuiLabel>"])


def legends(spec):
    out = [nav_button("legend_a", 573, 639, 420, 47, "Select", "legend_A"),
           nav_button("legend_b", 555, 611, 420, 47, "Back", "legend_B").replace("XuiNavButton", "XuiBackButton")]
    if spec.get("legend_y"):
        out.append(nav_button("legend_y", 233, 611, 420, 47, spec["legend_y"], "legend_Y"))
    return out


def header(spec, y=96):
    return label("labHeader", 156, y, 855, 47, "Label_Head", spec["header"])


def market_page(spec):
    """The Marketplace blade: btn_1line_icon rows on a 45 pitch (the
    blade's nav buttons, 32x32 icon slot empty until Frolick's icons
    exist), metaScene_MarketPlace beside them with the focused item's name
    and what it holds."""
    items = spec["items"]
    out = [header(spec)]
    for i, it in enumerate(items):
        nav = {}
        if i > 0:
            nav["NavUp"] = items[i - 1][0]
        if i < len(items) - 1:
            nav["NavDown"] = items[i + 1][0]
        out.append(nav_button(it[0], LIST_X, 153 + 45 * i, 420, 47, it[1], "btn_1line_icon", nav))
    meta = label("metaMarket", META_X, 153, 420, 442, "metaScene_MarketPlace")
    out.append(clip_group("metaClip", 0, 137, CANVAS_W, BODY_END - 137, [moved(meta, 0, -137)]))
    out.append(text("txt_MetaTitle", META_X + 18, 165, 384, 40, 22, 0x4010, ITEM_REST, presenter=False, value="", shadow=SHADOW))
    out.append(text("txt_Meta", META_X + 18, 213, 384, 360, 18, 0x4001, ITEM_REST, presenter=False, value="", shadow=SHADOW))
    return out + legends(spec)


def clip_group(id, x, y, w, h, children):
    """A group that clips what it holds to its box (the meta panes' edge
    strips run past the pane, the 360 clips them at the scene)."""
    return "\n".join(["<XuiGroup>", "<Properties>", " <Id>%s</Id>" % id, " <Width>%g</Width>" % w, " <Height>%g</Height>" % h,
                      " <Position>%g,%g,0</Position>" % (x, y), " <ClipChildren>true</ClipChildren>", "</Properties>"]
                     + children + ["</XuiGroup>"])


def moved(block, dx, dy):
    """A control block shifted by dx, dy (into a group's coordinates)."""
    x, y = [float(v) for v in prop(block, "Position").split(",")[:2]]
    return set_prop(block, "Position", "%g,%g,0" % (x + dx, y + dy))


# Spotlight's layout, from sokky's reference (1306x735) mapped into the
# page at 0.842 (the reference list's width onto Frolick's 420), keeping
# Frolick's sizes.  The live layout has no banner (sokky 2026-10-21): the
# tabs with equal room above and below their text, five rows.  FROLICK_SPOT_BANNER=1 builds
# the unused alternative with the banner's own band (714x80, the
# reference's 0.649 x 0.130 of the frame) and four rows under it.
SPOT_BANNER = os.environ.get("FROLICK_SPOT_BANNER", "0") == "1"
SPOT_TAB_GAP = 50       # between tab labels (the reference's 0.045)
SPOT_TAB_PT = 20
SPOT_RIGHT = 1004       # the body's right edge


def tab_width(name):
    """The label's width in DejaVu Sans (font.ttf) at SPOT_TAB_PT: the
    engine sizes the em at 4/3 of the point size."""
    from PIL import ImageFont
    f = ImageFont.truetype(os.environ.get("FONT", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"), round(SPOT_TAB_PT * 4 / 3))
    return f.getlength(name)


def spotlight_page(spec):
    """Spotlight: the banner band (optional), the category tabs, the
    btn_twoline_spot rows with the List_twoline counter under them,
    metaScene_spot beside them (its Pane_txt the item's name, Pane_txt1
    its description).  The tab row is laid out per selection (one strip
    per category, the script shows the current one), word widths and
    gaps as the reference spaces them."""
    out = [header(spec)]
    if SPOT_BANNER:
        # the featured banner (the 360's XBOX LIVE art, 065
        # DefaultFeaturedTitleBanner): an empty slot until Frolick has art
        out.append("\n".join(["<XuiImage>", "<Properties>", " <Id>imgBanner</Id>", " <Width>714</Width>", " <Height>80</Height>",
                              " <Position>218,146,0</Position>", " <ImagePath></ImagePath>", "</Properties>", "</XuiImage>"]))
        tab_y, rows = 230, 4
    else:
        tab_y, rows = 151, 5     # as much room above the tab text as below it (sokky)
    list_y = tab_y + 40
    tabs = SPOTLIGHT
    for cur in range(len(tabs)):
        texts = []
        x = LIST_X + 11
        # the previous tab ends a gap before the current one, cut by the
        # screen edge like the reference's
        prev = tabs[(cur - 1) % len(tabs)]
        texts.append(text("tab_%d_prev" % cur, -400, 0, x - SPOT_TAB_GAP + 400, 36, SPOT_TAB_PT, 0x4010 | 0x200, "0x40ffffff",
                          presenter=False, value=prev, shadow="0"))
        k = 0
        while x < SPOT_RIGHT - 40:
            name = tabs[(cur + k) % len(tabs)]
            w = tab_width(name)
            last = x + w + SPOT_TAB_GAP >= SPOT_RIGHT - 40
            colour = "0xffffffff" if k == 0 else ("0x40ffffff" if last else "0xa0ffffff")
            texts.append(text("tab_%d_%d" % (cur, k), x - LIST_X, 0, w + 8, 36, SPOT_TAB_PT, 0x4010, colour, presenter=False,
                              value=name, shadow=SHADOW if k == 0 else "0"))
            x += w + SPOT_TAB_GAP
            k += 1
        texts = [texts[0]] + [moved(t, LIST_X, 0) for t in texts[1:]]
        out.append(clip_group("tabs_%d" % cur, 0, tab_y, CANVAS_W, 36, texts).replace(
            " <ClipChildren>true</ClipChildren>", " <ClipChildren>true</ClipChildren>\n <Show>%s</Show>" % ("true" if cur == 0 else "false")))
    for i in range(rows):
        nav = {}
        if i > 0:
            nav["NavUp"] = "lstSpot_%d" % (i - 1)
        if i < rows - 1:
            nav["NavDown"] = "lstSpot_%d" % (i + 1)
        out.append(nav_button("lstSpot_%d" % i, LIST_X, list_y + 74 * i, 420, 72, "", "btn_twoline_spot", nav))
    out.append(text("labTotal", LIST_X + 13, list_y + 74 * rows, 112, 26, 16, 0x4010, ITEM_REST, presenter=False, value="", shadow=SHADOW))
    # the pane and its edge strips, clipped to the body (the strips run on
    # past the pane's foot)
    # the details box (the later skin's Metapane) under the pane's text,
    # with the reference's darker title band and the image band below it
    ph = 74 * rows - 2
    # The bands go under the box so its border draws over their ends:
    # they span the box's inside edge to edge (Metapane's edge strips sit
    # 2..6 in from each side).
    bx, bw = META_X + SPOT_BOX_INSET + SPOT_BAND_X, 420 - SPOT_BOX_INSET - 2 * SPOT_BAND_X
    title_h = round(ph * SPOT_TITLE_BAND)
    box = [figure("metaTitleBand", bx, list_y + SPOT_BAND_Y, bw, title_h - SPOT_BAND_Y, solid(SPOT_BAND_FILL)),
           figure("metaImageLine", bx, list_y + 2 * title_h, bw, 2, solid(SPOT_BAND_FILL)),
           label("metaBox", META_X + SPOT_BOX_INSET, list_y, 420 - SPOT_BOX_INSET, ph + 10, "Metapane")]   # (its foot sits 10 inside)
    meta = label("metaSpot", META_X, list_y, 420, ph, "metaScene_spot", "", " <Data1></Data1>")
    # the item's tile from the Archive in the image band, and its size and
    # where it comes from at the box's foot
    tile_h = title_h - 12
    tile = "\n".join(["<XuiImage>", "<Properties>", " <Id>imgSpotTile</Id>", " <Width>%g</Width>" % (tile_h * 4 / 3),
                      " <Height>%g</Height>" % tile_h, " <Position>%g,%g,0</Position>" % (bx + 30, list_y + title_h + 6),
                      " <ImagePath></ImagePath>", "</Properties>", "</XuiImage>"])
    credit = text("txt_SpotCredit", bx + 30, list_y + ph - 34, bw - 44, 26, 14, 0x4010 | STYLE_ELLIPSIS, ITEM_REST, presenter=False,
                  value="", shadow="0")
    # the Streaming Download mark (play as you download): a lime badge in
    # the image band beside the tile, or a line over the credit; the script
    # shows whichever FROLICK_STREAM_MARK picked, on items that stream
    bdg_x, bdg_y = bx + 30 + tile_h * 4 / 3 + 16, list_y + title_h + 10
    # (dark text: 0xff101010, as the body's 0xff0f0f0f turns white)
    stream = [figure("imgSpotBadge", bdg_x, bdg_y, 236, 34, solid("0xffc4ff46")),
              text("txt_SpotBadge", bdg_x, bdg_y, 236, 34, 12, 0x10 | 0x400 | 0x1000, "0xff101010", presenter=False,
                   value="STREAMING DOWNLOAD", shadow="0"),
              text("txt_SpotStream", bx + 30, list_y + ph - 62, bw - 44, 26, 14, 0x4010 | STYLE_ELLIPSIS, "0xffc4ff46",
                   presenter=False, value="", shadow="0")]
    out.append(clip_group("metaClip", 0, list_y - 16, CANVAS_W, BODY_END - (list_y - 16),
                          [moved(b, 0, -(list_y - 16)) for b in box + [tile, meta, credit] + stream]))
    out.append(text("txt_EmptyList", LIST_X + SPOT_TEXT_X - 60, list_y + 14, 380, 120, 18, 0x4000, ITEM_REST, presenter=False,
                    value="Nothing here yet.", shadow=SHADOW))
    return out + legends(spec)


BUILDERS = {"market": market_page, "spotlight": spotlight_page}


def page(file, spec):
    pad, first = spec["pad"], spec["first"]
    if spec["scene"] in BUILDERS:
        kept = BUILDERS[spec["scene"]](spec)
    elif spec["scene"] is None:
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
                kept += list_items(b, spec.get("lists", {}).get(id, []), spec.get("visual"), bool(spec.get("dynamic")),
                                   spec.get("sections"), spec.get("bar", False))
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
            # a read-only edit box (System Info) is a label of the script's text
            if b.startswith("<XuiEdit>"):
                b = b.replace("<XuiEdit>", "<XuiLabel>").replace("</XuiEdit>", "</XuiLabel>")
                b = re.sub(r"\s*<(Visual|ReadOnly|Multiline|SmoothScroll)>.*?</\1>", "", b)
            # clone: {source id (a trailing space makes a second copy): (new id, position)}
            for src, (nid, pos) in spec.get("clone", {}).items():
                if src.strip() == id:
                    kept.append(set_prop(set_prop(set_prop(b, "Id", nid), "Position", pos), "Text", ""))
            if id == "txt_Meta":
                b = set_prop(b, "Text", "")
            kept.append(b)

    # what a clone is called, and navigation the page changes ("" none)
    out = []
    clones = [nid for nid, _ in spec.get("clone", {}).values()]
    for b in kept:
        id = prop(b, "Id")
        if id in clones and id in spec.get("labels", {}):
            b = set_prop(b, "Text", spec["labels"][id])
        for k, v in spec.get("nav", {}).get(id, {}).items():
            b = set_prop(b, k, v) if v else re.sub(r"\s*<%s>.*?</%s>" % (k, k), "", b)
        out.append(b)
    kept = out

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
                        figure("body", 0, BODY_Y, CANVAS_W, BODY_END - BODY_Y, gradient([(0, PANEL_TOP), (1, PANEL_BOTTOM)]))
                        if spec.get("body", True) else "",
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
    if spec["scene"] in BUILDERS:
        return spec["list"], spec["rows"]
    src = open(os.path.join(BLADES, spec["scene"])).read()
    for b in element_blocks(src)[1]:
        if b.startswith("<XuiList>") or b.startswith("<XuiCommonList>"):
            return prop(b, "Id"), list_capacity(b, spec.get("visual"), spec.get("sections"))
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
            if key == "press" and "items" in spec:
                table.update((it[0], it[2]) for it in spec["items"])
            if key == "meta" and "items" in spec:
                table.update((it[0], it[3]) for it in spec["items"] if len(it) > 3)
            if not table:
                continue
            out += ["    if (page == %s)" % q(file), "    {"]
            for id, v in table.items():
                out.append("        if (id == %s)" % q(id))
                out.append("            return %s;" % q(v))
            out.append("    }")
        out += ['    return "";', "}", ""]
    # the meta pane whose highlight follows the focused row
    out += ["function PageMetaPane(page)", "{"]
    for file, spec in PAGES:
        if spec["scene"] in BUILDERS:
            out += ["    if (page == %s)" % q(file), "        return %s;" % q({"market": "metaMarket", "spotlight": "metaSpot"}[spec["scene"]])]
    out += ['    return "";', "}", ""]
    # a built page's rows, in order (the market list's row of an id)
    out += ["function PageItemRow(page, id)", "{"]
    for file, spec in PAGES:
        if spec["scene"] == "market":
            out += ["    if (page == %s)" % q(file), "    {"]
            for i, it in enumerate(spec["items"]):
                out += ["        if (id == %s)" % q(it[0]), "            return %d;" % i]
            out.append("    }")
    out += ["    return 0 - 1;", "}", ""]
    out += ["function PageItemName(page, id)", "{"]
    for file, spec in PAGES:
        if spec["scene"] == "market":
            out += ["    if (page == %s)" % q(file), "    {"]
            for it in spec["items"]:
                out += ["        if (id == %s)" % q(it[0]), "            return %s;" % q(it[1])]
            out.append("    }")
    out += ['    return "";', "}", ""]
    # Spotlight's tabs and items
    out += ["function SpotTabs()", "{", "    return %d;" % len(SPOTLIGHT), "}", ""]
    out += ["function SpotTab(t)", "{"]
    for t, name in enumerate(SPOTLIGHT):
        out += ["    if (t == %d)" % t, "        return %s;" % q(name)]
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
