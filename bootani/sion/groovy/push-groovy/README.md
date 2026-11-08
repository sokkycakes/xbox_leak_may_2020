# push-groovy: mirror the Sion to a MiSTer (Groovy core)

Test kit, nothing is installed. `sion-groovy` reads the plane the Sion is
scanning out, shrinks it to 720x480 on the GPU, LZ4-compresses it and
streams it to the MiSTer's Groovy core (UDP 32100), which puts it out as
native 480i on the MiSTer's analog port. The Sion's own HDMI output is not
touched.

MiSTer side: Groovy core installed (MiSTer_groovy in /media/fat, Groovy.rbf
in _Utility, `[Groovy] main=MiSTer_groovy` in MiSTer.ini), loaded, on wired
Ethernet. Composite/S-video needs the MiSTer YC cable and YC mode on;
otherwise it comes out as 15 kHz RGB.

Sion side (root): `sh run.sh <mister-ip> --test` first (colour bars and a
moving white box), then `sh run.sh <mister-ip>` to mirror the screen.
Ctrl+C stops it.

Bandwidth: a dash frame compresses to about 460 KB, so 60 fps needs about
220 Mbit/s: wired gigabit. `--rgb565` halves it; Wi-Fi will drop frames.
Latency: about one frame plus the network.
