#!/usr/bin/env python3
"""Adds the Guide's home pieces to a guide asset directory (mkguide.py runs
it when given the Blades packages):

  mkhome.py BLADES_DIR OUT_DIR

BLADES_DIR holds the Xbox 360 Blades dashboard's converted packages
(/mnt/project-files/xui360/blades-converted).  Writes

  GuideTabs.xui      the Blades two-tab strip (Marketplace's Game Downloads
                     page, dash_dashmark.xzp#1401_GameCatalogTabScene.xur):
                     its tab pieces, headers and 1To2 / 2To1 timelines, in a
                     group clipped to the page's width.  The page content,
                     title and back button are dropped; the headers read
                     Game and Settings.
  GuideTabsSkin.xui  the Blades skin visuals the strip draws (all figures),
                     byte for byte from dash_dashuisk.xzp#skin.xur.
  GuideGame.xui      the Game tab's expanded view, laid out from the Guide's
                     own skin visuals (Label_Head, Metapane, Label_Body_1line,
                     Label_BodyRight_1line): the title image, the name and
                     the session's facts.
"""
import os, subprocess, sys, tempfile
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
    ET.ElementTree(sroot).write(os.path.join(out, 'GuideTabsSkin.xui'))


def game(out):
    """The expanded view: the Guide's page heading, a meta pane with the
    title image centred at its top, the name under it, then label / value
    rows (the code fills the texts)."""
    root = ET.Element('XuiCanvas')
    p = ET.SubElement(root, 'Properties')
    ET.SubElement(p, 'Width').text = '1120'
    ET.SubElement(p, 'Height').text = '770'
    s = el(root, 'XuiScene', Id='scnGame', Width=1120, Height=770)
    el(s, 'XuiBackButton', Id='btnB', Width=420, Height=40, Position='119,613,0', Show='false',
       Visual='legend_B', Text='Back')
    el(s, 'XuiLabel', Id='labelHeading', Width=420, Height=47, Position='144,44,0', Visual='Label_Head')
    el(s, 'XuiLabel', Id='graphicMetapane', Width=420, Height=330, Position='146,100,0', Visual='Metapane')
    el(s, 'XuiImage', Id='imgTitle', Width=128, Height=128, Position='292,116,0',
       ImagePath='titleimage.png', SizeMode=4)
    el(s, 'XuiLabel', Id='txtName', Width=400, Height=35, Position='156,252,0', Visual='XuiLabelCenterJustify')
    for i in range(4):
        y = 296 + 32 * i
        el(s, 'XuiLabel', Id='txtKey%d' % i, Width=200, Height=35, Position='160,%d,0' % y,
           Visual='Label_Body_1line')
        el(s, 'XuiLabel', Id='txtValue%d' % i, Width=200, Height=35, Position='352,%d,0' % y,
           Visual='Label_BodyRight_1line')
    ET.ElementTree(root).write(os.path.join(out, 'GuideGame.xui'))


def main():
    blades, out = sys.argv[1:3]
    tmp = tempfile.mkdtemp()
    tabs(blades, out, tmp)
    game(out)
    print('%s: GuideTabs.xui, GuideTabsSkin.xui, GuideGame.xui' % out)


if __name__ == '__main__':
    main()
