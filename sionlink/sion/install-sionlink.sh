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
cd / && rm -rf /tmp/sionlink
echo
sion-dslink check
echo
sion-dslink restart
sion-dslink status
