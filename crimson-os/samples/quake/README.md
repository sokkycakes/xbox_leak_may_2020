# Quake shareware on the Game OS

`tyr-quake.exe` is [TyrQuake](https://disenchant.net/tyrquake/) (GPL v2),
built for 64-bit Windows with MinGW, software renderer, plus
`tyrquake-winpe.patch`: it falls back to the current display mode when Windows
PE has no saved one ("Unable to query desktop display settings"), no longer
crashes when the attract demo starts without a sound device, goes fullscreen
without a mode switch when the requested size is the desktop's (WinPE's display
driver can't switch modes), and adds `-desktopres` to pick that size.

Tested in the Era Game OS under QEMU/KVM: the attract demo plays about 100 s
after power-on. No sound yet (see below). The Game OS can't run 32-bit apps yet, so 64-bit only.

`make_quake_disk.sh` downloads id's Quake 1.06 shareware, checks `pak0.pak`,
and packs a game disk. The shareware data is not stored in this repo.

QEMU (KVM) with the Era Game OS image:

    samples/quake/make_quake_disk.sh quake.img
    qemu-system-x86_64 -enable-kvm -cpu host -m 3072 -smp 4 -cdrom crimson-game.iso -boot d \
        -drive file=quake.img,format=raw,if=ide -vga std \
        -device intel-hda -device hda-duplex -device e1000e,netdev=n0 -netdev user,id=n0

## Sound

Not working yet: the image has no `dsound.dll`, and the waveOut fallback finds
no audio device even with QEMU's `intel-hda`. `dsound.dll` is now on the
Game OS donor list; why no endpoint appears (Audiosrv not running, or the HDA
driver not binding) still needs checking.
