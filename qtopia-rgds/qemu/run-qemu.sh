#!/bin/sh
# Boot the QEMU stand-in for the RG DS: two 640x480 panels, each with its own
# touch device (the mouse in each QEMU window), a keyboard and a sound card.
# Needs qemu-system-aarch64 (Fedora: dnf install qemu-system-aarch64).
cd "$(dirname "$0")"
[ -f rootfs.ext4 ] || xz -dk rootfs.ext4.xz
exec qemu-system-aarch64 -M virt -cpu cortex-a55 -smp 4 -m 1024 \
    -kernel Image -append "rootwait root=/dev/vda console=ttyAMA0" \
    -drive file=rootfs.ext4,if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 \
    -device virtio-gpu-pci,xres=640,yres=480,id=gpu0 \
    -device virtio-gpu-pci,xres=640,yres=480,id=gpu1 \
    -device virtio-keyboard-pci \
    -device virtio-tablet-pci,display=gpu0 \
    -device virtio-tablet-pci,display=gpu1 \
    -audiodev pa,id=snd0 -device virtio-sound-pci,audiodev=snd0 \
    -display gtk,show-tabs=on -serial stdio "$@"
