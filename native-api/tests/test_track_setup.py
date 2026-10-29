"""Track setup through the public parser, retained authority and UI executor."""

import json
from concurrent.futures import ThreadPoolExecutor

import jsonschema

import test_wire_commands as wire
from test_host import HostHarness
from test_wire_commands import ROOT


class TrackSetupTests(HostHarness):
    send = wire.WireCommandTests.send
    get = wire.WireCommandTests.get
    receipt = wire.WireCommandTests.receipt

    def setUp(self):
        super().setUp()
        self.launch("setup-commands")
        self.state = self.public("get_state")
        self.target = self.state["tracks"][0]["ref"]
        self.receipt_schema = json.loads(
            (ROOT / "native-api/schema/command-receipt.schema.json").read_text()
        )
        self.request_schema = json.loads(
            (ROOT / "native-api/schema/command-request.schema.json").read_text()
        )

    def request(self, op, value, request_id=None, **extra):
        return {
            "apiVersion": "0.1",
            "op": op,
            "requestId": request_id or op,
            "target": self.target,
            "value": value,
            "waitMs": 1000,
            **extra,
        }

    def test_setters_noops_expected_conflicts_and_all_read_projections(self):
        rows = [
            ("set_track_colour", 0x123456, "colour"),
            ("set_track_transpose", -24, "transposeSemitones"),
            ("set_track_velocity_scale", 1.23456, "velocityScale"),
            ("set_midi_output_channel", 16, "midiOutputChannel"),
            ("set_midi_bank_enabled", True, "midiBankEnabled"),
            ("set_midi_bank_msb", 127, "midiBankMsb"),
            ("set_midi_bank_lsb", 0, "midiBankLsb"),
            ("set_midi_program_enabled", True, "midiProgramEnabled"),
            ("set_midi_program", 127, "midiProgram"),
        ]
        for op, value, field in rows:
            with self.subTest(op=op):
                request = self.request(op, value)
                jsonschema.validate(request, self.request_schema)
                applied = self.receipt(self.send(request), "applied")
                self.assertEqual(applied, self.send(request))
                self.assertEqual(applied, self.get(op))
                self.assertEqual(applied["verification"]["by"], ["model"])
                observed = self.public("get_state")["tracks"][0]["setup"][field]
                self.assertEqual(applied["verification"]["requestedValue"], observed)
                noop = self.receipt(self.send(self.request(op, value, op + "_noop")), "applied")
                self.assertFalse(noop["nativeInvoked"])
                self.assertFalse(noop["changed"])
                wrong = not value if isinstance(value, bool) else 1 if value == 0 else 0
                if op == "set_track_velocity_scale":
                    wrong = 2
                conflict = self.receipt(
                    self.send(self.request(op, value, op + "_conflict", expected={"value": wrong})),
                    "rejected",
                )
                self.assertEqual(conflict["error"]["code"], "PRECONDITION_FAILED")
                self.assertFalse(conflict["nativeInvoked"])
        state = self.public("get_state")
        for op in ("list_tracks", "recovery_snapshot"):
            self.assertEqual(state["tracks"], self.public(op)["tracks"])
        self.assertEqual(state["tracks"][0], self.public("current_track")["track"])

    def test_strict_scalar_types_ranges_and_extra_arguments(self):
        rows = [
            ("set_track_colour", [-1, 0x1000000, 1.5, True, "123"]),
            ("set_track_transpose", [-25, 25, 0.5, True]),
            ("set_track_velocity_scale", [0, -1, 2.01, True, "1"]),
            ("set_midi_output_channel", [-1, 17, 1.5, False]),
            ("set_midi_bank_enabled", [0, 1, "true", None]),
            ("set_midi_bank_msb", [-1, 128, 0.5, True]),
            ("set_midi_bank_lsb", [-1, 128, False]),
            ("set_midi_program_enabled", [0, 1, None]),
            ("set_midi_program", [-1, 128, True]),
        ]
        for op, values in rows:
            for value in values:
                with self.subTest(op=op, value=value):
                    request = self.request(op, value)
                    self.assertEqual(self.send(request)["error"]["code"], "INVALID_ARGUMENT")
                    with self.assertRaises(jsonschema.ValidationError):
                        jsonschema.validate(request, self.request_schema)
        base = self.request("set_track_colour", 0)
        for extra in (
            {"mixerGeneration": "1"},
            {"muteScope": "global"},
            {"pulse": "0"},
            {"sequence": self.state["selectedSequence"]["ref"]},
        ):
            self.assertEqual(self.send({**base, **extra})["error"]["code"], "INVALID_ARGUMENT")

    def test_expected_value_is_observed_after_queueing_and_before_noop(self):
        paused = self.install / "pause-ui"
        paused.touch()
        request = self.request("set_track_colour", 0, expected={"value": 0}, waitMs=0)
        self.receipt(self.send(request), "accepted")
        (self.install / "manual-setup").touch()
        paused.unlink()
        reply = self.receipt(self.get(request["requestId"]), "rejected")
        self.assertFalse(reply["nativeInvoked"])
        self.assertEqual(reply["error"]["code"], "PRECONDITION_FAILED")

    def test_queued_target_survives_rename_and_reorder(self):
        paused = self.install / "pause-ui"
        paused.touch()
        request = self.request("set_track_colour", 123, waitMs=0)
        self.receipt(self.send(request), "accepted")
        (self.install / "watch-rename").touch()
        (self.install / "watch-reorder").touch()
        paused.unlink()
        self.receipt(self.get(request["requestId"]), "applied")
        state = self.public("get_state")
        self.assertEqual(state["tracks"][1]["ref"], self.target)
        self.assertEqual(state["tracks"][1]["name"], "Renamed")
        self.assertEqual(state["tracks"][1]["setup"]["colour"], 123)
        self.assertEqual(state["tracks"][0]["setup"]["colour"], 0)

    def test_project_replacement_rejects_queued_edit_before_invocation(self):
        paused = self.install / "pause-ui"
        paused.touch()
        request = self.request("set_track_colour", 123, waitMs=0)
        self.receipt(self.send(request), "accepted")
        (self.install / "watch-replace").touch()
        paused.unlink()
        reply = self.receipt(self.get(request["requestId"]), "rejected")
        self.assertFalse(reply["nativeInvoked"])
        self.assertEqual(reply["error"]["code"], "STALE_PROJECT")

    def test_unqualified_family_rejects_even_a_desired_state_noop(self):
        (self.install / "policy-unsupported").touch()
        reply = self.receipt(self.send(self.request("set_track_colour", 0)), "rejected")
        self.assertFalse(reply["nativeInvoked"])
        self.assertEqual(reply["error"]["code"], "UNQUALIFIED_TRACK_TYPE")

    def test_request_identity_includes_edit_values_and_preconditions(self):
        first = self.request("set_track_transpose", -12)
        self.receipt(self.send(first), "applied")
        for other in ({**first, "value": 12}, {**first, "expected": {"value": -12}}):
            self.assertEqual(self.send(other)["error"]["code"], "REQUEST_ID_CONFLICT")
        request = self.request("set_midi_program", 9)
        with ThreadPoolExecutor(max_workers=8) as clients:
            replies = list(clients.map(lambda _: self.send(request), range(8)))
        self.assertTrue(all(r == replies[0] for r in replies))
        self.receipt(replies[0], "applied")

    def test_live_policy_rejects_playback_recording_song_and_external_source(self):
        for marker, error in (
            ("policy-playing", "PLAYBACK_CONTEXT_UNQUALIFIED"),
            ("policy-recording", "RECORDING_UNQUALIFIED"),
            ("policy-overdub-pending", "RECORDING_UNQUALIFIED"),
            ("policy-clip-playback", "PLAYBACK_CONTEXT_UNQUALIFIED"),
            ("policy-popup", "NOT_READY"),
            ("policy-automation-write", "AUTOMATION_UNQUALIFIED"),
        ):
            with self.subTest(marker=marker):
                path = self.install / marker
                path.touch()
                result = self.receipt(
                    self.send(self.request("set_track_colour", 123, marker)), "rejected"
                )
                self.assertEqual(result["error"]["code"], error)
                self.assertFalse(result["nativeInvoked"])
                path.unlink()

    def assert_postinvocation_loss(self, marker):
        (self.install / marker).touch()
        request = self.request("set_track_colour", 123, marker)
        reply = self.receipt(self.send(request), "unknown")
        self.assertTrue(reply["nativeInvoked"])
        self.assertIsNone(reply["changed"])
        self.assertEqual(reply, self.send(request))

    def test_project_loss_after_setter(self):
        self.assert_postinvocation_loss("setup-replace-after-set")

    def test_family_loss_after_setter(self):
        self.assert_postinvocation_loss("setup-family-after-set")

    def test_context_loss_after_setter(self):
        self.assert_postinvocation_loss("setup-context-after-set")

    def test_divergent_readback_times_out_without_replaying_setter(self):
        (self.install / "setup-diverged").touch()
        request = self.request("set_midi_program", 7)
        reply = self.receipt(self.send(request), "unknown")
        self.assertTrue(reply["nativeInvoked"])
        self.assertIsNone(reply["changed"])
        self.assertEqual(reply["error"]["code"], "VERIFICATION_TIMEOUT")
        self.assertEqual(reply, self.send(request))
        self.assertEqual(reply, self.get("set_midi_program"))

    def test_shared_setup_examples_match_the_contract(self):
        jsonschema.validate(
            json.loads((ROOT / "native-api/examples/track-setup-request.json").read_text()),
            self.request_schema,
        )
        jsonschema.validate(
            json.loads((ROOT / "native-api/examples/track-setup-applied.json").read_text()),
            self.receipt_schema,
        )

    def test_discovery_documents_subset_and_known_hardware(self):
        description = self.public()
        jsonschema.validate(
            description,
            json.loads((ROOT / "native-api/schema/describe-response.schema.json").read_text()),
        )
        cap = description["capabilities"]["set_midi_output_channel"]
        self.assertTrue(cap["available"])
        self.assertEqual(cap["families"], ["midi"])
        self.assertEqual(cap["undo"], "not_registered")
        self.assertFalse(cap["outputVerified"])
        self.assertEqual(description["capabilities"]["set_track_transpose"]["minimum"], -24)
        for op in (
            "rename_track",
            "create_track",
            "copy_track",
            "remove_track",
            "set_cv_port",
            "set_gate_port",
        ):
            self.assertFalse(description["capabilities"][op]["available"])
            self.assertEqual(description["capabilities"][op]["reason"], "NOT_QUALIFIED")
