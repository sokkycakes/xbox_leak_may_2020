This folder sets up remote access to the Sion.

  sion.conf          Wi-Fi network, root password, machine name
  authorized_keys    SSH public keys allowed to log in as root (optional),
                     one per line, e.g. the contents of id_ed25519.pub
  logs\network.txt   the machine's addresses, written when it gets one
  logs\update.txt    what happened to each image update

Once it's on the network:

  ssh root@sion.local                      a shell
  scp -r mydir root@sion.local:/opt/       copy files in (they last until
                                           reboot; the system runs from RAM)
  ssh root@sion.local sion persist /opt/x  keep them across reboots
  ssh root@sion.local sion restart dashboard   restart the dashboard

The dashboard is the Xbox's own, run by xbcompat (DASHBOARD="xbox" in
sion.conf), or Theseus (DASHBOARD="theseus"). Its hard disk is /data/xbox/hdd
(E: is partition1, with saves and E:\Games); put an extracted game disc in
/data/xbox/disc to have it in the tray. Its log: sion logs xbox.

Updating the whole system image (a new BOOTX64.EFI):

  scp BOOTX64.EFI root@sion.local:/data/
  ssh root@sion.local sion update /data/BOOTX64.EFI

The new image boots once on trial. It's kept if it comes up; if it panics,
hangs or fails to start SSH and the dashboard, the Sion reboots into the
image it had before (kept as EFI\sion\previous.efi). "sion rollback" goes
back by hand. "sion help" lists everything.

If the Sion can't boot at all, copy EFI\sion\previous.efi over
EFI\BOOT\BOOTX64.EFI on this stick from a PC.
