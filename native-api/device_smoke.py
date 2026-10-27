#!/usr/bin/env python3
"""Test the local service in a private temporary directory on MPC.

--state checks fresh reads through an existing authoritative lab attachment.
Default: stop the service and remove its files. --keep retains it for further
requests; neither mode installs an addin or startup job.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--binary", type=Path, default=ROOT / "builddir/arm/native-api/mpc-api")
    parser.add_argument("--keep", action="store_true")
    parser.add_argument(
        "--state",
        action="store_true",
        help="Validate reads through an existing authoritative attachment",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    ssh = [
        "ssh",
        "-F",
        "/dev/null",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=7",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "UpdateHostKeys=no",
        "root@" + args.host,
    ]

    def command(text, data=None):
        result = subprocess.run(ssh + [text], input=data, capture_output=True, timeout=15)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace")[:1000])
        return result.stdout

    # Read the local build before creating anything remotely.
    binary = args.binary.read_bytes()
    directory = "/tmp/mpc-api-describe-" + uuid.uuid4().hex[:12]
    socket_path = directory + "/api.sock"
    device_id = "dev_" + uuid.uuid4().hex
    report = {"remoteDirectory": directory, "socket": socket_path, "deviceId": device_id}
    created = False
    pid = None
    success = False
    try:
        before = command("pidof MPC").decode().strip()
        command("mkdir -m 700 " + shlex.quote(directory))
        created = True
        command(
            "cat > "
            + shlex.quote(directory + "/mpc-api")
            + " && chmod 700 "
            + shlex.quote(directory + "/mpc-api"),
            binary,
        )
        command("cat > " + shlex.quote(directory + "/device-id"), (device_id + "\n").encode())
        launch = [
            directory + "/mpc-api",
            "serve",
            "--socket",
            socket_path,
            "--device-id",
            device_id,
        ]
        started = (
            command(
                "nohup "
                + shlex.join(launch)
                + " > "
                + shlex.quote(directory + "/service.log")
                + " 2>&1 < /dev/null & echo $!"
            )
            .decode()
            .strip()
        )
        if not started.isdigit():
            raise RuntimeError("Could not identify the discovery service process")
        pid = int(started)
        report["servicePid"] = pid
        for attempt in range(10):
            try:
                command("test -S " + shlex.quote(socket_path))
                break
            except RuntimeError:
                if attempt == 9:
                    raise RuntimeError(
                        command("cat " + shlex.quote(directory + "/service.log")).decode()
                    )
                time.sleep(0.1)

        def call(value):
            payload = json.dumps(value, separators=(",", ":")).encode() + b"\n"
            return json.loads(
                command(
                    shlex.join([directory + "/mpc-api", "request", "--socket", socket_path]),
                    payload,
                )
            )

        result = call({"apiVersion": "0.1", "op": "describe"})
        assert result["deviceId"] == device_id
        assert result["readiness"]["mpcProcess"] == "running", (
            "Discovery did not identify the running MPC executable"
        )
        assert result["firmware"]["buildId"] is not None, "Missing live executable build ID"
        assert result["firmware"]["model"] is not None, "Missing device-tree model"
        assert result["capabilities"]["describe"]["available"]
        reads = {
            "get_state",
            "list_tracks",
            "current_track",
            "current_sequence",
            "recovery_snapshot",
        }
        assert all(
            c["available"] == (args.state and op in reads)
            for op, c in result["capabilities"].items()
            if op != "describe"
        )
        assert result["readiness"]["status"] == ("ready" if args.state else "discovery_only")
        if args.state:
            import sys

            sys.path.insert(0, str(ROOT / "tools"))
            from mpc_live_state import MemorySession
            from mpc_track_inventory import capture as track_inventory

            profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
            with_session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
            try:
                independent = with_session.snapshot(with_session.active_project())
                inventory = track_inventory(with_session, flags=True)
                snapshots = {op: call({"apiVersion": "0.1", "op": op}) for op in sorted(reads)}
                state = snapshots["get_state"]
                assert "error" not in state, state
                from mpc_command_policy import capture as command_context

                scope = "sequence" if command_context(with_session)["perSequenceMute"] else "global"
                assert state["muteContext"]["scope"] == scope
                for snapshot in snapshots.values():
                    assert snapshot["muteContext"] == state["muteContext"]
                if scope == "sequence":
                    assert state["muteContext"]["sequence"] == state["selectedSequence"]["ref"]
                else:
                    assert state["muteContext"]["sequence"] is None
                report["muteScopeMatched"] = scope
                assert state["tracks"] == snapshots["list_tracks"]["tracks"]
                assert state["tracks"] == snapshots["recovery_snapshot"]["tracks"]
                assert state["selectedSequence"] == snapshots["current_sequence"]["sequence"]
                assert (
                    snapshots["current_track"]["track"]["ref"]["trackId"]
                    == state["selectedTrackId"]
                )
                for actual, expected in zip(state["tracks"], inventory["tracks"], strict=True):
                    for key in ("index", "uiIndex", "collection", "name", "type"):
                        assert actual[key] == expected[key], (key, actual, expected)
                    for field in ("requested", "audioSide", "silenced"):
                        assert actual[field] == expected[field], (field, actual, expected)
                from mpc_mixer_controls import independent as mixer_observations
                import struct

                mixer_rows = {
                    r["userIndex"]: r
                    for r in mixer_observations(with_session)
                    if r["userIndex"] is not None
                }
                for index, track in enumerate(state["tracks"]):
                    mixer, expected = track["mixer"], mixer_rows[index]
                    if expected["family"] not in ("plugin", "drum", "keygroup", "audio"):
                        assert not mixer["known"] and mixer["generation"] is None
                        continue
                    assert mixer["generation"] and mixer["verification"] == ["model"]
                    for field in ("volume", "pan"):
                        assert struct.pack("<f", mixer[field]) == struct.pack("<f", expected[field])
                    for i, send in enumerate(mixer["sends"]):
                        assert send["index"] == i
                        assert struct.pack("<f", send["value"]) == struct.pack(
                            "<f", expected["sends"][i]
                        )
                    assert mixer["insertRackEnabled"] == expected["rack"]
                    assert [slot["enabled"] for slot in mixer["inserts"]] == expected["slots"]
                report["mixerFieldsMatchedIndependentReader"] = True
                assert all(
                    not bus["mixer"]["known"] and bus["mixer"]["generation"] is None
                    for bus in state["buses"]
                )
                assert len(state["buses"]) == len(inventory["buses"])
                after_inventory = track_inventory(with_session, flags=True)
                for key in ("tracks", "buses", "selectedTrack", "counts"):
                    assert after_inventory[key] == inventory[key]
                assert state["sequenceSlotCount"] == independent["sequenceSlots"]
                after_state = with_session.snapshot(with_session.active_project())
                for key in (
                    "tracks",
                    "buses",
                    "selectedTrack",
                    "selectedSequence",
                    "sequenceSlots",
                ):
                    assert after_state[key] == independent[key]
                samples = [call({"apiVersion": "0.1", "op": "get_state"}) for _ in range(20)]
                assert all("error" not in sample for sample in samples), samples
                assert len({sample["revision"] for sample in samples}) == len(samples)
                assert all(sample["instanceId"] == result["instanceId"] for sample in samples)
                durations = sorted(sample["sample"]["durationMs"] for sample in samples)
                report.update(
                    snapshots=snapshots,
                    sampleCount=len(samples),
                    captureP95Ms=durations[18],
                    independentInventory=inventory,
                    independentReadMatched=True,
                )
            finally:
                with_session.close()
        if result["readiness"]["nativeAdapter"] == "attached":
            assert result["readiness"]["adapter"]["codeVerified"]
            assert result["readiness"]["adapter"]["qualification"] == "bounded_lab_proof"
        assert call({"apiVersion": "0.1", "op": "describe"})["instanceId"] == result["instanceId"]
        assert (
            call({"apiVersion": "9", "op": "describe"})["error"]["code"]
            == "UNSUPPORTED_API_VERSION"
        )
        assert (
            call({"apiVersion": "0.1", "op": "raw_call"})["error"]["code"]
            == "UNSUPPORTED_OPERATION"
        )
        assert call({"apiVersion": "0.1", "op": "set_mute"})["error"]["code"] == "INVALID_ARGUMENT"
        assert command("stat -c %a " + shlex.quote(socket_path)).strip() == b"600"
        assert command("pidof MPC").decode().strip() == before
        report.update(
            description=result,
            instanceStable=True,
            errorResponsesVerified=True,
            socketMode="600",
            mpcPidUnchanged=True,
        )
        success = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        if created and not (success and args.keep):
            try:
                if pid is not None:
                    # A PID from this launch only; verify its command before signaling.
                    actual = command(f"cat /proc/{pid}/cmdline")
                    if actual.split(b"\0", 1)[0] != (directory + "/mpc-api").encode():
                        raise RuntimeError(
                            "Cleanup refused: service PID no longer identifies our executable"
                        )
                    command(f"kill -TERM {pid}")
                    for attempt in range(10):
                        try:
                            command("test ! -S " + shlex.quote(socket_path))
                            break
                        except RuntimeError:
                            if attempt == 9:
                                raise
                            time.sleep(0.1)
                command(
                    "rm -f "
                    + shlex.join(
                        [
                            directory + "/mpc-api",
                            directory + "/device-id",
                            directory + "/service.log",
                        ]
                    )
                    + " && rmdir "
                    + shlex.quote(directory)
                )
                report["temporaryFilesRemoved"] = True
            except Exception as error:
                report["cleanupError"] = str(error)
        report["leftRunning"] = bool(success and args.keep)
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(
            json.dumps(
                {
                    k: report[k]
                    for k in (
                        "remoteDirectory",
                        "socket",
                        "servicePid",
                        "leftRunning",
                        "error",
                        "cleanupError",
                    )
                    if k in report
                }
            )
        )
    else:
        print(text, end="")
    return 0 if success and "cleanupError" not in report else 1


if __name__ == "__main__":
    raise SystemExit(main())
