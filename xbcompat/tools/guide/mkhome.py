#!/usr/bin/env python3
"""Adds the Guide's home pieces to a guide asset directory (mkguide.py runs
it when given the Blades packages):

  mkhome.py BLADES_DIR OUT_DIR FROLICK_DIR

BLADES_DIR holds the Xbox 360 Blades dashboard's converted packages
(/mnt/project-files/xui360/blades-converted); FROLICK_DIR is a checkout's
xbcompat/tools/frolickdash on the Frolick branch
(claude/frolick-xap-dash-4fvafa), whose tools/genpage.py makes Frolick's
pages.  Writes

  GuideTabs.xui      the Blades two-tab strip (Marketplace's Game Downloads
                     page, dash_dashmark.xzp#1401_GameCatalogTabScene.xur):
                     its tab pieces, headers and 1To2 / 2To1 timelines, in a
                     group clipped to the page's width.  The page content,
                     title and back button are dropped; the headers read
                     Game and Settings.
  GuideTabsSkin.xui  the Blades skin visuals the strip draws (all figures),
                     byte for byte from dash_dashuisk.xzp#skin.xur.
  GuideGame.xui      the Game tab's fuller view: Frolick's Game Summary
                     page (genpage.py's page_game, the Blades'
                     401_GameDetails in Frolick's dress) as it is, and
  GuideRecent.xui, GuideDownloads.xui, GuideSaves.xui, GuideConfirm.xui
                     Frolick's Recent Games, Active Downloads, Saved Games
                     and Yes / No pages, for Quick Launch, Downloads and
                     Game Storage
  GuideGameSkin.xui  its skin (page_skin.xui) with the button images it
                     names.
"""
import os, re, shutil, struct, subprocess, sys, tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
XUR2XUI = os.path.join(HERE, '..', 'frolickdash', 'tools', 'xur2xui.py')
TAB_VISUALS = ['tab_inactive_shine_2', 'tab_inactive_B', 'tab_active_glow_1', 'tab_active_C',
               'tab_active_B', 'Label_TabTitle']
# The strip's band in the scene (the page panel spans x 144-999), clipped so
# the bars that run off the panel's left edge stop at it.
CLIP_X, CLIP_Y, CLIP_W, CLIP_H = 144, 84, 855, 132


def conv(xzp, scene, tmp):
    out = os.path.join(tmp, scene.replace('.xur', '.xui'))
    subprocess.run([sys.executable, XUR2XUI, xzp, scene, out], check=True, stdout=subprocess.DEVNULL)
    return ET.parse(out).getroot()


def pid(e):
    p = e.find('Properties')
    return (p.findtext('Id') if p is not None else None) or ''


def prop(parent, key, text):
    p = parent.find('Properties')
    if p is None:
        p = ET.SubElement(parent, 'Properties')
    e = p.find(key)
    if e is None:
        e = ET.SubElement(p, key)
    e.text = text


def el(parent, cls, **props):
    e = ET.SubElement(parent, cls)
    p = ET.SubElement(e, 'Properties')
    for k, v in props.items():
        ET.SubElement(p, k).text = str(v)
    return e


