#!/bin/sh
# Runs on the Sion: puts the devkit dashboard in the Xbox dashboard's place
# from the kit copied to /tmp/devkit.  The Xbox dashboard (xboxdash.xbe) is
# not touched: the devkit dashboard lists it and opens it.
#
#   /opt/xbcompat/dash/devkit/    the devkit dashboard (C:\devkit)
#   /usr/libexec/xbox-session     taught XBOX_DASH=devkit (the copy it
#                                 replaces is kept as xbox-session.pre-devkit)
#   /usr/libexec/xbox-hostinfo    keeps E:\DEVKIT\hostinfo.txt current
#   /etc/default/xbox             XBOX_DASH=devkit
#
# All of it is kept across reboots with `sion persist`.  revert-devkit.sh
# turns it off again.
K=/tmp/devkit
D=/opt/xbcompat/dash/devkit
S=/usr/libexec/xbox-session
[ -f "$K/devkitdash.xbe" ] || { echo "no kit in $K"; exit 1; }
cd "$K" && sha256sum -c SHA256SUMS > /dev/null || { echo "the kit is damaged (sha256sum -c SHA256SUMS)"; exit 1; }

# The session: only once, and only onto a copy the patch fits.
if ! grep -q 'XBOX_DASH' "$S"; then
	rm -rf /tmp/devkit-session && mkdir /tmp/devkit-session || exit 1
	cp "$S" /tmp/devkit-session/xbox-session
	if (cd /tmp/devkit-session && patch -p1 < "$K/xbox-session.patch" > /dev/null 2>&1) &&
	   sh -n /tmp/devkit-session/xbox-session; then
		:
	elif [ "$(sha256sum < "$S" | cut -d' ' -f1)" = "$(cut -d' ' -f1 "$K/xbox-session.base.sha256")" ]; then
		cp "$K/xbox-session" /tmp/devkit-session/xbox-session
	else
		echo "this image's $S is not one the kit knows how to change; nothing was installed"
		exit 1
	fi
fi

/etc/init.d/S98xbox stop > /dev/null
sleep 2
mkdir -p "$D/media"
cp "$K/devkitdash.xbe" "$K/devkitdash.map" "$K/xbox-hostinfo" "$D/"
cp "$K"/media/* "$D/media/"
chmod -R a+rX "$D"
cp "$K/xbox-hostinfo" /usr/libexec/xbox-hostinfo && chmod 755 /usr/libexec/xbox-hostinfo
if [ -f /tmp/devkit-session/xbox-session ]; then
	[ -f "$S.pre-devkit" ] || cp "$S" "$S.pre-devkit"
	cp /tmp/devkit-session/xbox-session "$S" && chmod 755 "$S"
fi
rm -rf /tmp/devkit-session

touch /etc/default/xbox
if grep -q '^XBOX_DASH=' /etc/default/xbox; then
	sed -i 's/^XBOX_DASH=.*/XBOX_DASH=devkit/' /etc/default/xbox
else
	printf '# The devkit dashboard (C:\\devkit) instead of the Xbox dashboard; remove for the Xbox dashboard\nXBOX_DASH=devkit\n' >> /etc/default/xbox
fi

keep="$D /usr/libexec/xbox-hostinfo /etc/default/xbox $S"
[ -f "$S.pre-devkit" ] && keep="$keep $S.pre-devkit"
# shellcheck disable=SC2086
sion persist $keep > /dev/null
/etc/init.d/S98xbox start > /dev/null
sleep 8
tail -n 2 /var/log/xbox-session.log
pidof xbcompat > /dev/null && echo 'the devkit dashboard is running' || echo 'the dashboard is not running (see /var/log/xbox.log)'
