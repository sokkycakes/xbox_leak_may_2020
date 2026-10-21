#!/bin/bash
#
# Points XBOX LIVE on the Sion at Insignia (insignia.live) instead of the local
# test server, and registers the Sion's console with Insignia. Needs
# push-livedash, push-liveaccounts and push-liveguide already installed. Run on
# the PC from a checkout of this branch:
#
#   bootani/sion/xbox/push-insignia/install.sh             (ssh host "sion")
#   SION=root@192.168.1.247 bootani/sion/xbox/push-insignia/install.sh
#
# Registering is what the Register button of Insignia's Setup Assistant does
# (that homebrew can't run under xbcompat): the console's serial number, hard
# disk key and online key go to insignia.live once, from this PC. Then get a
# subscription code at insignia.live and use New Account on the Sion.
#
# Back to the test server (its accounts come back, the Insignia ones are kept):
#   ssh sion 'echo local > /data/xbox-live/service'
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SION=${SION:-sion}

cd "$HERE"
md5sum -c --quiet MD5SUMS

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/xbox-insignia
mkdir -p "$K/liveserver"
cp "$REPO"/xbcompat/tools/liveserver/*.py "$K/liveserver/"
cp sion-install.sh ../push-livedash/launch-title "$K/"

# BusyBox tar on the Sion has no -z.
(cd "$T" && tar -cf - xbox-insignia) | gzip | ssh "$SION" \
    'rm -rf /tmp/xbox-insignia && gunzip -c | tar -C /tmp -xf - && sh /tmp/xbox-insignia/sion-install.sh'

# Register once. The console-id stays between the Sion, this PC and insignia.live.
if ssh "$SION" test -f /data/xbox-live/insignia-registered; then
	echo "insignia: this console is already registered"
else
	ssh "$SION" cat /data/xbox/hdd/console-id > "$T/console-id"
	if python3 "$REPO/xbcompat/tools/liveserver/insignia.py" register "$T/console-id"; then
		ssh "$SION" 'date > /data/xbox-live/insignia-registered'
	else
		echo "insignia: not registered (the reason is above); XBOX LIVE won't sign in until it is"
	fi
fi
