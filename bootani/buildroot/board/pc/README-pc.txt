Crimson generic PC test stick
=============================

The Sion's system (Xbox boot animation, then the dashboard under xbcompat)
built to start on any 64-bit UEFI PC, to see how it behaves there.

Booting
- Plug the stick in and pick it in the PC's boot menu (F12, F11, F8 or Esc
  at power on, depending on the maker). It needs UEFI boot; Secure Boot must
  be off (the image isn't signed).
- Nothing on the PC's own disks is changed. The system runs from RAM; only
  this stick is written.
- Graphics: Intel (i915/xe), AMD (amdgpu, older Radeons with radeon) and
  NVIDIA (nouveau, open driver: slow on newer cards) have drivers. With none
  of those, the firmware's framebuffer is used and the GPU is done in
  software (slow, but you get a picture).

What it writes here (read it on any PC)
- sion/logs/hardware/boot-NNN/  one folder per boot:
    summary.txt  the machine, CPU, GPU and the driver it got, screens,
                 sound cards, network, pads, missing firmware, and whether
                 the dashboard is running
    dmesg.txt, pci.txt, usb.txt, drm.txt, sound.txt, net.txt, input.txt,
    cpu.txt, and logs/ (xbox.log is the dashboard's own log)
  Written 20 s after boot, again a minute later, and when it shuts down.
- sion/logs/network.txt  the addresses it got (Wi-Fi or Ethernet).

Settings: sion/sion.conf (Wi-Fi, password, name; this stick is
crimson-test.local). Put sion.conf DASHBOARD=theseus there to run Theseus.
