#!/bin/sh
# Builds the static hostapd 2.11 the kit ships, with hostapd-2.11-sionlink.patch
# (ignore_unacked_join). Needs libnl-genl-3-dev (static libs). Prints its path.
set -e
here=$(cd "$(dirname "$0")" && pwd)
W=${1:-/tmp/sionlink-hostapd}
mkdir -p "$W" && cd "$W"
[ -f hostapd-2.11.tar.gz ] || curl -sSfLO https://w1.fi/releases/hostapd-2.11.tar.gz
rm -rf hostapd-2.11 && tar xzf hostapd-2.11.tar.gz
cd hostapd-2.11 && patch -p1 -s < "$here/hostapd-2.11-sionlink.patch"
cd hostapd
cat > .config <<'CFG'
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_IEEE80211N=n
CONFIG_NO_RADIUS=y
CONFIG_NO_ACCOUNTING=y
CONFIG_NO_VLAN=y
CONFIG_CTRL_IFACE=y
CONFIG_WEP=y
CFG
make -s -j8 LDFLAGS=-static LIBS="-lnl-genl-3 -lnl-3 -lpthread -lm" >&2
echo "$PWD/hostapd"
