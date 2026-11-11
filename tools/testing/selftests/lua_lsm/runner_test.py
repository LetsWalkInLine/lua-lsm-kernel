#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host-only regression tests for packaging and optional source metadata."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "lua_lsm_runner", Path(__file__).with_name("run-qemu.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTest(unittest.TestCase):
    def test_shallow_install_without_git(self):
        with patch.object(runner, "__file__", "/lua_lsm/run-qemu.py"), \
                patch.object(runner.subprocess, "run") as run:
            self.assertIsNone(runner.source_commit())
            run.assert_not_called()

    def test_install_inside_unrelated_repository(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / ".git").mkdir()
            with patch.object(runner, "__file__", str(root / "lua_lsm/run-qemu.py")), \
                    patch.object(runner.subprocess, "run") as run:
                self.assertIsNone(runner.source_commit())
                run.assert_not_called()

    def test_source_tree_metadata_is_optional(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / ".git").mkdir()
            script = root / "tools/testing/selftests/lua_lsm/run-qemu.py"
            with patch.object(runner, "__file__", str(script)), \
                    patch.object(runner.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, "abc123\n", "")
                self.assertEqual(runner.source_commit(), "abc123")
                run.return_value = subprocess.CompletedProcess([], 128, "", "not a repo")
                self.assertIsNone(runner.source_commit())
                run.side_effect = FileNotFoundError("git")
                self.assertIsNone(runner.source_commit())

    def test_network_with_or_without_busybox_ip(self):
        for applets in ("sh\nip\n", "sh\n"):
            with self.subTest(applets=applets), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                busybox = base / "busybox"
                busybox.write_bytes(b"busybox")
                ip = base / "ip"
                ip.write_bytes(b"iproute2")
                out = base / "out"
                out.mkdir()
                # Isolate host tools; exercise real filesystem/applet replacement.
                def run(argv, **kwargs):
                    output = "" if argv[0] == "ldd" else b"archive"
                    return subprocess.CompletedProcess(argv, 0, output, "")

                with patch.object(runner.subprocess, "check_output", return_value=applets), \
                        patch.object(runner.subprocess, "run", side_effect=run):
                    image = runner.make_image(out, "network", busybox, ip)
                self.assertTrue(image.is_file())
                installed_ip = out / "rootfs/bin/ip"
                self.assertFalse(installed_ip.is_symlink())
                self.assertEqual(installed_ip.read_bytes(), b"iproute2")
                self.assertTrue((out / "rootfs/bin/sh").is_symlink())


if __name__ == "__main__":
    unittest.main()
