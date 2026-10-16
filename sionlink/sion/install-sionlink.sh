#!/bin/sh
# Runs on the Sion: installs Sion Link from /tmp/sionlink into /opt/sionlink,
# keeps it across reboots, checks the Wi-Fi card and starts the DS network.
set -e
mkdir -p /opt/sionlink/bin /data/sionlink
cd /tmp/sionlink
cp hostapd sionlinkd linktest /opt/sionlink/bin/
[ -f ffhost ] && cp ffhost /opt/sionlink/bin/
[ -f sionsniff ] && cp sionsniff /opt/sionlink/bin/
cp hostapd-sionlink.conf.in /opt/sionlink/
cp linktest.nds /opt/sionlink/
[ -f iwl_ratefix.ko ] && cp iwl_ratefix.ko /opt/sionlink/
for f in ffcc-gba.nds ffcc-gba.cia ffcc-gba.3dsx; do
    [ -f "$f" ] && cp "$f" /opt/sionlink/
done
if [ -f 3ds-keys.txt ]; then
    cp 3ds-keys.txt /data/sionlink/3ds-keys.txt
    chmod 600 /data/sionlink/3ds-keys.txt
fi
cp sion-dslink /usr/bin/sion-dslink
chmod 755 /opt/sionlink/bin/* /usr/bin/sion-dslink
persist_list="/opt/sionlink/bin/hostapd /opt/sionlink/bin/sionlinkd \
    /opt/sionlink/bin/linktest /opt/sionlink/linktest.nds \
    /opt/sionlink/hostapd-sionlink.conf.in /usr/bin/sion-dslink"
for f in /opt/sionlink/bin/ffhost /opt/sionlink/bin/sionsniff /opt/sionlink/iwl_ratefix.ko \
         /opt/sionlink/ffcc-gba.nds /opt/sionlink/ffcc-gba.cia \
         /opt/sionlink/ffcc-gba.3dsx; do
    [ -f "$f" ] && persist_list="$persist_list $f"
done
sion persist $persist_list >/dev/null
# Teach sion-netd to leave the DS network alone, then restart only its
# loop: wpa_supplicant and the home Wi-Fi link keep running.
if ! grep -q 'ifc" = sionds' /usr/libexec/sion-netd; then
    awk '{ print } index($0, "-e \"$p/device\" ] || continue") { print "\t\t[ \"$ifc\" = sionds ] && continue  # Sion Link DS network" }' \
        /usr/libexec/sion-netd > /tmp/sion-netd.new
    cat /tmp/sion-netd.new > /usr/libexec/sion-netd && rm -f /tmp/sion-netd.new
    grep -q 'ifc" = sionds' /usr/libexec/sion-netd || { echo "couldn't update sion-netd; stopping"; exit 1; }
    sion persist /usr/libexec/sion-netd >/dev/null
    start-stop-daemon -K -q -p /var/run/sion-netd.pid
    start-stop-daemon -S -q -b -m -p /var/run/sion-netd.pid -x /usr/libexec/sion-netd
    echo "updated sion-netd"
fi
# ...and the connector's beacon interface (an earlier install only knew sionds)
if ! grep -q 'ifc" = sionmon' /usr/libexec/sion-netd; then
    awk '{ print } index($0, "ifc\" = sionds ]") { print "\t\t[ \"$ifc\" = sionmon ] && continue  # Sion Link beacons" }' \
        /usr/libexec/sion-netd > /tmp/sion-netd.new
    cat /tmp/sion-netd.new > /usr/libexec/sion-netd && rm -f /tmp/sion-netd.new
    sion persist /usr/libexec/sion-netd >/dev/null
    start-stop-daemon -K -q -p /var/run/sion-netd.pid
    start-stop-daemon -S -q -b -m -p /var/run/sion-netd.pid -x /usr/libexec/sion-netd
    echo "updated sion-netd for the beacon interface"
fi
cd / && rm -rf /tmp/sionlink
echo
sion-dslink check
echo
sion-dslink restart
sion-dslink status
