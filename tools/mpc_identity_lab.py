#!/usr/bin/env python3
"""Capture and compare opaque identities around manual disposable-project edits.

Attach with mpc_native_lab.py --identities --keep. Each invocation captures one
checkpoint, independently reads native metadata, and checks requested invariants.
Stale-track checks send an expected-state mute request and require rejection
before a setter runs. Use only disposable projects with the owner's approval.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from mpc_live_state import MemorySession

ROOT = Path(__file__).resolve().parents[1]


def reference(snapshot, kind, index):
    objects = [obj for obj in snapshot[kind] if obj["index"] == index]
    if len(objects) != 1:
        raise ValueError(f"No unique {kind} entry at index {index}")
    obj = objects[0]
    return " ".join(
        [
            snapshot["instanceId"],
            snapshot["projectId"],
            snapshot["projectGeneration"],
            obj["id"],
            obj["generation"],
        ]
    )


def pair(value):
    if not re.fullmatch(r"\d+:\d+", value):
        raise argparse.ArgumentTypeError("Expected old-index:new-index")
    return tuple(map(int, value.split(":")))


def run(args):
    attachment = json.loads(args.attachment.read_text())
    remote, pid = attachment["remoteDirectory"], attachment["pid"]
    if (
        not re.fullmatch(r"/tmp/mpc-native-lab-[0-9a-f]{12}", remote)
        or type(pid) is not int
        or pid <= 0
        or not attachment.get("bridgeRetained")
    ):
        raise ValueError("Expected a retained lab attachment report")
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
        result = subprocess.run(ssh + [text], input=data, capture_output=True, timeout=20)
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace")[:300])
        return result.stdout

    def request(text):
        return json.loads(command(f"{remote}/native_client {pid}", (text + "\n").encode()))

    if command("pidof MPC").decode().strip() != str(pid):
        raise RuntimeError("MPC process changed: attach again; previous references remain stale")
    status = command(f"{remote}/native_client {pid}", b"ADAPTER_STATUS\n").decode().split()
    if len(status) != 5 or status[1] != attachment["adapter"]["instanceId"]:
        raise RuntimeError("Adapter instance changed: use its current attachment report")
    report = json.loads(args.report.read_text()) if args.report.exists() else {"checkpoints": {}}
    if args.checkpoint in report["checkpoints"]:
        raise ValueError("Checkpoint already exists; choose a new name")
    current = request("IDENTITIES")
    if not current.get("ok"):
        raise RuntimeError("Identity capture failed: " + current.get("error", "unknown"))
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    try:
        if session.pid != pid:
            raise RuntimeError("Process changed during capture")
        observed = session.snapshot(session.active_project())
    finally:
        session.close()
    cases = []

    def check(name, ok, **evidence):
        cases.append({"name": name, "passed": bool(ok), **evidence})

    check("independent normal-track count", len(current["tracks"]) == len(observed["tracks"]))
    check("independent sequence-slot count", current["sequenceSlots"] == observed["sequenceSlots"])
    for kind, metadata in [("tracks", "selectedTrack"), ("sequences", "selectedSequence")]:
        index = observed[metadata]["index"]
        selected = current["selectedTrackId" if kind == "tracks" else "selectedSequenceId"]
        expected = next((obj["id"] for obj in current[kind] if obj["index"] == index), "0")
        check("independent " + metadata, selected == expected)
    if args.compare:
        previous = report["checkpoints"][args.compare]["identities"]
        same_project = (
            current["instanceId"],
            current["projectId"],
            current["projectGeneration"],
        ) == (previous["instanceId"], previous["projectId"], previous["projectGeneration"])
        if args.new_project:
            check("project incarnation changed", not same_project)
        if args.new_instance:
            check("adapter instance changed", current["instanceId"] != previous["instanceId"])
        for kind, pairs in [("tracks", args.same_track), ("sequences", args.same_sequence)]:
            for old_index, new_index in pairs:
                check(
                    f"stable {kind} {old_index}:{new_index}",
                    reference(previous, kind, old_index) == reference(current, kind, new_index),
                )
        for index in args.stale_track:
            receipt = request("HANDLE_SET mute 0 0 " + reference(previous, "tracks", index))
            check(
                f"stale track {index} rejected before setter",
                receipt.get("error") == "STALE_TARGET" and receipt.get("nativeInvoked") is False,
                receipt=receipt,
            )
        for index in args.stale_sequence:
            receipt = request("HANDLE_SEQUENCE " + reference(previous, "sequences", index))
            check(
                f"stale sequence {index} rejected",
                receipt.get("error") == "STALE_TARGET",
                receipt=receipt,
            )
    for index in args.read_track:
        receipt = request("HANDLE_READ " + reference(current, "tracks", index))
        check(f"current track {index} readable", receipt.get("ok"), receipt=receipt)
    checkpoint = {
        "pid": pid,
        "buildId": profile["buildId"],
        "identities": current,
        "observed": observed,
        "cases": cases,
        "passed": all(case["passed"] for case in cases),
    }
    report["checkpoints"][args.checkpoint] = checkpoint
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "checkpoint": args.checkpoint,
                "passed": checkpoint["passed"],
                "cases": cases,
                "tracks": observed["tracks"],
                "selectedSequence": observed["selectedSequence"],
            }
        )
    )
    return checkpoint["passed"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--compare")
    parser.add_argument("--same-track", type=pair, action="append", default=[])
    parser.add_argument("--same-sequence", type=pair, action="append", default=[])
    parser.add_argument("--stale-track", type=int, action="append", default=[])
    parser.add_argument("--stale-sequence", type=int, action="append", default=[])
    parser.add_argument("--read-track", type=int, action="append", default=[])
    parser.add_argument("--new-project", action="store_true")
    parser.add_argument("--new-instance", action="store_true")
    args = parser.parse_args()
    if (
        args.same_track
        or args.same_sequence
        or args.stale_track
        or args.stale_sequence
        or args.new_project
        or args.new_instance
    ) and not args.compare:
        parser.error("Identity comparisons require --compare CHECKPOINT")
    raise SystemExit(0 if run(args) else 1)


if __name__ == "__main__":
    main()
