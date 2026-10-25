#!/bin/sh
# The titles for the Raspberry Pi image (bootani/buildroot,
# raspberrypi_xbcompat_defconfig): the boot animation and the dashboard,
# taken from a Sion bundle (tools/sion/bundle.sh, which builds them from the
# leak), and XDK samples to try, each with its library map.
#
# usage: titles.sh OUT.tar.gz SION-BUNDLE.tar.gz [OVERRIDE-DIR...] [-- SAMPLE.exe...]
#
# Files in an OVERRIDE-DIR replace the bundle's: Bootscreen.* in boot/,
# anything else in dash/ (newer dashboard builds pushed to the Sion since).
set -e
out=$1 bundle=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)
xbc=$(dirname "$(dirname "$here")")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tar -C "$work" -xzf "$bundle" xbcompat/boot xbcompat/dash xbcompat/cache/xbcompat
stage=$work/xbcompat
while [ $# -gt 0 ] && [ "$1" != -- ]; do
	for f in "$1"/*; do
		case "${f##*/}" in
		Bootscreen.*) cp "$f" "$stage/boot/" ;;
		xboxdash.*|*.xip|*.xtf) cp "$f" "$stage/dash/" ;;
		esac
	done
	shift
done
[ "$1" = -- ] && shift

# Samples: xbrun.py converts each to an XBE beside its media and makes its
# map; a stand-in for xbcompat reports where they are.
mkdir -p "$stage/samples"
cat > "$work/where" <<'EOS'
#!/bin/sh
map= ; while [ $# -gt 1 ]; do [ "$1" = --hle ] && map=$2; shift; done
echo "$map $1"
EOS
chmod +x "$work/where"
for exe in "$@"; do
	name=$(basename "$exe" .exe)
	set -- $(XDG_CACHE_HOME=$work/cache XBCOMPAT_BIN=$work/where python3 "$xbc/tools/xbrun.py" "$exe")
	mkdir -p "$stage/samples/$name"
	cp -r "$(dirname "$2")"/. "$stage/samples/$name/"
	cp "$1" "$stage/samples/$name/xbcompat.map"
	echo "sample $name"
done

mkdir -p "$stage/cache/xbcompat/maps"
for sample in "$stage"/samples/*; do
	[ -f "$sample/default.xbe" ] && [ -s "$sample/xbcompat.map" ] || continue
	python3 "$here/cache-map.py" "$sample/default.xbe" --map "$sample/xbcompat.map" \
		--cache "$stage/cache/xbcompat/maps"
done

tar -C "$stage" -czf "$out" .
echo "wrote $out"
