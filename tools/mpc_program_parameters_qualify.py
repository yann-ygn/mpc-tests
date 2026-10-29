#!/usr/bin/env python3
"""Reversible US #45 research on an existing disposable drum Program, pad zero.

Build the ARM tools first. Stops/restarts only the installed API host; never
installs an addin, restarts MPC, saves a project, or changes selection.
Run while idle on Main, Mixer or Track Mute with target Automation Off.
Optional automation gate tests temporarily switch global modes and restore Off.
"""

import argparse
import json
from pathlib import Path
import re
import struct
import time
import uuid

from mpc_command_policy import capture
from mpc_live_state import MemorySession
from mpc_program_parameters import PROFILE, ROOT, independent, validate


def run(args):
    profile = validate(json.loads(PROFILE.read_text()))
    state_profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", state_profile)
    remote = "/tmp/mpc-us45-" + uuid.uuid4().hex[:12]
    report = {"story": 45, "buildId": profile["firmware"]["buildId"], "rows": []}
    host_cli = "/data/mpc-addins/mpc-native-api/mpc-api"
    loaded = stopped = created = False
    identities = before = baseline = None
    handle = worker = None
    automation_changed = False

    def button(name, action=None):
        query = "name=" + name + ("&action=" + action if action else "")
        session.command("wget -qO /dev/null 'http://127.0.0.1:6720/button?" + query + "'")

    def toggle_off():
        button("SHIFT", "down")
        try:
            button("AUTOMATION")
        finally:
            button("SHIFT", "up")

    def restore_automation():
        nonlocal automation_changed
        if not automation_changed:
            return
        modes = {r["automation"] for r in capture(session)["tracks"]}
        if modes == {2}:
            button("AUTOMATION")
            modes = {r["automation"] for r in capture(session)["tracks"]}
        if modes == {1}:
            toggle_off()
        if {r["automation"] for r in capture(session)["tracks"]} != {0}:
            raise RuntimeError("Global automation did not return to Off")
        automation_changed = False

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    def command(line):
        return json.loads(
            session.command(f"{remote}/native_client {session.pid}", (line + "\n").encode())
        )

    def target_line(operation, parameter, *values, identity=None):
        identity = identity or identities
        track = next(t for t in identity["tracks"] if t["index"] == args.track)
        fields = [
            operation,
            identity["instanceId"],
            identity["project"],
            identity["projectGeneration"],
            track["id"],
            track["generation"],
            parameter["nativeId"],
            *values,
        ]
        return " ".join(map(str, fields))

    def read(parameter):
        result = command(target_line("READ", parameter))
        if not result["ok"] or result["tid"] != session.pid:
            raise RuntimeError(f"Native read failed: {result}")
        return result["after"]

    def set_value(parameter, desired, expected):
        result = command(target_line("SET", parameter, desired, expected))
        if not result["ok"]:
            raise RuntimeError(f"Native setter rejected: {result}")
        return result

    parameters = [
        p for p in profile["parameters"] if p["classification"] in ("candidate", "native_control")
    ]
    all_parameters = parameters
    invalid_keys = set()
    if args.parameters:
        requested = set(args.parameters)
        invalid_keys = requested - {p["key"] for p in parameters}
        parameters = [p for p in parameters if p["key"] in requested]
    report["targetTrackIndex"] = args.track
    report["parameterKeys"] = [p["key"] for p in parameters]
    try:
        if invalid_keys:
            raise ValueError("Unknown or unqualified parameter requested")
        if profile["firmware"]["buildId"] != state_profile["buildId"]:
            raise RuntimeError("Parameter/state firmware mismatch")
        code = session.read_many([(session.base + f["rva"], 32) for f in profile["functions"]])
        if code != [bytes.fromhex(f["prefixHex"]) for f in profile["functions"]]:
            raise RuntimeError("Parameter code fingerprint mismatch")
        report["fingerprintsMatched"] = True
        before = session.snapshot(session.active_project())
        report["before"] = before
        context = capture(session)
        report["contextBefore"] = context
        if (
            context["playing"]
            or context["recordMode"]
            or context["tracks"][args.track]["automation"]
        ):
            raise RuntimeError("Start stopped, recording Off, target Automation Off")
        baseline = independent(session, profile, args.track)
        report["parametersBefore"] = baseline
        save()
        status = json.loads(session.command(host_cli + " host-status"))
        report["installedBefore"] = status
        if status["status"] != "ready":
            raise RuntimeError("Installed native API host is not ready")
        status = json.loads(session.command(host_cli + " host-stop"))
        stopped = True
        if status["status"] != "stopped":
            raise RuntimeError("Installed host did not drain")
        session.command("mkdir -m 700 " + remote)
        created = True
        for name in ("hotload", "native_client", "test_payload.so", "program_parameter_lab.so"):
            session.command(
                "cat > " + remote + "/" + name,
                (ROOT / "builddir/arm/tools" / name).read_bytes(),
            )
        session.command(f"chmod 700 {remote}/hotload {remote}/native_client")
        proof = session.command(
            f"MPC_NATIVE_FIXTURE_DIR={remote} {remote}/hotload --self-test {remote}/test_payload.so"
        )
        if b"self_test_restored_read=true" not in proof:
            raise RuntimeError("Disposable loader self-test failed")
        report["loaderSelfTest"] = True
        workers = (
            session.command(
                f"for f in /proc/{session.pid}/task/*/comm; do "
                'if test "$(cat "$f")" = "privtap inject"; then echo "$f"; fi; done'
            )
            .decode()
            .splitlines()
        )
        if len(workers) != 1:
            raise RuntimeError("Expected one idle privtap injection worker")
        worker = int(workers[0].split("/")[-2])
        result = json.loads(
            session.command(
                f"{remote}/hotload load {session.pid} {worker} {remote}/program_parameter_lab.so"
            )
        )
        handle, loaded = result["handle"], True
        # Persist recovery coordinates before any native write.
        report["attachment"] = {
            "remoteDirectory": remote,
            "pid": session.pid,
            "worker": worker,
            "handle": handle,
        }
        save()
        identities = command("LIST")
        if not identities["ok"]:
            raise RuntimeError(f"Identity capture failed: {identities}")
        report["identities"] = identities
        for parameter in parameters:
            key = parameter["key"]
            original = baseline[key]["normalized"]
            if abs(read(parameter) - original) > 1e-7:
                raise RuntimeError("Native and independent baseline disagree")
            for value in (0.0, 0.25, 0.5, 0.75, 1.0):
                value = struct.unpack("<f", struct.pack("<f", value))[0]
                current = read(parameter)
                result = set_value(parameter, value, current)
                row = {"parameter": key, "desired": value, "result": result}
                report["rows"].append(row)
                save()
                deadline = time.monotonic() + 3
                while True:
                    observed = independent(session, profile, args.track)[key]
                    if abs(observed["normalized"] - value) < 1e-6:
                        break
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f"Independent readback disagreed: {key}: {observed}")
                    time.sleep(0.03)
                row["independentReadback"] = observed
                row["nativeReadback"] = read(parameter)
                if abs(row["nativeReadback"] - observed["normalized"]) > 1e-7:
                    raise RuntimeError("Native getter and independent readback disagree")
                noop = set_value(parameter, row["nativeReadback"], row["nativeReadback"])
                if noop["nativeInvoked"]:
                    raise RuntimeError("No-op invoked a native setter")
                conflict = command(target_line("SET", parameter, value, 0.1))
                if (
                    conflict["ok"]
                    or conflict["nativeInvoked"]
                    or conflict["error"] != "PRECONDITION_FAILED"
                ):
                    raise RuntimeError(f"Expected conflict was not rejected: {conflict}")
                row.update(noop=noop, conflict=conflict)
                save()
            set_value(parameter, original, read(parameter))
            print(f"{key}: 5 values/readbacks, no-ops and conflicts passed", flush=True)
        negatives = ["LIST garbage", "QUIT garbage", "READ 0 0 0 0 514 garbage"]
        parameter = parameters[0]
        for value in ("nan", "inf", "-0.1", "1.1"):
            negatives.append(target_line("SET", parameter, value, 1))
        negatives.append(target_line("SET", {"nativeId": 1069}, 0.5, 0))
        negatives.append(target_line("SET", {"nativeId": 123456}, 0.5, 0))
        report["invalidRequests"] = [command(line) for line in negatives]
        if any(r["ok"] or r["nativeInvoked"] for r in report["invalidRequests"]):
            raise RuntimeError("Invalid parameter request was invoked")
        stale = json.loads(json.dumps(identities))
        stale["projectGeneration"] = str(int(stale["projectGeneration"]) + 999)
        report["staleTarget"] = command(target_line("SET", parameter, 0.5, 1, identity=stale))
        if report["staleTarget"]["ok"] or report["staleTarget"]["nativeInvoked"]:
            raise RuntimeError("Stale generation was invoked")
        other = next((t for t in before["tracks"] if t["type"] == "plugin"), None)
        if other:
            unsupported = json.loads(json.dumps(identities))
            source = next(t for t in identities["tracks"] if t["index"] == other["index"])
            target = next(t for t in unsupported["tracks"] if t["index"] == args.track)
            target.update(id=source["id"], generation=source["generation"])
            report["unsupportedFamily"] = command(
                target_line("SET", parameter, 0.25, 0, identity=unsupported)
            )
            result = report["unsupportedFamily"]
            if (
                result["ok"]
                or result["nativeInvoked"]
                or result["error"] != "UNSUPPORTED_TRACK_TYPE"
            ):
                raise RuntimeError("Plugin target entered the drum parameter path")
        if args.automation_gates:
            if {r["automation"] for r in context["tracks"]} != {0}:
                raise RuntimeError("Automation gates require all tracks initially Off")
            automation_changed = True
            toggle_off()
            report["automationGates"] = []
            for mode in (1, 2):
                if {r["automation"] for r in capture(session)["tracks"]} != {mode}:
                    raise RuntimeError("Unexpected automation mode for gate test")
                for parameter in all_parameters:
                    current = read(parameter)
                    result = command(target_line("SET", parameter, 0.25, current))
                    if result["ok"] or result["nativeInvoked"]:
                        raise RuntimeError("Automation gate invoked a parameter setter")
                    report["automationGates"].append(
                        {"mode": mode, "parameter": parameter["key"], "result": result}
                    )
                    save()
                if mode == 1:
                    button("AUTOMATION")
            restore_automation()
            if independent(session, profile, args.track) != baseline:
                raise RuntimeError("Automation rejection changed parameters")
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            restore_automation()
            if loaded:
                if identities and identities.get("ok") and baseline and report["rows"]:
                    for parameter in parameters:
                        desired = baseline[parameter["key"]]["normalized"]
                        set_value(parameter, desired, read(parameter))
                    for _ in range(30):
                        if independent(session, profile, args.track) == baseline:
                            break
                        time.sleep(0.05)
                    else:
                        raise RuntimeError("Independent parameter restoration failed")
                    report["parametersRestored"] = True
                result = command("QUIT")
                if not result["ok"]:
                    raise RuntimeError(f"Lab did not detach observers: {result}")
                for attempt in range(40):
                    try:
                        session.command(f"{remote}/hotload unload {session.pid} {worker} {handle}")
                        break
                    except RuntimeError as error:
                        if attempt == 39 or "refused unload:" not in str(error):
                            raise
                        time.sleep(0.1)
                if remote in session.command(f"cat /proc/{session.pid}/maps").decode():
                    raise RuntimeError("Parameter lab mapping remains")
                report["labUnloaded"] = True
                loaded = False
            if stopped and not loaded:
                status = json.loads(session.command(host_cli + " host-start"))
                for _ in range(40):
                    status = json.loads(session.command(host_cli + " host-status"))
                    if status["status"] == "ready":
                        break
                    time.sleep(0.1)
                report["installedAfter"] = status
                if status["status"] != "ready":
                    raise RuntimeError("Installed API host did not recover")
            if created and not loaded:
                for name in (
                    "hotload",
                    "native_client",
                    "test_payload.so",
                    "program_parameter_lab.so",
                ):
                    session.command("rm " + remote + "/" + name)
                for marker in session.command("find " + remote + " -type f").decode().splitlines():
                    if not re.fullmatch(
                        re.escape(remote) + r"/mpc-native-fixture-\d+-(loaded|unloaded)", marker
                    ):
                        raise RuntimeError("Unexpected lab file: " + marker)
                    session.command("rm " + marker)
                session.command("rmdir " + remote)
                report["bundleRemoved"] = True
            report["after"] = session.snapshot(session.active_project())
            fields = ("selectedTrack", "selectedSequence", "sequenceSlots", "tracks", "buses")
            report["stateRestored"] = before is not None and all(
                before[k] == report["after"][k] for k in fields
            )
            report["pidUnchanged"] = (
                session.command("pidof MPC").strip() == str(session.pid).encode()
            )
            report["contextAfter"] = capture(session)
            if not report["stateRestored"] or not report["pidUnchanged"]:
                raise RuntimeError("State or process changed")
        except Exception as error:
            report["cleanupError"] = str(error)
        session.close()
        save()
    print(
        json.dumps(
            {
                k: report.get(k)
                for k in (
                    "passed",
                    "parametersRestored",
                    "stateRestored",
                    "labUnloaded",
                    "error",
                    "cleanupError",
                )
            }
        )
    )
    return int(bool(report.get("error") or report.get("cleanupError")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="mpc-key-37.lan.arpa")
    parser.add_argument(
        "--track", type=int, required=True, help="Zero-based disposable drum Track index"
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--parameters", nargs="+", help="Repeat only named members of the fixed subset"
    )
    parser.add_argument(
        "--automation-gates",
        action="store_true",
        help="Also reject Read/Write writes; requires all tracks initially Off and restores Off",
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
