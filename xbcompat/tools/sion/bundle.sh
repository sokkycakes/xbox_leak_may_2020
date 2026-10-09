#!/bin/bash
# Build /opt/xbcompat for the Sion image: xbcompat, the boot animation
# (ani2's Bootscreen.xbe) and the dashboard (xboxdash.xbe), with the 32-bit
# userspace they run on. The Sion's own system is 64-bit Buildroot; this is a
# self-contained i386 tree beside it, with its own dynamic loader, so nothing
# in the 64-bit system changes.
#
#   tools/sion/bundle.sh [WORK]          (default WORK=xbcompat/build-sion)
#
# Output: WORK/xbcompat-sion.tar.gz, unpacked by the Buildroot package
# (bootani/buildroot/package/xbcompat) into /opt/xbcompat:
#
#   bin/xbcompat          xbcompat, built against the SDL2 below (GL=opengl)
#   bin/xbox-av           holds the picture between titles (tools/sion/av)
#   bin/xbrun             runs a title the way tools/xbrun.py does: builds its
#                         library map first (so the dashboard can launch games)
#   lib/                  glibc, SDL2 (KMSDRM, ALSA, udev; with the Sion's
#                         patches/sdl2), Mesa (iris + softpipe, no LLVM),
#                         glvnd, ALSA, libudev, and python3 for bin/xbrun
#   tools/                xbrun.py and the map tools, XbSymbolDatabaseCLI
#   boot/Bootscreen.xbe   the boot animation, and its map
#   dash/                 the dashboard's C: partition (xboxdash.xbe, XIPs,
#                         fonts, ADPCM audio as the retail dash shipped it)
#   cache/xbcompat/       signatures and maps computed here
#
# Runs on Ubuntu 24.04 (x86_64 with i386 multiarch). Needs, besides Wine for
# the two title builds (see tools/dashbuild/build.sh):
#   apt install gcc-multilib g++-multilib meson ninja-build python3-mako cmake \
#     patchelf libgl-dev:i386 libegl-dev:i386 libopengl-dev:i386 libglvnd-dev:i386 \
#     libdrm-dev:i386 libasound2-dev:i386 libudev-dev:i386 libexpat1-dev:i386 \
#     zlib1g-dev:i386 libzstd-dev:i386
# NEWGAMES (the March 2001 Games scenes) is passed on to dashbuild.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
XBC=$(cd "$HERE/../.." && pwd)
REPO=$(cd "$XBC/../bootani/buildroot" && pwd)
WORK=${1:-$XBC/build-sion}
SRC=$WORK/src
STAGE=$WORK/stage           # build products, installed with prefix /usr
OUT=$WORK/opt/xbcompat      # the bundle as it lands on the Sion
ML=/usr/lib/i386-linux-gnu  # the host's i386 libraries
mkdir -p "$SRC" "$STAGE"
J=$(nproc)

SDL_VER=2.32.10
MESA_VER=23.3.6             # the last Mesa whose iris builds without intel-clc (and so LLVM)
PY=python3.12

fetch() { # url
    [ -f "$SRC/$(basename "$1")" ] || curl -sSfL -o "$SRC/$(basename "$1")" "$1"
}

