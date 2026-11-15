# Quake shareware on the Game OS

`tyr-quake.exe` is [TyrQuake](https://disenchant.net/tyrquake/) (GPL v2),
built for 64-bit Windows with MinGW, software renderer, plus
`tyrquake-nosound.patch` (it crashed when the attract demo started with sound
off or no audio device). The Game OS can't run 32-bit apps yet, so 64-bit only.

`make_quake_disk.sh` downloads id's Quake 1.06 shareware, checks `pak0.pak`,
and packs a game disk. The shareware data is not stored in this repo.

QEMU (KVM) with the Era Game OS image:

    samples/quake/make_quake_disk.sh quake.img
    qemu-system-x86_64 -enable-kvm -cpu host -m 3072 -smp 4 -cdrom crimson-game.iso -boot d \
        -drive file=quake.img,format=raw,if=ide -vga std \
        -device intel-hda -device hda-duplex -device e1000e,netdev=n0 -netdev user,id=n0
