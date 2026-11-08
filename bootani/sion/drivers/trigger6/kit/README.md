# push-trigger6

Linux driver for sokky's j5create JUA365 (USB 3.0 to dual HDMI, MCT Trigger 6
chip, USB 0711:5601). Source and notes: bootani/sion/drivers/trigger6 in the repo.

- trigger6.ko: built from vanilla 6.18.7 + the Sion fragments (vermagic
  `6.18.7 SMP preempt mod_unload`). All 119 symbols it needs are built into
  that kernel (trigger6.undef; check with the same comm as push-usbaudio).
- t6-pattern: static test-pattern tool (`t6-pattern /dev/dri/cardN <port 0|1> [seconds]`).
- sion-install.sh: copies both, adds the module to modules.dep and
  sion-extra.conf, persists, loads it, lists the adapter's ports.
  TEST=1 also draws colour bars on each connected port for 10 s (skipped
  while a game is running). Nothing is restarted; the Chromebox HDMI stays card0.

Undo: rmmod trigger6; remove the trigger6 lines from
/etc/modules-load.d/sion-extra.conf and modules.dep (backup modules.dep.pre-trigger6).
