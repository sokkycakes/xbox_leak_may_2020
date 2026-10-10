# Pi game-card hotplug

Validated 2026-10-10 with the running Dashboard and native ARM renderer.

## Cause and changes

The Pi/ARM branch of xbcompat.mk did not install sion-cards or S35cards.
The running Pi had neither service, and /media was empty even though Linux
could see the USB card. The x86/Sion image did install them.

The service also depended on Sion-only helpers and util-linux blkid options
that the Pi's BusyBox build does not provide. Its removal check only tested
whether a device existed in sysfs. A connected reader can remain there with
zero capacity after its card is removed.

The shared service now works without Sion-specific dependencies and is
installed on both image variants. It:

- Mounts USB/SD media read-only, using mount's filesystem detection.
- Protects disks with mounted system partitions (including /dev/root,
  identified through mountinfo's major/minor numbers) or Xbox/Sion boot/data
  labels. Internal drives and optical drives are excluded.
- Detects missing devices, zero-capacity readers, and changed media identities
  using diskseq, device numbers, and disk/partition sizes.
- Drops stale mounts and leaves one service interval before mounting a
  replacement, including a replacement assigned a different device name.
  This lets the Dashboard see an empty slot even when cart filenames match.
- Retries failed mounts after approximately ten seconds, or immediately when
  media identity changes, rather than suppressing that reader indefinitely.

The service checks every two seconds. The Dashboard checks CARD0 roughly
once per second, so updates take a few seconds rather than occurring instantly.
Media polling on the tested Pi already inherits the kernel's 2000 ms default;
udev storage rules are installed. Its real card diskseq changed from 30 to 35
during the session, and the service logged removal/replacement and remount.
No kernel polling setting was changed.

## Validation

Ten isolated tests execute the actual shell service against fake sysfs and
mount commands. They cover insertion, persistent-reader removal/reinsertion,
whole-device cards, disconnected USB devices, same-name and different-name
replacement, system-disk protection, internal/optical exclusion, and recovery
from both changed-media and transient mount failures.

Run them with:

    python3 xbcompat/tests/cards_test.py

On the Pi, /dev/sda1 mounted at /media/sda1 as read-only FAT and exposed
arcade.kzi and dolphin.kzi. The installed XBE contains the Games polling code;
its live last-poll timestamp advanced along with the Dashboard clock.

With the Dashboard process kept running, briefly pausing the mount service
and unmounting the read-only card changed the live Games collection signature
from 40|dolphin.kzi|arcade.kzi| to 40|. Resuming the service remounted the card,
and the signature returned to 40|dolphin.kzi|arcade.kzi|. This signature is
updated by Scan(), confirming a rescan rather than just host mount detection.
No Dashboard binary or script changes were needed.

The service is installed at /usr/sbin/sion-cards and enabled by
/etc/init.d/S35cards on the Pi. It remains running; card mounting will also
start at the next boot. The Dashboard and its approved sound boost were not
restarted during this work.

The scripted live test verified removal and reinsertion of the mounted card.
A full physical swap to a different game card and the details-screen
transition still need user confirmation. Multiple simultaneously inserted
cards with identical cart filenames are not a new selection protocol: CARD0
continues to resolve the first card found, as before.
