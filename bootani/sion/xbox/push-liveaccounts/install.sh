#!/bin/bash
#
# Updates the Live server on the Sion to the one the retail Live dashboard
# signs in to (accounts, Account Management, New Account) and puts Live
# accounts on the Sion's hard disk. Needs push-livedash and the retail
# xodash already installed. Run on the PC from a checkout of this branch:
#
#   bootani/sion/xbox/push-liveaccounts/install.sh [GAMERTAG...]   (default Sokky)
#   SION=root@192.168.1.247 bootani/sion/xbox/push-liveaccounts/install.sh
#
# Then open XBOX LIVE on the dashboard and pick the gamertag. New Account
# makes more (any subscription code and card details are accepted).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SION=${SION:-sion}
[ $# -gt 0 ] || set -- Sokky

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
K=$T/xbox-live-accounts
mkdir -p "$K/liveserver"
cp "$REPO"/xbcompat/tools/liveserver/*.py "$K/liveserver/"
cp "$HERE/sion-install.sh" "$K/"

# BusyBox tar on the Sion has no -z.
(cd "$T" && tar -cf - xbox-live-accounts) | gzip | ssh "$SION" \
    "rm -rf /tmp/xbox-live-accounts && gunzip -c | tar -C /tmp -xf - && sh /tmp/xbox-live-accounts/sion-install.sh $*"
