"""Fetch the pinned upstream SDL3 source into the local build directory."""
import hashlib
from pathlib import Path, PurePosixPath
import urllib.request
import zipfile

root = Path(__file__).resolve().parent / 'build'
root.mkdir(exist_ok=True)
archive = root / 'SDL3-source.zip'
expected = '9cd42377704398796071b8597cd7e21da254a43bad98c4199739647adc13fa6f'
url = 'https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.zip'
if not archive.exists():
    urllib.request.urlretrieve(url, str(archive))
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('SDL archive checksum mismatch. No source was extracted.')
with zipfile.ZipFile(archive) as source:
    for member in source.infolist():
        name = PurePosixPath(member.filename)
        if name.is_absolute() or '..' in name.parts or ':' in member.filename or '\\' in member.filename:
            raise SystemExit('Unsafe archive member: ' + member.filename)
    source.extractall(root)
print(root / 'SDL3-3.4.18')
