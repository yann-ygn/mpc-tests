#!/usr/bin/env python3
"""Read-only preflight for the native control paths; never calls a setter.

Checks the running build, code fingerprints, live objects, and property listeners.
The separate control profile records analysis evidence, not an enabled write API.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path

from mpc_live_state import MemorySession, SnapshotChanged, u32


ROOT = Path(__file__).resolve().parent.parent


def listeners(session, owner, offset):
    """Read the intrusive listener list used by the two notifying setters."""
    prop = owner + offset
    header = session.read(prop, 16)
    sentinel, node = prop + 4, u32(header, 8)
    seen, result = set(), []
    while node != sentinel:
        if not node or node in seen or len(seen) >= 64:
            raise SnapshotChanged("Invalid or changing property listener list")
        seen.add(node)
        data = session.read(node, 16)
        # +0xc is a tagged Listener pointer; its low two bits are flags.
        tagged = u32(data, 12)
        listener = tagged & ~3
        if listener:
            vtable = u32(session.read(listener, 4))
            if not session.base <= vtable < session.base + 0x7000000:
                raise SnapshotChanged("Listener virtual table is outside the MPC image")
            rtti = u32(session.read(vtable - 4, 4))
            name_ptr = u32(session.read(rtti + 4, 4))
            raw = session.read(name_ptr, 192)
            name = raw.split(b"\0", 1)[0].decode("ascii", errors="replace")
            result.append({"typeNamePrefix": name, "enabled": tagged & 3 == 0})
        node = u32(data, 4)
    return result


def audit(session, control):
    if session.profile["buildId"] != control["buildId"]:
        raise RuntimeError("State and control profiles have different build IDs")
    functions = []
    for function in control["functions"]:
        expected = bytes.fromhex(function["prefixHex"])
        if not 16 <= len(expected) <= 256:
            raise RuntimeError("Invalid function fingerprint length")
        address = session.base + function["rva"]
        if session.read(address, len(expected)) != expected:
            raise RuntimeError("Native code fingerprint differs: " + function["name"])
        functions.append(
            {
                "name": function["name"],
                "rva": hex(function["rva"]),
                "runtimeAddress": hex(address),
                "prefixMatched": True,
            }
        )
    project = session.find_project()
    before = session.snapshot(project)
    po, poolo = session.profile["offsets"]["project"], session.profile["offsets"]["trackPool"]
    project_data = session.read(project, 0xAA8)
    if not session.valid_project(project_data):
        raise SnapshotChanged("Project changed")
    pool = u32(project_data, po["trackPool"])
    addresses = session.vector(session.read(pool, 0x50), poolo, "normal")
    if len(addresses) != len(before["tracks"]):
        raise SnapshotChanged("Track topology changed")
    tracks = []
    for index, address in enumerate(addresses):
        track = session.read(address, 0x64C)
        if u32(track) != session.base + session.profile["vtables"]["track"]:
            raise SnapshotChanged("Track no longer has the expected type")
        name_ptr = u32(track, session.profile["offsets"]["track"]["name"])
        name = session.read(name_ptr, 256).split(b"\0", 1)[0].decode("utf-8")
        if name != before["tracks"][index]["name"]:
            raise SnapshotChanged("Track order or name changed")
        program = u32(track, session.profile["offsets"]["track"]["program"])
        program_data = session.read(program, 0x270)
        if u32(program_data) - session.base not in {
            int(x) for x in session.profile["programTypes"]
        }:
            raise SnapshotChanged("Program no longer has a known type")
        bindings = []
        for binding in control["bindings"]:
            owner, data = (
                (program, program_data)
                if binding["propertyOwner"] == "program"
                else (address, track)
            )
            script_value = data[binding["scriptValueOffset"]]
            observed = data[binding["observedValueOffset"]]
            if script_value not in (0, 1) or observed not in (0, 1):
                raise SnapshotChanged("Invalid boolean field")
            bindings.append(
                {
                    "name": binding["name"],
                    "scriptGetterValue": bool(script_value),
                    "observerValue": bool(observed),
                    "listeners": listeners(session, owner, binding["propertyOffset"]),
                }
            )
        tracks.append({"index": index, "name": name, "bindings": bindings})
    after = session.snapshot(project)
    for field in ("selectedTrack", "selectedSequence", "tracks", "buses", "sequenceSlots"):
        if before[field] != after[field]:
            raise SnapshotChanged("Live state changed during the audit; repeat while idle")
    return {
        "capturedAt": datetime.now(timezone.utc).isoformat(),
        "pid": session.pid,
        "buildId": control["buildId"],
        "mode": "read-only-audit",
        "nativeCallsTested": False,
        "writesEnabled": False,
        "functions": functions,
        "tracks": tracks,
        "stateStableAcrossAudit": True,
        "limitations": [
            "Listener traversal is not atomic; run while idle.",
            "Matching code and listeners do not prove native invocation is safe.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--binary", type=Path, default=ROOT / "builddir/arm/tools/mem_probe")
    parser.add_argument(
        "--profile", type=Path, default=ROOT / "tools/profiles/key37-mpc3.9.1.2.json"
    )
    parser.add_argument(
        "--control-profile",
        type=Path,
        default=ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json",
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    session = MemorySession(args.host, args.binary, json.loads(args.profile.read_text()))
    try:
        report = audit(session, json.loads(args.control_profile.read_text()))
    finally:
        session.close()
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
