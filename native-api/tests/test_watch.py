"""Watch through the real Unix transport, host lifecycle and isolated native UI."""

from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import socket
import subprocess
import time

import jsonschema
from referencing import Registry, Resource

from test_host import HostHarness

ROOT = Path(__file__).resolve().parents[2]
SCHEMAS = ROOT / "native-api/schema"


class WatchTests(HostHarness):
    def setUp(self):
        super().setUp()
        self.launch("native-commands")
        self.streams = []
        registry = Registry().with_resources(
            (p.name, Resource.from_contents(json.loads(p.read_text())))
            for p in SCHEMAS.glob("*.schema.json")
        )
        self.validator = jsonschema.Draft202012Validator(
            json.loads((SCHEMAS / "watch-response.schema.json").read_text()), registry=registry
        )

    def tearDown(self):
        for client, reader in self.streams:
            reader.close()
            client.close()
        super().tearDown()

    def stream(self, cursor=None):
        client = socket.socket(socket.AF_UNIX)
        client.settimeout(3)
        client.connect(str(self.api))
        request = {"apiVersion": "0.1", "op": "watch"}
        if cursor is not None:
            request["cursor"] = cursor
        client.sendall(json.dumps(request).encode() + b"\n")
        reader = client.makefile("rb")
        self.streams.append((client, reader))
        return client, reader

    def frame(self, reader):
        line = reader.readline(1048577)
        self.assertTrue(line.endswith(b"\n"), line[:256])
        value = json.loads(line)
        self.validator.validate(value)
        if value["type"] in {"ack", "snapshot", "change", "heartbeat"}:
            self.assertEqual(value["cursor"], value["instanceId"] + ":" + value["revision"])
        return value

    def until(self, reader, kind, timeout=8):
        stop = time.monotonic() + timeout
        while time.monotonic() < stop:
            value = self.frame(reader)
            if value["type"] == kind:
                return value
            self.assertNotIn(value["type"], {"resync_required", "terminated"}, value)
        self.fail(f"No {kind} frame")

    def start(self):
        client, reader = self.stream()
        ack = self.frame(reader)
        self.assertEqual(ack["type"], "ack")
        self.assertFalse(ack["resumed"])
        state = self.frame(reader)
        self.assertEqual(state["type"], "snapshot")
        self.assertEqual(state["revision"], state["state"]["revision"])
        self.assertEqual(state["cursor"], ack["cursor"])
        return client, reader, state

    def test_snapshot_interval_replays_manual_edits_and_selection_outside_command_screens(self):
        snapshot = self.public("get_state")
        cursor = snapshot["instanceId"] + ":" + snapshot["revision"]
        (self.install / "policy-sounds").touch()
        (self.install / "manual-mute").touch()
        (self.install / "watch-select").touch()
        time.sleep(0.25)  # Change occurs after snapshot, before subscription.
        client, reader = self.stream(cursor)
        ack = self.frame(reader)
        self.assertTrue(ack["resumed"])
        change = self.until(reader, "change")
        self.assertEqual(change["previousRevision"], snapshot["revision"])
        self.assertTrue(change["tracks"][0]["requested"]["muted"])
        self.assertEqual(change["selectedTrackId"], snapshot["tracks"][1]["ref"]["trackId"])
        self.assertEqual(change["coverage"]["count"], 2)
        self.assertIsNone(change["tracks"][0]["audioSide"]["muted"])
        client.sendall(b'{"apiVersion":"0.1","op":"unwatch"}\n')
        self.assertEqual(self.until(reader, "terminated")["reason"], "UNSUBSCRIBED")
        self.assertEqual(reader.read(1), b"")

    def test_fresh_stream_heartbeats_and_c_cli(self):
        _, reader, _ = self.start()
        heartbeat = self.until(reader, "heartbeat")
        self.assertEqual(heartbeat["revision"], "1")
        cli = subprocess.Popen(
            [os.environ["MPC_API_TEST_BINARY"], "watch", "--socket", str(self.api)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            self.assertEqual(json.loads(cli.stdout.readline())["type"], "ack")
            self.assertEqual(json.loads(cli.stdout.readline())["type"], "snapshot")
        finally:
            cli.terminate()
            cli.communicate(timeout=3)
        description = self.public()
        self.assertFalse(description["capabilities"]["watch"]["available"])
        self.assertEqual(
            description["capabilities"]["watch"]["reason"], "DEVICE_QUALIFICATION_PENDING"
        )

    def test_schema_examples(self):
        request_schema = json.loads((SCHEMAS / "watch-request.schema.json").read_text())
        for name in ["watch-request", "watch-resume-request", "unwatch-request"]:
            value = json.loads((ROOT / f"native-api/examples/{name}.json").read_text())
            jsonschema.validate(value, request_schema)
        for name in ["ack", "snapshot", "change", "heartbeat", "terminated", "resync-required"]:
            value = json.loads((ROOT / f"native-api/examples/watch-{name}.json").read_text())
            self.validator.validate(value)
        for name in ["watch-request.schema.json", "watch-response.schema.json"]:
            jsonschema.Draft202012Validator.check_schema(json.loads((SCHEMAS / name).read_text()))

    def test_api_command_changes_resume_after_discarded_delivery(self):
        client, reader, first = self.start()
        reader.close()
        client.close()
        target = first["state"]["tracks"][0]["ref"]
        command = {
            "apiVersion": "0.1",
            "op": "set_mute",
            "requestId": "watch-mute",
            "target": target,
            "value": True,
            "waitMs": 1000,
        }
        result = self.call(json.dumps(command).encode() + b"\n", self.api)
        self.assertEqual(result["status"], "applied", result)
        _, reader = self.stream(first["cursor"])
        self.assertTrue(self.frame(reader)["resumed"])
        change = self.until(reader, "change")
        self.assertTrue(change["tracks"][0]["requested"]["muted"])
        _, retry = self.stream(first["cursor"])
        self.frame(retry)
        self.assertEqual(self.until(retry, "change"), change)
        actual = self.public("get_state")
        self.assertEqual(actual["tracks"][0]["requested"], change["tracks"][0]["requested"])

    def test_topology_reconciliation_preserves_order_and_ids(self):
        _, reader, first = self.start()
        before = first["state"]["tracks"]
        (self.install / "watch-reorder").touch()
        replacement = self.until(reader, "snapshot")
        self.assertEqual(replacement["previousRevision"], first["revision"])
        after = replacement["state"]["tracks"]
        self.assertEqual([t["ref"] for t in after], [t["ref"] for t in reversed(before)])
        self.assertEqual([t["uiIndex"] for t in after], [0, 1])
        self.assertEqual(
            [t["generation"] for t in after], [t["generation"] for t in reversed(before)]
        )

    def test_metadata_audit_observes_manual_rename(self):
        _, reader, _ = self.start()
        (self.install / "watch-rename").touch()
        replacement = self.until(reader, "snapshot")
        self.assertEqual(replacement["state"]["tracks"][0]["name"], "Renamed")

    def test_project_replacement_requires_resync_for_live_and_resumed_streams(self):
        _, reader, first = self.start()
        (self.install / "watch-replace").touch()
        self.until(reader, "resync_required")
        self.assertEqual(reader.read(1), b"")
        _, resumed = self.stream(first["cursor"])
        self.assertEqual(self.frame(resumed)["type"], "resync_required")
        recovered = self.public("recovery_snapshot")
        self.assertNotEqual(recovered["projectId"], first["projectId"])

    def test_stop_start_and_service_restart_invalidate_cursor(self):
        _, reader, first = self.start()
        self.call(b"STOP\n")
        end = reader.readline()
        if end:
            self.assertEqual(json.loads(end)["type"], "terminated")
        self.assertEqual(reader.read(1), b"")
        self.call(b"START\n")
        self.wait_status({"ready"})
        _, resumed = self.stream(first["cursor"])
        self.assertEqual(self.frame(resumed)["type"], "resync_required")
        self.assertNotEqual(self.public()["instanceId"], first["instanceId"])

    def test_invalid_expired_and_future_cursors_are_explicit(self):
        _, _, first = self.start()
        for cursor in ["run_" + "f" * 32 + ":1", first["instanceId"] + ":999999"]:
            _, reader = self.stream(cursor)
            self.assertEqual(self.frame(reader)["type"], "resync_required")
            self.assertEqual(reader.read(1), b"")
        for cursor in [
            "bad",
            first["instanceId"] + ":0",
            first["instanceId"] + ":01",
            first["instanceId"] + ":18446744073709551616",
        ]:
            result = self.call(
                json.dumps({"apiVersion": "0.1", "op": "watch", "cursor": cursor}).encode() + b"\n",
                self.api,
            )
            self.assertEqual(result["error"]["code"], "INVALID_ARGUMENT")

    def test_strict_request_and_unsubscribe_parsing(self):
        for request in [
            b'{"apiVersion":"0.1","op":"watch","trackId":"1"}\n',
            b'{"apiVersion":"0.1","op":"watch","op":"watch"}\n',
            b'{"apiVersion":"0.1","op":"watch"}\n{}\n',
        ]:
            self.assertEqual(self.call(request, self.api)["error"]["code"], "INVALID_ARGUMENT")
        client, reader, _ = self.start()
        client.sendall(b'{"apiVersion":"0.1","op":"get_state"}\n')
        self.assertEqual(self.until(reader, "terminated")["reason"], "INVALID_REQUEST")

    def test_fragmented_setup_and_partial_unsubscribe_have_bounded_input(self):
        client = socket.socket(socket.AF_UNIX)
        client.settimeout(3)
        client.connect(str(self.api))
        reader = client.makefile("rb")
        self.streams.append((client, reader))
        client.sendall(b'{"apiVersion":"0.1",')
        self.assertIn("capabilities", self.public())
        client.sendall(b'"op":"watch"}\n')
        self.assertEqual(self.frame(reader)["type"], "ack")
        self.assertEqual(self.frame(reader)["type"], "snapshot")
        client.sendall(b'{"apiVersion":')
        before = time.monotonic()
        reader.read()  # EOF within the input deadline, despite heartbeat output.
        self.assertLess(time.monotonic() - before, 2)
        self.assertIn("capabilities", self.public())

    def test_eight_stalled_subscribers_keep_commands_and_shutdown_available(self):
        slow = []
        for _ in range(8):
            client, reader, _ = self.start()
            slow.append((client, reader))
        self.assertEqual(
            self.call(b'{"apiVersion":"0.1","op":"watch"}\n', self.api)["error"]["code"],
            "NOT_READY",
        )
        # Large full-state frames exercise partial writes and output/replay bounds.
        for _ in range(70):
            self.assertIn("tracks", self.public("get_state"))
        before = time.monotonic()
        self.assertIn("capabilities", self.public())
        self.assertLess(time.monotonic() - before, 1)
        self.call(b"STOP\n")
        self.wait_status({"stopped"})

    def test_slow_subscriber_does_not_interrupt_healthy_delivery(self):
        self.start()  # Leave one subscriber unread.
        _, reader, _ = self.start()

        def consume():
            count = 0
            for _ in range(20):
                self.until(reader, "snapshot")
                count += 1
            return count

        with ThreadPoolExecutor() as pool:
            healthy = pool.submit(consume)
            for _ in range(20):
                self.public("get_state")
            self.assertEqual(healthy.result(timeout=5), 20)

    def test_paused_ui_is_bounded_and_watch_recovers_explicitly(self):
        _, reader, _ = self.start()
        pause = self.install / "pause-ui"
        pause.touch()
        self.until(reader, "resync_required")
        before = time.monotonic()
        self.assertIn("capabilities", self.public())
        self.assertLess(time.monotonic() - before, 0.5)
        pause.unlink()
        self.assertIn("tracks", self.public("recovery_snapshot"))
