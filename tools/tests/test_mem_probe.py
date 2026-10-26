"""Read real memory through the built diagnostic helper; exercise bounded gathers."""

import ctypes
import os
import subprocess
import unittest


class MemoryProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = os.environ["MPC_MEMORY_TEST_PROBE"]
        libc = ctypes.CDLL(None, use_errno=True)
        # Only this short-lived fixture process allows its read-only helper to
        # observe it under Linux Yama. The instrument's policy is never changed.
        if libc.prctl(0x59616D61, ctypes.c_ulong(-1), 0, 0, 0):
            raise OSError(ctypes.get_errno(), "test fixture ptrace permission")
        cls.buffer = ctypes.create_string_buffer(b"native independent reader")
        cls.address = ctypes.addressof(cls.buffer)

    def run_probe(self, *arguments):
        return subprocess.run(
            [self.probe, str(os.getpid()), "read", *map(str, arguments)],
            capture_output=True,
            check=False,
        )

    def test_single_and_disjoint_repeated_ranges_preserve_exact_order(self):
        result = self.run_probe(f"{self.address:x}", 6)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"native")
        result = self.run_probe(
            f"{self.address + 19:x}",
            6,
            f"{self.address:x}",
            6,
            f"{self.address + 19:x}",
            6,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"readernativereader")

    def test_invalid_ranges_and_aggregate_overflow_fail_before_output(self):
        address = f"{self.address:x}"
        for arguments in (
            (address, 0),
            (address, -1),
            (address, "2x"),
            ("xyz", 1),
            ("ffffffffffffffff", 1),
            (address, 8 * 1024 * 1024 + 1),
            (address, 8 * 1024 * 1024, address, 1),
            (address, 1, address),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_probe(*arguments)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, b"")

    def test_unmapped_read_is_an_error_without_a_successful_capture(self):
        result = self.run_probe("1", 1)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
