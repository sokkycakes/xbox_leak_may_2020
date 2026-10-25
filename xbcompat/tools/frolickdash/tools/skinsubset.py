#!/usr/bin/env python3
"""Pull the visuals the padfocus pages use out of the Xbox 360 Blades
dashboard's skin (dash_dashuisk.xzp#skin.xur, converted to text with
xur2xui.py) into one small skin scene, byte for byte the same visuals.

    skinsubset.py SKIN.xui OUT.xui VISUAL...

A visual is an XuiVisual anywhere in the skin named by its Id; the output
is an XuiCanvas with one XuiScene holding them, the way the runtime looks
visuals up (any depth, by Id).
"""
import sys, xml.etree.ElementTree as ET


def pid(e):
    p = e.find('Properties')
    return (p.findtext('Id') if p is not None else None) or ''


def main():
    skin, out, want = sys.argv[1], sys.argv[2], sys.argv[3:]
    t = ET.parse(skin).getroot()
    found = {}

    def walk(e):
        for c in e:
            if c.tag in ('Properties', 'Timelines'):
                continue
            if c.tag == 'XuiVisual' and pid(c) in want:
                found[pid(c)] = c
            else:
                walk(c)
    walk(t)
    root = ET.Element('XuiCanvas', version='000c')
    sc = ET.SubElement(root, 'XuiScene')
    pr = ET.SubElement(sc, 'Properties')
    ET.SubElement(pr, 'Id').text = 'BladesSkin'
    for k in want:
        if k in found:
            sc.append(found[k])
        else:
            print('missing', k, file=sys.stderr)
    ET.ElementTree(root).write(out)
    print('wrote', out, len(found), 'visuals')


if __name__ == '__main__':
    main()
