#!/usr/bin/env python3
"""Add a Games area to a copy of the dashboard's private/ui/dash folder.

    patch_dash.py DASHDIR

- default.xap: a GAMES item above MEMORY on the main menu (index -1, so the
  existing 0..2 indices used elsewhere keep their meaning), opening games.xap.
- MainMenu5/default.xap: a fourth pod, a copy of the top (MEMORY) pod turned
  one step further around the ring and labelled GAMES.
- Games2/: a copy of the Music2 list scene with a GAMES heading.
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
shutil.copytree(os.path.join(dash, "Music2"), os.path.join(dash, "Games2"))
g = rd("Games2/default.xap")
g = sub1(g, 'geometry Text { font "Heading" text "MUSIC COLLECTION" }', 'geometry Text { font "Heading" text "GAMES" }')
wr("Games2/default.xap", g)
shutil.copy(os.path.join(here, "games.xap"), os.path.join(dash, "games.xap"))
print("patched", dash)
