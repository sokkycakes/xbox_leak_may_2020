#!/usr/bin/env python3
"""Check map identity and failed-scan recovery without executing Xbox code."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import xbrun

class MapCacheTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="map-cache-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / "default.xbe"
        self.image.write_bytes(b"XBEH test title")
        self.binary = self.root / "runtime"
        self.binary.write_bytes(b"\x7fELF host table one")
        self.cli = self.root / "scanner"
        self.cli.write_bytes(b"scanner one")
        self.adapter = self.root / "xbsymmap.py"
        self.adapter.write_text("adapter one")
        self.cache = self.root / "cache"
        self.calls = 0
        self.mode = "success"
        self.patch(mock.patch.object(xbrun, "CACHE", str(self.cache)))
        self.patch(mock.patch.object(xbrun, "HERE", str(self.root)))
        self.patch(mock.patch.object(xbrun, "xdk_build", return_value=5849))
        self.patch(mock.patch.object(xbrun, "xbsymdb_cli", return_value=str(self.cli)))
        self.patch(mock.patch.object(xbrun, "run", side_effect=self.scan))
        self.patch(mock.patch.dict(os.environ, {"XBCOMPAT_HLE_BINARY": str(self.binary),
                                                "XBCOMPAT_BIN": "/shell/wrapper"}))

    def patch(self, patcher):
        patcher.start()
        self.addCleanup(patcher.stop)

    def scan(self, *args):
        self.calls += 1
        self.assertEqual(args[4], str(self.binary))
        if self.mode == "empty":
            return
        Path(args[-1]).write_text("_Test@0 0x11000\n")
        if self.mode == "failure":
            raise RuntimeError("interrupted scanner")

    def prepare(self):
        return Path(xbrun.make_map(str(self.image)))

    def test_cache_reused_but_rebuilt_for_runtime_scanner_and_adapter_changes(self):
        original = self.prepare()
        self.assertEqual(self.prepare(), original)
        self.assertEqual(self.calls, 1)
        for source in (self.binary, self.cli, self.adapter):
            source.write_bytes(source.read_bytes() + b" changed")
            replacement = self.prepare()
            self.assertNotEqual(replacement, original)
            original = replacement
        self.assertEqual(self.calls, 4)

    def test_failed_scan_does_not_publish_partial_map_and_can_retry(self):
        self.mode = "failure"
        with self.assertRaisesRegex(RuntimeError, "interrupted"):
            self.prepare()
        self.assertEqual(list(self.cache.iterdir()), [])
        self.mode = "success"
        self.assertTrue(self.prepare().is_file())

    def test_empty_scanner_result_rejected(self):
        self.mode = "empty"
        with self.assertRaisesRegex(RuntimeError, "empty HLE map"):
            self.prepare()
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_shell_wrapper_is_not_a_host_symbol_table(self):
        self.binary.write_bytes(b"#!/bin/sh\nexec box86 runtime\n")
        with self.assertRaisesRegex(RuntimeError, "runtime ELF"):
            self.prepare()
        self.assertEqual(self.calls, 0)

if __name__ == "__main__":
    unittest.main()
