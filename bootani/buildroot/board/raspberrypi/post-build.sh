#!/bin/sh
# Raspberry Pi: the kernel's modules come prebuilt with the Foundation's
# kernel8.img (rpi-firmware's modules/*-v8+), so install them from there.
set -e
TARGET_DIR="$1"
FW=$(ls -d "$BUILD_DIR"/rpi-firmware-*/ | head -n1)
rm -rf "$TARGET_DIR"/lib/modules
mkdir -p "$TARGET_DIR"/lib/modules
for d in "$FW"modules/*-v8+; do
	cp -a "$d" "$TARGET_DIR"/lib/modules/
done

# /data: the Xbox hard disk (see xbox-session), on the card's third partition.
# By device, not LABEL=: busybox mount runs before udev and has no blkid.
mkdir -p "$TARGET_DIR"/data
sed -i '/ \/data /d' "$TARGET_DIR"/etc/fstab
echo '/dev/mmcblk0p3 /data ext4 defaults,noatime,nofail 0 2' >> "$TARGET_DIR"/etc/fstab

# The display mode everything uses: NTSC 720x480, as on the Sion.
mkdir -p "$TARGET_DIR"/etc/default
echo 'KMS_MODE=720x480' > "$TARGET_DIR"/etc/default/kms

# The network address on the login screen and the console, so there's
# something to ssh to without looking it up on the router.
mkdir -p "$TARGET_DIR"/usr/share/udhcpc/default.script.d
cat > "$TARGET_DIR"/usr/share/udhcpc/default.script.d/show-address <<'HOOK'
#!/bin/sh
case "$1" in bound|renew) ;; *) exit 0;; esac
printf 'xbcompat on Raspberry Pi\nssh root@%s (password xbox)\n\n' "$ip" > /etc/issue
printf '\n%s: ssh root@%s\n' "$(hostname)" "$ip" > /dev/tty1 2>/dev/null
exit 0
HOOK
chmod +x "$TARGET_DIR"/usr/share/udhcpc/default.script.d/show-address

# A login on the GPIO header's serial port (pins 8 TX, 10 RX, 6 ground;
# 115200 8N1) as well as on the screen: whichever UART the firmware made
# the serial console (ttyS0 on a Pi 3 with Bluetooth, ttyAMA0 otherwise).
cat > "$TARGET_DIR"/usr/libexec/serial-getty <<'GETTY'
#!/bin/sh
# (Not the one /dev/console is, flag C: inittab's own getty is there.)
while read -r c _ flags _; do
	case "$flags" in *C*) continue;; esac
	case "$c" in
	ttyS[0-9]*|ttyAMA[0-9]*) exec /sbin/getty -L "$c" 115200 vt100;;
	esac
done < /proc/consoles
exec sleep 2147483647
GETTY
chmod +x "$TARGET_DIR"/usr/libexec/serial-getty
grep -q serial-getty "$TARGET_DIR"/etc/inittab ||
	sed -i '/GENERIC_SERIAL/a ::respawn:/usr/libexec/serial-getty' "$TARGET_DIR"/etc/inittab

# Keep the Ethernet port eth0 even when cmdline.txt loses net.ifnames=0
# (eudev would rename it enx<MAC>, and /etc/network/interfaces names eth0).
mkdir -p "$TARGET_DIR"/etc/udev/rules.d
ln -sf /dev/null "$TARGET_DIR"/etc/udev/rules.d/80-net-name-slot.rules
