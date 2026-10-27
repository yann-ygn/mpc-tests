#!/usr/bin/env python3
"""Read-only diagnostic of the native command policy's current live sources.

This external observation is not a command permission or lifetime authority.
The in-process policy re-resolves and checks the same sources on the UI thread.
No settings, mode, project, addin or native properties are changed.
"""

import argparse
import json
import os
from pathlib import Path

from mpc_live_state import MemorySession, SnapshotChanged, u32

ROOT = Path(__file__).resolve().parents[1]


def capture(session):
    p = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json").read_text())[
        "commandContext"
    ]
    identity = session.profile["identity"]

    def pointer(owner, offset):
        result = u32(session.read(owner + offset, 4))
        if not result or result % 4:
            raise SnapshotChanged("Invalid live context pointer")
        return result

    def obj(address, vtable):
        if u32(session.read(address, 4)) != session.base + vtable:
            raise SnapshotChanged("Unexpected live context owner")
        return address

    app = obj(pointer(session.base, identity["applicationSlot"]), identity["applicationVtable"])
    window = obj(pointer(app, identity["applicationWindow"]), identity["windowVtable"])
    component = obj(pointer(window, identity["windowComponent"]), identity["componentVtable"])
    engine = obj(component + identity["componentEngine"], identity["engineVtable"])
    mode = obj(pointer(engine, p["engineMode"]), p["modeVtable"])
    thread = obj(pointer(engine, p["engineAudioThread"]), p["audioThreadVtable"])
    sequencer = obj(pointer(thread, p["audioThreadSequencer"]), p["sequencerVtable"])
    timeline = obj(pointer(sequencer, p["sequencerTimeline"]), p["timelineVtable"])
    recorder = obj(pointer(sequencer, p["sequencerRecorder"]), p["recorderVtable"])
    record_properties = pointer(recorder, p["recorderProperties"])
    obj(record_properties, p["recordPropertiesVtable"])
    if pointer(recorder, p["recorderTimeline"]) != timeline:
        raise SnapshotChanged("Recorder and sequencer have different timelines")
    arm = obj(pointer(engine, p["engineArmBehaviour"]), p["armBehaviourVtable"])
    project = session.active_project()
    if (
        pointer(arm, p["armBehaviourProperties"]) != record_properties
        or pointer(arm, p["armBehaviourProject"]) != project
    ):
        raise SnapshotChanged("Recorder and arm behaviour have different owners")
    mute = pointer(engine, p["engineMuteProperties"])
    solo = obj(pointer(engine, p["engineSoloBehaviour"]), p["soloBehaviourVtable"])
    solo_property = pointer(solo, p["soloProperty"])
    mode_data, timeline_data, mute_data, arm_data, solo_data, recording, playback, song = (
        session.read_many(
            [
                (mode + p["modeValue"], p["popupValue"] - p["modeValue"] + 4),
                (timeline + p["timelinePlaying"], 1),
                (mute + p["perSequenceMute"], 1),
                (record_properties + p["armModeValue"], 4),
                (solo_property + p["soloValue"], 4),
                (record_properties + p["recordModeValue"], 4),
                (sequencer + p["sequencerMode"], 4),
                (sequencer + p["sequencerSongSource"], 4),
            ]
        )
    )
    po, poolo = (session.profile["offsets"][g] for g in ("project", "trackPool"))
    pool = pointer(project, po["trackPool"])
    tracks = session.vector(session.read(pool, 0xA0), poolo, "user")
    programs = [
        u32(data)
        for data in session.read_many(
            [(track + session.profile["offsets"]["track"]["program"], 4) for track in tracks]
        )
    ]
    rows = []
    for index, (program, data) in enumerate(
        zip(programs, session.read_many([(a, p["programAutomation"] + 4) for a in programs]))
    ):
        family = session.profile["programTypes"].get(str(u32(data) - session.base), "unknown")
        rows.append(
            {"index": index, "family": family, "automation": u32(data, p["programAutomation"])}
        )
    return {
        "mode": u32(mode_data),
        "popup": u32(mode_data, p["popupValue"] - p["modeValue"]),
        "playing": timeline_data[0],
        "recordMode": u32(recording),
        "playbackMode": u32(playback),
        "songSourceActive": bool(u32(song)),
        "perSequenceMute": mute_data[0],
        "recordArmMode": u32(arm_data),
        "soloMode": u32(solo_data),
        "tracks": rows,
        "atomic": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    try:
        result = capture(session)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
    finally:
        session.close()


if __name__ == "__main__":
    main()
