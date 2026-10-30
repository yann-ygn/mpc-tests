"""Continuous numeric traffic through the authenticated Unix service."""

import json
from pathlib import Path
import socket
import time

import jsonschema

from test_host import HostHarness
import test_mixer as mixer_tests


class ContinuousTests(HostHarness):
    def setUp(self):
        super().setUp()
        self.launch("mixer-commands")
        self.state = self.public("get_state")
        self.target = self.state["tracks"][0]["ref"]
        self.generation = self.state["tracks"][0]["mixer"]["generation"]
        root = Path(__file__).resolve().parents[1] / "schema"
        self.receipt_schema = json.loads((root / "command-receipt.schema.json").read_text())
        self.request_schema = json.loads((root / "command-request.schema.json").read_text())
        self.request_validator = jsonschema.Draft202012Validator(self.request_schema)

    send = mixer_tests.MixerTests.send
    get = mixer_tests.MixerTests.get
    receipt = mixer_tests.MixerTests.receipt
    command = mixer_tests.MixerTests.command

    def mixer(self, request_id="volume", op="set_volume", value=0.125, **extra):
        generation = extra.pop("mixerGeneration", self.generation)
        return self.command(request_id, op, value, mixerGeneration=generation, **extra)

    def test_bounded_receipts_and_independent_discrete_capacity(self):
        info = self.public()["continuousMixer"]
        self.assertTrue(info["available"])
        self.assertEqual(info["pendingCapacity"], 8)
        self.assertEqual(info["controlCapacity"], 64)
        self.assertFalse(info["discreteCapacityShared"])
        root = Path(__file__).resolve().parents[1]
        example = json.loads((root / "examples/continuous-mixer-request.json").read_text())
        self.request_validator.validate(example)
        retained = self.mixer("retained", value=0.75)
        self.receipt(self.send(retained), "applied")
        last = None
        for i in range(160):
            req = self.mixer(str(i), value=(i % 10) / 10, continuous=True)
            self.request_validator.validate(req)
            result = self.receipt(self.send(req), "applied")
            self.assertEqual(result["retention"], "supersedable")
            self.assertEqual(self.send(req), result)
            if last:
                self.assertEqual(self.get(last)["error"]["code"], "OUTCOME_UNAVAILABLE")
            last = str(i)
        self.receipt(self.get("retained"), "applied")
        for i in range(127):
            self.receipt(self.send(self.command("flag" + str(i), value=False)), "applied")
        self.assertEqual(self.send(self.command("overflow"))["error"]["code"], "RETENTION_CAPACITY")
        # A saturated retained class also cannot consume continuous capacity.
        self.receipt(self.send(self.mixer("still_continuous", continuous=True)), "applied")

    def test_pending_canonical_retries_conflicts_and_fifo(self):
        pause = self.install / "pause-ui"
        pause.touch()
        first = self.mixer("first", continuous=True, waitMs=0, startWithinMs=5000)
        self.receipt(self.send(first), "accepted")
        self.receipt(self.send(first), "accepted")
        for change in (
            {"value": 0.5},
            {"continuous": False},
            {"mixerGeneration": "999"},
            {"startWithinMs": 4000},
        ):
            self.assertEqual(self.send({**first, **change})["error"]["code"], "REQUEST_ID_CONFLICT")
        next_req = {**first, "requestId": "next", "value": 0.5}
        self.assertEqual(self.send(next_req)["error"]["code"], "QUEUE_FULL")
        self.assertEqual(self.get("next")["error"]["code"], "OUTCOME_UNAVAILABLE")
        # Legacy command targeting that control queues after first and preserves FIFO.
        legacy = self.mixer("legacy", value=0.75, waitMs=0, startWithinMs=5000)
        self.receipt(self.send(legacy), "accepted")
        pause.unlink()
        self.receipt(self.get("legacy"), "applied")
        self.assertEqual(self.public("get_state")["tracks"][0]["mixer"]["volume"], 0.75)
        self.receipt(
            self.send({**next_req, "expected": {"value": 0.75}, "waitMs": 1000}), "applied"
        )
        self.assertEqual(self.get("first")["error"]["code"], "OUTCOME_UNAVAILABLE")
        self.receipt(self.get("legacy"), "applied")

    def test_full_continuous_queue_preserves_discrete_slots(self):
        pause = self.install / "pause-ui"
        pause.touch()
        requests = []
        for track in self.state["tracks"][:2]:
            for op, extras in (
                ("set_volume", {}),
                ("set_pan", {}),
                ("set_send_level", {"sendIndex": 0}),
                ("set_send_level", {"sendIndex": 1}),
            ):
                req = self.mixer(
                    "busy" + str(len(requests)),
                    op=op,
                    target=track["ref"],
                    mixerGeneration=track["mixer"]["generation"],
                    continuous=True,
                    waitMs=0,
                    startWithinMs=5000,
                    **extras,
                )
                requests.append(req)
                self.receipt(self.send(req), "accepted")
        req = self.mixer("over", op="set_send_level", sendIndex=3, continuous=True, waitMs=0)
        self.assertEqual(self.send(req)["error"]["code"], "QUEUE_FULL")
        for i in range(16):
            self.receipt(
                self.send(self.command("flag" + str(i), waitMs=0, startWithinMs=5000)), "accepted"
            )
        self.assertEqual(
            self.send(self.command("flag_over", waitMs=0))["error"]["code"], "QUEUE_FULL"
        )
        pause.unlink()
        self.receipt(self.get("flag15"), "applied")

    def test_continuous_disconnect_and_shutdown_preserve_pending_ownership(self):
        pause = self.install / "pause-ui"
        pause.touch()
        req = self.mixer("disconnect", continuous=True, waitMs=30000, startWithinMs=5000)
        with socket.socket(socket.AF_UNIX) as client:
            client.connect(str(self.api))
            client.sendall(json.dumps(req).encode() + b"\n")
            until = time.monotonic() + 1
            while self.get("disconnect", waitMs=0).get("status") != "accepted":
                self.assertLess(time.monotonic(), until)
        other = {**req, "requestId": "other", "waitMs": 0}
        self.assertEqual(self.send(other)["error"]["code"], "QUEUE_FULL")
        pause.unlink()
        self.receipt(self.get("disconnect"), "applied")
        pause.touch()
        self.receipt(self.send(other), "accepted")
        self.assertIn(self.call(b"STOP\n")["status"], {"draining", "stopped"})
        pause.unlink()
        self.wait_status({"stopped"})
        self.call(b"START\n")
        self.wait_status({"ready"})
        self.assertEqual(self.get("other")["error"]["code"], "OUTCOME_UNAVAILABLE")

    def test_continuous_strict_option_and_expected_preconditions(self):
        base = self.mixer(continuous=True)
        for value in (None, 1, "true", {}, []):
            req = {**base, "continuous": value}
            self.assertFalse(jsonschema.Draft202012Validator(self.request_schema).is_valid(req))
            self.assertEqual(self.send(req)["error"]["code"], "INVALID_ARGUMENT")
        for op in ("set_mute", "set_insert_enabled", "set_track_transpose", "play"):
            req = {**base, "op": op}
            self.assertFalse(jsonschema.Draft202012Validator(self.request_schema).is_valid(req))
            self.assertEqual(self.send(req)["error"]["code"], "INVALID_ARGUMENT")
        raw = json.dumps(base)[:-1] + ',"continuous":true}'
        self.assertEqual(
            self.call(raw.encode() + b"\n", self.api)["error"]["code"], "INVALID_ARGUMENT"
        )
        out = self.receipt(self.send({**base, "expected": {"value": 0}}), "rejected")
        self.assertEqual(out["error"]["code"], "PRECONDITION_FAILED")
        self.assertFalse(out["nativeInvoked"])

    def test_continuous_read_and_write_qualification_is_unchanged(self):
        mode = self.install / "policy-automation-read"
        mode.touch()
        for op in ("set_volume", "set_pan"):
            req = self.mixer(op, op=op, value=0.25, continuous=True)
            self.receipt(self.send(req), "applied")
            self.assertEqual(
                self.send({**req, "requestId": op + "_conflict", "expected": {"value": 0.75}})[
                    "error"
                ]["code"],
                "PRECONDITION_FAILED",
            )
        req = self.mixer("read_send", op="set_send_level", sendIndex=0, continuous=True)
        self.assertEqual(self.send(req)["error"]["code"], "MIXER_AUTOMATION_UNQUALIFIED")
        mode.unlink()
        (self.install / "policy-automation-write").touch()
        self.assertEqual(
            self.send(self.mixer("write", continuous=True))["error"]["code"],
            "MIXER_AUTOMATION_UNQUALIFIED",
        )
