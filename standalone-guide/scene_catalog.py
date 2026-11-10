"""Catalog the supplied converted XUI packages, retaining the original paths.

The package directory and XUIB STRN tables are read exactly as the original XUI
resource loader reads them. No inferred button destinations are generated.
"""
from pathlib import Path
import struct
import json
import re
from recover_blades import strings

ROOT = Path(__file__).resolve().parent

def package_files(blob):
    assert blob[:4] == b'XUIZ'
    count = struct.unpack_from('>H', blob, 20)[0]
    data = 22 + struct.unpack_from('>I', blob, 16)[0]
    at = 22
    result = {}
    for _ in range(count):
        size, offset, length = struct.unpack_from('>IIB', blob, at)
        at += 9
        name = blob[at:at + length * 2].decode('utf-16-be')
        at += length * 2
        assert data + offset + size <= len(blob)
        result[name] = blob[data + offset:data + offset + size]
    return result

def c_string(text):
    return 'L' + json.dumps(text, ensure_ascii=True)

def native_schemas():
    cache = ROOT / 'x360/xui-property-schema.json'
    native_log = ROOT / 'build/native-classes.log'
    if not native_log.exists():
        saved = json.loads(cache.read_text())
        return saved['schemas'], saved['chains']
    schemas = {}
    chains = {}
    owner = None
    for line in native_log.read_text().splitlines():
        match = re.match(r'class (\w+) level (\d+) (\w+) count=(\d+)', line)
        if match:
            name, level, owner, count = match.groups()
            schemas.setdefault(owner, [])
            chains.setdefault(name, []).append(owner)
        match = re.match(r'  prop (\d+) (\w+) type=(\d+) flags=0x([0-9a-f]+)', line)
        if match and owner:
            index, name, kind, flags = match.groups()
            index = int(index)
            if len(schemas[owner]) == index:
                schemas[owner].append((name, int(kind), int(flags, 16)))
    chains['DashScene'] = ['DashScene', 'XuiScene', 'XuiControl', 'XuiElement']
    # The supplied converter report declares HUDScene -> XuiScene. Only read its
    # inherited properties here; its application-specific legend data is not
    # needed to recover DefaultFocus and the scene transitions.
    chains['HUDScene'] = ['HUDScene', 'XuiScene', 'XuiControl', 'XuiElement']
    # Original dash.xex 920a5558: three string properties in this exact order.
    schemas['DashScene'] = [('PanelSettings', 5, 4), ('PanelStrings', 5, 4), ('PanelScenePaths', 5, 0)]
    cache.write_text(json.dumps({'schemas': schemas, 'chains': chains}, indent=2), encoding='utf-8')
    return schemas, chains

def root_metadata(blob, schemas, chains):
    labels, sections = strings(blob)
    at = sections['DATA'][0]
    def take(fmt):
        nonlocal at
        value = struct.unpack_from('>' + fmt, blob, at)[0]
        at += struct.calcsize('>' + fmt)
        return value
    def text(index):
        return labels[index - 1] if index else ''
    def properties(name):
        nonlocal at
        result = {}
        for owner in reversed(chains[name]):
            if owner == 'HUDScene':
                break
            head = take('B')
            width = head & 7
            mask = int.from_bytes(blob[at:at + width], 'big')
            at += width
            for index, (key, kind, flags) in enumerate(schemas[owner]):
                if mask & (1 << index):
                    if flags & 1:
                        raise ValueError('array property')
                    if kind == 1: value = take('B')
                    elif kind == 4: value = take('f')
                    elif kind == 5: value = text(take('H'))
                    elif kind in (2, 3, 6, 7, 8, 10): value = take('I')
                    else: raise ValueError('structured property')
                    result[key] = value
        return result
    def node():
        name = text(take('H'))
        flags = take('B')
        if flags & 1:
            take('H')
            props = properties(name)
        else: props = {}
        return name, flags, props
    name, flags, props = node()
    if name == 'XuiCanvas' and flags & 2 and take('I'):
        name, flags, props = node()
    return name, props

def main():
    entries = []
    schemas, chains = native_schemas()
    original_root = Path('C:/Users/sokky/Downloads/Blades_unpacked')
    for package in sorted((ROOT / 'x360/blades-converted').glob('*.xzp')):
        original_package = ROOT / 'x360/blades' / package.name
        originals = package_files(original_package.read_bytes()) if original_package.exists() else {}
        for name, blob in package_files(package.read_bytes()).items():
            if name.lower().endswith('.xur') and '/' not in name and '\\' not in name:
                labels, _ = strings(blob)
                metadata = {}
                original = original_root / package.stem / name
                source_blob = originals.get(name)
                if source_blob is None and original.exists():
                    source_blob = original.read_bytes()
                if source_blob is None:
                    source_blob = blob
                if source_blob is not None:
                    try:
                        native_class, metadata = root_metadata(source_blob, schemas, chains)
                    except (KeyError, ValueError, IndexError, struct.error) as error:
                        metadata = {'unparsed': str(error)}
                entries.append({'package': package.name, 'scene': name, 'strings': labels, 'metadata': metadata})
    header = ['/* Generated from the supplied scene packages by scene_catalog.py. */',
              'typedef struct {const WCHAR *package,*scene,*focus,*panel_ids,*panel_texts,*panel_paths;} BladesScene;',
              'static const BladesScene blades_scenes[]={']
    for e in entries:
        m = e['metadata']
        values = [e['package'], e['scene'], m.get('DefaultFocus',''), m.get('PanelSettings',''),
                  m.get('PanelStrings',''), m.get('PanelScenePaths','')]
        header.append('    {' + ','.join(c_string(v) for v in values) + '},')
    header += ['};', '#define BLADES_SCENE_COUNT (sizeof(blades_scenes)/sizeof(blades_scenes[0]))']
    (ROOT / 'blades_catalog.h').write_text('\n'.join(header) + '\n', encoding='utf-8')
    (ROOT / 'x360/scene-catalog.json').write_text(json.dumps(entries, indent=2), encoding='utf-8')
    print('Cataloged', len(entries), 'supplied scenes')

if __name__ == '__main__':
    main()
