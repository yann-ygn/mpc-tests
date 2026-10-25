"""Failure-path checks for the device experiment's restoration and evidence."""

import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from mpc_flag_semantics import Matrix, STATE_KEYS


class RestorationTests(unittest.TestCase):
    def matrix(self):
        matrix = Matrix.__new__(Matrix)
        before = dict.fromkeys(STATE_KEYS)
        before.update(selectedTrack={"index": 0}, tracks=[{"type": "plugin"}])
        flags = [{"requested": {"mute": False}}]
        matrix.select = Mock()
        matrix.targets = Mock(return_value=[(1, 2), (3, 4)])
        matrix.snapshot = Mock(return_value=copy.deepcopy(before))
        matrix.independent = Mock(return_value=flags)
        matrix.restore = Mock()
        return matrix, flags

    def test_readback_failure_restores_and_retains_partial_evidence(self):
        matrix, flags = self.matrix()
        matrix.set = Mock(return_value={"ok": True, "nativeInvoked": True})
        matrix.settle = Mock(side_effect=RuntimeError("audio copy did not converge"))
        rows = []
        with self.assertRaisesRegex(RuntimeError, "did not converge"):
            matrix.row(0, "mute", True, rows)
        matrix.restore.assert_called_once_with(flags, 0)
        self.assertTrue(rows[0]["set"]["nativeInvoked"])
        self.assertTrue(rows[0]["restored"])

    def test_repeated_native_invocation_is_a_failure_and_still_restores(self):
        matrix, flags = self.matrix()
        matrix.set = Mock(return_value={"ok": True, "nativeInvoked": True, "changed": True})
        matrix.settle = Mock(return_value=flags)
        rows = []
        with self.assertRaisesRegex(RuntimeError, "Already-desired"):
            matrix.row(0, "mute", True, rows)
        matrix.restore.assert_called_once_with(flags, 0)
        self.assertIn("repeat", rows[0])

    def test_restoration_mismatch_cannot_report_a_successful_row(self):
        matrix, _ = self.matrix()
        matrix.set = Mock(return_value={"ok": False, "error": "VALUE_REJECTED"})
        after = matrix.snapshot.return_value.copy()
        after["selectedTrack"] = {"index": 1}
        matrix.snapshot.side_effect = [
            matrix.snapshot.return_value,
            matrix.snapshot.return_value,
            after,
        ]
        rows = []
        with self.assertRaisesRegex(RuntimeError, "Full state differs"):
            matrix.row(0, "mute", True, rows)
        self.assertFalse(rows[0]["restored"])
        self.assertEqual(rows[0]["constraint"], "VALUE_REJECTED")

    def test_restoration_error_retains_the_final_observation(self):
        matrix, _ = self.matrix()
        matrix.set = Mock(return_value={"ok": False, "error": "VALUE_REJECTED"})
        matrix.restore.side_effect = RuntimeError("peer arm could not be restored")
        rows = []
        with self.assertRaisesRegex(RuntimeError, "peer arm"):
            matrix.row(0, "mute", True, rows)
        self.assertIn("after", rows[0])


if __name__ == "__main__":
    unittest.main()
