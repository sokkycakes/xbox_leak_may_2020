#!/usr/bin/env python3
"""Add a Games area to a copy of the dashboard's private/ui/dash folder.

    patch_dash.py DASHDIR

- default.xap: a GAMES item above MEMORY on the main menu (index -1, so the
  existing 0..2 indices used elsewhere keep their meaning), opening games.xap.
- MainMenu5/default.xap: a fourth pod, a copy of the top (MEMORY) pod turned
  one step further around the ring and labelled GAMES.
- Games2/: the Settings home scene with a GAMES heading.  It was modelled
  as the Games screen (S_Home_text_games rows, S_Home_GameModule pods).
- games.xap: the area script (from this folder).
- The screen saver from Theseus's UIX-style dashboard: after five idle
  minutes on the main menu the pods and button hint fade out, the ring
  drifts to one of three camera angles and the cell wall spins faster
  (MainMenu5's scene is wrapped in theMainMenuLevel for it).  Elsewhere the
  screen dims, as Microsoft's dashboard did.
"""
import math, os, re, shutil, sys

here = os.path.dirname(os.path.abspath(__file__))
dash = sys.argv[1]


def rd(p):
    return open(os.path.join(dash, p), encoding="latin-1").read()


def wr(p, s):
    open(os.path.join(dash, p), "w", encoding="latin-1", newline="").write(s)


def sub1(s, old, new):
    if s.count(old) != 1:
        sys.exit("patch_dash: expected one match of %r" % old[:60])
    return s.replace(old, new)


def block(s, start):
    """The text of the node starting at `start`, through its closing brace."""
    i = s.index("{", start)
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "{":
            depth += 1
        elif s[j] == "}":
            depth -= 1
            if depth == 0:
                return s[start:j + 1]
    sys.exit("patch_dash: unbalanced braces")


# ---- main menu scene: a fourth pod ---------------------------------------
mm = rd("MainMenu5/default.xap")
start = mm.index("DEF Main_memory_ringpin_2 Transform")
pod = block(mm, start)
names = set(re.findall(r"DEF (\S+)", pod))
for n in sorted(names, key=len, reverse=True):
    pod = re.sub(r"\b(DEF|USE) " + re.escape(n) + r"(?=\s)", r"\1 " + n + "_g", pod)
pod = sub1(pod, 'text "MEMORY"', 'text "GAMES"')
# Same pod, one step (0.45 rad, the spacing of the existing three) further
# round the ring: only its position turns, so its label stays level.
old_t = "translation 56.830002 32.290001 101.900002"
x, y = 56.830002, 32.290001
cs, sn = math.cos(0.45), math.sin(0.45)
pod = pod[:pod.rindex(old_t)] + "translation %f %f 101.900002" % (x * cs - y * sn, x * sn + y * cs) + \
    pod[pod.rindex(old_t) + len(old_t):]
games_pod = pod + "\n\t\t\t\t"
mm = mm[:start] + games_pod + mm[start:]
# The whole scene in one transform, so the screen saver can move it.
first = mm.index("DEF ring Transform")
mm = mm[:first] + "DEF theMainMenuLevel Transform\n{\n    children\n    [\n" + mm[first:].rstrip() + "\n    ]\n}\n"
wr("MainMenu5/default.xap", mm)

# ---- main menu logic ------------------------------------------------------
d = rd("default.xap")
d = sub1(d, '''/*
DEF theGamesInline Inline
{
    visible false
    url "Games.xap"
''', '''DEF theGamesInline Inline
{
    visible false
    url "games.xap"
''')
d = sub1(d, '''        theGamesInline.visible = true;
}
*/''', '''        theGamesInline.visible = true;
}''')

