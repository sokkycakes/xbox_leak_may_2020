#!/usr/bin/env python3
"""Exercise the real card service against fake sysfs and mount commands."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "tools/sion/cards/sion-cards"
MOCK = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
root = Path(os.environ["CARD_TEST_ROOT"])
cmd = Path(sys.argv[0]).name
args = sys.argv[1:]
with (root / "calls").open("a") as f:
    f.write(json.dumps([cmd] + args) + "\n")
mounts = root / "mounts"
if cmd == "mount":
    assert args[:2] == ["-o", "ro,noatime"], args
    dev, dest = args[2:]
    if (root / ("fail-" + Path(dev).name)).exists():
        sys.exit(1)
    with mounts.open("a") as f:
        f.write(f"{dev} {dest} vfat ro,noatime 0 0\n")
elif cmd == "umount":
    assert args[0] == "-l", args
    mounts.write_text("".join(x for x in mounts.read_text().splitlines(True)
                             if x.split()[1] != args[1]))
elif cmd == "blkid":
    label = root / ("label-" + Path(args[0]).name)
    if label.exists():
        print(f'{args[0]}: LABEL="{label.read_text()}" UUID="example"')
'''

class CardsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cards-test-")
        self.root = Path(self.tmp.name)
        for name in ("sys", "bin", "media", "state", "devices"):
            (self.root / name).mkdir()
        for name in ("mounts", "mountinfo", "calls"):
            (self.root / name).touch()
        for cmd in ("mount", "umount", "blkid", "logger"):
            p = self.root / "bin" / cmd
            p.write_text(MOCK)
            p.chmod(0o755)
        self.env = dict(os.environ, CARD_TEST_ROOT=str(self.root))
        self.env["PATH"] = str(self.root / "bin") + ":" + os.environ["PATH"]
        for key, rel in {"MEDIA":"media", "SYS":"sys", "MOUNTS":"mounts",
                         "MOUNTINFO":"mountinfo", "STATE":"state", "PIDFILE":"pid"}.items():
            self.env["SION_CARDS_" + key] = str(self.root / rel)

    def tearDown(self):
        self.tmp.cleanup()

    def disk(self, name="sda", size=100, seq=1, partition=True, bus="usb1", number="8:0"):
        p = self.root / "devices" / bus / name
        p.mkdir(parents=True)
        for key, value in {"size": size, "diskseq": seq, "dev": number}.items():
            (p / key).write_text(str(value))
        (self.root / "sys" / name).symlink_to(p)
        if partition:
            part = p / (name + ("p1" if name.startswith("mmc") else "1"))
            part.mkdir()
            (part / "size").write_text(str(size - 1))
            (part / "partition").write_text("1")
            major, minor = number.split(":")
            (part / "dev").write_text(f"{major}:{int(minor) + 1}")
            (self.root / "sys" / part.name).symlink_to(part)
        return p

    def poll(self):
        subprocess.run(["sh", str(SCRIPT), "--once"], env=self.env, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10)

    def calls(self, cmd):
        return [x for line in (self.root / "calls").read_text().splitlines()
                if (x := json.loads(line))[0] == cmd]

    def mounted(self):
        return [line.split()[0] for line in (self.root / "mounts").read_text().splitlines()]

    def test_partition_insert_remove_and_reinsert(self):
        disk = self.disk()
        self.poll()
        self.assertEqual(self.mounted(), ["/dev/sda1"])
        (disk / "size").write_text("0")  # reader and partition still exist
        self.poll()
        self.assertEqual(self.mounted(), [])
        (disk / "size").write_text("100")
        (disk / "diskseq").write_text("2")
        self.poll()
        self.assertEqual(self.mounted(), ["/dev/sda1"])

    def test_same_name_same_size_swap_has_empty_interval(self):
        disk = self.disk()
        self.poll()
        (disk / "diskseq").write_text("2")
        self.poll()
        self.assertEqual(self.mounted(), [])
        self.poll()
        self.assertEqual(self.mounted(), ["/dev/sda1"])
        self.assertEqual(len(self.calls("mount")), 2)

    def test_disconnected_usb_is_unmounted(self):
        self.disk()
        self.poll()
        (self.root / "sys/sda1").unlink()
        (self.root / "sys/sda").unlink()
        self.poll()
        self.assertEqual(self.mounted(), [])

    def test_new_device_name_swap_has_empty_interval(self):
        self.disk()
        self.poll()
        (self.root / "sys/sda1").unlink()
        (self.root / "sys/sda").unlink()
        self.disk("sdb", number="8:16")
        self.poll()
        self.assertEqual(self.mounted(), [])
        self.poll()
        self.assertEqual(self.mounted(), ["/dev/sdb1"])

    def test_empty_reader_and_whole_device_card(self):
        disk = self.disk(size=0, partition=False)
        self.poll()
        self.assertEqual(self.calls("mount"), [])
        (disk / "size").write_text("100")
        self.poll()
        self.assertEqual(self.mounted(), ["/dev/sda"])

    def test_protect_root_data_and_system_labels(self):
        self.disk("mmcblk0", bus="mmc0", number="179:0")
        self.disk("sda")
        self.disk("sdb", number="8:16")
        self.disk("sdc", number="8:32")
        self.disk("sdd", number="8:48")
        (self.root / "mountinfo").write_text(
            "1 0 179:1 / / rw - ext4 /dev/root rw\n"
            "2 0 8:17 / /data rw - ext4 /dev/sdb1 rw\n")
        (self.root / "label-sda1").write_text("XBOXBOOT")
        (self.root / "label-sdc1").write_text("SIONBOOT")
        self.poll()
        self.assertEqual(self.mounted(), ["/dev/sdd1"])
        self.assertEqual(self.calls("umount"), [])

    def test_ignore_internal_and_optical_drives(self):
        self.disk("nvme0n1", bus="pci0000", partition=False)
        self.disk("sr0", partition=False)
        self.poll()
        self.assertEqual(self.calls("mount"), [])

    def test_mount_failure_retries_new_media_only(self):
        disk = self.disk()
        fail = self.root / "fail-sda1"
        fail.touch()
        self.poll()
        self.poll()
        self.assertEqual(len(self.calls("mount")), 1)
        fail.unlink()
        (disk / "diskseq").write_text("2")
        self.poll()
        self.assertEqual(self.mounted(), ["/dev/sda1"])

    def test_transient_mount_failure_retries_after_backoff(self):
        self.disk()
        fail = self.root / "fail-sda1"
        fail.touch()
        self.poll()
        fail.unlink()
        for _ in range(5):
            self.poll()
        self.assertEqual(self.mounted(), ["/dev/sda1"])
        self.assertEqual(len(self.calls("mount")), 2)

    def test_failed_reader_retries_after_empty_without_diskseq(self):
        disk = self.disk()
        (disk / "diskseq").unlink()
        fail = self.root / "fail-sda1"
        fail.touch()
        self.poll()
        (disk / "size").write_text("0")
        self.poll()
        fail.unlink()
        (disk / "size").write_text("100")
        self.poll()
        self.assertEqual(self.mounted(), ["/dev/sda1"])

if __name__ == "__main__":
    unittest.main()
