import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "mpc_firmware_profile", ROOT / "native-api/adapter/firmware_profile.py"
)
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.state = json.loads(profile.STATE.read_text())
        self.control = json.loads(profile.CONTROL.read_text())

    def test_known_profile_validates(self):
        profile.validate(self.state, self.control)

    def test_invalid_profiles_rejected(self):
        cases = {
            "schema": lambda s, c: s.update(profileSchemaVersion=2),
            "boolean_schema": lambda s, c: s.update(profileSchemaVersion=True),
            "abi": lambda s, c: s["abi"].update(pointerBits=64),
            "build": lambda s, c: c.update(buildId="1122"),
            "version": lambda s, c: c.update(firmwareVersion="other"),
            "family": lambda s, c: c["supportedProgramFamilies"].append("midi"),
            "program_type": lambda s, c: s["programTypes"].update({"4": "unqualified"}),
            "program_vtable": lambda s, c: s["programTypes"].update({"1": "audio"}),
            "qualification": lambda s, c: c.update(capabilityQualification="production"),
            "fingerprint_length": lambda s, c: c["functions"][0].update(prefixHex="abcd"),
            "duplicate_function": lambda s, c: c["functions"].append(
                copy.deepcopy(c["functions"][0])
            ),
            "unverified_setter": lambda s, c: c["bindings"][0].update(setterRva=100),
            "unverified_filter": lambda s, c: c["bindings"][2].update(filterInvokerRva=100),
            "owner": lambda s, c: c["bindings"][0].update(propertyOwner="track"),
            "offset": lambda s, c: c["bindings"][0].update(propertyOffset=0x10004),
            "alignment": lambda s, c: s["vtables"].update(project=1),
        }
        for label, mutate in cases.items():
            with self.subTest(label=label):
                state, control = copy.deepcopy(self.state), copy.deepcopy(self.control)
                mutate(state, control)
                with self.assertRaises(ValueError):
                    profile.validate(state, control)


class NativeAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = os.environ.get(
            "MPC_NATIVE_TEST_FIXTURE", ROOT / "builddir/host/native-api/tests/native_fixture"
        )
        cls.library = os.environ.get(
            "MPC_NATIVE_TEST_LIBRARY", ROOT / "builddir/host/native-api/libmpc_native.so"
        )
        cls.readelf = os.environ.get("MPC_READELF", "readelf")

    def test_isolated_native_guards_and_typed_operations(self):
        run = subprocess.run([str(self.fixture)], check=True, capture_output=True, text=True)
        self.assertGreaterEqual(int(run.stdout.split("=")[1]), 25)

    def test_shared_library_export_allowlist(self):
        output = subprocess.run(
            [self.readelf, "-W", "--dyn-syms", str(self.library)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        names = {
            row[7]
            for line in output.splitlines()
            if len(row := line.split()) >= 8 and row[4] in ("GLOBAL", "WEAK") and row[6] != "UND"
        }
        self.assertEqual(
            names,
            {
                "mpc_native_open",
                "mpc_native_close",
                "mpc_native_get_status",
                "mpc_native_read_track",
                "mpc_native_set_mute",
                "mpc_native_set_solo",
                "mpc_native_set_record_armed",
                "mpc_native_error_name",
                "mpc_executor_open",
                "mpc_executor_open_authoritative",
                "mpc_executor_wait_ready",
                "mpc_executor_get_status",
                "mpc_executor_submit",
                "mpc_executor_wait",
                "mpc_executor_cancel",
                "mpc_executor_release",
                "mpc_executor_shutdown",
                "mpc_executor_close",
            },
        )
        dynamic = subprocess.run(
            [self.readelf, "-d", str(self.library)], check=True, capture_output=True, text=True
        ).stdout
        self.assertIn("Library soname: [libmpc_native.so]", dynamic)


if __name__ == "__main__":
    unittest.main()
