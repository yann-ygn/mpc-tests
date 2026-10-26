"""Validate domain receipts emitted by the actual executor/command fixture."""

import json
import os
from pathlib import Path
import subprocess
import unittest

import jsonschema

ROOT = Path(__file__).resolve().parents[2]


class CommandContractTests(unittest.TestCase):
    def test_wire_examples_and_retained_lookup_schema(self):
        for name, schema in (
            ("command-request", "command-request"),
            ("sequence-mute-request", "command-request"),
            ("get-command-request", "get-command-request"),
        ):
            request = json.loads((ROOT / f"native-api/examples/{name}.json").read_text())
            contract = json.loads((ROOT / f"native-api/schema/{schema}.schema.json").read_text())
            jsonschema.Draft202012Validator.check_schema(contract)
            validator = jsonschema.Draft202012Validator(contract)
            validator.validate(request)
            self.assertFalse(validator.is_valid({**request, "caller": "admin"}))
        self.assertFalse(validator.is_valid({**request, "waitMs": 30001}))
        for target in ({"index": 0}, {"uiIndex": 0}, {"collection": "audio", "index": 0}):
            self.assertFalse(validator.is_valid({**request, "target": target}))

    def test_receipts_and_native_verification_match_the_contract(self):
        fixture = os.environ.get(
            "MPC_COMMANDS_TEST_FIXTURE", ROOT / "builddir/host/native-api/tests/commands_fixture"
        )
        result = subprocess.run([str(fixture)], capture_output=True, text=True, check=True)
        schema = json.loads((ROOT / "native-api/schema/command-receipt.schema.json").read_text())
        jsonschema.Draft202012Validator.check_schema(schema)
        validator = jsonschema.Draft202012Validator(schema)
        receipts = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertGreaterEqual(len(receipts), 25)
        self.assertEqual(
            {r["status"] for r in receipts},
            {"accepted", "applied", "rejected", "cancelled", "unknown"},
        )
        for receipt in receipts:
            with self.subTest(request=receipt["requestId"], status=receipt["status"]):
                validator.validate(receipt)
                if receipt["status"] == "applied":
                    self.assertEqual(receipt["verification"]["requestedValue"], receipt["value"])
                    if receipt["op"] == "set_mute":
                        self.assertEqual(receipt["verification"]["by"], ["model"])
                        self.assertIsNone(receipt["verification"]["audioSideValue"])
                    else:
                        self.assertIn("audio_copy", receipt["verification"]["by"])
                        self.assertEqual(
                            receipt["verification"]["audioSideValue"], receipt["value"]
                        )
        running = next(
            r for r in receipts if r["status"] == "unknown" and r["execution"] == "started"
        )
        self.assertIsNone(running["nativeInvoked"])
        self.assertIsNone(running["verification"])
        self.assertTrue(
            any(c["path"] == "selectedTrackId" for r in receipts for c in r["observedChanges"])
        )
        for receipt in receipts:
            if receipt["error"] and receipt["error"]["code"] == "PRECONDITION_FAILED":
                self.assertFalse(receipt["error"]["details"]["expectedValue"])
                self.assertTrue(receipt["error"]["details"]["observedValue"])

    def test_domain_schema_rejects_toggle_coercion_indices_and_revision_preconditions(self):
        schema = json.loads((ROOT / "native-api/schema/command-request.schema.json").read_text())
        jsonschema.Draft202012Validator.check_schema(schema)
        validator = jsonschema.Draft202012Validator(schema)
        request = {
            "apiVersion": "0.1",
            "op": "set_solo",
            "requestId": "example",
            "target": {"instanceId": "run_" + "0" * 32, "projectId": "1", "trackId": "2"},
            "value": True,
        }
        validator.validate(request)
        for value in (1, "true", None):
            self.assertFalse(validator.is_valid({**request, "value": value}))
        self.assertFalse(validator.is_valid({**request, "expected": {"revision": "1"}}))
        for target in ({"index": 0}, {"uiIndex": 0}, {"collection": "audio", "index": 0}):
            self.assertFalse(validator.is_valid({**request, "target": target}))
        self.assertFalse(validator.is_valid({**request, "startWithinMs": 30001}))
        self.assertFalse(validator.is_valid({**request, "op": "toggle_solo"}))


if __name__ == "__main__":
    unittest.main()
