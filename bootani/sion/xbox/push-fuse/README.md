
Note (2026-10-21): an init.d script added by the overlay runs too late for
rcS, so S06fuse never ran at boot. push-wincart installs
/etc/modules-load.d/sion-extra.conf (plus modules.dep lines) instead, which
S11modules honours; S06fuse is kept only as a manual loader.
