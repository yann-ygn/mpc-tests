#!/usr/bin/env python3
"""Temporary native-control experiment, pinned to the validated Key 37 build.

Default: load bridge, verify UI-thread callback, stop and unload it.
--pulse briefly changes the SELECTED normal track's flag and restores that flag.
Requires the existing idle privtap worker. Does not install an addin or restart MPC.
"""
import argparse
import json
import os
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
    directory = ROOT / "build/native-control"
    names = ["hotload", "native_client", "native_bridge.so", "test_payload.so"]
    for name in names:
        if not (directory / name).is_file():
            raise RuntimeError("Build the native tools first with tools/build_native_control.py")
    session = MemorySession(args.host, ROOT / "build/internals/mem_probe", state_profile)
    remote = "/tmp/mpc-native-lab-" + uuid.uuid4().hex[:12]
    handle, created, loaded, worker = None, False, False, None
    report = {"buildId": state_profile["buildId"], "pid": session.pid, "pulse": args.pulse}
    try:
        for function in control["functions"]:
            if session.read(session.base + function["rva"], 32) != bytes.fromhex(function["prefixHex"]):
                raise RuntimeError("Native code fingerprint changed: " + function["name"])
        project = session.find_project()
        before = session.snapshot(project)
        report["before"] = before
        project_data = session.read(project, 0xaa8)
        track = u32(project_data, state_profile["offsets"]["project"]["selectedTrack"])
        program = u32(session.read(track, 0x64c), state_profile["offsets"]["track"]["program"])
        if args.pulse and (before["selectedTrack"]["index"] is None
                           or before["selectedTrack"]["type"] not in ("plugin", "drum")):
            raise RuntimeError("Select an existing plugin or drum track for this bounded test")
        worker_paths = session.command(
            f'for f in /proc/{session.pid}/task/*/comm; do '
            'if test "$(cat "$f")" = "privtap inject"; then echo "$f"; fi; done'
        ).decode().splitlines()
        if len(worker_paths) != 1:
            raise RuntimeError("Expected one existing privtap injection worker")
        worker = int(worker_paths[0].split("/")[-2])
        session.command("mkdir -m 700 " + remote)
        created = True
        for name in names:
            session.command("cat > " + remote + "/" + name, data=(directory / name).read_bytes())
        session.command(f"chmod 700 {remote}/hotload {remote}/native_client")
        result = session.command(f"MPC_NATIVE_FIXTURE_DIR={remote} {remote}/hotload --self-test {remote}/test_payload.so")
        if b"self_test_restored_read=true" not in result:
            raise RuntimeError("Disposable ARM loader test failed")
        report["isolatedLoaderTestPassed"] = True
        session.command(f"test ! -S /tmp/mpc-native-lab-{session.pid}.sock")
        result = json.loads(session.command(f"{remote}/hotload load {session.pid} {worker} {remote}/native_bridge.so"))
        handle, loaded = result["handle"], True
        time.sleep(0.15)
        ping = json.loads(session.command(f"{remote}/native_client {session.pid}", data=b"PING\n"))
        report["callback"] = ping
        if not ping["ok"] or ping["tid"] != session.pid:
            raise RuntimeError("Callback did not run on the verified MPC main thread")
        if args.pulse:
            field = {"mute": "muted", "solo": "soloed", "arm": "recordArmed"}[args.pulse]
            original = int(before["selectedTrack"][field])
            command = (f"PULSE {args.pulse} {1-original} {original} {args.hold_ms} "
                       f"{project:x} {track:x} {program:x}\n")
            process = subprocess.Popen(session.ssh + [f"{remote}/native_client {session.pid}"],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
            if report["during"]["selectedTrack"][field] != bool(1-original):
                raise RuntimeError("Independent memory reader did not confirm the native write")
            report["independentWriteVerified"] = True
        after = session.snapshot(project)
        report["after"] = after
        report["observedStateRestored"] = all(before[k] == after[k] for k in
            ("selectedTrack", "selectedSequence", "tracks", "buses", "sequenceSlots"))
        if not report["observedStateRestored"]:
            raise RuntimeError("State differs after the pulse; inspect after before further experiments")
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            if loaded:
                reply = json.loads(session.command(f"{remote}/native_client {session.pid}", data=b"QUIT\n"))
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
                if remote + "/native_bridge.so" in maps:
                    raise RuntimeError("Bridge is still mapped")
                report["bridgeUnloaded"] = True
            if created and not loaded:
                session.command(f"rm -f {remote}/hotload {remote}/native_client {remote}/native_bridge.so "
                                f"{remote}/test_payload.so {remote}/mpc-native-fixture-* && rmdir {remote}")
                report["temporaryFilesRemoved"] = True
        except Exception as error:
            report["cleanupError"] = str(error)
            report["remoteDirectory"] = remote
        finally:
            session.close()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--pulse", choices=("mute", "solo", "arm"))
    parser.add_argument("--hold-ms", type=int, default=6000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 5000 <= args.hold_ms <= 10000:
        parser.error("Use 5000..10000 ms to allow independent SSH readback")
    report = experiment(args)
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(json.dumps({k: report[k] for k in ("pid", "pulse", "independentWriteVerified",
              "observedStateRestored", "bridgeUnloaded", "error", "cleanupError") if k in report}))
    else:
        print(text, end="")
    return 1 if "error" in report or "cleanupError" in report else 0


if __name__ == "__main__":
    raise SystemExit(main())
