"""Run the persistent host, real transport and real adapter with an isolated UI."""

import json
import os
from pathlib import Path
import re
import signal
import socket
import stat
import subprocess
import tempfile
import time
import unittest

from test_describe import PROFILE, ROOT, elf


class HostTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mpc-host-")
        self.root = Path(self.temp.name)
        self.install = self.root / "installation"
        self.install.mkdir()
        self.config = self.install / "mpc-native-api.conf"
        self.config.write_text("enabled=1\n")
        self.runtime = self.root / "run"
        self.control = self.runtime / "control.sock"
        self.api = self.runtime / "api.sock"
        model = self.root / "sys/firmware/devicetree/base/model"
        model.parent.mkdir(parents=True)
        model.write_text(PROFILE["deviceTreeModel"])
        process = self.root / "proc/100"
        process.mkdir(parents=True)
        image = self.root / "MPC"
        image.write_bytes(elf(bytes.fromhex(PROFILE["buildId"])))
        (process / "exe").symlink_to(image)
        (process / "stat").write_text("100 (fixture) S " + "0 " * 18 + "200 0\n")
        self.process = None
        self.fixture = os.environ["MPC_HOST_TEST_FIXTURE"]

    def launch(self, mode="normal"):
        self.process = subprocess.Popen(
            [self.fixture, str(self.install), str(self.runtime), str(self.root), mode],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return self.wait_status({"ready"} if mode == "normal" else {"discovery_only"})

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            out, err = self.process.communicate(timeout=5)
            self.assertEqual(self.process.returncode, 0, err.decode())
            self.assertIn(b"callbacks_drained=true process_settings_preserved=true", out)
            self.assertFalse(self.api.exists())
            self.assertFalse(self.control.exists())
        self.temp.cleanup()

    def call(self, payload=b"STATUS\n", path=None):
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(str(path or self.control))
            client.sendall(payload)
            output = b""
            while b"\n" not in output:
                chunk = client.recv(16384)
                if not chunk:
                    break
                output += chunk
        return json.loads(output)

    def public(self, op="describe"):
        return self.call(json.dumps({"apiVersion": "0.1", "op": op}).encode() + b"\n", self.api)

    def wait_status(self, expected):
        until = time.monotonic() + 5
        last = None
        while time.monotonic() < until:
            if self.process.poll() is not None:
                self.fail(self.process.communicate()[1].decode())
            try:
                last = self.call()
                if last["status"] in expected:
                    return last
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            time.sleep(0.02)
        self.fail(f"Expected {expected}, last status: {last}")

    def test_reads_stop_restart_identity_and_permissions(self):
        status = self.launch()
        first = self.public()
        self.assertTrue(first["capabilities"]["get_state"]["available"])
        state = self.public("get_state")
        self.assertEqual(state["tracks"][0]["name"], 'Live "é"\n')
        self.assertEqual(state["instanceId"], first["instanceId"])
        self.assertFalse(first["capabilities"]["set_mute"]["available"])
        self.assertEqual(self.public("set_mute")["error"]["code"], "NOT_READY")
        self.assertEqual(stat.S_IMODE(self.runtime.stat().st_mode), 0o700)
        for path in (self.control, self.api):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(self.call(b"STOP\n")["status"], "stopped")
        self.assertFalse(self.api.exists())
        self.assertTrue(self.control.exists())
        self.call(b"START\n")
        self.wait_status({"ready"})
        second = self.public()
        self.assertEqual(second["deviceId"], status["deviceId"])
        self.assertNotEqual(second["instanceId"], first["instanceId"])
        self.assertNotEqual(second["readiness"]["adapter"], first["readiness"]["adapter"])
        self.assertTrue(status["restartRequiredToUnload"])
        cli = subprocess.run(
            [os.environ["MPC_API_TEST_BINARY"], "host-status", "--socket", str(self.control)],
            capture_output=True,
            check=True,
        )
        self.assertEqual(json.loads(cli.stdout)["status"], "ready")

    def test_startup_waits_without_blocking_discovery_or_stop(self):
        (self.install / "gate").touch()
        self.launch_waiting()
        self.assertEqual(self.public()["readiness"]["status"], "discovery_only")
        self.assertEqual(self.call(b"STOP\n")["status"], "stopped")
        (self.install / "gate").unlink()
        self.call(b"START\n")
        self.wait_status({"ready"})

    def launch_waiting(self):
        # launch() expects native readiness; waiting/disabled are deliberate here.
        self.process = subprocess.Popen(
            [self.fixture, str(self.install), str(self.runtime), str(self.root), "normal"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return self.wait_status({"waiting_for_ui", "disabled", "configuration_error"})

    def test_timeout_retains_callbacks_until_ui_resumes(self):
        self.launch()
        self.public("get_state")  # Register native lifecycle observers.
        paused = self.install / "pause-ui"
        paused.touch()
        status = self.call(b"STOP\n")
        self.assertEqual(status["status"], "draining")
        self.assertFalse(self.api.exists())
        self.assertEqual(self.call(b"START\n")["status"], "draining")
        paused.unlink()
        self.wait_status({"stopped"})
        self.call(b"START\n")
        self.wait_status({"ready"})
        self.assertIn("tracks", self.public("get_state"))

    def test_build_and_code_mismatches_leave_discovery_available(self):
        for mode, error in (("build", "BUILD_MISMATCH"), ("fingerprint", "FINGERPRINT_MISMATCH")):
            with self.subTest(mode=mode):
                status = self.launch(mode)
                self.assertEqual(status["nativeError"], error)
                result = self.public()
                self.assertEqual(result["readiness"]["status"], "discovery_only")
                self.assertFalse(result["capabilities"]["get_state"]["available"])
                self.process.terminate()
                self.assertEqual(self.process.wait(timeout=5), 0)
                self.process.communicate()

    def test_disabled_and_malformed_configuration_can_be_repaired(self):
        self.config.write_text("enabled=0\n")
        self.launch_waiting()
        self.assertFalse(self.api.exists())
        self.config.write_text("enabled=1\nunknown=1\n")
        self.assertEqual(self.call(b"START\n")["status"], "configuration_error")
        self.config.write_text("enabled=1\n")
        self.call(b"START\n")
        self.wait_status({"ready"})

    def test_exclusive_host_and_stale_socket_recovery(self):
        self.launch()
        duplicate = subprocess.run(
            [self.fixture, str(self.install), str(self.runtime), str(self.root), "normal"],
            capture_output=True,
            timeout=3,
        )
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertEqual(self.call()["status"], "ready")
        device = self.public()["deviceId"]
        self.process.kill()
        self.process.communicate()
        self.assertTrue(self.api.exists())
        self.launch()
        self.assertEqual(self.public()["deviceId"], device)

    def test_socket_replacement_is_preserved_and_slow_control_is_bounded(self):
        self.launch()
        with socket.socket(socket.AF_UNIX) as slow:
            slow.connect(str(self.control))
            slow.sendall(b"S")
            before = time.monotonic()
            self.assertEqual(self.call()["status"], "ready")
            self.assertLess(time.monotonic() - before, 1)
        self.assertFalse(self.call(b"STATUS\n\0extra")["ok"])
        self.api.unlink()
        self.api.write_text("replacement")
        self.call(b"STOP\n")
        self.assertEqual(self.api.read_text(), "replacement")
        self.assertEqual(self.call(b"START\n")["status"], "service_failed")
        self.api.unlink()

    def test_cleanup_requires_exited_host_and_preserves_replacement_paths(self):
        self.launch()

        def cleanup():
            return subprocess.run(
                [
                    os.environ["MPC_API_TEST_BINARY"],
                    "host-cleanup",
                    "--directory",
                    str(self.runtime),
                ],
                capture_output=True,
            )

        self.assertNotEqual(cleanup().returncode, 0)
        self.assertTrue(self.control.exists())
        self.assertTrue(self.api.exists())
        self.call(b"STOP\n")
        self.assertNotEqual(cleanup().returncode, 0)
        self.process.kill()  # Reproduce service termination without ELF destructors.
        self.process.communicate()
        self.assertTrue(self.control.exists())
        self.assertEqual(cleanup().returncode, 0)
        self.assertFalse(self.control.exists())
        self.assertTrue((self.runtime / "host.lock").is_file())
        self.api.write_text("owned replacement")
        self.control.symlink_to(self.api)
        self.assertNotEqual(cleanup().returncode, 0)
        self.assertTrue(self.control.is_symlink())
        self.assertEqual(self.api.read_text(), "owned replacement")

    def test_library_is_inert_in_other_executables_and_exports_nothing(self):
        library = os.environ["MPC_HOST_TEST_LIBRARY"]
        dynamic = subprocess.check_output([os.environ["MPC_READELF"], "-Wd", library], text=True)
        sanitizer = re.search(r"\[(libasan\.so[^\]]*)\]", dynamic)
        preload = sanitizer[1] + ":" + library if sanitizer else library
        subprocess.run(["/bin/true"], env={**os.environ, "LD_PRELOAD": preload}, check=True)
        self.assertIn("NODELETE", dynamic)
        self.assertNotIn("libmpc_native.so", dynamic)


if __name__ == "__main__":
    unittest.main()
