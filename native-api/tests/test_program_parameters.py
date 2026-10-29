"""Parameter metadata cannot promote static IDs into public native commands."""

import copy
import importlib.util
import json
from pathlib import Path
import unittest

from jsonschema import ValidationError

from test_host import HostHarness

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "program_parameters", ROOT / "tools/mpc_program_parameters.py"
)
parameters = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parameters)


class ParameterMetadataTests(unittest.TestCase):
    def setUp(self):
        self.profile = json.loads(parameters.PROFILE.read_text())

    def test_catalog_validates_and_header_excludes_enum_only_and_unused_ids(self):
        parameters.validate(self.profile)
        header = parameters.header(self.profile)
        ids = header.split("static const unsigned param_ids[] = {", 1)[1]
        self.assertIn("514u,", ids)
        self.assertNotIn("1069u,", ids)
        self.assertNotIn("1073u,", ids)
        self.assertNotIn("555u,", ids)
        self.assertEqual(len([p for p in self.profile["parameters"] if p["value"]]), 6)

    def test_capability_and_evidence_mutations_fail_closed(self):
        mutations = {
            "enable_operation": lambda p: p["capability"].update(available=True),
            "add_public_operation": lambda p: p["capability"].update(
                publicOperation="set_parameter"
            ),
            "public_write": lambda p: p["parameters"][0].update(publicWrite=True),
            "public_read": lambda p: p["parameters"][0].update(publicRead=True),
            "invented_family": lambda p: p["parameters"][0].update(families=["plugin"]),
            "duplicate_id": lambda p: p["parameters"][0].update(
                nativeId=p["parameters"][1]["nativeId"]
            ),
            "duplicate_key": lambda p: p["parameters"][0].update(key=p["parameters"][1]["key"]),
            "wrong_firmware": lambda p: p["firmware"].update(buildId="0" * 40),
            "wrong_model": lambda p: p["firmware"].update(model="MPC Live"),
            "missing_landmark": lambda p: p["functions"].pop(),
            "duplicate_landmark": lambda p: p["functions"].append(p["functions"][0]),
            "short_fingerprint": lambda p: p["functions"][0].update(prefixHex="00"),
            "invalid_range": lambda p: p["parameters"][0]["value"].update(minimum=2),
            "invented_range": lambda p: p["parameters"][0]["value"].update(maximum=2),
            "invented_clamp": lambda p: p["parameters"][0]["value"].update(
                getterCanonicalization="binary32_endpoint_clamp"
            ),
            "missing_readback": lambda p: p["parameters"][0].update(readback=None),
            "unused_as_control": lambda p: p["parameters"][6].update(
                classification="native_control"
            ),
            "unsupported_as_candidate": lambda p: p["parameters"][-1].update(
                classification="candidate"
            ),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                altered = copy.deepcopy(self.profile)
                mutation(altered)
                with self.assertRaises((ValueError, ValidationError)):
                    parameters.header(altered)


class ParameterCapabilityTests(HostHarness):
    def test_ready_command_host_keeps_generic_parameter_writes_disabled(self):
        self.launch("commands")
        description = self.public()
        self.assertEqual(
            description["capabilities"]["set_parameter"],
            {"available": False, "reason": "NOT_QUALIFIED"},
        )
        before = self.public("get_state")
        request = {
            "apiVersion": "0.1",
            "op": "set_parameter",
            "requestId": "must-not-write",
            "target": before["tracks"][0]["ref"],
            "parameterId": 514,
            "value": 0.5,
        }
        self.assertEqual(self.public("set_parameter")["error"]["code"], "UNSUPPORTED_OPERATION")
        result = self.call(json.dumps(request).encode() + b"\n", self.api)
        self.assertEqual(result["error"]["code"], "INVALID_ARGUMENT")
        after = self.public("get_state")
        self.assertEqual(before["tracks"], after["tracks"])


if __name__ == "__main__":
    unittest.main()
