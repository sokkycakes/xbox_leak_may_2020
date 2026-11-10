"""End-to-end graphics checks of the real executable. Run after build.py.

Requires pefile and Pillow. Each process is bounded; nothing modifies the system
runtime or launches a game. The isolated case has the EXE and its font asset.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import pefile
from PIL import Image

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / 'build'
EXE = BUILD / 'Blades-Dashboard.exe'

def run_case(name, exe, arguments, image_name=None, required=(), expected=0):
    if image_name:
        (exe.parent / image_name).unlink(missing_ok=True)
    result = subprocess.run([str(exe), '--probe', *arguments], cwd=exe.parent,
                            timeout=20, check=False)
    log = (exe.parent / 'blades.log').read_text(errors='replace')
    assert result.returncode == expected, (name, result.returncode, log[-1500:])
    assert 'Unhandled exception' not in log, name
    for line in required:
        assert line in log, (name, line)
    if image_name:
        image = Image.open(exe.parent / image_name).convert('RGB')
        assert image.size == (1280, 720)
        assert len(image.getcolors(1280 * 720)) > 500, (name, 'blank/simple frame')
    (BUILD / ('verification-' + name + '.log')).write_text(log)
    print('PASS', name)
    return {'case': name, 'exit_code': result.returncode, 'screenshot': image_name}

def main():
    pe = pefile.PE(str(EXE))
    imports = [entry.dll.decode().lower() for entry in pe.DIRECTORY_ENTRY_IMPORT]
    assert 'xlive.dll' not in imports and 'sdl3.dll' not in imports
    # Test a real scene package loaded through file://, independent of embedding.
    fixture = BUILD / 'scene-fixture.xzp'
    for kind in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if str(kind.name) == 'RT_RCDATA':
            for name in kind.directory.entries:
                if str(name.name) == 'HUD':
                    entry = name.directory.entries[0].data.struct
                    fixture.write_bytes(pe.get_data(entry.OffsetToData, entry.Size))
    common = ('Graphics reset: 0x00000000', 'Xui scene cleanup: 0x00000000',
              'Xui Convection font: 0x00000000', 'Xui Guide font alias: 0x00000000',
              'No xlive.dll or SDL3.dll loaded: 0x00000001')
    results = []
    results.append(run_case('guide', EXE, ['--pc-guide'], 'guide-preview.png',
                            (*common, 'Focus after Down:', 'Standalone button action: 0x00000002')))
    results.append(run_case('dashboard', EXE, ['--dashboard'], 'dashboard-preview.png',
                            (*common, 'Standalone button action: 0x0000000B')))
    uri = 'file://' + fixture.as_posix() + '#MainMenuSignedOut.xur'
    results.append(run_case('external-scene', EXE, ['--scene=' + uri], 'scene-preview.png',
                            (*common, 'External scene load: 0x00000000')))
    results.append(run_case('missing-scene', EXE, ['--scene=' + str(BUILD / 'nonexistent.xur')],
                            required=('External scene load:',), expected=13))
    isolated = BUILD / 'isolated'
    isolated.mkdir(exist_ok=True)
    shutil.copy2(EXE, isolated / EXE.name)
    shutil.copytree(BUILD / 'assets', isolated / 'assets', dirs_exist_ok=True)
    results.append(run_case('isolated-executable', isolated / EXE.name, ['--pc-guide'], 'guide-preview.png', common))
    report = {'executable_sha256': hashlib.sha256(EXE.read_bytes()).hexdigest(),
              'results': results, 'physical_controller_tested': False,
              'online_services': 'not implemented; standalone UI host only'}
    (BUILD / 'verification.json').write_text(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