i = d.index("function UpdateMainMenu()")
old = block(d, i)
d = d.replace(old, '''function UpdateMainMenu()
{
    var c = theMainMenu.children[0].children[0];
    var a = (nCurMainMenuItem - 1) * 0.45;

    c.game_select_pod_inner02_g.visible = (nCurMainMenuItem == -1);
    c.game_select_pod_inner02.visible = (nCurMainMenuItem == 0);
    c.game_select_pod_inner.visible = (nCurMainMenuItem == 1);
    c.game_select_pod_inner03.visible = (nCurMainMenuItem == 2);

    c.theMenuItems.SetRotation(0, 0, 1, a);

    c.theMusicItem_g.SetRotation(0, 1, 0, a);
    c.theMusicItem.SetRotation(0, 1, 0, a);
    c.theMemoryItem.SetRotation(0, 1, 0, a);
    c.theSettingsItem.SetRotation(0, 1, 0, a);

    c.MemoryPanelMaterial_g.name = "FlatSurfaces2sided";
    c.MemoryPanelMaterial.name = "FlatSurfaces2sided";
    c.MusicPanelMaterial.name = "FlatSurfaces2sided";
    c.SettingsPanelMaterial.name = "FlatSurfaces2sided";

    c.MemoryTextMaterial_g.name = "NavType";
    c.MemoryTextMaterial.name = "NavType";
    c.MusicTextMaterial.name = "NavType";
    c.SettingsTextMaterial.name = "NavType";

    if (nCurMainMenuItem == -1)
    {
        c.MemoryPanelMaterial_g.name = "GameHilite";
        c.MemoryTextMaterial_g.name = "HilightedType";
    }
    else if (nCurMainMenuItem == 0)
    {
        c.MemoryPanelMaterial.name = "GameHilite";
        c.MemoryTextMaterial.name = "HilightedType";
    }
    else if (nCurMainMenuItem == 1)
    {
        c.MusicPanelMaterial.name = "GameHilite";
        c.MusicTextMaterial.name = "HilightedType";
    }
    else
    {
        c.SettingsPanelMaterial.name = "GameHilite";
        c.SettingsTextMaterial.name = "HilightedType";
    }
}''')

d = sub1(d, '''            if (nCurMainMenuItem == 0)
            {
                theGamesMenuIn.Play();
                GoToMemory();''', '''            if (nCurMainMenuItem == -1)
            {
                theGamesMenuIn.Play();
                GoToGames();
            }
            else if (nCurMainMenuItem == 0)
            {
                theGamesMenuIn.Play();
                GoToMemory();''')
d = sub1(d, "            if (nCurMainMenuItem > 0)", "            if (nCurMainMenuItem > -1)")
# Launching a game goes through theLauncherLevel, as a disc launch does: the
# Xbox logo goes up, and after a moment the dashboard persists it and
# reboots into the game, so the logo stays on screen until the game draws.
d = sub1(d, "var g_bAboutToReboot;", "var g_bAboutToReboot;\nvar g_bLaunchGame;")
d = sub1(d, "    g_bAboutToReboot = false;", "    g_bAboutToReboot = false;\n    g_bLaunchGame = false;")
d = sub1(d, '''            else
                theDiscDrive.LaunchDisc();  // This will boot to title/dashboard or DVD player''', '''            else if (g_bLaunchGame)
                theGamesInline.children[0].LaunchPendingGame();
            else
                theDiscDrive.LaunchDisc();  // This will boot to title/dashboard or DVD player''')
d = sub1(d, "DEF theLauncherLevel Level", '''function LaunchGameWithLogo()
{
    g_bLaunchGame = true;
    theLauncherLevel.GoTo();
}

DEF theLauncherLevel Level''')

# ---- screen saver (Theseus's UIX-style one) -------------------------------
d = sub1(d, '''    function OnStart()
    {
        theScreen.brightness = 0.1;
    }

    function OnEnd()
    {
        theScreen.brightness = 1;
    }''', '''    function OnStart()
    {
        theScreen.brightness = 0.1;
        StartScreenSaverView();
    }

    function OnEnd()
    {
        theScreen.brightness = 1;
        StopScreenSaverView();
    }''')
d = sub1(d, "    path Viewpoint\n    {\n        fieldOfView 1.300000\n        orientation -0.177400 -0.983500",
         "    path DEF theMainMenuViewpoint Viewpoint\n    {\n        fieldOfView 1.300000\n        orientation -0.177400 -0.983500")
d = sub1(d, "            Waver\n            {\n                rpm 0.75",
         "            DEF theMainMenuWaver Waver\n            {\n                rpm 0.75")
