"""Bounded native rendering/input checks for the source-backed Blades host."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
from PIL import Image

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / 'build'
EXE = BUILD / 'Blades-Dashboard.exe'
OUTPUT = BUILD / 'blades-verification'
COMMON = ('Graphics reset: 0x00000000', 'Xui scene cleanup: 0x00000000',
          'No xlive.dll or SDL3.dll loaded: 0x00000001')

def case(name, keys='', required=(), arguments=(), timeout=25):
    env = dict(os.environ)
    env['XUI_KEYS'] = keys
    count = len([k for k in keys.split(',') if k])
    env['XUI_END'] = str(max(180, 100 + count * 20))
    result = subprocess.run([str(EXE), '--probe', *arguments], cwd=BUILD,
                            env=env, timeout=timeout, check=False)
    log = (BUILD / 'blades.log').read_text(errors='replace')
    (OUTPUT / (name + '.log')).write_text(log, encoding='utf-8')
    assert result.returncode == 0, (name, result.returncode, log[-1000:])
    assert 'Unhandled exception' not in log, name
    for text in (*COMMON, *required):
        assert text in log, (name, text, log[-1500:])
    image_name = 'dashboard-preview.png' if '--dashboard' in arguments else 'guide-preview.png' if '--pc-guide' in arguments else 'scene-preview.png'
    image = Image.open(BUILD / image_name).convert('RGB')
    assert image.size == (1280, 720), name
    assert len(image.getcolors(1280 * 720)) > 500, (name, 'empty render')
    shutil.copy2(BUILD / image_name, OUTPUT / (name + '.png'))
    print('PASS', name, flush=True)
    return {'case': name, 'exit_code': result.returncode, 'screenshot': name + '.png'}

def audit():
    env = dict(os.environ)
    env.pop('XUI_KEYS', None)
    result = subprocess.run([str(EXE), '--audit-scenes'], cwd=BUILD, env=env,
                            timeout=55, check=False)
    log = (BUILD / 'blades.log').read_text(errors='replace')
    (OUTPUT / 'scene-audit.log').write_text(log, encoding='utf-8')
    assert result.returncode == 0 and 'Unhandled exception' not in log
    scenes = []
    for block in log.split('Audit scene index:')[1:]:
        lines = block.splitlines()
        entry = {'index': int(lines[0].strip(), 16), 'package': lines[1], 'scene': lines[2]}
        for key, label in [('visuals', 'Scene audit visuals'), ('skin', 'Scene audit skin'),
                           ('routes', 'Scene audit routes'), ('unresolved_count', 'Scene audit unresolved'),
                           ('native_action_count', 'Scene audit native handlers')]:
            found = re.search(re.escape(label) + r': 0x([0-9A-F]+)', block)
            if found:
                entry[key] = int(found.group(1), 16)
        entry['loaded'] = 'Scene audit load failed' not in block and entry.get('visuals', entry.get('skin', -1)) == 0
        entry['unresolved_routes'] = [{'control': m.group(1), 'path': m.group(2)} for m in
                                      re.finditer(r'Unresolved authored route\n([^\n]+)\n([^\n]+)', block)]
        entry['native_actions'] = re.findall(r'Native action control\n([^\n]+)', block)
        scenes.append(entry)
    (OUTPUT / 'scene-audit.json').write_text(json.dumps(scenes, indent=2), encoding='utf-8')
    assert len(scenes) == 324, len(scenes)
    assert all(e['loaded'] for e in scenes), [e for e in scenes if not e['loaded']]
    print('PASS all supplied scenes and skin libraries', flush=True)
    return scenes

def main():
    OUTPUT.mkdir(exist_ok=True)
    results = []
    cases = [
        ('blades-home', '', ('Blades submenu navigation ready',)),
        ('console-settings', 'R,R,A', ('dash_consoles.xzp#dashSysCslSet.xur',)),
        ('display', 'R,R,A,A', ('dash_consoles.xzp#dashSysCslSetDisplay.xur', 'Blades submenu depth: 0x00000002')),
        ('nested-back', 'R,R,A,A,B,B,A', ('Blades Back depth: 0x00000000', 'dash_consoles.xzp#dashSysCslSet.xur')),
        ('memory', 'R,R,D,D,A', ('dash_memory.xzp#DeviceSelector.xur',)),
        ('network', 'R,R,D,D,D,A', ('dash_network.xzp#ConnStatus.xur',)),
        ('arcade', 'D,D,A', ('dash_arcade.xzp#2500_LiveArcadeHome.xur',)),
        ('music', 'R,D,A', ('dash_music.xzp#1000_MusicMain.xur',)),
        ('pictures', 'R,D,D,A', ('dash_pictures.xzp#900_PicturesMain.xur',)),
        ('video', 'R,D,D,D,A', ('dash_videos.xzp#VideosMain.xur',)),
        ('profile-scene', 'D,A', ('createprofile_cp.xzp#CreateGamerProfile.xur',)),
        ('profile-create-unavailable', 'D,A,A', ('A profile has not been created.',)),
        ('guide-options', 'G,D,D,D,A', ('hud_hud.xzp#Options.xur',)),
        ('guide-online', 'G,D,D,D,A,A', ('hud_hud.xzp#OptionsOnline.xur',)),
        ('guide-voice', 'G,D,D,D,A,D,A', ('hud_hud.xzp#OptionsVoice.xur',)),
        ('guide-controller', 'G,D,D,D,A,D,D,A', ('hud_hud.xzp#OptionsController.xur',)),
        ('guide-notifications', 'G,D,D,D,A,D,D,D,A', ('hud_hud.xzp#OptionsNotifications.xur',)),
        ('guide-themes', 'G,D,D,D,A,D,D,D,D,A', ('hud_hud.xzp#SkinSelect.xur',)),
        ('guide-back', 'G,D,D,D,A,A,B,B,B', ('Blades Back depth: 0x00000000', 'Xbox Guide open: 0x00000000')),
        ('guide-from-submenu', 'R,R,A,A,G,G,B,B', ('Xbox Guide open: 0x00000000', 'Blades Back depth: 0x00000000')),
        ('guide-from-example', 'F,G', ('Xbox Guide open: 0x00000001',)),
    ]
    for name, keys, required in cases:
        results.append(case(name, keys, required))
    results.append(case('example-dashboard', '', ('Dashboard scene: 0x00000000',), ('--dashboard',)))
    results.append(case('original-pc-guide', '', ('Guide scene attach: 0x00000000',), ('--pc-guide',)))
    scenes = audit()
    report = {'executable_sha256': hashlib.sha256(EXE.read_bytes()).hexdigest(),
              'results': results, 'audited_files': len(scenes),
              'authored_routes': sum(e.get('routes', 0) for e in scenes),
              'unresolved_routes': sum(e.get('unresolved_count', 0) for e in scenes),
              'physical_controller_tested': False,
              'console_services': 'Not connected; no console operations are reported as performed.'}
    (OUTPUT / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Wrote', OUTPUT / 'verification.json', flush=True)

if __name__ == '__main__':
    main()
