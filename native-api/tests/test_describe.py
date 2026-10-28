"""Exercise the actual socket service against synthetic proc/device-tree fixtures."""

import concurrent.futures
import contextlib
import json
import os
from pathlib import Path
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import time
import threading
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT / "builddir/host/native-api/mpc-api"
PROFILE = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
REQUEST = b'{"apiVersion":"0.1","op":"describe"}\n'


def elf(build_id, bits=32):
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
    note += b"\0" * (-len(note) % 4)
    ident = b"\x7fELF" + bytes([1 if bits == 32 else 2, 1, 1]) + bytes(9)
    # Place the note away from the research build's fixed file offset.
    if bits == 32:
        header = ident + struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, 52, 0, 0, 52, 32, 1, 0, 0, 0)
        entry = struct.pack("<IIIIIIII", 4, 1024, 0, 0, len(note), len(note), 4, 4)
    else:
        header = ident + struct.pack("<HHIQQQIHHHHHH", 3, 62, 1, 0, 64, 0, 0, 64, 56, 1, 0, 0, 0)
        entry = struct.pack("<IIQQQQQQ", 4, 4, 1024, 0, 0, len(note), len(note), 4)
    return (header + entry).ljust(1024, b"\0") + note


class DescribeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = Path(os.environ.get("MPC_API_TEST_BINARY", BINARY))
        # Optional external validation; core socket/metadata tests use only stdlib.
        try:
            import jsonschema
        except ImportError:
            cls.validator = None
        else:
            schema = json.loads(
                (ROOT / "native-api/schema/describe-response.schema.json").read_text()
            )
            jsonschema.Draft202012Validator.check_schema(schema)
            cls.validator = jsonschema.Draft202012Validator(schema)
            state_schema = json.loads(
                (ROOT / "native-api/schema/state-response.schema.json").read_text()
            )
            jsonschema.Draft202012Validator.check_schema(state_schema)
            cls.state_validator = jsonschema.Draft202012Validator(state_schema)
            request_schema = json.loads(
                (ROOT / "native-api/schema/describe-request.schema.json").read_text()
            )
            jsonschema.Draft202012Validator.check_schema(request_schema)
            jsonschema.validate(json.loads(REQUEST), request_schema)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mpc-api-test-")
        self.root = Path(self.temp.name)
        self.sock = self.root / "api.sock"
        model = self.root / "sys/firmware/devicetree/base/model"
        model.parent.mkdir(parents=True)
        model.write_bytes(PROFILE["deviceTreeModel"].encode() + b"\0")
        self.add_mpc()
        self.process = None
        self.start()

    def add_mpc(self, pid=100, started=200, build_id=None, bits=32):
        process = self.root / f"proc/{pid}"
        process.mkdir(parents=True, exist_ok=True)
        (process / "comm").write_text("MPC Main Thread\n")
        self.set_start(pid, started)
        image = self.root / f"images/{pid}/MPC"
        image.parent.mkdir(parents=True, exist_ok=True)
        image.write_bytes(elf(bytes.fromhex(build_id or PROFILE["buildId"]), bits))
        if not (process / "exe").is_symlink():
            (process / "exe").symlink_to(image)

    def set_start(self, pid, started):
        # Field 22 follows fields 3..21. Parentheses/spaces in comm must not shift parsing.
        (self.root / f"proc/{pid}/stat").write_text(
            f"{pid} (MPC (test worker)) S " + "0 " * 18 + f"{started} 0\n"
        )

    def start(self):
        self.process = subprocess.Popen(
            [
                str(self.binary),
                "serve",
                "--socket",
                str(self.sock),
                "--device-id",
                "dev_fixture",
                "--root",
                str(self.root),
            ],
            stderr=subprocess.PIPE,
        )
        until = time.monotonic() + 5
        while not self.sock.exists():
            if self.process.poll() is not None:
                self.fail(self.process.stderr.read().decode())
            if time.monotonic() > until:
                self.fail("Service did not bind")
            time.sleep(0.01)

    def stop(self):
        if self.process:
            self.process.send_signal(signal.SIGTERM)
            self.process.communicate(timeout=3)
            self.assertEqual(self.process.returncode, 0)
            self.process = None

    def tearDown(self):
        try:
            self.stop()
        finally:
            self.temp.cleanup()

    def call(self, request=REQUEST, shutdown=False):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(3)
            client.connect(str(self.sock))
            client.sendall(request)
            if shutdown:
                client.shutdown(socket.SHUT_WR)
            chunks = []
            while chunk := client.recv(16384):
                chunks.append(chunk)
                if b"\n" in chunk:
                    break
        response = json.loads(b"".join(chunks))
        if self.validator:
            if "revision" in response:
                self.state_validator.validate(response)
            else:
                self.validator.validate(response)
        return response

    def assert_unavailable(self, result, reason):
        self.assertEqual(result["capabilities"]["describe"], {"available": True, "reason": None})
        for op, capability in result["capabilities"].items():
            if op != "describe":
                self.assertFalse(capability["available"], op)
                self.assertEqual(capability["reason"], reason)

    def test_live_metadata_and_truthful_discovery_only_capabilities(self):
        result = self.call()
        self.assertEqual(result["deviceId"], "dev_fixture")
        self.assertRegex(result["instanceId"], r"^run_[0-9a-f]{32}$")
        self.assertEqual(result["firmware"]["model"], PROFILE["deviceTreeModel"])
        self.assertEqual(result["firmware"]["buildId"], PROFILE["buildId"])
        self.assertEqual(result["firmware"]["version"], "3.9.1.2")
        self.assertEqual(result["firmware"]["versionSource"], "build_profile")
        self.assertEqual(result["readiness"]["firmwareProfile"], "matched")
        self.assertEqual(result["readiness"]["nativeAdapter"], "not_attached")
        self.assertEqual(result["readableFields"], [])
        self.assertEqual(result["limits"]["queueCapacity"], 0)
        self.assertEqual(result["limits"]["requestRetentionMs"], 0)
        self.assert_unavailable(result, "NOT_IMPLEMENTED")

    def test_identity_stable_until_process_or_service_restart(self):
        before = self.call()
        self.assertEqual(before["instanceId"], self.call()["instanceId"])
        self.set_start(100, 201)
        replaced = self.call()
        self.assertNotEqual(before["instanceId"], replaced["instanceId"])
        self.stop()
        self.assertFalse(self.sock.exists())
        self.start()
        restarted = self.call()
        self.assertNotEqual(replaced["instanceId"], restarted["instanceId"])
        self.assertEqual(restarted["deviceId"], before["deviceId"])

    def test_same_build_on_other_model_is_not_supported(self):
        (self.root / "sys/firmware/devicetree/base/model").write_bytes(
            b"Akai Professional MPC Other\0"
        )
        result = self.call()
        self.assertEqual(result["readiness"]["firmwareProfile"], "unsupported")
        self.assert_unavailable(result, "UNSUPPORTED_FIRMWARE")

    def test_unknown_build_reports_observed_id_without_guessing_version(self):
        self.add_mpc(build_id="11223344", bits=64)
        result = self.call()
        self.assertEqual(result["firmware"]["buildId"], "11223344")
        self.assertIsNone(result["firmware"]["version"])
        self.assertIsNone(result["firmware"]["versionSource"])
        self.assert_unavailable(result, "UNSUPPORTED_FIRMWARE")

    def test_missing_or_multiple_mpc_processes(self):
        (self.root / "proc/100/exe").unlink()
        stopped = self.call()
        self.assertEqual(stopped["readiness"]["mpcProcess"], "not_running")
        self.assertIsNone(stopped["firmware"]["buildId"])
        self.assert_unavailable(stopped, "NOT_READY")
        self.add_mpc()
        self.add_mpc(pid=101)
        result = self.call()
        self.assertEqual(result["readiness"]["mpcProcess"], "ambiguous")
        self.assert_unavailable(result, "NOT_READY")

    def test_thread_names_do_not_override_executable_identity(self):
        (self.root / "proc/100/comm").write_text("JUCE Message Thread\n")
        self.assertEqual(self.call()["readiness"]["mpcProcess"], "running")
        other = self.root / "proc/101"
        other.mkdir()
        (other / "comm").write_text("MPC\n")
        image = self.root / "images/other"
        image.write_bytes(b"unrelated")
        (other / "exe").symlink_to(image)
        self.assertEqual(self.call()["readiness"]["mpcProcess"], "running")

    def test_bad_elf_and_missing_model_remain_discoverable(self):
        (self.root / "proc/100/exe").write_bytes(b"not ELF")
        result = self.call()
        self.assertEqual(result["readiness"]["mpcProcess"], "unreadable")
        self.assertIsNone(result["firmware"]["buildId"])
        self.assert_unavailable(result, "NOT_READY")
        self.add_mpc()
        (self.root / "sys/firmware/devicetree/base/model").unlink()
        result = self.call()
        self.assertIsNone(result["firmware"]["model"])
        self.assert_unavailable(result, "NOT_READY")

    def test_json_string_escaping_and_metadata_utf8(self):
        path = self.root / "sys/firmware/devicetree/base/model"
        path.write_bytes('MPC "é"\\x\n\0'.encode())
        self.assertEqual(self.call()["firmware"]["model"], 'MPC "é"\\x')
        path.write_bytes(b"bad\xff\0")
        self.assertIsNone(self.call()["firmware"]["model"])

    def test_whitespace_order_and_escaped_identifiers(self):
        response = self.call(b' { "op" : "de\\u0073cribe", "api\\u0056ersion" : "0.1" } \r\n')
        self.assertNotIn("error", response)

    def test_invalid_requests_are_bounded_and_do_not_crash_server(self):
        requests = [
            b"{}\n",
            b"[]\n",
            b"null\n",
            b'{"apiVersion":0.1,"op":"describe"}\n',
            b'{"apiVersion":"0.1","op":"describe","extra":"x"}\n',
            b'{"apiVersion":"0.1","op":"describe","op":"describe"}\n',
            b'{"op":"describe","op":"describe"}\n',
            b'{"apiVersion":"0.1","op":"describe"}x\n',
            b'{"apiVersion":"0.1","op":"de\x00scribe"}\n',
            b'{"apiVersion":"0.1","op":"\\ud800"}\n',
            b'{"apiVersion":"0.1","op":"\\x64escribe"}\n',
            b"x" * 1025 + b"\n",
        ]
        for request in requests:
            with self.subTest(request=request[:100]):
                self.assertEqual(self.call(request)["error"]["code"], "INVALID_ARGUMENT")
        self.assertNotIn("error", self.call())

    def test_versions_and_operations_return_structured_errors(self):
        self.assertEqual(
            self.call(b'{"apiVersion":"2","op":"describe"}\n')["error"]["code"],
            "UNSUPPORTED_API_VERSION",
        )
        self.assertEqual(
            self.call(b'{"apiVersion":"0.1","op":"raw_call"}\n')["error"]["code"],
            "UNSUPPORTED_OPERATION",
        )
        self.assertEqual(
            self.call(b'{"apiVersion":"0.1","op":"set_mute"}\n')["error"]["code"],
            "INVALID_ARGUMENT",
        )

    def test_socket_permissions_and_live_socket_preserved(self):
        self.assertEqual(stat.S_IMODE(self.sock.stat().st_mode), 0o600)
        attempt = subprocess.run(
            [str(self.binary), "serve", "--socket", str(self.sock), "--device-id", "other"],
            capture_output=True,
            timeout=3,
        )
        self.assertNotEqual(attempt.returncode, 0)
        self.assertNotIn("error", self.call())

    def test_public_or_symlink_parent_refused(self):
        public = self.root / "public"
        public.mkdir(mode=0o755)
        link = self.root / "link"
        link.symlink_to(self.root, target_is_directory=True)
        for parent in (public, link):
            attempt = subprocess.run(
                [
                    str(self.binary),
                    "serve",
                    "--socket",
                    str(parent / "other.sock"),
                    "--device-id",
                    "other",
                ],
                capture_output=True,
                timeout=3,
            )
            self.assertNotEqual(attempt.returncode, 0)
            self.assertFalse((parent / "other.sock").exists())

    def test_slow_and_disconnected_clients_do_not_block_discovery(self):
        with contextlib.closing(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)) as slow:
            slow.settimeout(3)
            slow.connect(str(self.sock))
            slow.sendall(b"{")
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                results = list(pool.map(lambda _: self.call(), range(12)))
            self.assertTrue(all("error" not in result for result in results))
            self.assertEqual(len({r["instanceId"] for r in results}), 1)
            self.assertEqual(json.loads(slow.recv(16384))["error"]["code"], "INVALID_ARGUMENT")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as disconnected:
            disconnected.connect(str(self.sock))
            disconnected.sendall(REQUEST)
        self.assertNotIn("error", self.call())

    def test_truncated_line_and_cli_relay(self):
        self.assertEqual(self.call(b"{", shutdown=True)["error"]["code"], "INVALID_ARGUMENT")
        result = subprocess.run(
            [str(self.binary), "request", "--socket", str(self.sock)],
            input=REQUEST.rstrip(),
            capture_output=True,
            check=True,
            timeout=3,
        )
        self.assertEqual(json.loads(result.stdout)["deviceId"], "dev_fixture")

    @contextlib.contextmanager
    def attachment(self, *, matching_peer=True, response=None, stall=False):
        pid = os.getpid() if matching_peer else 100
        if matching_peer:
            (self.root / "proc/100/exe").unlink(missing_ok=True)
            self.add_mpc(pid=pid)
        path = self.root / f"tmp/mpc-native-lab-{pid}.sock"
        path.parent.mkdir(mode=0o700, exist_ok=True)
        state = {"epoch": "adp_" + "a" * 32, "response": response, "tag": "MPC_ADAPTER_V2"}
        finished = threading.Event()
        state["state_idle"] = threading.Event()
        state["state_idle"].set()
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.settimeout(0.05)
        server.bind(str(path))
        server.listen(4)

        def serve():
            while not finished.is_set():
                try:
                    client, _ = server.accept()
                except TimeoutError:
                    continue
                with client:
                    client.settimeout(0.2)
                    try:
                        data = client.recv(128)
                        if stall:
                            finished.wait(0.15)
                            continue
                        if data == b"ADAPTER_STATUS\n":
                            payload = (
                                state["response"]
                                or f"{state['tag']} {state['epoch']} key37-mpc3.9.1.2 {PROFILE['buildId']} VERIFIED\n".encode()
                            )
                            client.sendall(payload)
                        elif data.startswith(b"STATE_V6 "):
                            if state.get("stall_state"):
                                state["state_idle"].clear()
                                finished.wait(0.3)
                                state["state_idle"].set()
                                continue
                            wire = bytearray(state["wire"])
                            wire[8:45] = state["epoch"].encode() + b"\0"
                            if state.get("empty"):
                                wire[48:80] = bytes(32)
                                wire[108:128] = bytes(20)
                                struct.pack_into("<I4xQ", wire, 140480, 2, 0)
                            if state.get("bad_instance"):
                                wire[8] = ord("x")
                            if state.get("bad_count"):
                                # Track count follows attempts in the fixed snapshot header.
                                struct.pack_into("<I", wire, 108, 137)
                            # ABI 5: retained 128-byte prefix and 392-byte track rows.
                            if state.get("bad_ui_index"):
                                struct.pack_into("<I", wire, 128 + 16, 1)
                            if state.get("bad_collection"):
                                struct.pack_into("<I", wire, 128 + 24, 3)
                            if state.get("bad_collection_index"):
                                struct.pack_into("<I", wire, 128 + 28, 128)
                            if state.get("duplicate_collection_index"):
                                struct.pack_into("<I", wire, 128 + 392 + 28, 0)
                            if state.get("bad_type"):
                                wire[128 + 32 + 256] = ord("X")
                            if state.get("partial_fields"):
                                struct.pack_into("<I", wire, 128 + 312, 1)
                            if state.get("bad_field_mask"):
                                struct.pack_into("<I", wire, 128 + 312, 64)
                            if state.get("bad_mute_scope"):
                                struct.pack_into("<I", wire, 140480, 99)
                            if state.get("bad_sequence_generation"):
                                struct.pack_into("<Q", wire, 140488, 9999)
                            if state.get("missing_active_sequence"):
                                struct.pack_into("<Q", wire, 72, 0)
                                struct.pack_into("<I4xQ", wire, 140480, 1, 0)
                            if state.get("bad_abi"):
                                struct.pack_into("<I", wire, 0, 99)
                            payload = struct.pack("<III", 6, len(wire), state.get("error", 0))
                            if not state.get("error"):
                                payload += wire
                            if state.get("truncate"):
                                payload = payload[:20]
                            if state.get("replace"):
                                state["epoch"] = "adp_" + "b" * 32
                            client.sendall(payload)
                    except (OSError, TimeoutError):
                        pass

        worker = threading.Thread(target=serve)
        worker.start()
        try:
            yield state
        finally:
            finished.set()
            worker.join(timeout=1)
            server.close()
            path.unlink()

    def snapshot_attachment(self, attachment, mixed=False):
        fixture = Path(
            os.environ.get(
                "MPC_NATIVE_INVENTORY_FIXTURE" if mixed else "MPC_NATIVE_IDENTITY_FIXTURE",
                ROOT
                / "builddir/host/native-api/tests"
                / ("inventory_fixture" if mixed else "identity_fixture"),
            )
        )
        wire = subprocess.run(
            [str(fixture), "--snapshot-wire"], check=True, capture_output=True
        ).stdout
        attachment.update(tag="MPC_ADAPTER_STATE_V6", wire=wire)

    def test_snapshot_operations_share_fresh_observations_and_handles(self):
        with self.attachment() as attachment:
            self.snapshot_attachment(attachment)
            description = self.call()
            self.assertTrue(description["capabilities"]["get_state"]["available"])
            self.assertFalse(description["capabilities"]["set_mute"]["available"])
            self.assertFalse(description["capabilities"]["watch"]["available"])
            results = {}
            for operation in (
                "get_state",
                "list_tracks",
                "current_track",
                "current_sequence",
                "recovery_snapshot",
            ):
                results[operation] = self.call(
                    json.dumps({"apiVersion": "0.1", "op": operation}).encode() + b"\n"
                )
                self.assertEqual(results[operation]["instanceId"], description["instanceId"])
                self.assertFalse(results[operation]["sample"]["atomic"])
                self.assertEqual(results[operation]["sample"]["freshness"], "fresh")
            full = results["get_state"]
            self.assertEqual(full["tracks"], results["list_tracks"]["tracks"])
            self.assertEqual(full["tracks"][0], results["current_track"]["track"])
            self.assertEqual(full["selectedSequence"], results["current_sequence"]["sequence"])
            self.assertEqual(full["tracks"], results["recovery_snapshot"]["tracks"])
            self.assertEqual(len({r["revision"] for r in results.values()}), 5)
            self.assertEqual(full["tracks"][0]["ref"]["instanceId"], description["instanceId"])
            self.assertTrue(full["tracks"][0]["requested"]["soloed"])
            self.assertFalse(full["tracks"][0]["audioSide"]["soloed"])
            self.assertIsNone(full["tracks"][0]["audioSide"]["muted"])
            self.assertEqual(full["sequenceSlotCount"], 2)
            self.assertEqual(full["usedSequenceCount"], 1)
            self.assertEqual(full["selectedSequence"]["bpm"], 123.5)
            self.assertIsNone(full["transport"]["effectiveBpm"])
            self.assertIn('"é"\n', full["tracks"][0]["name"])

    def test_mixed_inventory_audio_selection_and_collection_indices(self):
        with self.attachment() as attachment:
            self.snapshot_attachment(attachment, mixed=True)
            results = {}
            for op in ("get_state", "list_tracks", "recovery_snapshot", "current_track"):
                results[op] = self.call(
                    json.dumps({"apiVersion": "0.1", "op": op}).encode() + b"\n"
                )
                if self.validator:
                    self.state_validator.validate(results[op])
            tracks = results["get_state"]["tracks"]
            self.assertEqual([t["uiIndex"] for t in tracks], list(range(8)))
            self.assertEqual(
                [t["type"] for t in tracks],
                ["plugin", "drum", "keygroup", "clip", "midi", "cv", None, "audio"],
            )
            self.assertEqual([t["index"] for t in tracks], [0, 1, 2, 3, 4, 5, 6, 0])
            self.assertEqual(tracks[-1]["collection"], "audio")
            self.assertEqual(results["current_track"]["track"], tracks[-1])
            for op in ("list_tracks", "recovery_snapshot"):
                self.assertEqual(results[op]["tracks"], tracks)
            self.assertEqual(len({t["ref"]["trackId"] for t in tracks}), 8)
            self.assertFalse(tracks[-1]["requested"]["muted"])
            self.assertNotIn("requested.muted", tracks[-1]["unsupportedFields"])
            self.assertEqual(len(results["get_state"]["buses"]), 5)
            for bus in results["get_state"]["buses"]:
                self.assertEqual(bus["collection"], "master")
                self.assertIsNone(bus["ref"])
                self.assertIsNone(bus["uiIndex"])

    def test_old_snapshot_attachment_is_not_compatible(self):
        with self.attachment() as attachment:
            for tag in (
                "MPC_ADAPTER_STATE_V1",
                "MPC_ADAPTER_STATE_V2",
                "MPC_ADAPTER_STATE_V3",
                "MPC_ADAPTER_STATE_V4",
                "MPC_ADAPTER_V1",
            ):
                attachment["tag"] = tag
                self.assertEqual(self.call()["readiness"]["nativeAdapter"], "not_attached")
                result = self.call(b'{"apiVersion":"0.1","op":"get_state"}\n')
                self.assertEqual(result["error"]["code"], "NOT_READY")

    def test_partial_fields_preserve_independent_nulls_and_operation_requirements(self):
        with self.attachment() as attachment:
            self.snapshot_attachment(attachment)
            attachment["partial_fields"] = True
            request = b'{"apiVersion":"0.1","op":"get_state"}\n'
            track = self.call(request)["tracks"][0]
            self.assertFalse(track["requested"]["muted"])
            self.assertIsNone(track["requested"]["soloed"])
            self.assertIsNone(track["requested"]["recordArmed"])
            self.assertIsNone(track["silenced"])
            support = track["flagSupport"]
            self.assertEqual(support["readableFields"], ["requested.muted"])
            self.assertTrue(support["operations"]["set_mute"]["available"])
            self.assertFalse(support["operations"]["set_solo"]["available"])
            self.assertEqual(set(track["unsupportedFields"]), set(support["unsupportedReasons"]))
            self.assertFalse(support["outputVerified"])
            attachment["bad_field_mask"] = True
            self.assertEqual(self.call(request)["error"]["code"], "READ_FAILED")

    def test_no_active_project_is_an_empty_fresh_snapshot(self):
        with self.attachment() as attachment:
            self.snapshot_attachment(attachment)
            attachment["empty"] = True
            for operation in ("get_state", "current_track", "current_sequence"):
                result = self.call(
                    json.dumps({"apiVersion": "0.1", "op": operation}).encode() + b"\n"
                )
                self.assertIsNone(result["projectId"])
                if operation == "get_state":
                    self.assertIsNone(result["project"])
                    self.assertEqual(result["tracks"], [])
                    self.assertEqual(result["buses"], [])
                    self.assertEqual(result["sequenceSlotCount"], 0)
                else:
                    self.assertIsNone(
                        result["track" if operation == "current_track" else "sequence"]
                    )

    def test_snapshot_failures_never_return_cached_state(self):
        request = b'{"apiVersion":"0.1","op":"get_state"}\n'
        with self.attachment() as attachment:
            self.snapshot_attachment(attachment)
            self.assertIn("revision", self.call(request))
            for mode in (
                "truncate",
                "bad_instance",
                "bad_count",
                "bad_abi",
                "bad_mute_scope",
                "bad_sequence_generation",
                "missing_active_sequence",
                "bad_ui_index",
                "bad_collection",
                "bad_collection_index",
                "duplicate_collection_index",
                "bad_type",
                "stall_state",
            ):
                with self.subTest(mode=mode):
                    attachment[mode] = True
                    self.assertEqual(self.call(request)["error"]["code"], "READ_FAILED")
                    attachment[mode] = False
                    self.assertTrue(attachment["state_idle"].wait(1))
                    self.assertIn("revision", self.call(request))
            attachment["error"] = 20  # MPC_NATIVE_STATE_UNSTABLE
            self.assertEqual(self.call(request)["error"]["code"], "STATE_UNSTABLE")
            attachment["error"] = 0
            attachment["replace"] = True
            self.assertEqual(self.call(request)["error"]["code"], "STATE_UNSTABLE")
            attachment["replace"] = False
            recovered = self.call(request)
            self.assertEqual(recovered["revision"], "1")
            self.assertEqual(recovered["instanceId"], self.call()["instanceId"])

    def test_verified_attachment_rotates_instance_without_enabling_commands(self):
        with self.attachment() as attachment:
            first = self.call()
            self.assertEqual(first["readiness"]["nativeAdapter"], "attached")
            self.assertEqual(first["readiness"]["adapter"]["qualification"], "bounded_lab_proof")
            self.assertTrue(first["readiness"]["adapter"]["codeVerified"])
            self.assertEqual(first["instanceId"], self.call()["instanceId"])
            self.assert_unavailable(first, "NOT_IMPLEMENTED")
            attachment["epoch"] = "adp_" + "b" * 32
            second = self.call()
            self.assertNotEqual(first["instanceId"], second["instanceId"])
        third = self.call()
        self.assertEqual(third["readiness"]["nativeAdapter"], "not_attached")
        self.assertNotEqual(second["instanceId"], third["instanceId"])

    def test_attachment_requires_matching_mpc_peer_pid(self):
        with self.attachment(matching_peer=False):
            self.assertEqual(self.call()["readiness"]["nativeAdapter"], "not_attached")

    def test_bad_or_unresponsive_attachment_does_not_claim_readiness(self):
        with self.attachment() as attachment:
            for payload in [
                b"MPC_ADAPTER_ERROR FINGERPRINT_MISMATCH\n",
                b"MPC_ADAPTER_V2 bad profile build VERIFIED\n",
                b"MPC_ADAPTER_V2 adp_" + b"a" * 32 + b" key37-mpc3.9.1.2 1122 VERIFIED\n",
            ]:
                attachment["response"] = payload
                self.assertEqual(self.call()["readiness"]["nativeAdapter"], "not_attached")
        with self.attachment(stall=True):
            started = time.monotonic()
            result = self.call()
            self.assertEqual(result["readiness"]["nativeAdapter"], "not_attached")
            self.assertLess(time.monotonic() - started, 0.8)


if __name__ == "__main__":
    unittest.main()
