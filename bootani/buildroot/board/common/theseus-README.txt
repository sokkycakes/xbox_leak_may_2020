This folder makes the stick a test loop for the Theseus dashboard.

At every boot the machine:
- copies everything under opt\ over /opt/theseus (drop a new build or
  changed Configs/Data here, laid out like /opt/theseus),
- reads theseus.env, a shell file, e.g.
      THESEUS_ARGS="--some-flag"
      KMS_MODE=1920x1080      (dashboard display mode; default 720x480,
                               the display's preferred mode if it lacks it)
      THESEUS_SCENE=640x480   (size the dashboard draws at before it is
                               stretched over the display; empty = the
                               display mode)
- writes logs\system.txt (displays and modes, input and USB devices, sound),
  then logs\theseus.log and logs\dmesg.txt every few seconds.

Leave the folder in place, even empty: the machine looks for it to find
the stick.
