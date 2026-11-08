# push-trigger6

Linux driver for sokky's j5create JUA365 (USB 3.0 to dual HDMI, MCT Trigger 6
chip, USB 0711:5601). Source and notes: bootani/sion/drivers/trigger6 in the repo.

- trigger6.ko: built from vanilla 6.18.7 + the Sion fragments (vermagic
  `6.18.7 SMP preempt mod_unload`). All 119 symbols it needs are built into
  that kernel (trigger6.undef; check with the same comm as push-usbaudio).
- t6-pattern: static test-pattern tool (`t6-pattern /dev/dri/cardN <port 0|1> [seconds]`).
- t6-mirror + S99t6mirror: shows the main screen on the adapter (720x480p,
  30 fps, `-r`/`-m`/`-a` via /etc/default/t6-mirror T6_MIRROR_ARGS). Reads
  the main card's scanout buffer: GPU import through EGL (any tiling), else
  CPU for linear / X / Y tiled. Buffers are tracked by dma-buf identity, not
  framebuffer ID (IDs get reused; caching by ID made an old picture flicker).
  Runs whenever an adapter is present; log /var/log/t6-mirror.log;
  `touch /etc/sion/t6-mirror.off` keeps it off. Started at boot via
  /etc/sion/late-start.
- trigger6.ko (2026-11-08 b): stops waiting for frame-done reports after 3
  misses (the JUA365 sends nothing on its interrupt endpoint, which capped
  output at ~10 fps); `slots=` parameter; logs the first interrupt reports.
- sion-install.sh: stops any running mirror, copies everything, adds the module to modules.dep and
  sion-extra.conf, persists, loads it, lists the adapter's ports.
  TEST=1 also draws colour bars on each connected port for 10 s (skipped
  while a game is running). Nothing is restarted; the Chromebox HDMI stays card0.

Undo: /etc/init.d/S99t6mirror stop; remove S99t6mirror from /etc/sion/late-start; rmmod trigger6; remove the trigger6 lines from
/etc/modules-load.d/sion-extra.conf and modules.dep (backup modules.dep.pre-trigger6).
