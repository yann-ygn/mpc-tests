"""Run the production policy through the public transport and emulated live owners."""

import test_wire_commands as wire_tests
from test_host import HostHarness


class LivePolicyTests(HostHarness):
    command = wire_tests.WireCommandTests.command
    send = wire_tests.WireCommandTests.send
    get = wire_tests.WireCommandTests.get

    def setUp(self):
        super().setUp()
        self.launch("native-commands")
        self.target = self.public("get_state")["tracks"][0]["ref"]

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
            "policy-playing",
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
                        expected = (
                            "READ_FAILED"
                            if marker in {"policy-broken-owner", "policy-instance-mismatch"}
                            else ("STATE_UNSTABLE" if marker == "policy-race" else "NOT_READY")
                        )
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
        (self.install / "policy-playing").touch()
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