# ---- SDL2, with the Sion's KMSDRM patches ------------------------------
if [ ! -f "$STAGE/usr/lib/libSDL2-2.0.so.0" ]; then
    echo "== SDL2 $SDL_VER"
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
    rm -rf "$SRC/SDL2-$SDL_VER"
    tar -C "$SRC" -xf "$SRC/SDL2-$SDL_VER.tar.gz"
    for p in "$REPO"/patches/sdl2/*.patch; do patch -d "$SRC/SDL2-$SDL_VER" -p1 -s < "$p"; done
    mkdir -p "$SRC/SDL2-$SDL_VER/b32"
    (
        cd "$SRC/SDL2-$SDL_VER/b32"
        CC="gcc -m32" PKG_CONFIG_LIBDIR=$ML/pkgconfig:/usr/share/pkgconfig ../configure -q \
            --host=i686-linux-gnu --prefix=/usr --disable-static \
            --enable-video-kmsdrm --disable-video-x11 --disable-video-wayland --disable-video-vulkan \
            --disable-video-opengles1 --disable-video-rpi --disable-video-directfb \
            --enable-alsa --disable-pulseaudio --disable-pipewire --disable-jack --disable-sndio \
            --disable-esd --disable-arts --disable-nas --disable-libsamplerate \
            --enable-libudev --disable-dbus --disable-ime --disable-ibus --disable-fcitx \
            --disable-hidapi-libusb
        make -s -j"$J"
        make -s install DESTDIR="$STAGE"
    )
    sed -i "s|^prefix=.*|prefix=$STAGE/usr|" "$STAGE/usr/lib/pkgconfig/sdl2.pc"
fi

# ---- Mesa: iris for the Sion's UHD 620, softpipe for anything else ---------
if [ ! -f "$STAGE/usr/lib/dri/iris_dri.so" ]; then
    echo "== Mesa $MESA_VER"
    fetch "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz"
    rm -rf "$SRC/mesa-$MESA_VER"
    tar -C "$SRC" -xf "$SRC/mesa-$MESA_VER.tar.xz"
    cat > "$SRC/i386.cross" <<EOF
[binaries]
c = ['gcc', '-m32']
cpp = ['g++', '-m32']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = '$ML/pkgconfig:/usr/share/pkgconfig'
[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOF
    (
        cd "$SRC/mesa-$MESA_VER"
        meson setup build --cross-file ../i386.cross --prefix=/usr --libdir=lib --buildtype=release \
            -Db_ndebug=true -Dgallium-drivers=iris,swrast -Dvulkan-drivers= -Dllvm=disabled -Dplatforms= \
            -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dglvnd=true -Dgles1=disabled -Dgles2=enabled \
            -Dshared-glapi=enabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=enabled -Dtools= \
            -Dbuild-tests=false -Dvideo-codecs= -Dgallium-va=disabled -Dgallium-vdpau=disabled \
            -Dgallium-xa=disabled -Dgallium-nine=false -Dgallium-opencl=disabled -Dgallium-rusticl=false \
            -Dlmsensors=disabled -Dosmesa=false > ../mesa-setup.log
        ninja -C build -j"$J" > ../mesa-build.log
        DESTDIR="$STAGE" ninja -C build install > /dev/null
    )
fi

# ---- XbSymbolDatabase (MIT): library maps for titles from other XDKs -------
XBSYMDB_COMMIT=$(sed -n 's/^XBSYMDB_COMMIT = "\(.*\)"/\1/p' "$XBC/tools/xbrun.py")
if [ ! -x "$STAGE/XbSymbolDatabaseCLI" ]; then
    echo "== XbSymbolDatabase ${XBSYMDB_COMMIT:0:12}"
    [ -d "$SRC/XbSymbolDatabase" ] || git clone -q https://github.com/Cxbx-Reloaded/XbSymbolDatabase.git "$SRC/XbSymbolDatabase"
    git -C "$SRC/XbSymbolDatabase" checkout -q "$XBSYMDB_COMMIT"
    cmake -S "$SRC/XbSymbolDatabase" -B "$SRC/XbSymbolDatabase/b32" -DCMAKE_BUILD_TYPE=Release \
        "-DCMAKE_C_FLAGS=-m32 -Wno-stringop-overread" "-DCMAKE_CXX_FLAGS=-m32 -Wno-stringop-overread" > /dev/null
    cmake --build "$SRC/XbSymbolDatabase/b32" --target XbSymbolDatabaseCLI -j"$J" > /dev/null
    cp "$SRC/XbSymbolDatabase/b32/projects/cli/XbSymbolDatabaseCLI" "$STAGE/"
fi

# ---- python3 (i386) for xbrun.py on the Sion ------------------------------
if [ ! -x "$STAGE/py/usr/bin/$PY" ]; then
    echo "== $PY"
    mkdir -p "$SRC/debs" && rm -rf "$STAGE/py"
    (cd "$SRC/debs" && apt-get download -q "$PY-minimal:i386" "lib$PY-minimal:i386" "lib$PY-stdlib:i386" > /dev/null)
    for d in "$SRC"/debs/*"$PY"*_i386.deb; do dpkg -x "$d" "$STAGE/py"; done
fi

# ---- xbcompat ----------------------------------------------------------
echo "== xbcompat"
make -s -C "$XBC" -j"$J" BUILD="$WORK/xbcompat-build" GL=opengl \
    PKGCFG="PKG_CONFIG_PATH=$STAGE/usr/lib/pkgconfig:$ML/pkgconfig pkg-config" "$WORK/xbcompat-build/xbcompat"

# ---- the titles ----------------------------------------------------------
[ -f "$WORK/ani/Bootscreen.xbe" ] || "$XBC/tools/anibuild/build.sh" "$WORK/ani"
[ -f "$WORK/dash/run/hdd/partition2/xboxdash.xbe" ] || "$XBC/tools/dashbuild/build.sh" "$WORK/dash"

# ---- assemble ----------------------------------------------------------
echo "== assemble $OUT"
rm -rf "$WORK/opt"
mkdir -p "$OUT/bin" "$OUT/lib/dri" "$OUT/tools" "$OUT/boot" "$OUT/share/glvnd/egl_vendor.d" "$OUT/cache/xbcompat"
cp "$WORK/xbcompat-build/xbcompat" "$OUT/bin/"
# The display keeper, a static 64-bit program like the Sion's own.
gcc -O2 -Wall -static -s -o "$OUT/bin/xbox-av" "$HERE/av/xbox-av.c"
cp "$STAGE/XbSymbolDatabaseCLI" "$OUT/tools/"
cp "$STAGE/usr/lib/libSDL2-2.0.so.0" "$STAGE"/usr/lib/libEGL_mesa.so.0 "$STAGE"/usr/lib/libgbm.so.1 \
   "$STAGE"/usr/lib/libglapi.so.0 "$OUT/lib/"
cp "$STAGE/usr/lib/dri/iris_dri.so" "$OUT/lib/dri/"
ln -s iris_dri.so "$OUT/lib/dri/kms_swrast_dri.so"
ln -s iris_dri.so "$OUT/lib/dri/swrast_dri.so"
cp "$STAGE/usr/share/drirc.d/00-mesa-defaults.conf" "$OUT/share/"
printf '{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_mesa.so.0"}}\n' \
    > "$OUT/share/glvnd/egl_vendor.d/50_mesa.json"
# Loaded with dlopen (SDL: EGL, GBM, DRM, ALSA, udev; GL through glvnd).
for l in libEGL.so.1 libOpenGL.so.0 libGLdispatch.so.0 libdrm.so.2 libasound.so.2 libudev.so.1; do
    cp -L "$ML/$l" "$OUT/lib/"
done
# python: the interpreter and the parts of the standard library xbrun uses.
cp "$STAGE/py/usr/bin/$PY" "$OUT/bin/python3"
PYLIB=$STAGE/py/usr/lib/$PY
mkdir -p "$OUT/lib/$PY"
(cd "$PYLIB" && tar --exclude=__pycache__ --exclude=test --exclude=tests --exclude=idlelib --exclude=tkinter \
    --exclude=turtledemo --exclude=ensurepip --exclude=lib2to3 --exclude=pydoc_data --exclude=unittest \
    --exclude=asyncio --exclude=email --exclude=http --exclude=xml --exclude=sqlite3 --exclude=curses \
    --exclude=venv --exclude=multiprocessing -cf - .) | tar -C "$OUT/lib/$PY" -xf -
for t in xbrun.py xbedump.py findsigs.py mksigs.py xbsymmap.py pe2xbe.py; do cp "$XBC/tools/$t" "$OUT/tools/"; done
# Everything those need from the host's i386 libraries, glibc included.
closure() {
    find "$OUT/bin" "$OUT/lib" "$OUT/tools" -type f \( -name '*.so*' -o -perm -u+x \) | while read -r f; do
        file -b "$f" | grep -q '^ELF 32' && ldd "$f" 2>/dev/null
    done | awk '$2 == "=>" && $3 ~ /^\// { print $3 } $1 ~ /^\/lib\/ld-linux/ { print $1 }' | sort -u
}
for _ in 1 2 3; do
    for l in $(closure); do
        b=$(basename "$l")
        [ -e "$OUT/lib/$b" ] || cp -L "$l" "$OUT/lib/$b"
    done
done
# Every executable uses this loader; LD_LIBRARY_PATH (bin/xbrun, the Sion's
# xbox-env) points it at lib/. (An rpath would do too, but patchelf adding
# one to the python binary leaves it crashing at startup.)
for f in "$OUT/bin/xbcompat" "$OUT/bin/python3" "$OUT/tools/XbSymbolDatabaseCLI"; do
    patchelf --set-interpreter /opt/xbcompat/lib/ld-linux.so.2 "$f"
done
strip -s "$OUT/bin/xbcompat" "$OUT/tools/XbSymbolDatabaseCLI" 2>/dev/null || true

# The boot animation.
cp "$WORK/ani/Bootscreen.xbe" "$OUT/boot/"
# The dashboard's partition, with the ADPCM sounds under the plain names, as
# the retail dashboard was laid out (private/ui/xapp/xbupd.cmd).
mkdir -p "$OUT/dash"
(cd "$WORK/dash/run/hdd/partition2" && tar --exclude='Audio' -chf - .) | tar -C "$OUT/dash" -xf -
for d in "$WORK"/dash/run/hdd/partition2/Audio/*; do
    n=$(basename "$d")
    case $n in *ADPCM) continue ;; esac
    src=$d; [ -d "${d}ADPCM" ] && src=${d}ADPCM
    mkdir -p "$OUT/dash/Audio/$n"
    cp -L "$src"/* "$OUT/dash/Audio/$n/"
done
# Library maps for the two titles, and the leak's signatures for any other
# XDK 4400 title (mksigs needs the leak's .lib files, which the Sion hasn't).
export XDG_CACHE_HOME=$OUT/cache XBCOMPAT_BIN=$OUT/bin/xbcompat
python3 - "$XBC/tools" "$OUT" <<'EOF'
import os, shutil, sys
sys.path.insert(0, sys.argv[1])
import xbrun
out = sys.argv[2]
for xbe, name in ((out + "/boot/Bootscreen.xbe", "Bootscreen.map"), (out + "/dash/xboxdash.xbe", "xboxdash.map")):
    m = xbrun.make_map(xbe)
    shutil.copy(m, os.path.join(os.path.dirname(xbe), name))
    os.remove(m)
# Every combination, not only those with D3D8: the XBLA disc's CDXU/reboot.xbe
# links XAPILIB alone, and with no signatures for that it failed to start.
import itertools
for suffix in ("", "d"):
    names = [n + suffix for n in ("d3d8", "dsound", "xapilib")]
    for k in (1, 2, 3):
        for libs in itertools.combinations(names, k):
            xbrun.signatures(list(libs))
EOF

cat > "$OUT/bin/xbrun" <<'EOF'
#!/bin/sh
# Run an XBE under xbcompat the way tools/xbrun.py does, with this tree's
# python, map tools and signatures. Maps go to $XDG_CACHE_HOME/xbcompat.
X=/opt/xbcompat
export LD_LIBRARY_PATH=$X/lib PYTHONHOME=$X PYTHONDONTWRITEBYTECODE=1 \
    LIBGL_DRIVERS_PATH=$X/lib/dri GBM_DRIVERS_PATH=$X/lib/dri \
    __EGL_VENDOR_LIBRARY_FILENAMES=$X/share/glvnd/egl_vendor.d/50_mesa.json \
    XBCOMPAT_BIN=$X/bin/xbcompat XBSYMDB_CLI=$X/tools/XbSymbolDatabaseCLI
exec $X/bin/python3 $X/tools/xbrun.py "$@"
EOF
chmod +x "$OUT/bin/xbrun"
echo "$(git -C "$XBC" rev-parse --short HEAD) $(date -u +%F)" > "$OUT/VERSION"

tar -C "$WORK/opt" -czf "$WORK/xbcompat-sion.tar.gz" xbcompat
du -sh "$OUT" "$WORK/xbcompat-sion.tar.gz"