d = sub1(d, "var g_bLaunchGame;", "var g_bLaunchGame;\nvar g_bScreenSaverView;\nvar g_nScreenSaverStir;")
d = sub1(d, "    g_bLaunchGame = false;", "    g_bLaunchGame = false;\n    g_bScreenSaverView = false;\n    g_nScreenSaverStir = 0;")
d = sub1(d, """    var a = (nCurMainMenuItem - 1) * 0.45;
""", """    var a = (nCurMainMenuItem - 1) * 0.45;
    g_nScreenSaverStir = g_nScreenSaverStir + nCurMainMenuItem + 2;
""")
d = d.rstrip() + '''

////////////////////////////////////////////////////////////////////////////
// Screen saver view (from Theseus's UIX-style dashboard): on the main menu
// the pods and the button hint fade out, the ring drifts slowly to one of
// three angles under a wider, lower camera, and the cell wall spins faster.

function StartScreenSaverView()
{
    if (theMainMenuViewpoint.isBound)
    {
        var c = theMainMenu.children[0].children[0];
        // Math.random starts the same way every boot: moving round the
        // menu stirs it, so the first screen saver isn't always the same.
        var x = g_nScreenSaverStir + Math.floor(Math.random() * 3);
        x = x - Math.floor(x / 3) * 3;
        c.theMenuItems.SetAlpha(0);
        c.select.SetAlpha(0);
        c.theMainMenuLevel.fade = 100;
        if (x == 0)
        {
            c.theMainMenuLevel.SetTranslation(30, -104, 4);
        }
        else if (x == 1)
        {
            c.theMainMenuLevel.SetTranslation(74, -46, -14);
            c.theMainMenuLevel.SetRotation(-0.0006, 0, 0.00005, 0.314159);
        }
        else
        {
            c.theMainMenuLevel.SetTranslation(-24, -36, -262);
            c.theMainMenuLevel.SetRotation(-0.001, 0.0003, 0.0003, 0.314159);
        }
        theMainMenuWaver.rpm = 2.25;
        theScreen.brightness = 0.8;
        theMainMenuAlternateViewpoint.isBound = true;
        g_bScreenSaverView = true;
    }
}

function StopScreenSaverView()
{
    if (g_bScreenSaverView)
    {
        var c = theMainMenu.children[0].children[0];
        g_bScreenSaverView = false;
        c.theMainMenuLevel.fade = 0.25;
        c.theMainMenuLevel.SetRotation(0, 0, 0, 0);
        c.theMainMenuLevel.SetTranslation(0, 0, 0);
        c.theMenuItems.SetAlpha(1);
        c.select.SetAlpha(1);
        theMainMenuWaver.rpm = 0.75;
        if (theMainMenuAlternateViewpoint.isBound)
            theMainMenuViewpoint.isBound = true;
    }
}

DEF theMainMenuAlternateViewpoint Viewpoint
{
    fieldOfView 1.755000
    orientation -0.177400 -1.983500 -0.036250 -0.045440
    position -15.180000 -112.299999 174.300003
    jump false
}
'''
wr("default.xap", d)

# ---- the Games screen -----------------------------------------------------
if os.path.isdir(os.path.join(dash, "Games2")):
    shutil.rmtree(os.path.join(dash, "Games2"))
os.makedirs(os.path.join(dash, "Games2"))
g = rd("Settings3/default.xap")
g = sub1(g, 'geometry Text { font "Heading" text "SETTINGS" }', 'geometry Text { font "Heading" text "GAMES" }')
g = sub1(g, 'text "Testing123"', 'text ""')

# The game pod from mainmenu5.xip (the "music" pod: game_pod, game_podshell_1-3,
# game_podsupport_1-4, gamepod_backing...; not its label or highlight glow) in place of the settings orb.  The
# meshes stay in mainmenu5.xip, which is never unloaded, and are reached with
# ../MainMenu5/ urls.  The pod keeps the transform chain it has on the main
# menu (ring > Main_memory_ringpin_3 > game_arm1 > game_arm_2 > music, with the
# ring at rest), is moved so its centre (10.26 -16.74 52.97 in the main menu)
# sits where the settings orb is, and is turned by the rotation between the two
# cameras so it faces the Games camera the way it faces the main menu camera.
POD_PARTS = ["game_podshell_2", "game_podsupport_1", "game_podsocket_inner", "game_pod",
             "game_arm01", "game_podshell_3", "game_podsupport_4", "game_podshell_1",
             "game_nozzle", "game_podsupport_3", "game_podsupport_2", "gamepod_backing"]
mm = rd("MainMenu5/default.xap")
music = block(mm, mm.index("DEF music Transform"))
parts = []
for n in POD_PARTS:
    t = block(music, music.index("DEF %s Transform" % n))
    parts.append(re.sub(r'url "([^"]+\.xm)"', r'url "../MainMenu5/\1"', t))
