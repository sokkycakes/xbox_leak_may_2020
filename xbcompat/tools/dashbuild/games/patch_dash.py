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
print("patched", dash)
