#!/usr/bin/env python3
"""Temporary native-control experiment, pinned to the validated Key 37 build.

Default: load bridge, verify UI-thread callback, stop and unload it.
--pulse briefly changes the SELECTED normal track's flag and restores that flag.
Requires the existing idle privtap worker. Does not install an addin or restart MPC.
"""

import argparse
import json
import os
import re
import shlex
from pathlib import Path
import subprocess
import sys
import time
import uuid

from mpc_live_state import MemorySession, u32

ROOT = Path(__file__).resolve().parent.parent


def experiment(args):
    state_profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    control = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json").read_text())
    directory = ROOT / "builddir/arm/tools"
    names = ["hotload", "native_client", "native_bridge.so", "libmpc_native.so", "test_payload.so"]
    for name in names:
        if not (directory / name).is_file():
            raise RuntimeError("Build the native tools first with meson compile -C builddir/arm")
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", state_profile)
    remote = "/tmp/mpc-native-lab-" + uuid.uuid4().hex[:12]
    handle, created, loaded, worker = None, False, False, None
    report = {"buildId": state_profile["buildId"], "pid": session.pid, "pulse": args.pulse}
    report["host"] = args.host
    try:
        for function in control["functions"]:
            if session.read(session.base + function["rva"], 32) != bytes.fromhex(
                function["prefixHex"]
            ):
                raise RuntimeError("Native code fingerprint changed: " + function["name"])
        project = session.active_project()
        before = session.snapshot(project)
        report["before"] = before
        project_data = session.read(project, 0xAA8)
        track = u32(project_data, state_profile["offsets"]["project"]["selectedTrack"])
        program = u32(session.read(track, 0x64C), state_profile["offsets"]["track"]["program"])
        if args.pulse and (
            before["selectedTrack"]["index"] is None
            or before["selectedTrack"]["type"] not in ("plugin", "drum")
        ):
            raise RuntimeError("Select an existing plugin or drum track for this bounded test")
        worker_paths = (
            session.command(
                f"for f in /proc/{session.pid}/task/*/comm; do "
                'if test "$(cat "$f")" = "privtap inject"; then echo "$f"; fi; done'
            )
            .decode()
            .splitlines()
        )
        if len(worker_paths) != 1:
            raise RuntimeError("Expected one existing privtap injection worker")
        # The unload guard checks all executor workers in MPC, including the
        # persistent NODELETE host's independent executor. Avoid loading a lab
        # which cannot later satisfy that guard while the installed host runs.
        existing_executors = session.command(
            f"for f in /proc/{session.pid}/task/*/comm; do "
            'if test "$(cat "$f")" = "mpc executor"; then echo executor; fi; done'
        ).strip()
        if existing_executors:
            raise RuntimeError(
                "A native executor is already running. Stop the persistent native host before "
                "the temporary lab, and start it again after lab cleanup."
            )
        worker = int(worker_paths[0].split("/")[-2])
        session.command("mkdir -m 700 " + remote)
        created = True
        for name in names:
            session.command("cat > " + remote + "/" + name, data=(directory / name).read_bytes())
        session.command(f"chmod 700 {remote}/hotload {remote}/native_client")
        result = session.command(
            f"MPC_NATIVE_FIXTURE_DIR={remote} {remote}/hotload --self-test {remote}/test_payload.so"
        )
        if b"self_test_restored_read=true" not in result:
            raise RuntimeError("Disposable ARM loader test failed")
        report["isolatedLoaderTestPassed"] = True
        session.command(f"test ! -S /tmp/mpc-native-lab-{session.pid}.sock")
        if "/libmpc_native.so" in session.command(f"cat /proc/{session.pid}/maps").decode():
            raise RuntimeError(
                "A native adapter is already mapped; detach the existing attachment first"
            )
        result = json.loads(
            session.command(
                f"{remote}/hotload load {session.pid} {worker} {remote}/native_bridge.so"
            )
        )
        handle, loaded = result["handle"], True
        time.sleep(0.15)
        ping = json.loads(session.command(f"{remote}/native_client {session.pid}", data=b"PING\n"))
        report["callback"] = ping
        if not ping["ok"] or ping["tid"] != session.pid:
            raise RuntimeError("Callback did not run on the verified MPC main thread")
        if args.identities:
            identities = json.loads(
                session.command(f"{remote}/native_client {session.pid}", data=b"IDENTITIES\n")
            )
            report["identities"] = identities
            if not identities["ok"]:
                raise RuntimeError("Authoritative identity capture failed")
        status = (
            session.command(f"{remote}/native_client {session.pid}", data=b"ADAPTER_STATUS\n")
            .decode()
            .split()
        )
        if (
            len(status) != 5
            or status[0] not in ("MPC_ADAPTER_V2", "MPC_ADAPTER_STATE_V4")
            or status[3] != state_profile["buildId"]
            or status[4] != "VERIFIED"
        ):
            raise RuntimeError("Native adapter status/guard verification failed")
        report["adapter"] = {
            "instanceId": status[1],
            "profileId": status[2],
            "buildId": status[3],
            "codeVerified": True,
        }
        if args.identities:
            selected = next(
                t for t in identities["tracks"] if t["id"] == identities["selectedTrackId"]
            )
            command = (
                f"HANDLE_READ {identities['instanceId']} {identities['projectId']} "
                f"{identities['projectGeneration']} {selected['id']} {selected['generation']}\n"
            )
        else:
            command = f"READ {project:x} {track:x} {program:x}\n"
        read = json.loads(
            session.command(
                f"{remote}/native_client {session.pid}",
                data=command.encode(),
            )
        )
        if not read["ok"] or (not args.identities and read["tid"] != session.pid):
            raise RuntimeError("Typed adapter read failed on the MPC UI thread")
        if args.identities:
            observed = read
        else:
            target_read = [t for t in read["tracks"] if int(t["track"], 16) == track]
            if len(target_read) != 1:
                raise RuntimeError("Adapter read did not identify the selected target")
            observed = target_read[0]
        for native, snapshot in [
            ("mute", "muted"),
            ("solo", "soloed"),
            ("arm", "recordArmed"),
            ("silenced", "silenced"),
        ]:
            if args.identities and native == "silenced":
                continue
            if bool(observed[native]) != before["selectedTrack"][snapshot]:
                raise RuntimeError("Typed adapter read disagreed with the independent snapshot")
        report["typedReadVerified"] = True
        if args.pulse:
            field = {"mute": "muted", "solo": "soloed", "arm": "recordArmed"}[args.pulse]
            original = int(before["selectedTrack"][field])
            command = (
                f"PULSE {args.pulse} {1 - original} {original} {args.hold_ms} "
                f"{project:x} {track:x} {program:x}\n"
            )
            process = subprocess.Popen(
                session.ssh + [f"{remote}/native_client {session.pid}"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            phases = []
            try:
                process.stdin.write(command)
                process.stdin.close()
                for line in process.stdout:
                    phase = json.loads(line)
                    phases.append(phase)
                    print(args.pulse + ": " + phase["phase"], file=sys.stderr, flush=True)
                    if phase["phase"] == "applied":
                        report["during"] = session.snapshot(project)
            finally:
                # The bridge restores even if this client disconnects; let its bounded pulse finish.
                process.wait(timeout=25)
            report["nativePhases"] = phases
            if process.returncode or len(phases) != 4 or not all(p["ok"] for p in phases):
                raise RuntimeError("Native pulse failed; inspect nativePhases and current state")
            if report["during"]["selectedTrack"][field] != bool(1 - original):
                raise RuntimeError("Independent memory reader did not confirm the native write")
            report["independentWriteVerified"] = True
        after = session.snapshot(project)
        report["after"] = after
        report["observedStateRestored"] = all(
            before[k] == after[k]
            for k in ("selectedTrack", "selectedSequence", "tracks", "buses", "sequenceSlots")
        )
        if not report["observedStateRestored"]:
            raise RuntimeError(
                "State differs after the pulse; inspect after before further experiments"
            )
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            retain = loaded and args.keep and "error" not in report
            if retain:
                report.update(
                    bridgeRetained=True, remoteDirectory=remote, handle=handle, worker=worker
                )
            if loaded and not retain:
                reply = json.loads(
                    session.command(f"{remote}/native_client {session.pid}", data=b"QUIT\n")
                )
                if not reply["ok"]:
                    raise RuntimeError("Bridge still has pending callbacks; leave it mapped")
                # The loader refuses unload until the lab worker exits and UI returns to poll().
                for attempt in range(10):
                    time.sleep(0.1)
                    try:
                        session.command(f"{remote}/hotload unload {session.pid} {worker} {handle}")
                        loaded = False
                        break
                    except RuntimeError as error:
                        if attempt == 9 or "refused unload:" not in str(error):
                            raise
                maps = session.command(f"cat /proc/{session.pid}/maps").decode()
                if any(
                    remote + "/" + name in maps for name in ("native_bridge.so", "libmpc_native.so")
                ):
                    raise RuntimeError("Bridge or adapter is still mapped")
                report["bridgeUnloaded"] = True
            if created and not loaded:
                session.command(
                    f"rm -f {remote}/hotload {remote}/native_client {remote}/native_bridge.so {remote}/libmpc_native.so "
                    f"{remote}/test_payload.so {remote}/mpc-native-fixture-* && rmdir {remote}"
                )
                report["temporaryFilesRemoved"] = True
        except Exception as error:
            report["cleanupError"] = str(error)
            report["remoteDirectory"] = remote
        finally:
            session.close()
    return report


def detach(args):
    report = json.loads(args.detach.read_text())
    remote, pid, worker, handle = (
        report[k] for k in ("remoteDirectory", "pid", "worker", "handle")
    )
    if (
        not re.fullmatch(r"/tmp/mpc-native-lab-[0-9a-f]{12}", remote)
        or type(pid) is not int
        or pid <= 0
        or type(worker) is not int
        or worker <= 0
        or worker == pid
        or not re.fullmatch(r"[0-9a-f]{8}", handle)
        or not report.get("bridgeRetained")
    ):
        raise RuntimeError("Expected a retained attachment report from --keep")
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
            raise RuntimeError(result.stderr.decode(errors="replace")[:500])
        return result.stdout

    status = command(f"{remote}/native_client {pid}", b"ADAPTER_STATUS\n").decode().split()
    if len(status) != 5 or status[1] != report["adapter"]["instanceId"]:
        raise RuntimeError("Adapter instance changed; refusing to detach a different attachment")
    if not json.loads(command(f"{remote}/native_client {pid}", b"QUIT\n"))["ok"]:
        raise RuntimeError("Callbacks have not drained; attachment remains mapped")
    for attempt in range(10):
        try:
            command(f"{remote}/hotload unload {pid} {worker} {handle}")
            break
        except RuntimeError as error:
            if attempt == 9 or "refused unload:" not in str(error):
                raise
            time.sleep(0.1)
    maps = command(f"cat /proc/{pid}/maps").decode()
    if any(remote + "/" + name in maps for name in ("native_bridge.so", "libmpc_native.so")):
        raise RuntimeError("A retained mapping remains; temporary files were kept")
    files = [
        remote + "/" + name
        for name in (
            "hotload",
            "native_client",
            "native_bridge.so",
            "libmpc_native.so",
            "test_payload.so",
        )
    ]
    command(
        "rm -f "
        + shlex.join(files)
        + " "
        + shlex.quote(remote)
        + "/mpc-native-fixture-* && rmdir "
        + shlex.quote(remote)
    )
    report.update(bridgeRetained=False, bridgeUnloaded=True, temporaryFilesRemoved=True)
    args.detach.write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--pulse", choices=("mute", "solo", "arm"))
    parser.add_argument("--hold-ms", type=int, default=6000)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--identities", action="store_true", help="Use the authoritative project/handle resolver"
    )
    parser.add_argument(
        "--keep",
        action="store_true",
        help="Retain the read-only-tested lab attachment for describe probes",
    )
    parser.add_argument(
        "--detach", type=Path, help="Stop/unload exactly the attachment recorded by --keep"
    )
    args = parser.parse_args()
    if args.keep and args.pulse:
        parser.error("--keep cannot be combined with a write experiment")
    if args.identities and args.pulse:
        parser.error("--identities is a read-only attachment check")
    if args.detach and (args.keep or args.pulse):
        parser.error("--detach cannot be combined with --keep or --pulse")
    if not 5000 <= args.hold_ms <= 10000:
        parser.error("Use 5000..10000 ms to allow independent SSH readback")
    report = detach(args) if args.detach else experiment(args)
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(
            json.dumps(
                {
                    k: report[k]
                    for k in (
                        "pid",
                        "pulse",
                        "independentWriteVerified",
                        "typedReadVerified",
                        "observedStateRestored",
                        "bridgeUnloaded",
                        "bridgeRetained",
                        "error",
                        "cleanupError",
                    )
                    if k in report
                }
            )
        )
    else:
        print(text, end="")
    return 1 if "error" in report or "cleanupError" in report else 0


if __name__ == "__main__":
    raise SystemExit(main())
