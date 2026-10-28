"""Exercise public command delivery through the real host, handlers and executor."""

from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import socket
import time

import jsonschema

from test_host import HostHarness

ROOT = Path(__file__).resolve().parents[2]


class WireCommandTests(HostHarness):
    def setUp(self):
        super().setUp()
        self.launch("commands")
        self.description = self.public()
        self.state = self.public("get_state")
        self.target = self.state["tracks"][0]["ref"]
        self.receipt_schema = json.loads(
            (ROOT / "native-api/schema/command-receipt.schema.json").read_text()
        )

    def send(self, request):
        return self.call(json.dumps(request).encode() + b"\n", self.api)

    def command(self, request_id="wire", op="set_mute", value=True, **extra):
        return {
            "apiVersion": "0.1",
            "op": op,
            "requestId": request_id,
            "target": self.target,
            "value": value,
            "waitMs": 1000,
            **extra,
        }

    def get(self, request_id, **extra):
        return self.send(
            {
                "apiVersion": "0.1",
                "op": "get_command",
                "instanceId": self.target["instanceId"],
                "requestId": request_id,
                "waitMs": 1000,
                **extra,
            }
        )

    def receipt(self, response, status):
        jsonschema.Draft202012Validator(self.receipt_schema).validate(response)
        self.assertEqual(response["status"], status, response)
        return response

    def test_discovery_and_all_setters_use_retained_verified_receipts(self):
        jsonschema.validate(
            self.description,
            json.loads((ROOT / "native-api/schema/describe-response.schema.json").read_text()),
        )
        limits = self.description["limits"]
        self.assertEqual(limits["queueCapacity"], 16)
        self.assertEqual(limits["receiptCapacity"], 128)
        self.assertEqual(limits["requestRetentionMs"], 600000)
        self.assertTrue(self.description["capabilities"]["watch"]["available"])
        for op in ("set_mute", "set_solo", "set_record_armed"):
            with self.subTest(op=op):
                self.assertTrue(self.description["capabilities"][op]["available"])
                for value in (True, False):
                    request = self.command(op + str(value), op, value)
                    result = self.receipt(self.send(request), "applied")
                    self.assertEqual(result["verification"]["requestedValue"], value)
                    self.assertEqual(result, self.get(request["requestId"]))

    def test_expected_value_is_checked_after_queueing_and_before_noop(self):
        paused = self.install / "pause-ui"
        paused.touch()
        request = self.command(expected={"value": False}, waitMs=0)
        self.receipt(self.send(request), "accepted")
        (self.install / "manual-mute").touch()
        paused.unlink()
        result = self.receipt(self.get("wire"), "rejected")
        self.assertEqual(result["error"]["code"], "PRECONDITION_FAILED")
        self.assertEqual(result["execution"], "not_started")
        self.assertFalse(result["nativeInvoked"])

    def test_retries_are_canonical_and_do_not_reapply_after_manual_edits(self):
        request = self.command(value=False)
        first = self.receipt(self.send(request), "applied")
        self.assertFalse(first["changed"])
        (self.install / "manual-mute").touch()
        self.assertTrue(self.public("get_state")["tracks"][0]["requested"]["muted"])
        reordered = dict(reversed(list(request.items())))
        reordered["waitMs"] = 0
        reordered["startWithinMs"] = 1000
        self.assertEqual(self.send(reordered), first)
        self.assertTrue(self.public("get_state")["tracks"][0]["requested"]["muted"])
        for changed in (
            {"value": True},
            {"op": "set_solo"},
            {"expected": {"value": False}},
            {"startWithinMs": 1001},
            {"target": {**self.target, "trackId": "999"}},
        ):
            self.assertEqual(
                self.send({**request, **changed})["error"]["code"], "REQUEST_ID_CONFLICT"
            )

    def test_concurrent_duplicates_share_one_result(self):
        request = self.command(op="set_solo", value=True)
        with ThreadPoolExecutor(max_workers=8) as pool:
            responses = list(pool.map(self.send, [request] * 8))
        for response in responses:
            self.receipt(response, "applied")
            self.assertEqual(response, responses[0])
        self.assertTrue(responses[0]["changed"])

    def test_disconnect_and_wait_timeout_preserve_work_without_blocking_other_clients(self):
        paused = self.install / "pause-ui"
        paused.touch()
        request = self.command(startWithinMs=5000, waitMs=1500)
        with socket.socket(socket.AF_UNIX) as client:
            client.connect(str(self.api))
            client.sendall(json.dumps(request).encode() + b"\n")
            until = time.monotonic() + 1
            while True:
                result = self.get("wire", waitMs=0)
                if result.get("status") == "accepted":
                    break
                self.assertLess(time.monotonic(), until)
            before = time.monotonic()
            self.assertIn("capabilities", self.public())
            self.assertLess(time.monotonic() - before, 0.5)
        before = time.monotonic()
        self.receipt(self.get("wire", waitMs=30), "accepted")
        self.assertLess(time.monotonic() - before, 0.5)
        paused.unlink()
        self.receipt(self.get("wire"), "applied")

    def test_queue_start_expiry_and_backpressure_are_bounded(self):
        paused = self.install / "pause-ui"
        paused.touch()
        self.receipt(self.send(self.command("expiry", startWithinMs=10, waitMs=100)), "cancelled")
        for i in range(16):
            self.receipt(self.send(self.command(str(i), startWithinMs=5000, waitMs=0)), "accepted")
        self.assertEqual(
            self.send(self.command("overflow", waitMs=0))["error"]["code"], "QUEUE_FULL"
        )
        self.receipt(self.send(self.command("0", startWithinMs=5000, waitMs=0)), "accepted")
        self.assertIn("capabilities", self.public())
        paused.unlink()
        self.receipt(self.get("15"), "applied")

    def test_unexpired_receipts_are_not_evicted_under_retention_pressure(self):
        for i in range(128):
            self.receipt(self.send(self.command(str(i), value=False)), "applied")
        self.assertEqual(self.send(self.command("overflow"))["error"]["code"], "RETENTION_CAPACITY")
        self.receipt(self.get("0"), "applied")
        self.receipt(self.send(self.command("0", value=False)), "applied")

    def test_live_policy_is_rechecked_at_dispatch_including_noops(self):
        paused = self.install / "pause-ui"
        paused.touch()
        self.receipt(self.send(self.command(value=False, waitMs=0)), "accepted")
        denied = self.install / "deny-policy"
        denied.touch()
        paused.unlink()
        result = self.receipt(self.get("wire"), "rejected")
        self.assertEqual(result["error"]["code"], "NOT_READY")
        self.assertFalse(result["nativeInvoked"])
        denied.unlink()
        (self.install / "sequence-mute").touch()
        self.receipt(self.send(self.command("sequence")), "rejected")
        self.receipt(self.send(self.command("solo", op="set_solo")), "applied")

    def test_unknown_after_native_execution_is_retained_and_deduplicated(self):
        (self.install / "lag-audio").touch()
        request = self.command(op="set_record_armed", value=True)
        result = self.receipt(self.send(request), "unknown")
        self.assertEqual(result["execution"], "completed")
        self.assertTrue(result["nativeInvoked"])
        self.assertEqual(result["error"]["code"], "VERIFICATION_TIMEOUT")
        self.assertEqual(result, self.get("wire"))
        self.assertEqual(result, self.send(request))

    def test_stop_drains_pending_delivery_and_restart_retires_authority(self):
        self.receipt(self.send(self.command()), "applied")
        old_instance = self.target["instanceId"]
        self.assertEqual(self.call(b"STOP\n")["status"], "stopped")
        self.call(b"START\n")
        self.wait_status({"ready"})
        self.assertNotEqual(self.public()["instanceId"], old_instance)
        self.assertEqual(self.get("wire")["error"]["code"], "OUTCOME_UNAVAILABLE")
        self.assertEqual(self.send(self.command())["error"]["code"], "STALE_INSTANCE")

    def test_shutdown_drops_long_delivery_waits_and_drains_without_waiting_for_clients(self):
        paused = self.install / "pause-ui"
        paused.touch()
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(str(self.api))
            client.sendall(json.dumps(self.command(waitMs=30000)).encode() + b"\n")
            until = time.monotonic() + 1
            while self.get("wire", waitMs=0).get("status") != "accepted":
                self.assertLess(time.monotonic(), until)
            before = time.monotonic()
            status = self.call(b"STOP\n")["status"]
            self.assertIn(status, {"draining", "stopped"})
            self.assertLess(time.monotonic() - before, 1)
            self.assertEqual(client.recv(1), b"")
        paused.unlink()
        self.wait_status({"stopped"})

    def test_wire_parser_rejects_types_unknown_fields_duplicates_and_handle_overflow(self):
        base = self.command()
        mutations = (
            [{"value": x} for x in (0, 1, "true", None, [], {})]
            + [{"waitMs": x} for x in (-1, 30001, True, "1", 1.5, None)]
            + [{"startWithinMs": x} for x in (0, -1, 30001, False, "1000")]
            + [{"expected": x} for x in ({}, {"value": 1}, {"value": False, "revision": "1"})]
            + [
                {"target": {"index": 0}},
                {"target": {"uiIndex": 0}},
                {"target": {"collection": "audio", "index": 0}},
                {"target": {**self.target, "uiIndex": 0}},
                {"caller": "admin"},
                {"address": 123},
                {"instanceId": base["target"]["instanceId"]},
            ]
        )
        for field in ("projectId", "trackId"):
            for value in (0, "0", "01", "-1", "18446744073709551616", "1e2", ""):
                mutations.append({"target": {**self.target, field: value}})
        for fields in mutations:
            with self.subTest(fields=fields):
                self.assertEqual(self.send({**base, **fields})["error"]["code"], "INVALID_ARGUMENT")
        encoded = json.dumps(base)
        for raw in (
            encoded[:-1] + ',"value":false}',
            encoded.replace('"instanceId":', '"instanceId":"run_' + "0" * 32 + '","instanceId":'),
            encoded[:-1] + ',"\\u0076alue":true}',
        ):
            self.assertEqual(
                self.call(raw.encode() + b"\n", self.api)["error"]["code"], "INVALID_ARGUMENT"
            )
        self.assertEqual(self.get("wire")["error"]["code"], "OUTCOME_UNAVAILABLE")
        escaped = encoded.replace('"op"', '"\\u006fp"').replace('"wire"', '"w\\u0069re"')
        self.receipt(self.call(escaped.encode() + b"\n", self.api), "applied")
        self.receipt(self.send(base), "applied")

    def test_mute_scope_parser_requires_complete_consistent_sequence_references(self):
        sequence = self.state["selectedSequence"]["ref"]
        base = self.command(muteScope="sequence", sequence=sequence)
        invalid = [{"muteScope": x} for x in (None, True, 1, {}, "active", "")] + [
            {"op": "set_solo"},
            {"op": "set_record_armed"},
            {"muteScope": "global"},
            {"sequence": None},
            {"sequence": {}},
            {"sequence": {**sequence, "projectId": "999"}},
            {"sequence": {**sequence, "instanceId": "run_" + "f" * 32}},
            {"sequence": {**sequence, "index": 0}},
        ]
        for key in sequence:
            invalid.append({"sequence": {k: v for k, v in sequence.items() if k != key}})
        for key in ("sequenceId", "generation", "projectId"):
            for value in (0, "0", "01", "18446744073709551616", "-1", None, True):
                invalid.append({"sequence": {**sequence, key: value}})
        for fields in invalid:
            with self.subTest(fields=fields):
                self.assertEqual(self.send({**base, **fields})["error"]["code"], "INVALID_ARGUMENT")
        for missing in ("sequence", "muteScope"):
            self.assertEqual(
                self.send({k: v for k, v in base.items() if k != missing})["error"]["code"],
                "INVALID_ARGUMENT",
            )
        raw = json.dumps(base)
        for encoded in (
            raw.replace(
                '"muteScope": "sequence"', '"muteScope": "sequence", "muteScope": "global"'
            ),
            raw.replace('"generation":', '"generation": "1", "generat\\u0069on":'),
            raw.replace('"sequence":', '"sequence": ' + json.dumps(sequence) + ', "sequence":'),
        ):
            self.assertEqual(
                self.call(encoded.encode() + b"\n", self.api)["error"]["code"], "INVALID_ARGUMENT"
            )
        request = self.command(value=False)
        first = self.receipt(self.send(request), "applied")
        self.assertEqual(first, self.send({**request, "muteScope": "global"}))
