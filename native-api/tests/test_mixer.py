"""Mixer contracts through the real authenticated socket, host and managed worker."""

from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import socket
import struct
import subprocess

import jsonschema

from test_host import HostHarness
import test_wire_commands as wire_tests

ROOT = Path(__file__).resolve().parents[2]


class MixerTests(HostHarness):
    send = wire_tests.WireCommandTests.send
    get = wire_tests.WireCommandTests.get
    receipt = wire_tests.WireCommandTests.receipt
    command = wire_tests.WireCommandTests.command

    def setUp(self):
        super().setUp()
        self.launch("mixer-commands")
        self.state = self.public("get_state")
        self.target = self.state["tracks"][0]["ref"]
        self.generation = self.state["tracks"][0]["mixer"]["generation"]
        self.receipt_schema = json.loads(
            (ROOT / "native-api/schema/command-receipt.schema.json").read_text()
        )
        self.request_schema = json.loads(
            (ROOT / "native-api/schema/command-request.schema.json").read_text()
        )
        self.assertIsNotNone(self.generation)

    def mixer(self, request_id="volume", op="set_volume", value=0.125, **extra):
        return self.command(request_id, op, value, mixerGeneration=self.generation, **extra)

    def test_shared_mixer_examples_match_the_strict_contract(self):
        for name in ("mixer-request", "pan-read-request", "send-request", "insert-request"):
            example = json.loads((ROOT / f"native-api/examples/{name}.json").read_text())
            jsonschema.validate(example, self.request_schema)
        for name in ("mixer-applied", "pan-read-superseded"):
            example = json.loads((ROOT / f"native-api/examples/{name}.json").read_text())
            jsonschema.validate(example, self.receipt_schema)
        for path in (ROOT / "native-api/examples").glob("describe*response.json"):
            jsonschema.validate(
                json.loads(path.read_text()),
                json.loads((ROOT / "native-api/schema/describe-response.schema.json").read_text()),
            )
        jsonschema.validate(
            json.loads((ROOT / "native-api/examples/state-response.json").read_text()),
            json.loads((ROOT / "native-api/schema/state-response.schema.json").read_text()),
        )

    def test_numeric_boolean_endpoints_noops_conflicts_and_float_quantization(self):
        for op, indices in (
            ("set_volume", [{}]),
            ("set_pan", [{}]),
            ("set_send_level", [{"sendIndex": i} for i in range(4)]),
            ("set_insert_rack_enabled", [{}]),
            ("set_insert_enabled", [{"slotIndex": 0}, {"slotIndex": 3}]),
        ):
            numeric = op in ("set_volume", "set_pan", "set_send_level")
            for index in indices:
                for endpoint in (0, 1):
                    value = endpoint if numeric else bool(endpoint)
                    label = op + str(index) + str(endpoint)
                    label = "case_" + str(abs(hash(label)))
                    r = self.mixer(label, op, value, **index)
                    jsonschema.validate(r, self.request_schema)
                    result = self.receipt(self.send(r), "applied")
                    self.assertEqual(result["verification"]["by"], ["model"])
                    self.assertEqual(result["verification"]["requestedValue"], value)
                    self.assertEqual(self.get(label), result)
                    noop = self.receipt(self.send({**r, "requestId": label + "n"}), "applied")
                    self.assertFalse(noop["nativeInvoked"])
                    self.assertFalse(noop["changed"])
                    expected = 1 - endpoint if numeric else not value
                    conflict = self.receipt(
                        self.send({**r, "requestId": label + "c", "expected": {"value": expected}}),
                        "rejected",
                    )
                    self.assertEqual(conflict["error"]["code"], "PRECONDITION_FAILED")
                    self.assertFalse(conflict["nativeInvoked"])
        r = self.mixer("quantized", value=0.123456789)
        result = self.receipt(self.send(r), "applied")
        actual = struct.unpack("f", struct.pack("f", r["value"]))[0]
        self.assertEqual(result["value"], r["value"])
        self.assertEqual(self.send({**r, "value": actual})["error"]["code"], "REQUEST_ID_CONFLICT")
        self.assertEqual(result["verification"]["requestedValue"], actual)
        noop = self.receipt(
            self.send({**r, "requestId": "binary32_noop", "expected": {"value": actual}}), "applied"
        )
        self.assertFalse(noop["nativeInvoked"])
        state = self.public("get_state")
        jsonschema.validate(
            state, json.loads((ROOT / "native-api/schema/state-response.schema.json").read_text())
        )
        self.assertAlmostEqual(state["tracks"][0]["mixer"]["volume"], actual, places=8)

    def test_strict_numeric_types_indices_scope_and_request_conflicts(self):
        original = self.mixer()
        for update in (
            {"value": True},
            {"value": "0.5"},
            {"value": None},
            {"value": -0.1},
            {"value": 1.01},
            {"expected": {"value": True}},
            {"sendIndex": 0},
            {"mixerGeneration": "0"},
            {"muteScope": "global"},
            {"slotIndex": 1},
        ):
            with self.subTest(update=update):
                r = {**original, **update}
                self.assertFalse(jsonschema.Draft202012Validator(self.request_schema).is_valid(r))
                self.assertEqual(self.send(r)["error"]["code"], "INVALID_ARGUMENT")
        raw = json.dumps(original)
        for token in ("NaN", "Infinity", "1e309", "1e-9999", "+0.5", ".5", "01", "0x1", "1."):
            payload = raw.replace("0.125", token).encode() + b"\n"
            self.assertEqual(self.call(payload, self.api)["error"]["code"], "INVALID_ARGUMENT")
        self.receipt(self.send(original), "applied")
        for update in (
            {"value": 0.25},
            {"expected": {"value": 0.125}},
            {"mixerGeneration": "9999"},
            {"op": "set_pan"},
        ):
            self.assertEqual(
                self.send({**original, **update})["error"]["code"], "REQUEST_ID_CONFLICT"
            )
        r = self.mixer("send", op="set_send_level", sendIndex=0)
        self.receipt(self.send(r), "applied")
        self.assertEqual(self.send({**r, "sendIndex": 1})["error"]["code"], "REQUEST_ID_CONFLICT")
        for index in (1, 2):
            empty = self.receipt(
                self.send(
                    self.mixer("empty" + str(index), "set_insert_enabled", False, slotIndex=index)
                ),
                "rejected",
            )
            self.assertEqual(empty["error"]["code"], "EMPTY_INSERT")
            self.assertFalse(empty["nativeInvoked"])

    def test_live_automation_playback_context_and_disconnect_ownership(self):
        for marker in ("policy-mixer", "policy-track-mute", "policy-playing"):
            path = self.install / marker
            path.touch()
            self.receipt(self.send(self.mixer(marker)), "applied")
            path.unlink()
        for marker, code in (
            ("policy-automation-write", "MIXER_AUTOMATION_UNQUALIFIED"),
            ("policy-recording", "RECORDING_UNQUALIFIED"),
            ("policy-song-source", "PLAYBACK_CONTEXT_UNQUALIFIED"),
        ):
            path = self.install / marker
            path.touch()
            result = self.receipt(self.send(self.mixer(marker)), "rejected")
            self.assertEqual(result["error"]["code"], code)
            self.assertFalse(result["nativeInvoked"])
            path.unlink()
        (self.install / "policy-automation-read").touch()
        paused = self.install / "pause-ui"
        paused.touch()
        r = self.mixer("disconnected", waitMs=0)
        with socket.socket(socket.AF_UNIX) as client:
            client.connect(str(self.api))
            client.sendall(json.dumps(r).encode() + b"\n")
            client.recv(100000)
        paused.unlink()
        result = self.receipt(self.get(r["requestId"]), "applied")
        self.assertEqual(self.send(r), result)
        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: self.send(r), range(16)))
        self.assertTrue(all(x == result for x in results))

    def test_read_volume_writes_noops_sources_and_other_operation_gates(self):
        (self.install / "policy-automation-read").touch()
        for marker in ("policy-mixer", "policy-track-mute", "policy-playing"):
            path = self.install / marker
            path.touch()
            try:
                for index, value in enumerate((0, 1, 0.123456789)):
                    request = self.mixer(marker + str(index), value=value)
                    result = self.receipt(self.send(request), "applied")
                    self.assertTrue(result["nativeInvoked"])
                    expected = struct.unpack("<f", struct.pack("<f", value))[0]
                    self.assertEqual(result["verification"]["requestedValue"], expected)
                    self.assertEqual(self.send(request), result)
                    noop = self.receipt(
                        self.send(
                            self.mixer(
                                marker + str(index) + "noop", value=value, expected={"value": value}
                            )
                        ),
                        "applied",
                    )
                    self.assertFalse(noop["nativeInvoked"])
                    conflict = self.send(
                        self.mixer(
                            marker + str(index) + "conflict", value=value, expected={"value": 0.75}
                        )
                    )
                    self.assertEqual(conflict["error"]["code"], "PRECONDITION_FAILED")
            finally:
                path.unlink()
        for op in ("set_send_level", "set_insert_rack_enabled", "set_insert_enabled"):
            extra = (
                {"sendIndex": 0}
                if op == "set_send_level"
                else {"slotIndex": 0}
                if op == "set_insert_enabled"
                else {}
            )
            result = self.send(
                self.mixer(op, op=op, value=False if "enabled" in op else 0.5, **extra)
            )
            self.assertEqual(result["error"]["code"], "MIXER_AUTOMATION_UNQUALIFIED")
            self.assertFalse(result["nativeInvoked"])
        (self.install / "policy-read-source-unknown").touch()
        result = self.send(self.mixer("unknown-source", value=0.75))
        self.assertEqual(result["error"]["code"], "AUTOMATION_SOURCE_UNQUALIFIED")
        self.assertFalse(result["nativeInvoked"])
        # Source restrictions never bypass expected checks or remove a verified no-op.
        result = self.send(self.mixer("source-noop", value=0.123456789))
        self.assertEqual(result["status"], "applied")
        self.assertFalse(result["nativeInvoked"])
        result = self.send(
            self.mixer("source-conflict", value=0.123456789, expected={"value": 0.75})
        )
        self.assertEqual(result["error"]["code"], "PRECONDITION_FAILED")
        support = self.public("get_state")["tracks"][0]["mixer"]["operations"]
        self.assertEqual(support["set_volume"]["automation"]["read"], "qualified_writes")
        self.assertEqual(support["set_pan"]["automation"]["read"], "qualified_writes")

    def test_read_pan_float_boundaries_source_noops_and_parallel_receipt_ownership(self):
        (self.install / "policy-automation-read").touch()
        for index, value in enumerate((0, 0.5, 1, 0.123456789, 1e-40, 1 - 1e-9)):
            request = self.mixer("pan-boundary-" + str(index), "set_pan", value)
            result = self.receipt(self.send(request), "applied")
            self.assertEqual(result["value"], value)
            self.assertEqual(
                result["verification"]["requestedValue"],
                struct.unpack("f", struct.pack("f", value))[0],
            )
            self.assertEqual(self.get(request["requestId"]), result)
        stale = self.send({**self.mixer("pan-stale", "set_pan", 0), "mixerGeneration": "99999"})
        self.assertEqual(stale["error"]["code"], "STALE_MIXER")
        self.assertFalse(stale["nativeInvoked"])
        (self.install / "policy-read-source-unknown").touch()
        result = self.send(self.mixer("pan-source-noop", "set_pan", 1))
        self.assertEqual(result["status"], "applied")
        self.assertFalse(result["nativeInvoked"])
        result = self.send(self.mixer("pan-source-conflict", "set_pan", 1, expected={"value": 0}))
        self.assertEqual(result["error"]["code"], "PRECONDITION_FAILED")
        result = self.send(self.mixer("pan-source-write", "set_pan", 0))
        self.assertEqual(result["error"]["code"], "AUTOMATION_SOURCE_UNQUALIFIED")
        self.assertFalse(result["nativeInvoked"])
        (self.install / "policy-read-source-unknown").unlink()
        requests = [self.mixer("pan-pressure-" + str(i), "set_pan", (i + 1) / 10) for i in range(8)]
        with ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(self.send, requests))
        for request, result in zip(requests, results, strict=True):
            self.assertEqual(result["status"], "applied", result)
            self.assertEqual(self.get(request["requestId"]), result)
            self.assertEqual(self.send(request), result)

    def test_stale_generation_and_mixer_fixture_receipts(self):
        stale = self.receipt(self.send({**self.mixer(), "mixerGeneration": "99999"}), "rejected")
        self.assertEqual(stale["error"]["code"], "STALE_MIXER")
        self.assertFalse(stale["nativeInvoked"])
        fixture = ROOT / "builddir/host/native-api/tests/mixer_fixture"
        output = subprocess.check_output([str(fixture)], text=True)
        rows = [json.loads(line) for line in output.splitlines()]
        self.assertGreater(len(rows), 50)
        for row in rows:
            jsonschema.validate(row, self.receipt_schema)
        self.assertTrue(any(row["status"] == "unknown" for row in rows))
