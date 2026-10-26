"""Failure-path checks for the device experiment's restoration and evidence."""

import copy
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from mpc_flag_semantics import STATE_KEYS, Matrix
from mpc_screen_flags import ScreenMatrix


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


class ScreenMatrixTests(unittest.TestCase):
    def test_midi_index_after_audio_maps_to_the_canonical_command_target(self):
        matrix = ScreenMatrix.__new__(ScreenMatrix)
        matrix.ui_indices = {0: 0, 6: 8}
        matrix.identities = {
            "projectId": "1",
            "tracks": [{"index": i, "id": str(10 + i)} for i in range(9)],
        }
        matrix.instance = "run_" + "0" * 32
        submission = matrix.submission(6, "arm", True, False, request_id="drum")
        self.assertIn(" 1 18 drum ", submission)

    def test_selection_uses_track_list_order_when_audio_precedes_the_target(self):
        matrix = ScreenMatrix.__new__(ScreenMatrix)
        matrix.ui_indices = {0: 0, 6: 8}
        matrix.project = 100
        matrix.session = Mock()
        matrix.session.profile = {"offsets": {"project": {"selectedTrackIndex": 4}}}
        matrix.session.read.side_effect = [(6).to_bytes(4, "little"), (8).to_bytes(4, "little")]
        matrix.ui, matrix.screen = Mock(), Mock()
        matrix.select(6)
        self.assertEqual(matrix.ui.call_count, 3)
        matrix.ui.assert_called_with("/tap", x=770, y=770)
        matrix.screen.assert_called_once()


if __name__ == "__main__":
    unittest.main()
