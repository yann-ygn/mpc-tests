"""Public navigation through the real parser, authority, executor and live policy."""

import json
from concurrent.futures import ThreadPoolExecutor

import jsonschema

import test_wire_commands as wire
from test_wire_commands import ROOT


from test_host import HostHarness


class NavigationTests(HostHarness):
    send = wire.WireCommandTests.send
    get = wire.WireCommandTests.get
    receipt = wire.WireCommandTests.receipt

    # Inherit the harness, not the flag test cases.
    def setUp(self):
        super().setUp()
        self.launch("navigation-commands")
        self.state = self.public("get_state")
        self.target = self.state["tracks"][0]["ref"]
        self.sequence = self.state["selectedSequence"]["ref"]
        self.receipt_schema = json.loads(
            (ROOT / "native-api/schema/command-receipt.schema.json").read_text()
        )

    def nav(self, op, request_id=None, **extra):
        target = dict(self.target)
        if op != "select_track":
            target.pop("trackId")
        request = {
            "apiVersion": "0.1",
            "op": op,
            "requestId": request_id or op,
            "target": target,
            "waitMs": 1000,
            **extra,
        }
        if op != "select_track":
            request.setdefault("sequence", self.sequence)
        return request

    def test_capabilities_name_scope_and_completion(self):
        description = self.public()
        jsonschema.validate(
            description,
            json.loads((ROOT / "native-api/schema/describe-response.schema.json").read_text()),
        )
        for op in (
            "play",
            "stop",
            "play_start",
            "locate",
            "select_track",
            "select_sequence",
            "set_sequence_bpm",
        ):
            cap = description["capabilities"][op]
            self.assertTrue(cap["available"])
            self.assertTrue(cap["viewIndependent"])
            self.assertEqual(cap["requestReplay"], "retained_result")
        self.assertFalse(description["capabilities"]["select_sequence"]["queuedSelection"])
        self.assertFalse(description["capabilities"]["set_sequence_bpm"]["effectiveTempoKnown"])

    def test_navigation_transitions_and_retry_after_manual_playback(self):
        stop = self.nav("stop")
        first = self.receipt(self.send(stop), "applied")
        self.assertFalse(first["nativeInvoked"])
        locate = self.nav("locate", pulse="960")
        moved = self.receipt(self.send(locate), "applied")
        self.assertEqual(moved["verification"]["transport"]["pulse"], "960")
        self.assertTrue(moved["nativeInvoked"])
        request = self.nav("play_start")
        played = self.receipt(self.send(request), "applied")
        self.assertTrue(played["verification"]["transport"]["playing"])
        self.assertEqual(played["verification"]["transport"]["pulse"], "0")
        self.receipt(self.send(self.nav("stop", "later_stop")), "applied")
        self.assertEqual(self.send(request), played)
        self.assertFalse(self.public("get_state")["transport"]["playing"])
        conflict = self.send(self.nav("play", "play_start"))
        self.assertEqual(conflict["error"]["code"], "REQUEST_ID_CONFLICT")
        self.assertEqual(self.get("play_start"), played)

    def test_sequence_tempo_is_explicit_and_binary32_verified(self):
        request = self.nav("set_sequence_bpm", bpm=123.456)
        first = self.receipt(self.send(request), "applied")
        self.assertTrue(first["nativeInvoked"])
        self.assertEqual(first, self.send(request))
        state = self.public("get_state")
        self.assertAlmostEqual(state["selectedSequence"]["bpm"], 123.456, places=4)
        self.assertIsNone(state["transport"]["effectiveBpm"])
        self.assertIsNone(state["transport"]["tempoSource"])
        noop = self.receipt(
            self.send(self.nav("set_sequence_bpm", "tempo_noop", bpm=123.456)), "applied"
        )
        self.assertFalse(noop["nativeInvoked"])
        for value in (True, "120", 0, 19.9, 1000, None):
            with self.subTest(value=value):
                self.assertEqual(
                    self.send(self.nav("set_sequence_bpm", "invalid", bpm=value))["error"]["code"],
                    "INVALID_ARGUMENT",
                )

    def test_track_and_inactive_sequence_references(self):
        target = self.state["tracks"][1]["ref"]
        request = self.nav("select_track", target=target)
        self.receipt(self.send(request), "applied")
        self.assertEqual(self.public("get_state")["selectedTrackId"], target["trackId"])
        next_sequence = self.state["sequences"][1]["ref"]
        self.receipt(self.send(self.nav("select_sequence", sequence=next_sequence)), "applied")
        self.assertEqual(self.public("get_state")["selectedSequence"]["ref"], next_sequence)
        stale_active = self.receipt(self.send(self.nav("play")), "rejected")
        self.assertEqual(stale_active["error"]["code"], "SEQUENCE_NOT_ACTIVE")
        self.assertFalse(stale_active["nativeInvoked"])

    def test_concurrent_duplicates_and_nonidempotent_replay(self):
        request = self.nav("play_start")
        with ThreadPoolExecutor(max_workers=8) as clients:
            results = list(clients.map(lambda _: self.send(request), range(8)))
        for response in results:
            self.receipt(response, "applied")
            self.assertEqual(response, results[0])
        self.receipt(self.send(self.nav("stop")), "applied")
        self.assertEqual(self.send(request), results[0])
        self.assertFalse(self.public("get_state")["transport"]["playing"])

    def test_strict_types_and_target_domains(self):
        base = self.nav("locate", pulse="0")
        for change in (
            {"pulse": -1},
            {"pulse": "-1"},
            {"pulse": "01"},
            {"pulse": "2147483648"},
            {"pulse": "1.0"},
            {"pulse": True},
            {"value": True},
            {"expected": {"value": False}},
            {"mixerGeneration": "1"},
            {"sequence": {**self.sequence, "projectId": "999"}},
            {"target": self.target},
        ):
            with self.subTest(change=change):
                self.assertEqual(self.send({**base, **change})["error"]["code"], "INVALID_ARGUMENT")
        schema = json.loads((ROOT / "native-api/schema/command-request.schema.json").read_text())
        for op in ("play", "stop", "play_start", "select_sequence", "select_track"):
            jsonschema.validate(self.nav(op), schema)
        jsonschema.validate(base, schema)

    def test_dispatch_time_policy_and_uncertain_postinvocation_failure(self):
        for marker, error in (
            ("policy-recording", "RECORDING_UNQUALIFIED"),
            ("policy-overdub-pending", "RECORDING_UNQUALIFIED"),
            ("policy-clip-playback", "PLAYBACK_CONTEXT_UNQUALIFIED"),
            ("policy-popup", "NOT_READY"),
            ("policy-broken-owner", "NOT_READY"),
        ):
            with self.subTest(marker=marker):
                path = self.install / marker
                path.touch()
                result = self.receipt(self.send(self.nav("play", marker)), "rejected")
                self.assertEqual(result["error"]["code"], error)
                self.assertFalse(result["nativeInvoked"])
                path.unlink()
        (self.install / "navigation-replace-after-set").touch()
        result = self.receipt(self.send(self.nav("locate", pulse="960")), "unknown")
        self.assertTrue(result["nativeInvoked"])
        self.assertEqual(result, self.get("locate"))

    def test_native_queue_drain_is_required_even_when_model_matches(self):
        (self.install / "navigation-pending-after-set").touch()
        request = self.nav("play")
        first = self.receipt(self.send(request), "unknown")
        self.assertTrue(first["nativeInvoked"])
        self.assertIsNone(first["changed"])
        self.assertTrue(first["verification"]["transport"]["playing"])
        self.assertEqual(first["error"]["code"], "VERIFICATION_TIMEOUT")
        self.assertEqual(self.send(request), first)
        self.assertEqual(self.get("play"), first)

    def test_play_start_requires_stopped_playback(self):
        self.receipt(self.send(self.nav("play")), "applied")
        first = self.receipt(self.send(self.nav("play_start")), "rejected")
        self.assertEqual(first["error"]["code"], "PLAYBACK_CONTEXT_UNQUALIFIED")
        self.assertFalse(first["nativeInvoked"])
        self.assertTrue(self.public("get_state")["transport"]["playing"])

    def test_play_start_does_not_play_if_locate_was_not_observed(self):
        (self.install / "navigation-locate-diverged").touch()
        request = self.nav("play_start")
        first = self.receipt(self.send(request), "unknown")
        self.assertTrue(first["nativeInvoked"])
        self.assertEqual(first["error"]["code"], "VERIFICATION_TIMEOUT")
        self.assertFalse(first["verification"]["transport"]["playing"])
        self.assertEqual(first["verification"]["transport"]["pulse"], "960")
        self.assertEqual(self.send(request), first)

    def test_context_loss_after_locate_cannot_start_playback(self):
        (self.install / "navigation-context-after-set").touch()
        request = self.nav("play_start")
        first = self.receipt(self.send(request), "unknown")
        self.assertTrue(first["nativeInvoked"])
        self.assertEqual(first["error"]["code"], "STATE_UNSTABLE")
        self.assertIsNone(first["changed"])
        self.assertEqual(self.send(request), first)
        self.assertFalse(self.public("get_state")["transport"]["playing"])
