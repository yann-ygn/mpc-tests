"""Reproducible payload and shared-installer integration in scratch systemd trees."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest

from test_describe import ROOT

spec = importlib.util.spec_from_file_location("mpc_package", ROOT / "native-api/package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mpc-package-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.installer = (
            Path(os.environ["MPC_VST_PLUGINS"]) / "tools/release/addin"
            if "MPC_VST_PLUGINS" in os.environ
            else package.DEFAULT_INSTALLER
        )
        if not (self.installer / "install.sh").is_file():
            self.skipTest("Shared addin installer checkout absent; set MPC_VST_PLUGINS")
        artifacts = self.root / "artifacts"
        artifacts.mkdir()
        # Only the shared installer's ELF-header admission is under test here.
        header = b"\x7fELF\x01\x01\x01" + bytes(9) + b"\x03\x00\x28\x00"
        (artifacts / "mpc_native_host.so").write_bytes(header + bytes(64))
        (artifacts / "mpc-api").write_bytes(Path(os.environ["MPC_API_TEST_BINARY"]).read_bytes())
        self.payload = self.root / "mpc-native-api-0.1.0"
        self.payload.mkdir()
        package.stage(self.payload, artifacts, self.installer, {"version": "0.1.0"})

    def test_checksums_and_reproducible_archive(self):
        for line in (self.payload / "SHA256SUMS").read_text().splitlines():
            expected, name = line.split("  ")
            self.assertEqual(
                expected, hashlib.sha256((self.payload / name).read_bytes()).hexdigest()
            )
        one, two = self.root / "one.tar.gz", self.root / "two.tar.gz"
        package.archive(self.payload, one, 1000)
        for path in self.payload.iterdir():
            os.utime(path, (2000, 2000))
            path.chmod(0o700)
        package.archive(self.payload, two, 1000)
        self.assertEqual(one.read_bytes(), two.read_bytes())
        self.assertEqual(
            json.loads((self.payload / "PACKAGE.json").read_text())["version"], "0.1.0"
        )

    def test_install_upgrade_and_remove_preserve_other_addins_and_identity(self):
        unit = self.root / "system/usr/lib/systemd/system/acvs.service"
        unit.parent.mkdir(parents=True)
        base = "/usr/lib/firmware.so:/data/mpc-addins/privtap/privtap.so:/data/mpc-addins/remote/remote.so"
        unit.write_text(f"[Service]\nEnvironment=LD_PRELOAD={base}\n")
        target = self.root / "addins/mpc-native-api"
        log = self.root / "systemctl.log"
        mounts = self.root / "mounts"
        mounts.write_text("/dev/root / ext4 rw,relatime 0 0\n")
        # The shared installer calls sync. This scratch-tree fixture needs no
        # disk durability and must not wait for unrelated host-wide writeback.
        test_bin = self.root / "bin"
        test_bin.mkdir()
        sync = test_bin / "sync"
        sync.write_text("#!/bin/sh\nexit 0\n")
        sync.chmod(0o755)
        env = {
            **os.environ,
            "PATH": str(test_bin) + os.pathsep + os.environ["PATH"],
            "ADDIN_INSTALL_TEST": "1",
            "SYSTEMD_ROOT": str(self.root / "system"),
            "ADDIN_TEST_LOG": str(log),
            "ADDIN_MOUNTS": str(mounts),
        }
        shell = [shutil.which("busybox"), "sh"] if shutil.which("busybox") else ["sh"]

        def run(script, *args):
            return subprocess.run(
                [*shell, str(script), "-y", "-t", str(target), *args],
                env=env,
                check=True,
                capture_output=True,
            )

        run(self.payload / "install.sh", "-n")
        self.assertIn(f"LD_PRELOAD={base}:{target}/mpc_native_host.so", unit.read_text())
        self.assertNotIn("restart", log.read_text())
        self.assertEqual(stat.S_IMODE((target / "mpc-api").stat().st_mode), 0o755)
        (target / "mpc-native-api.conf").write_text("enabled=0\n")
        (target / "device-id").write_text("persistent-identity")
        run(self.payload / "install.sh")
        self.assertEqual((target / "mpc-native-api.conf").read_text(), "enabled=0\n")
        self.assertEqual(unit.read_text().count(str(target / "mpc_native_host.so")), 1)
        self.assertEqual(log.read_text().count("restart acvs"), 1)
        run(target / "uninstall.sh")
        self.assertEqual(unit.read_text(), f"[Service]\nEnvironment=LD_PRELOAD={base}\n")
        self.assertEqual((target / "device-id").read_text(), "persistent-identity")
        self.assertFalse((target / "mpc_native_host.so").exists())
        self.assertEqual(log.read_text().count("restart acvs"), 2)


if __name__ == "__main__":
    unittest.main()
