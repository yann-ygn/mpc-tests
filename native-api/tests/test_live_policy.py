"""Run the production policy through the public transport and emulated live owners."""

import json
import socket
import time

import jsonschema

import test_wire_commands as wire_tests
from test_host import HostHarness


class LivePolicyTests(HostHarness):
    command = wire_tests.WireCommandTests.command
    send = wire_tests.WireCommandTests.send
    get = wire_tests.WireCommandTests.get
    receipt = wire_tests.WireCommandTests.receipt

    def setUp(self):
        super().setUp()
        self.launch("native-commands")
        self.target = self.public("get_state")["tracks"][0]["ref"]
        self.receipt_schema = json.loads(
            (wire_tests.ROOT / "native-api/schema/command-receipt.schema.json").read_text()
        )

    def test_all_operations_apply_with_qualified_live_sources(self):
        for alternate in (False, True):
            marker = self.install / "policy-arm-alternate"
            if alternate:
                marker.touch()
            for op in ("set_mute", "set_solo", "set_record_armed"):
                value = not alternate
                result = self.send(self.command(op + str(alternate), op=op, value=value))
                self.assertEqual(result["status"], "applied", result)
                self.assertTrue(result["nativeInvoked"])
                self.assertEqual(result["verification"]["requestedValue"], value)

    def test_qualified_screens_and_discovery(self):
        caps = self.public("describe")["capabilities"]
        for op in ("set_mute", "set_solo", "set_record_armed"):
            self.assertEqual(caps[op]["screens"], ["main", "mixer", "track_mute"])
            self.assertEqual(caps[op]["transportStates"], ["stopped", "sequence_playback"])
            self.assertIn("recording_off", caps[op]["conditions"])
            self.assertFalse(caps[op]["sampleAccurate"])
            self.assertFalse(caps[op]["quantizedScheduling"])
        for marker in ("policy-mixer", "policy-track-mute"):
            path = self.install / marker
            path.touch()
            try:
                for value in (True, False):
                    for op in ("set_mute", "set_solo", "set_record_armed"):
                        result = self.send(
                            self.command(marker + op + str(value), op=op, value=value)
                        )
                        self.assertEqual(result["status"], "applied", result)
                        self.assertTrue(result["nativeInvoked"])
            finally:
                path.unlink()

    def test_playback_dispatch_noops_conflicts_and_retained_timing(self):
        paused = self.install / "pause-ui"
        paused.touch()
        request = self.command("start-before-dispatch", waitMs=0)
        self.assertEqual(self.send(request)["status"], "accepted")
        (self.install / "policy-playing").touch()
        paused.unlink()
        result = self.get(request["requestId"])
        self.assertEqual(result["status"], "applied", result)
        self.assertTrue(result["nativeInvoked"])
        self.assertGreaterEqual(result["timing"]["totalMs"], result["timing"]["queueMs"])
        self.assertEqual(self.send(request), result)
        for screen in (None, "policy-mixer", "policy-track-mute"):
            path = self.install / screen if screen else None
            if path:
                path.touch()
            try:
                for op in ("set_mute", "set_solo", "set_record_armed"):
                    for value in (False, True):
                        label = str(screen) + op + str(value)
                        receipt = self.send(self.command(label, op=op, value=value))
                        self.assertEqual(receipt["status"], "applied", receipt)
                        noop = self.send(self.command(label + "noop", op=op, value=value))
                        self.assertEqual(noop["status"], "applied", noop)
                        self.assertFalse(noop["nativeInvoked"])
                        conflict = self.send(
                            self.command(
                                label + "conflict",
                                op=op,
                                value=not value,
                                expected={"value": not value},
                            )
                        )
                        self.assertEqual(conflict["error"]["code"], "PRECONDITION_FAILED")
                        self.assertFalse(conflict["nativeInvoked"])
            finally:
                if path:
                    path.unlink()

    def test_playback_disconnects_expiry_pressure_and_shutdown(self):
        (self.install / "policy-playing").touch()
        wire_tests.WireCommandTests.test_disconnect_and_wait_timeout_preserve_work_without_blocking_other_clients(
            self
        )
        wire_tests.WireCommandTests.test_queue_start_expiry_and_backpressure_are_bounded(self)

    def test_stop_drains_while_playback_remains_active(self):
        (self.install / "policy-playing").touch()
        wire_tests.WireCommandTests.test_stop_drains_pending_delivery_and_restart_retires_authority(
            self
        )

    def test_shutdown_cancels_queued_delivery_while_playing(self):
        (self.install / "policy-playing").touch()
        wire_tests.WireCommandTests.test_shutdown_drops_long_delivery_waits_and_drains_without_waiting_for_clients(
            self
        )

    def test_queued_command_qualifies_the_screen_at_dispatch(self):
        for marker in ("policy-mixer", "policy-track-mute"):
            paused = self.install / "pause-ui"
            paused.touch()
            request = self.command(marker, waitMs=0)
            result = self.send(request)
            self.assertEqual(result["status"], "accepted")
            screen = self.install / marker
            screen.touch()
            paused.unlink()
            result = self.get(marker)
            self.assertEqual(result["status"], "applied", result)
            self.assertTrue(result["nativeInvoked"])
            screen.unlink()
            self.assertEqual(
                self.send(self.command(marker + "restore", value=False))["status"], "applied"
            )

    def test_screen_transition_after_setter_retains_uncertainty(self):
        (self.install / "policy-change-after-set").touch()
        request = self.command("screen-transition")
        result = self.send(request)
        self.assertEqual(result["status"], "unknown", result)
        self.assertTrue(result["nativeInvoked"])
        self.assertEqual(result["error"]["code"], "STATE_UNSTABLE")
        self.assertEqual(self.send(request), result)
        state = self.public("get_state")
        self.assertTrue(state["tracks"][0]["requested"]["muted"])

    def test_modes_playback_settings_and_automation_reject_before_native_access(self):
        for marker in (
            "policy-sounds",
            "policy-song",
            "policy-clips",
            "policy-popup",
            "policy-invalid-playing",
            "policy-recording",
            "policy-overdub",
            "policy-record-pending",
            "policy-overdub-pending",
            "policy-record-unknown",
            "policy-song-playback",
            "policy-clip-playback",
            "policy-playback-unknown",
            "policy-song-source",
            "policy-recorder-mismatch",
            "policy-record-owner",
            "policy-arm-mode",
            "policy-solo-mode",
            "policy-invalid-bool",
            "policy-automation-read",
            "policy-automation-write",
            "policy-broken-owner",
            "policy-instance-mismatch",
            "policy-race",
        ):
            path = self.install / marker
            path.touch()
            try:
                for op in ("set_mute", "set_solo", "set_record_armed"):
                    with self.subTest(marker=marker, op=op):
                        result = self.send(self.command(marker + op, op=op, value=False))
                        self.assertEqual(result["status"], "rejected", result)
                        self.assertFalse(result["nativeInvoked"])
                        self.assertEqual(result["execution"], "not_started")
                        expected = "NOT_READY"
                        if marker in {
                            "policy-broken-owner",
                            "policy-instance-mismatch",
                            "policy-recorder-mismatch",
                            "policy-record-owner",
                        }:
                            expected = "READ_FAILED"
                        elif marker == "policy-race":
                            expected = "STATE_UNSTABLE"
                        elif marker.startswith("policy-automation-"):
                            expected = "AUTOMATION_UNQUALIFIED"
                        elif marker in {
                            "policy-recording",
                            "policy-overdub",
                            "policy-record-pending",
                            "policy-overdub-pending",
                            "policy-record-unknown",
                        }:
                            expected = "RECORDING_UNQUALIFIED"
                        elif marker in {
                            "policy-song-playback",
                            "policy-clip-playback",
                            "policy-playback-unknown",
                            "policy-song-source",
                        }:
                            expected = "PLAYBACK_CONTEXT_UNQUALIFIED"
                        self.assertEqual(result["error"]["code"], expected)
            finally:
                path.unlink()

    def test_mute_scope_does_not_disable_solo_or_arm(self):
        (self.install / "sequence-mute").touch()
        result = self.send(self.command("mute"))
        self.assertEqual(result["status"], "rejected", result)
        self.assertFalse(result["nativeInvoked"])
        for op in ("set_solo", "set_record_armed"):
            result = self.send(self.command(op, op=op))
            self.assertEqual(result["status"], "applied", result)

    def test_queueing_does_not_cache_a_previously_qualified_mode(self):
        paused = self.install / "pause-ui"
        paused.touch()
        result = self.send(self.command(waitMs=0))
        self.assertEqual(result["status"], "accepted")
        (self.install / "policy-record-pending").touch()
        paused.unlink()
        result = self.get("wire")
        self.assertEqual(result["status"], "rejected", result)
        self.assertFalse(result["nativeInvoked"])

    def test_family_and_native_reference_guards_precede_automation_reads(self):
        marker = self.install / "policy-unsupported"
        marker.touch()
        try:
            result = self.send(self.command("unsupported", value=False))
        finally:
            marker.unlink()
        self.assertEqual(result["status"], "rejected", result)
        self.assertEqual(result["error"]["code"], "UNQUALIFIED_TRACK_TYPE")
        self.assertFalse(result["nativeInvoked"])
        for key, code in (("projectId", "STALE_PROJECT"), ("trackId", "TARGET_NOT_FOUND")):
            result = self.send(
                self.command(key, target={**self.target, key: "18446744073709551615"})
            )
            self.assertEqual(result["status"], "rejected", result)
            self.assertEqual(result["error"]["code"], code)
            self.assertFalse(result["nativeInvoked"])

    def sequence_command(self, request_id="sequence", sequence=None, **extra):
        state = self.public("get_state")
        return self.command(
            request_id,
            muteScope="sequence",
            sequence=sequence or state["selectedSequence"]["ref"],
            **extra,
        )

    def test_sequence_patterns_active_only_reads_retries_and_expected_values(self):
        (self.install / "sequence-mute").touch()
        request = self.sequence_command(expected={"value": False})
        first = self.send(request)
        self.assertEqual(first["status"], "applied", first)
        self.assertEqual(first["sequence"], request["sequence"])
        self.assertEqual(
            first["verification"]["muteContext"],
            {"scope": "sequence", "sequence": request["sequence"]},
        )
        jsonschema.validate(
            first,
            json.loads(
                (wire_tests.ROOT / "native-api/schema/command-receipt.schema.json").read_text()
            ),
        )
        self.assertTrue(
            any(
                c.get("muteContext", {}).get("sequence") == request["sequence"]
                for c in first["observedChanges"]
            )
        )
        other = self.install / "sequence-two"
        other.touch()
        state = self.public("get_state")
        self.assertFalse(state["tracks"][0]["requested"]["muted"])
        self.assertNotEqual(state["muteContext"]["sequence"], request["sequence"])
        self.assertEqual(self.send(request), first)
        rejected = self.send({**request, "requestId": "inactive"})
        self.assertEqual(rejected["error"]["code"], "SEQUENCE_NOT_ACTIVE")
        self.assertFalse(rejected["nativeInvoked"])
        self.assertEqual(self.public("get_state")["selectedSequence"], state["selectedSequence"])
        self.assertEqual(
            self.send({**request, "sequence": state["selectedSequence"]["ref"]})["error"]["code"],
            "REQUEST_ID_CONFLICT",
        )
        self.assertEqual(
            self.send(
                {k: v for k, v in {**request, "muteScope": "global"}.items() if k != "sequence"}
            )["error"]["code"],
            "REQUEST_ID_CONFLICT",
        )
        other.unlink()
        self.assertTrue(self.public("get_state")["tracks"][0]["requested"]["muted"])
        noop = self.send({**request, "requestId": "sequence-noop", "expected": {"value": True}})
        self.assertEqual(noop["status"], "applied", noop)
        self.assertFalse(noop["nativeInvoked"])
        conflict = self.send({**request, "requestId": "sequence-expected"})
        self.assertEqual(conflict["error"]["code"], "PRECONDITION_FAILED")
        self.assertFalse(conflict["nativeInvoked"])
        for op in (
            "get_state",
            "list_tracks",
            "current_track",
            "current_sequence",
            "recovery_snapshot",
        ):
            snapshot = self.public(op)
            self.assertEqual(snapshot["muteContext"], first["verification"]["muteContext"])
            jsonschema.validate(
                snapshot,
                json.loads(
                    (wire_tests.ROOT / "native-api/schema/state-response.schema.json").read_text()
                ),
            )

    def test_sequence_scope_setting_and_generation_are_preconditions_even_for_noops(self):
        request = self.sequence_command(value=False)
        result = self.send(request)
        self.assertEqual(result["error"]["code"], "MUTE_SCOPE_MISMATCH")
        self.assertFalse(result["nativeInvoked"])
        (self.install / "sequence-mute").touch()
        request["sequence"]["generation"] = "18446744073709551615"
        result = self.send({**request, "requestId": "stale-generation"})
        self.assertEqual(result["error"]["code"], "STALE_SEQUENCE")
        self.assertFalse(result["nativeInvoked"])

    def test_sequence_and_scope_changes_in_queue_and_after_setter(self):
        for marker, error, invoked in (
            ("sequence-two", "SEQUENCE_NOT_ACTIVE", False),
            ("sequence-after-set", "SEQUENCE_NOT_ACTIVE", True),
            ("scope-after-set", "MUTE_SCOPE_MISMATCH", True),
        ):
            with self.subTest(marker=marker):
                (self.install / "sequence-mute").touch()
                request = self.sequence_command(marker)
                pause = self.install / "pause-ui"
                pause.touch()
                self.assertEqual(self.send({**request, "waitMs": 0})["status"], "accepted")
                path = self.install / marker
                path.touch()
                pause.unlink()
                result = self.get(marker)
                self.assertEqual(result["error"]["code"], error, result)
                self.assertEqual(result["nativeInvoked"], invoked)
                self.assertEqual(result["execution"], "completed" if invoked else "not_started")
                self.assertEqual(result["status"], "unknown" if invoked else "rejected")
                self.assertEqual(self.send(request), result)
                path.unlink()
                # Return the first sequence's value to false for the next row.
                self.send(self.sequence_command(marker + "-restore", value=False))
        pause = self.install / "pause-ui"
        request = self.sequence_command("setting-queued", value=False)
        pause.touch()
        self.assertEqual(self.send({**request, "waitMs": 0})["status"], "accepted")
        (self.install / "sequence-mute").unlink()
        pause.unlink()
        result = self.get("setting-queued")
        self.assertEqual(result["error"]["code"], "MUTE_SCOPE_MISMATCH")
        self.assertFalse(result["nativeInvoked"])

    def test_sequence_disconnect_keeps_address_and_retained_result(self):
        (self.install / "sequence-mute").touch()
        request = self.sequence_command("sequence-disconnect")
        pause = self.install / "pause-ui"
        pause.touch()
        with socket.socket(socket.AF_UNIX) as client:
            client.connect(str(self.api))
            client.sendall(json.dumps(request).encode() + b"\n")
            until = time.monotonic() + 1
            while self.get("sequence-disconnect", waitMs=0).get("status") != "accepted":
                self.assertLess(time.monotonic(), until)
        pause.unlink()
        result = self.get("sequence-disconnect")
        self.assertEqual(result["status"], "applied", result)
        self.assertEqual(self.send(request), result)
