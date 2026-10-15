#!/bin/sh
# Runs on the Sion: installs Sion Link from /tmp/sionlink into /opt/sionlink,
# keeps it across reboots, checks the Wi-Fi card and starts the DS network.
set -e
mkdir -p /opt/sionlink/bin
cd /tmp/sionlink
cp hostapd sionlinkd /opt/sionlink/bin/
cp hostapd-sionlink.conf.in /opt/sionlink/
cp sion-dslink /usr/bin/sion-dslink
chmod 755 /opt/sionlink/bin/* /usr/bin/sion-dslink
sion persist /opt/sionlink/bin/hostapd /opt/sionlink/bin/sionlinkd \
    /opt/sionlink/hostapd-sionlink.conf.in /usr/bin/sion-dslink >/dev/null
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
cd / && rm -rf /tmp/sionlink
echo
sion-dslink check
echo
sion-dslink restart
sion-dslink status
