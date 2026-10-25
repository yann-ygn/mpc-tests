"""Exercise deployment guards against actual Meson-built ELF artifacts."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CHECKER = Path(__file__).resolve().parents[1] / "check_elf.py"
EXPORTS = CHECKER.parents[1] / "native-api/adapter/exports.map"


class ElfChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mpc-elf-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.binary = Path(os.environ["MPC_NATIVE_TEST_LIBRARY"])
        self.stamp = self.directory / "verified"

    def run_check(self, *options, valid=False):
        result = subprocess.run(
            [
                sys.executable,
                str(CHECKER),
                "--binary",
                str(self.binary),
                "--stamp",
                str(self.stamp),
                *options,
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode == 0, valid, result.stderr)
        self.assertEqual(self.stamp.exists(), valid)
        return result.stderr

    def test_accepts_the_adapter_contract(self):
        self.run_check("--exports", str(EXPORTS), "--soname", "libmpc_native.so", valid=True)

    def test_rejects_unexpected_exports(self):
        allowlist = self.directory / "exports.map"
        allowlist.write_text("{ local: *; };\n")
        self.assertIn("Export allowlist mismatch", self.run_check("--exports", str(allowlist)))

    def test_rejects_wrong_soname(self):
        self.assertIn("Expected SONAME", self.run_check("--soname", "wrong.so"))

    def test_rejects_missing_dependency_and_runpath(self):
        self.assertIn("Missing shared-library dependency", self.run_check("--needed", "missing.so"))
        self.assertIn("Expected RUNPATH", self.run_check("--runpath", "$ORIGIN"))

    def test_rejects_host_binary_for_arm_deployment(self):
        self.assertIn("ARM hard-float", self.run_check("--armhf"))

    def test_rejects_newer_glibc(self):
        self.assertIn("Requires glibc newer", self.run_check("--glibc-max", "0.0"))

    def test_rejects_absolute_library_dependency(self):
        patched = self.directory / "nonportable.so"
        data = self.binary.read_bytes()
        self.assertIn(b"libc.so.6\0", data)
        patched.write_bytes(data.replace(b"libc.so.6\0", b"/lib.so.6\0"))
        self.binary = patched
        self.assertIn("Nonportable shared-library", self.run_check())


if __name__ == "__main__":
    unittest.main()
