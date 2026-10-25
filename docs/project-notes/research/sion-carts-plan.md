# Carts, runtimes and saves on the Sion image

Written 2026-10-08 against `bootani/buildroot` on branch `claude/project-thread-abzdqq` (`sion_bootani_defconfig`, kernel 6.18.7 built from `x86_64_defconfig` plus `board/{common,sion}/linux.fragment`). Background: [kazeta-and-kazeta-plus.md](kazeta-and-kazeta-plus.md).

## Recommendation: use Kazeta's formats as they are

Keep `.kzi` (cart info), `.kzr` (runtime image) and `.kzp` (packaged cart) unchanged, along with the `.kazeta/share/run` entry point. That way sokky's carts work on Kazeta and on Sion, the KZI Cartridge Generator works, and Kazeta's emulator runtimes have a chance of running as they are. Sion-only behavior goes in extra keys that Kazeta ignores.

## What's already in place

- glibc toolchain (Bootlin x86_64 glibc stable), so glibc runtimes can run.
- `CONFIG_BLK_DEV_LOOP`, `EXT4_FS`, `VFAT_FS`, `ISO9660_FS` + `JOLIET`, `USB_STORAGE`, `SATA_AHCI` and `IA32_EMULATION` (32-bit processes, which xbcompat needs), all from x86_64_defconfig.
- BusyBox `mount` already handles `-o loop` and `-t overlay`. eudev is in the image.
- Mesa iris GL/EGL/GLES and SDL2 KMSDRM.

## What's missing

### 1. Kernel (add to `board/common/linux.fragment`)
```
CONFIG_OVERLAY_FS=y          # cart + runtime + save layer
CONFIG_EROFS_FS=y            # .kzr and .kzp images
CONFIG_EROFS_FS_ZIP=y        # lz4 compression
CONFIG_EROFS_FS_ZIP_LZMA=y   # Kazeta+ runtimes switched to lzma in 2026-02
CONFIG_EXFAT_FS=y            # SD cards and sticks over 32 GB come formatted exFAT
CONFIG_FUSE_FS=y             # AppImage-based runtimes (Kazeta's RetroArch ones)
CONFIG_UDF_FS=y              # later: Blu-ray/DVD carts on a USB drive
```
Only if the Sion gets used with an SD reader: `CONFIG_MMC`, `MMC_BLOCK`, `MMC_SDHCI`, `MMC_SDHCI_PCI`.

### 2. Persistent storage (the real gap)
The root filesystem is an initramfs, so `/var` and everything else is RAM and gets wiped at power-off. Saves need a real disk, and it **has to be ext4**: overlayfs can't use FAT or exFAT as an upper layer, because it needs xattrs and whiteout files.

Default pick: a **second partition on the boot stick**, ext4, labelled `SIONDATA`.
- genimage adds the partition, and a first-boot step runs `resize2fs` to grow it to fill the stick.
- Mount it at `/data`:
  - `/data/saves/default/<Id>/`: overlay upper per cart (E: for Xbox titles)
  - `/data/runtimes/`: runtimes installed on the console (Kazeta+'s lookup order)
  - `/data/cache/<Id>/`: Xbox Z: utility drive, LRU of 3
  - `/data/run/`: work and mount points
- Later option: the CXI3's internal M.2 SATA SSD (AHCI is already built in). That frees the stick, but it means wiping whatever is on the SSD.

BusyBox options: `e2fsprogs` (`mke2fs`, `e2fsck`, `resize2fs`). Optionally `exfatprogs` to check carts.

### 3. The cart launcher (new package, about 150 lines of sh)
A POSIX-sh port of Kazeta's `/usr/bin/kazeta`, dropping gamescope, polkit and pipewire:
1. **Find the cart.** Scan removable partitions, skipping the boot stick (`theseus-stick` already records it), and mount each read-only at `/media/<dev>`. Look for `*.kzi`/`*.kzp` at depth 2 or less. Honour `SetAsDefaultGame=true`.
2. **Mount a `.kzp`** with `mount -t erofs -o loop,ro`.
3. **Resolve the runtime:** `/data/runtimes`, then `/usr/share/sion/runtimes`, then the cart's media, then inside the `.kzp`, then `none.kzr`.
4. **Mount the stack:** `mount -t overlay -o lowerdir=<cart>:<runtime>,upperdir=/data/saves/default/<Id>,workdir=...,xino=off`. Keep `xino=off`: it's upstream's fix for 32-bit games, and xbcompat is 32-bit.
5. **Hand over the display.** Theseus has to exit or let go of the display first, the same way bootani does today. Then run `HOME=<merged> ./.kazeta/share/run /tmp/cart-exec` with `SDL_VIDEODRIVER=kmsdrm`, `KMS_MODE` and `AUDIODEV` already set by theseus-session.
6. **On exit:** unmount, `sync`, and return to Theseus.

Hook it into `theseus-session`'s loop the way Kazeta+ does it. Theseus writes `/var/run/sion/launch`, exits, and the loop runs the cart and then restarts Theseus. At boot, an inserted cart either autoboots or shows in the dashboard (Kazeta+'s `autoboot` setting).

### 4. Runtimes Sion can actually run
| Runtime | Contents | Notes |
|---|---|---|
| `none` | empty | static or self-contained native games |
| `xbox` | xbcompat + **its own i686 glibc, SDL2 (KMSDRM, ALSA), Mesa iris and libdrm** | Buildroot can't do multilib, so build an i686 sysroot separately and pack it into the `.kzr`. The game runs `Exec=default.xbe`. D:=cart, E:=save layer, Z:=`/data/cache/<Id>` |
| emulators | RetroArch + one core | Kazeta's AppImages need FUSE (or `APPIMAGE_EXTRACT_AND_RUN=1`). RetroArch has its own KMS context driver, so no gamescope is needed. Or build our own RetroArch on SDL2 KMSDRM |
| `windows` | later: gamescope + Xwayland + Proton + Mesa ANV | Needs `CONFIG_USER_NS` for bubblewrap and seat handling |

Building the `.kzr` and `.kzp` images happens on the cloud side with `mkfs.erofs` (Buildroot's `host-erofs-utils`), since sokky doesn't build. A folder cart needs no tools at all: copy the files and the `.kzi` onto a stick.

### 5. Theseus side
Showing a "cart inserted" entry and writing the launch file. There's also an optional save manager (copy or delete `/data/saves/<Id>`, like Kazeta's BIOS).

## Order to do it in
1. Kernel fragment + `SIONDATA` partition + e2fsprogs. This is the fast path, mostly a kernel relink plus an image change.
2. Launcher with the `none` runtime and a test cart (for example a static SDL2 demo), verified in QEMU with a second virtual USB disk.
3. The `xbox` runtime (i686 sysroot + xbcompat), using the XBLA disc games as test carts.
4. Emulator runtime.
5. Windows, if ever.
