#!/usr/bin/env python3
"""Independent, read-only mixed-track inventory for the supported firmware.

Uses the application owner path and explicit native collections, without loading
the adapter or calling a native function. Run while idle; this external reader
does not establish lifetimes. Reports contain no native addresses.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path

from mpc_live_state import MemorySession, SnapshotChanged, u32

ROOT = Path(__file__).resolve().parents[1]
MASTER_TYPES = {"input", "output", "master-output", "return", "submix"}


def capture(session, flags=False):
    profile = session.profile
    po, poolo, to = (profile["offsets"][k] for k in ("project", "trackPool", "track"))
    project = session.active_project()
    before = session.read(project, 0xAA8)
    pool = u32(before, po["trackPool"])
    pool_data = session.read(pool, 0x130)
    if u32(pool_data) != session.base + profile["vtables"]["trackPool"]:
        raise SnapshotChanged("Unexpected track-pool owner")
    vectors = {
        name: session.vector(pool_data, poolo, name, limit=512)
        for name in ("all", "user", "normal", "audio", "master")
    }
    limits = {"all": 264, "user": 136, "normal": 128, "audio": 8, "master": 128}
    for name, addresses in vectors.items():
        if len(addresses) > limits[name] or len(set(addresses)) != len(addresses):
            raise SnapshotChanged("Invalid inventory capacity or duplicate object")
    all_tracks, users, midi, audio, masters = (
        vectors[k] for k in ("all", "user", "normal", "audio", "master")
    )
    if (
        set(users) & set(masters)
        or set(midi) & set(audio)
        or set(users) | set(masters) != set(all_tracks)
        or set(midi) | set(audio) != set(users)
    ):
        raise SnapshotChanged("Native collections do not partition all tracks")
    data = session.read_many([(a, 0x650) for a in all_tracks])
    if any(u32(d) != session.base + profile["vtables"]["track"] for d in data):
        raise SnapshotChanged("Invalid track type")
    programs = [u32(d, to["program"]) for d in data]
    program_data = session.read_many([(a, 0x270 if flags else 4) for a in programs])
    names = session.read_many([(u32(d, to["name"]), 256) for d in data])
    rows = {}
    control = (
        json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json").read_text())
        if flags
        else None
    )
    for address, td, pd, name in zip(all_tracks, data, program_data, names):
        if b"\0" not in name:
            raise SnapshotChanged("Unbounded track name")
        kind = profile["programTypes"].get(str(u32(pd) - session.base))
        collection = (
            "midi_triggered" if address in midi else "audio" if address in audio else "master"
        )
        if kind and (
            (kind in MASTER_TYPES) != (address in masters)
            or (kind == "audio") != (address in audio)
        ):
            raise SnapshotChanged("Program and collection disagree")
        rows[address] = {
            "name": name.split(b"\0", 1)[0].decode("utf-8"),
            "type": kind,
            "collection": collection,
            "index": (midi if address in midi else audio if address in audio else masters).index(
                address
            ),
            "uiIndex": users.index(address) if address in users else None,
        }
        if flags:
            supported = (
                control["flagFamilies"].get(kind, {}).get("readableFields", [])
                if address in users
                else []
            )
            requested, copies = {}, {"muted": None}
            for field, binding in zip(("muted", "soloed", "recordArmed"), control["bindings"]):
                source = td if binding["propertyOwner"] == "track" else pd

                def observation(path, offset):
                    if path not in supported:
                        return None
                    if source[offset] > 1:
                        raise SnapshotChanged("Independent field is not boolean")
                    return bool(source[offset])

                requested[field] = observation("requested." + field, binding["scriptValueOffset"])
                if field != "muted":
                    copies[field] = observation(
                        "audioSide." + field, binding["observedValueOffset"]
                    )
            silenced = None
            if "silenced" in supported:
                value = pd[profile["offsets"]["program"]["silenced"]]
                if value > 1:
                    raise SnapshotChanged("Independent silenced field is not boolean")
                silenced = bool(value)
            rows[address].update(requested=requested, audioSide=copies, silenced=silenced)
    selected = u32(before, po["selectedTrack"])
    if (
        selected != u32(before, po["selectedTrackMirror"])
        or (selected and selected not in users)
        or (selected and users.index(selected) != u32(before, po["selectedTrackIndex"]))
    ):
        raise SnapshotChanged("Selected track and canonical index disagree")
    after = session.read(project, 0xAA8)
    after_pool = session.read(pool, 0x130)
    checked = ("selectedTrack", "selectedTrackMirror", "selectedTrackIndex", "trackPool")
    if (
        session.active_project() != project
        or not session.valid_project(after)
        or any(u32(before, po[k]) != u32(after, po[k]) for k in checked)
        or pool_data != after_pool
        or any(
            session.vector(after_pool, poolo, name, limit=512) != value
            for name, value in vectors.items()
        )
    ):
        raise SnapshotChanged("Inventory, order or selection changed during capture")
    after_programs = session.read_many([(a + to["program"], 4) for a in all_tracks])
    after_types = session.read_many([(a, 4) for a in programs])
    if [u32(p) for p in after_programs] != programs or after_types != [p[:4] for p in program_data]:
        raise SnapshotChanged("Program association changed during capture")
    return {
        "source": "independent_process_memory",
        "buildId": profile["buildId"],
        "capturedAt": datetime.now(timezone.utc).isoformat(),
        "atomic": False,
        "selectedTrack": rows.get(selected),
        "tracks": [rows[a] for a in users],
        "buses": [rows[a] for a in masters],
        "counts": {name: len(value) for name, value in vectors.items()},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    try:
        for attempt in range(3):
            try:
                result = capture(session)
                break
            except SnapshotChanged:
                if attempt == 2:
                    raise
                session.refresh_maps()
    finally:
        session.close()
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
