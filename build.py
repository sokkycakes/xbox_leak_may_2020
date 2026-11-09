"""Build a native EXE from the user's exact runtime, preserving original RVAs.

No runtime extraction or LoadLibrary of xlive.dll. Original file is read-only.
Only the fingerprint below is supported; addresses must not be guessed for other builds.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import pefile

SUPPORTED_SHA256 = '447e2106fce0a529a26555db6aada35df28c7f1a7128385bc02211643abd8024'
ROOT = Path(__file__).resolve().parent

def align(n, a):
    return (n + a - 1) // a * a

def sounds(out):
    """UI sounds: the packages' .xma files (XMA1, which no PC API decodes) as PCM WAV.

    assets/sounds/<name>.wav is committed; when ffmpeg is on PATH, missing files are made from the
    x360/blades-converted/*.xzp entries (same-named entries are identical across packages).
    The host (guide_sound.c) plays them when the engine's XuiSoundXAudio elements fire."""
    folder = ROOT / 'assets' / 'sounds'
    ffmpeg = shutil.which('ffmpeg')
    if ffmpeg:
        import tempfile
        for package in sorted((ROOT / 'x360' / 'blades-converted').glob('*.xzp')):
            blob = package.read_bytes()
            if blob[:4] != b'XUIZ':
                continue
            count = struct.unpack_from('>H', blob, 20)[0]
            data = 22 + struct.unpack_from('>I', blob, 16)[0]
            at = 22
            for _ in range(count):
                size, offset, length = struct.unpack_from('>IIB', blob, at); at += 9
                name = blob[at:at + length * 2].decode('utf-16-be'); at += length * 2
                if not name.lower().endswith('.xma'):
                    continue
                target = folder / (name[:-4] + '.wav')
                if target.exists():
                    continue
                folder.mkdir(parents=True, exist_ok=True)
                with tempfile.TemporaryDirectory() as tmp:
                    source = Path(tmp) / name
                    source.write_bytes(blob[data + offset:data + offset + size])
                    subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-i', str(source),
                                    '-ar', '48000', '-c:a', 'pcm_s16le', '-map_metadata', '-1',
                                    '-fflags', '+bitexact', '-flags:a', '+bitexact', str(target)], check=True)
    destination = out / 'assets' / 'sounds'
    if folder.is_dir():
        destination.mkdir(parents=True, exist_ok=True)
        for wav in folder.glob('*.wav'):
            shutil.copy2(wav, destination / wav.name)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runtime', type=Path, default=Path('C:/Windows/SysWOW64/xlive.dll'))
    ap.add_argument('--output', default='Blades-Dashboard.exe', help='Executable filename in build/')
    ap.add_argument('--reconstructed', action='store_true', help='runtime rebuilt from a previous build by reconstruct.py (cloud builds)')
    args = ap.parse_args()
    original = args.runtime.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    if digest != SUPPORTED_SHA256 and not args.reconstructed:
        raise SystemExit('Unsupported runtime SHA-256: ' + digest)
    pe = pefile.PE(data=original)
    assert pe.FILE_HEADER.Machine == 0x14c
    assert not hasattr(pe, 'DIRECTORY_ENTRY_TLS')
    out = ROOT / 'build'
    out.mkdir(exist_ok=True)
    font_source = ROOT / 'assets' / 'fonts' / 'Convection-Medium.ttf'
    font_destination = out / 'assets' / 'fonts' / font_source.name
    font_destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(font_source, font_destination)
    sounds(out)
    host_rva = align(pe.OPTIONAL_HEADER.SizeOfImage, 0x10000)
    host_base = pe.OPTIONAL_HEADER.ImageBase + host_rva
    defines = '#define ORIGINAL_ENTRY_RVA 0x%x\n' % pe.OPTIONAL_HEADER.AddressOfEntryPoint
    defines += '#define HOST_RVA 0x%x\n' % host_rva
    for library in pe.DIRECTORY_ENTRY_IMPORT:
        for symbol in library.imports:
            if symbol.name and symbol.name.decode().isidentifier():
                defines += '#define IAT_%s 0x%x\n' % (symbol.name.decode(), symbol.address - pe.OPTIONAL_HEADER.ImageBase)
    (out / 'runtime_layout.h').write_text(defines)
    clang = shutil.which('i686-w64-mingw32-clang') or shutil.which('clang')
    if not clang:
        raise SystemExit('LLVM-MinGW clang is required.')
    sdl_flags = ['-I'+str(out/'SDL3-3.4.18'/'include'), str(out/'sdl-x86'/'libSDL3.a')]
    libraries = ['-lm','-lkernel32','-luser32','-lgdi32','-lwinmm','-limm32','-lole32',
                 '-loleaut32','-lversion','-luuid','-ladvapi32','-lsetupapi','-lshell32','-ldinput8','-ld3d9']
    subprocess.run([clang, '--target=i686-w64-windows-gnu', '-Os', '-fno-builtin',
                    '-fno-stack-protector', '-I' + str(out),
                    str(ROOT / 'host.c'), '-o', str(out / 'host.exe'),
                    '-Wl,--image-base,0x%x' % host_base,
                    '-Wl,--subsystem,windows', '-Wl,--disable-dynamicbase',
                    *sdl_flags, *libraries], check=True)
    host = pefile.PE(str(out / 'host.exe'))
    assert host.OPTIONAL_HEADER.ImageBase == host_base

    # Add each host section at the address for which it was linked. Merge imports
    # into a new descriptor table; do not move any original code, data or resources.
    result = bytearray(original)
    sections = []
    for section in host.sections:
        raw = bytearray(section.get_data())
        sections.append([section, raw])
    def host_patch(rva, value):
        for sec, raw in sections:
            if sec.VirtualAddress <= rva < sec.VirtualAddress + len(raw):
                struct.pack_into('<I', raw, rva - sec.VirtualAddress, value)
                return
        raise ValueError('Unmapped host RVA: %x' % rva)

    descriptors = bytearray()
    for imp in pe.DIRECTORY_ENTRY_IMPORT:
        descriptors.extend(imp.struct.__pack__())
    for imp in host.DIRECTORY_ENTRY_IMPORT:
        desc = imp.struct
        for thunk_rva in set([desc.OriginalFirstThunk, desc.FirstThunk]):
            if not thunk_rva:
                continue
            offset = 0
            while True:
                value = struct.unpack('<I', host.get_data(thunk_rva + offset, 4))[0]
                if not value:
                    break
                if not value & 0x80000000:
                    host_patch(thunk_rva + offset, value + host_rva)
                offset += 4
        descriptors.extend(struct.pack('<5I',
            desc.OriginalFirstThunk + host_rva if desc.OriginalFirstThunk else 0,
            0, 0, desc.Name + host_rva, desc.FirstThunk + host_rva))
    descriptors.extend(bytes(20))

    gap_start = pe.OPTIONAL_HEADER.SizeOfImage
    gap_size = host_rva + host.sections[0].VirtualAddress - gap_start
    host_headers = bytearray(gap_size)
    header_offset = host_rva - gap_start
    host_headers[header_offset:header_offset + host.OPTIONAL_HEADER.SizeOfHeaders] = host.__data__[:host.OPTIONAL_HEADER.SizeOfHeaders]
    new_sections = [(b'.pad', gap_size, gap_start, host_headers, 0x40000040)]
    for i, (sec, raw) in enumerate(sections):
        flags = sec.Characteristics
        if any(sec.VirtualAddress <= imp.struct.FirstThunk < sec.VirtualAddress + sec.Misc_VirtualSize
               for imp in host.DIRECTORY_ENTRY_IMPORT):
            flags |= 0x80000000  # Loader writes this host IAT; code remains RX.
        new_sections.append((('.h%d' % i).encode(), sec.Misc_VirtualSize,
                             sec.VirtualAddress + host_rva, raw, flags))
    imports_rva = align(host_rva + host.OPTIONAL_HEADER.SizeOfImage, 0x1000)
    new_sections.append((b'.imports',len(descriptors),imports_rva,descriptors,0xc0000040))
    table = pe.sections[0].get_file_offset()
    count = pe.FILE_HEADER.NumberOfSections + len(new_sections)
    assert table + count * 40 <= pe.OPTIONAL_HEADER.SizeOfHeaders
    raw_end = max(s.PointerToRawData + s.SizeOfRawData for s in pe.sections)
    result = result[:raw_end]  # Drop obsolete certificate/overlay in this output only.
    for i, (name,size,rva,raw,flags) in enumerate(new_sections):
        raw_start = align(len(result),pe.OPTIONAL_HEADER.FileAlignment)
        raw_size = align(len(raw),pe.OPTIONAL_HEADER.FileAlignment)
        result.extend(bytes(raw_start-len(result)))
        result.extend(raw)
        result.extend(bytes(raw_size-len(raw)))
        header = struct.pack('<8s6I2HI',name,size,rva,raw_size,raw_start if raw_size else 0,0,0,0,0,flags)
        at = table + (pe.FILE_HEADER.NumberOfSections+i)*40
        result[at:at+40] = header
    def u16(field,value):
        struct.pack_into('<H',result,pe.FILE_HEADER.get_field_absolute_offset(field),value)
    def optional(field,value,fmt='<I'):
        struct.pack_into(fmt,result,pe.OPTIONAL_HEADER.get_field_absolute_offset(field),value)
    def directory(index,rva,size):
        struct.pack_into('<II',result,pe.OPTIONAL_HEADER.DATA_DIRECTORY[index].get_file_offset(),rva,size)
    u16('NumberOfSections',count)
    u16('Characteristics',(pe.FILE_HEADER.Characteristics & ~0x2000) | 1)
    optional('AddressOfEntryPoint',host_rva+host.OPTIONAL_HEADER.AddressOfEntryPoint)
    optional('SizeOfImage',align(imports_rva+len(descriptors),0x1000))
    optional('DllCharacteristics',pe.OPTIONAL_HEADER.DllCharacteristics & ~0x40,'<H')
    optional('CheckSum',0)
    directory(1,imports_rva,len(descriptors))
    directory(4,0,0)   # Modified binary is unsigned.
    directory(5,0,0)   # Fixed image base; preserves protected code's absolute addresses.
    directory(11,0,0)  # No bound imports.
    if hasattr(host, 'DIRECTORY_ENTRY_TLS'):
        tls = host.OPTIONAL_HEADER.DATA_DIRECTORY[9]
        directory(9,tls.VirtualAddress + host_rva,tls.Size)
    # Preserve original IAT directory so the loader temporarily unprotects it.
    # EXE activation manifests use ID 1; DLL manifests use ID 2.
    for kind in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if kind.id == 24:
            for name in kind.directory.entries:
                if name.id == 2:
                    struct.pack_into('<I',result,name.struct.get_file_offset(),1)
    if Path(args.output).name != args.output:
        raise SystemExit('--output must be a filename')
    target = out / args.output
    target.write_bytes(result)
    check = pefile.PE(data=result)
    assert not check.FILE_HEADER.Characteristics & 0x2000
    imports = [i.dll.decode() for i in check.DIRECTORY_ENTRY_IMPORT]
    assert 'xlive.dll' not in [x.lower() for x in imports]
    assert 'sdl3.dll' not in [x.lower() for x in imports]
    for first, second in zip(check.sections, check.sections[1:]):
        assert align(first.VirtualAddress + max(first.Misc_VirtualSize, first.SizeOfRawData), 0x1000) == second.VirtualAddress
    resources = []
    for kind in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if str(kind.name) == 'RT_RCDATA':
            for name in kind.directory.entries:
                for language in name.directory.entries:
                    entry = language.data.struct
                    before = pe.get_data(entry.OffsetToData,entry.Size)
                    after = check.get_data(entry.OffsetToData,entry.Size)
                    assert before == after
                    resources.append({'name':str(name.name),'size':len(before),
                                      'sha256':hashlib.sha256(before).hexdigest()})
    (out/'build-report.json').write_text(json.dumps({
        'source_sha256':digest,'output_sha256':hashlib.sha256(result).hexdigest(),
        'output':str(target),'imports':imports,'resources':resources,
        'font':{'family':'Convection Medium','path':str(font_destination.relative_to(out)),
                'sha256':hashlib.sha256(font_source.read_bytes()).hexdigest()},
        'sdl':'3.4.18, statically linked','status':'standalone XUI host; run --probe to verify rendering and input on this machine'
    },indent=2))
    print(target)

if __name__ == '__main__':
    main()
