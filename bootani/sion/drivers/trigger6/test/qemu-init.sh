#!/bin/sh
mount -t proc p /proc; mount -t sysfs s /sys; mount -t devtmpfs d /dev; mount -t tmpfs t /tmp
echo 8 > /proc/sys/kernel/printk
echo 0x06 > /sys/module/drm/parameters/debug 2>/dev/null
for m in udc-core dummy_hcd raw_gadget; do insmod /mods/$m.ko || echo "insmod $m failed"; done
/bin/t6emu hd none /dev/vda &
sleep 2
echo "=== loading trigger6"
insmod /mods/trigger6.ko
sleep 8
echo "=== fbdev should be up on output 0 (1080p EDID)"
ls /dev/dri /dev/fb* 2>&1
echo "=== hotplug output 1 with the 4K TV EDID"
echo "plug 1 uhd" > /tmp/t6ctl
sleep 2
echo "=== custom 2880x480p 108 MHz on output 1"
/bin/kmstest /dev/dri/card0 1 2880 480 108000 3432 525 4
echo "=== preferred mode on output 0 via KMS"
/bin/kmstest /dev/dri/card0 0 3
echo "=== unplug output 1"
echo "unplug 1" > /tmp/t6ctl
sleep 4
echo "=== rmmod-free end"
dmesg | grep -i -E "trigger6|t6|BUG|WARN|Oops|error" | tail -60
echo TEST-DONE
poweroff -f
