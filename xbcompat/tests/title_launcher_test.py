#!/usr/bin/env python3
"""Test the Pi title launcher without executing Xbox code."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "bootani/buildroot/package/xbcompat/xbox-title"
PREPARE = ROOT / "xbcompat/tools/pi/cache-map.py"

class TitleLauncherTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="title-launch-")
        self.root = Path(self.temp.name)
        self.cache = self.root / "maps"
        self.cache.mkdir()
        self.binary = self.root / "mock runtime"
        self.binary.write_text('#!/usr/bin/env python3\nimport json,os,sys\n'
            'print(json.dumps({"args":sys.argv[1:],"launcher":os.environ["XBCOMPAT_LAUNCHER"]}))\n')
        self.binary.chmod(0o755)
        self.env = dict(os.environ, XBCOMPAT_BIN=str(self.binary), XBCOMPAT_MAP_DIR=str(self.cache))
        self.env.pop("XBCOMPAT_LAUNCHER", None)

    def tearDown(self):
        self.temp.cleanup()

    def image(self, dirname="game", name="default.xbe", content=b"XBEH title one"):
        path = self.root / dirname / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def cached_map(self, image):
        path = self.cache / (hashlib.sha1(image.read_bytes()).hexdigest() + ".map")
        path.write_text("_Direct3D_CreateDevice@24 0x11000\n")
        return path

    def launch(self, image, *args):
        return subprocess.run(["sh", str(LAUNCHER), str(image), *args], env=self.env,
                              capture_output=True, text=True, timeout=5)

    def test_exact_digest_with_arguments_and_spaced_paths(self):
        image = self.image("card with spaces")
        mapfile = self.cached_map(image)
        result = self.launch(image, "--hdd", "/data/game saves", "--d-path", r"\Device\GameCard0")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["args"], ["--hle", str(mapfile), "--hdd", "/data/game saves",
                                         "--d-path", r"\Device\GameCard0", str(image)])
        self.assertEqual(output["launcher"], str(LAUNCHER))

    def test_same_filename_different_executables_do_not_share_map(self):
        first = self.image("cardA")
        second = self.image("cardB", content=b"XBEH title two")
        self.cached_map(first)
        self.assertEqual(self.launch(first).returncode, 0)
        result = self.launch(second)
        self.assertEqual(result.returncode, 78)
        self.assertEqual(result.stdout, "")

    def test_missing_map_fails_before_runtime(self):
        result = self.launch(self.image())
        self.assertEqual(result.returncode, 78)
        self.assertIn("no HLE map", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_exact_image_sidecar(self):
        image = self.image(name="alternate.xbe")
        sidecar = Path(str(image) + ".map")
        sidecar.write_text("symbol 0x11000\n")
        self.assertEqual(json.loads(self.launch(image).stdout)["args"][1], str(sidecar))

    def test_bundled_default_map(self):
        image = self.image()
        sidecar = image.parent / "xbcompat.map"
        sidecar.write_text("symbol 0x11000\n")
        self.assertEqual(json.loads(self.launch(image).stdout)["args"][1], str(sidecar))

    def test_generic_map_not_used_for_alternate_image(self):
        image = self.image(name="alternate.xbe")
        (image.parent / "xbcompat.map").write_text("symbol 0x11000\n")
        self.assertEqual(self.launch(image).returncode, 78)

    def test_empty_map_rejected(self):
        image = self.image()
        self.cached_map(image).write_text("")
        self.assertEqual(self.launch(image).returncode, 78)

    def test_configured_launcher_preserved(self):
        image = self.image()
        self.cached_map(image)
        self.env["XBCOMPAT_LAUNCHER"] = "/custom/launcher"
        self.assertEqual(json.loads(self.launch(image).stdout)["launcher"], "/custom/launcher")

    def scanner(self, body):
        tool = self.root / "map tool.py"
        tool.write_text(body)
        self.env["XBCOMPAT_MAP_TOOL"] = str(tool)
        self.env["XBCOMPAT_MAP_PYTHON"] = "/usr/bin/python3"
        self.env["XBSYMDB_CLI"] = str(self.binary)

    def test_automatic_map_for_unprepared_title(self):
        image = self.image("new card")
        generated = self.root / "generated map"
        generated.write_text("symbol 0x11000\n")
        self.scanner("import sys\nassert sys.argv[1:]==" + repr(["--map-only", str(image)]) +
                     "\nprint(" + repr(str(generated)) + ")\n")
        result = self.launch(image, "--hdd", "/data/game saves")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["args"],
                         ["--hle", str(generated), "--hdd", "/data/game saves", str(image)])

    def test_failed_scanner_does_not_run_guest(self):
        self.scanner("import sys\nprint('scanner failed',file=sys.stderr)\nsys.exit(1)\n")
        result = self.launch(self.image())
        self.assertEqual(result.returncode, 78)
        self.assertEqual(result.stdout, "")
        self.assertIn("scanner failed", result.stderr)

    def test_nonexistent_generated_map_does_not_run_guest(self):
        self.scanner("print('/nonexistent/generated.map')\n")
        result = self.launch(self.image())
        self.assertEqual(result.returncode, 78)
        self.assertEqual(result.stdout, "")

    def test_kernel_only_title_needs_no_map(self):
        self.scanner("# No replaced libraries.\n")
        image = self.image()
        result = self.launch(image)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["args"], [str(image)])

    def test_map_preparation_uses_entire_file_digest(self):
        image = self.image()
        source = self.root / "source.map"
        source.write_text("symbol 0x11000\n")
        subprocess.run(["python3", str(PREPARE), str(image), "--map", str(source),
                        "--cache", str(self.cache)], check=True, capture_output=True, text=True)
        prepared = self.cache / (hashlib.sha1(image.read_bytes()).hexdigest() + ".map")
        self.assertEqual(prepared.read_bytes(), source.read_bytes())
        image.write_bytes(b"XBEH changed title")
        self.assertEqual(self.launch(image).returncode, 78)

if __name__ == "__main__":
    unittest.main()