def tabs(blades, out, tmp):
    src = conv(os.path.join(blades, 'dash_dashmark.xzp'), '1401_GameCatalogTabScene.xur', tmp)
    ts = src.find('XuiTabScene')
    drop = {'figBackground', 'btnB', 'Tab1', 'Tab2', 'labSceneTitle', 'XuiSoundXAudio1'}
    root = ET.Element('XuiCanvas')
    p = ET.SubElement(root, 'Properties')
    ET.SubElement(p, 'Width').text = '1120'
    ET.SubElement(p, 'Height').text = '770'
    clip = el(root, 'XuiGroup', Id='tabClip', Width=CLIP_W, Height=CLIP_H,
              Position='%d,%d,0' % (CLIP_X, CLIP_Y), ClipChildren='true')
    strip = el(clip, 'XuiGroup', Id='tabStrip', Width=1120, Height=770, Position='%d,%d,0' % (-CLIP_X, -CLIP_Y))
    for c in list(ts):
        if c.tag == 'Properties' or pid(c) in drop:
            continue
        if c.tag == 'Timelines':
            for tl in list(c):
                if tl.tag == 'Timeline' and tl.findtext('Id') in drop:
                    c.remove(tl)
        elif c.tag == 'XuiFigure' and pid(c).startswith(('-', '*')):
            continue  # the author's hidden divider markers
        strip.append(c)
    names = {'tab_1_header': 'Game', 'tab_2_header': 'Settings'}
    for c in strip:
        if pid(c) in names:
            prop(c, 'Text', names[pid(c)])
    ET.ElementTree(root).write(os.path.join(out, 'GuideTabs.xui'))

    skin = conv(os.path.join(blades, 'dash_dashuisk.xzp'), 'skin.xur', tmp)
    found = {}

    def walk(e):
        for c in e:
            if c.tag in ('Properties', 'Timelines'):
                continue
            if c.tag == 'XuiVisual' and pid(c) in TAB_VISUALS:
                found[pid(c)] = c
            else:
                walk(c)
    walk(skin)
    missing = [v for v in TAB_VISUALS if v not in found]
    if missing:
        sys.exit('Blades skin lacks ' + ', '.join(missing))
    sroot = ET.Element('XuiCanvas')
    sc = el(sroot, 'XuiScene', Id='BladesTabSkin')
    for v in TAB_VISUALS:
        sc.append(found[v])
    # sokky: the titles centred in their tabs (the Blades sets them left)
    for tp in found['Label_TabTitle'].iter('XuiTextPresenter'):
        st = tp.find('Properties/TextStyle')
        st.text = str(int(st.text or 0) | 0x400)
    ET.ElementTree(sroot).write(os.path.join(out, 'GuideTabsSkin.xui'))


def game(frolick, out, tmp):
    dash = os.path.join(tmp, 'dash')
    os.makedirs(os.path.join(dash, 'pads'), exist_ok=True)
    subprocess.run([sys.executable, os.path.join(frolick, 'tools', 'genpage.py'), dash], check=True,
                   stdout=subprocess.DEVNULL)
    pads = os.path.join(dash, 'pads')
    # the Game tab's fuller view, and the pages its rows open: Quick Launch
    # (Recent Games), Downloads (Active Downloads), Game Storage (Saved
    # Games) and its Yes / No
    pages = {'page_game.xui': 'GuideGame.xui', 'page_recent.xui': 'GuideRecent.xui',
             'page_downloads.xui': 'GuideDownloads.xui', 'page_memory_saves.xui': 'GuideSaves.xui',
             'page_confirm.xui': 'GuideConfirm.xui', 'page_skin.xui': 'GuideGameSkin.xui'}
    texts = {k: open(os.path.join(pads, k)).read() for k in pages}
    # their images, under names of their own (the Guide has GFWL's A-Button...),
    # with the row icons the Guide fills in from code
    from PIL import Image
    names = set(['ico_64x_games.png', 'ico_64x_GameDownloads.png', 'ico_64x_Themes.png'])
    for t in texts.values():
        names |= set(re.findall(r'<(?:TextureFileName|ImagePath)>([^<]+\.png)<', t))
    for name in sorted(names):
        png = os.path.join(frolick, 'art', 'blades', name)
        if not os.path.exists(png):
            continue
        im = Image.open(png).convert('RGBA')
        with open(os.path.join(out, 'frolick_' + name[:-4] + '.xtx'), 'wb') as f:
            f.write(b'XTX1' + struct.pack('<II', *im.size) + im.tobytes('raw', 'BGRA'))
        for k in texts:
            texts[k] = texts[k].replace('>%s<' % name, '>frolick_%s<' % name)
    for k, v in pages.items():
        open(os.path.join(out, v), 'w').write(texts[k])


def main():
    blades, out, frolick = sys.argv[1:4]
    tmp = tempfile.mkdtemp()
    tabs(blades, out, tmp)
    game(frolick, out, tmp)
    print('%s: GuideTabs.xui, GuideTabsSkin.xui, GuideGame.xui, GuideGameSkin.xui and the list pages' % out)


if __name__ == '__main__':
    main()