chain = [
    "translation -57.98 -13.69 -84.760002 scale 1.08 1.08 1.08",
    "translation 62.779999 4.492 102.0 rotation 0.9975 -0.04792 0.05259 -1.573 "
    "scale 6.057 6.927 6.057 scaleOrientation 0.008927 0.9983 -0.05752 -0.08113",
    "translation -0.3521 -0.7823 0.518 rotation 0.1276 -0.01139 -0.9918 -1.591 "
    "scale 0.2251 0.2575 0.2575 scaleOrientation -0.3804 0.9218 0.07438 -0.1515",
    "translation -6.689 0.2368 -4.083 rotation 0.001397 -0.01584 0.9999 -3.163 "
    "scale 1.296 1.296 1.296 scaleOrientation -0.04207 -0.9378 0.3446 -0.6445",
    "translation 5.673 15.93 -1.773 rotation -0.6021 -0.5996 -0.5273 -2.191 "
    "scale 0.4581 0.4581 0.4581 scaleOrientation 0.04555 0.9968 -0.06495 -0.07254",
]
pod = "\n".join(parts)
for f in reversed(chain):
    pod = "Transform { %s children [\n%s\n] }" % (f, pod)
pod = ("DEF GamesPod Transform\n{\n"
       "    translation 107.33 0.508 -181.2\n"
       "    rotation -0.30226 0.95322 0.00176 0.07906\n"
       "    scale 1.2 1.2 1.2\n"
       "    children [ Transform { translation -10.26 16.744 -52.969 children [\n%s\n] } ]\n}\n\n" % pod)
g = sub1(g, "DEF orb_and_arm Transform", pod + "DEF orb_and_arm Transform")
wr("Games2/default.xap", g)
shutil.copy(os.path.join(here, "games.xap"), os.path.join(dash, "games.xap"))

# ---- Memory: installed games and downloads --------------------------------
# The hard disk's titles also list their downloads and the games installed
# from the Games screen (TitleCollection.cpp, from memory.patch); deleting
# one of those asks about a game, not a saved game.
m = rd("memory3.xap")
m = sub1(m, '''        else
        {
            msg = "ConfirmDeleteSave";
        }''', '''        else if (theFilesMenu.children[0].children[0].theSavedGameGrid.IsGameSelected())
        {
            msg = "ConfirmDeleteGame";
        }
        else
        {
            msg = "ConfirmDeleteSave";
        }''')
# A title's count says how many games and how many saves it holds.  (Its
# variables are declared at the top: one declared in a block is not seen
# after it.)
m = sub1(m, '''            var nCount = c.theSavedGameGrid.GetSavedGameCount(nTitle);
            var strType;
''', '''            var nCount = c.theSavedGameGrid.GetSavedGameCount(nTitle);
            var strType;
            var nGames = 0;
            var strCount;
            var strGames;
''')
m = sub1(m, '''            else
            {
                if (nCount == 1)
                    strType = "save";
                else
                    strType = "saves";
            }
            c.MetaLine2.text = nCount + " " + theTranslator.Translate(strType);
            c.MetaLine2a.text = nCount + " " + theTranslator.Translate(strType);''', '''            else
            {
                nGames = c.theSavedGameGrid.GetGameCount(nTitle);
                nCount = nCount - nGames;
                if (nCount == 1)
                    strType = "save";
                else
                    strType = "saves";
            }
            strCount = nCount + " " + theTranslator.Translate(strType);
            if (nGames > 0)
            {
                if (nGames == 1)
                    strGames = nGames + " " + theTranslator.Translate("game");
                else
                    strGames = nGames + " " + theTranslator.Translate("games");
                if (nCount > 0)
                    strCount = strGames + ", " + strCount;
                else
                    strCount = strGames;
            }
            c.MetaLine2.text = strCount;
            c.MetaLine2a.text = strCount;''')
wr("memory3.xap", m)
for lang in ["english", "japanese", "german", "french", "spanish", "italian"]:
    p = os.path.join(dash, lang + ".txt")
    t = open(p, "rb").read().decode("utf-16")
    line = next(l for l in t.split("\r\n") if l.startswith("ConfirmDeleteSave="))
    if "ConfirmDeleteGame=" not in t:
        t = t.replace(line, line + '\r\nConfirmDeleteGame="Are you sure you want to permanently delete this game?"', 1)
    open(p, "wb").write(t.encode("utf-16"))
print("patched", dash)
