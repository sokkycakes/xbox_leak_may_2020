# push-hudfix

Phantom Dust's HUD and menu textures flickered and drew garbled on the Pi.
This renderer compares HUD vertices whole (texture coordinates and colors
included), and queued drawing waits on the game's fences. Built from
xbcompat/ at commit a9a6879a.

| file | md5 |
|---|---|
| libxbcompat_renderer.so.1 | 9dca8230e6f8c27aec5edd8d1bb0ab79 |
| xbcompat-native.x86 | c6052ab87f464e39891840fa3dafdf9f |
| push-vblank.sh | dd25777b766a67d5d9da536667a3795d (unchanged) |
| vblank.sh | 28cea926a04fcafd0b3b0d305dc863e0 (unchanged) |
| S02cpufreq | c385fd2c08ee04ef2c1b75b8f369b8d0 (unchanged) |
| clocks.sh | 65d695544dda3324b349b2480e371bd7 (unchanged) |

Install from the PC: `PI=192.168.1.128 sh push-vblank.sh` (report in
vblank.txt). Undo: `PI=192.168.1.128 sh push-vblank.sh undo`.
/etc/default/xbox stays as it is.
