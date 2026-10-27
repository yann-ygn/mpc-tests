#!/usr/bin/env python3
"""US #53 reversible Read mute/solo/volume device qualification.

Requires an existing authoritative lab, normal sequence transport stopped at zero,
recording Off and targets already in Read. Never changes automation mode, timeline
content, projects, addins or routing. Native sequence mute parameter updates are
intentional. Keeps raw reports under ignored build/ and restores captured state.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

from mpc_automation_flags import beginning, require
from mpc_command_policy import capture
from mpc_live_state import MemorySession, u32
from mpc_mixer_controls import MixerMatrix, independent
from mpc_native_lab import ROOT
from mpc_screen_flags import SCREENS


def sources(session, matrix):
    """Independent timeline digest and parameter values, never write permission.

    Traversal runs only for before/after research evidence, outside MPC's UI
    thread. The production command path does not scan these timeline events.
    """
    layout = matrix.control["readSources"]
    sequence = u32(
        session.read(matrix.project + session.profile["offsets"]["project"]["selectedSequence"], 4)
    )
    tree = session.read(sequence + layout["sequenceClipTree"], 20)
    pending, clips, visited = [u32(tree, 4)], {}, set()
    while pending:
        batch = [node for node in pending if node]
        pending = []
        for address, entry in zip(
            batch, session.read_many([(node, 28) for node in batch]), strict=True
        ):
            if address in visited or len(visited) >= 264:
                raise RuntimeError("Unstable Sequence Track-to-Clip map")
            visited.add(address)
            clips[u32(entry, 16)] = u32(entry, 20)
            pending.extend([u32(entry, 8), u32(entry, 12)])
    if len(visited) != u32(tree, 16):
        raise RuntimeError("Sequence Clip map changed during capture")
    result = []
    for index, (track, program) in enumerate(matrix.targets()):
        prop = program + matrix.control["bindings"][0]["propertyOffset"]
        node = u32(session.read(prop + 8, 4))
        seen, clip = set(), None
        while node != prop + 4:
            if node in seen or len(seen) >= layout["listenerLimit"]:
                raise RuntimeError("Unstable native source subscription")
            seen.add(node)
            data = session.read(node, 16)
            listener = u32(data, 12) & ~3
            if listener and u32(session.read(listener, 4)) == (
                session.base + layout["muteListenerVtable"]
            ):
                data_listener = session.read(listener, 0x3C)
                if u32(data_listener, 0x24) != session.base + layout["muteListenerCallback"]:
                    raise RuntimeError("Unexpected native Clip callback")
                clip = u32(data_listener, 0x34)
            node = u32(data, 4)
        canonical_clip = clips.get(track, 0)
        if clip is not None and clip != canonical_clip:
            raise RuntimeError("Subscription does not match the active Sequence Clip map")
        clip = canonical_clip
        if not clip:
            result.append({"index": index, "clipPresent": False})
            continue
        data = session.read(clip, 0x488)
        if u32(data) != session.base + layout["clipVtable"]:
            raise RuntimeError("Unexpected source Clip")
        count = u32(data, layout["eventTree"] + 16)
        pending, events = [u32(data, layout["eventTree"] + 4)], {}
        while pending:
            batch = [node for node in pending if node]
            pending = []
            for address, event in zip(
                batch, session.read_many([(node, 0x48) for node in batch]), strict=True
            ):
                if address in events or len(events) >= 100000:
                    raise RuntimeError("Unstable native event tree")
                events[address] = event
                pending.extend([u32(event, 8), u32(event, 12)])
        if len(events) != count:
            raise RuntimeError("Native event count changed during capture")
        digest = hashlib.sha256(
            b"".join(struct.pack("<I", address) + events[address] for address in sorted(events))
        ).hexdigest()
        parameters, seen = [], set()
        node = u32(data, layout["clipParameterMap"] + 8)
        while node:
            if node in seen or len(seen) >= 4096:
                raise RuntimeError("Unstable native parameter map")
            seen.add(node)
            parameter = session.read(node, 20)
            parameters.append(
                {
                    "group": u32(parameter, 4),
                    "id": u32(parameter, 8),
                    "value": struct.unpack_from("<f", parameter, 12)[0],
                }
            )
            node = u32(parameter)
        automation_parameters = sorted(
            {
                (u32(event, 0x20), u32(event, 0x28))
                for event in events.values()
                if (u32(event, 0x18) & 255) == 2
            }
        )
        result.append(
            {
                "index": index,
                "clipPresent": True,
                "eventCount": count,
                "automationParameters": automation_parameters,
                "timelineSha256": digest,
                "parameters": sorted(parameters, key=lambda p: (p["group"], p["id"])),
            }
        )
    return result


class ReadMatrix(MixerMatrix):
    def volume(self, index):
        return struct.unpack("<f", self.session.read(self.targets()[index][1] + 0x5D8, 4))[0]

    def volume_row(self, index, selected, rows, source):
        original = self.volume(index)
        row = {
            "screen": self.args.screen,
            "playing": self.require_playing,
            "track": index,
            "selected": selected,
            "op": "set_volume",
            "commands": [],
        }
        rows.append(row)
        # Native Mixable parameter 7 in the Program namespace is normalized volume.
        populated = [257, 7] in source.get("automationParameters", []) or (257, 7) in source.get(
            "automationParameters", []
        )
        row["populatedVolumeSource"] = populated
        for value in (0.0, 1.0, 0.123456789):
            request = self.request(index, "set_volume", value)
            result = self.send(request)
            if not (
                self.require_playing
                and populated
                and result.get("status") == "unknown"
                and result.get("nativeInvoked") is True
                and result.get("error", {}).get("code") == "VERIFICATION_TIMEOUT"
            ):
                self.require(result)
            expected = struct.unpack("<f", struct.pack("<f", value))[0]
            actual = self.volume(index)
            row["commands"].append(result)
            row.setdefault("independentReadback", []).append(actual)
            if actual != expected and not (self.require_playing and populated):
                raise RuntimeError(
                    "Independent volume readback disagreed without an applicable native source"
                )
            replay = self.send(request)
            if {k: v for k, v in replay.items() if k != "diagnosticRoundTripMs"} != {
                k: v for k, v in result.items() if k != "diagnosticRoundTripMs"
            }:
                raise RuntimeError("Numeric replay changed its retained result")
            if not (self.require_playing and populated):
                noop = self.send(self.request(index, "set_volume", value, expected=value))
                self.require(noop, invoked=False)
                conflict = self.send(self.request(index, "set_volume", value, expected=0.75))
                self.require(conflict, "rejected", "PRECONDITION_FAILED", False)
                row["commands"].extend([noop, conflict])
        restored = self.send(self.request(index, "set_volume", original))
        row["restoreWhilePlaying"] = restored
        if not (
            self.require_playing
            and populated
            and restored.get("status") == "unknown"
            and restored.get("error", {}).get("code") == "VERIFICATION_TIMEOUT"
        ):
            self.require(restored)
        row["nativeUpdatesMaySupersede"] = self.require_playing and populated


def run(args):
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    matrix = None
    before = flags = mixers = None
    report = {"story": 53, "scope": args.scope, "rows": []}

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    try:
        args.screen = args.screens[0]
        args.live_policy, args.quick = True, False
        matrix = ReadMatrix(args, session, json.loads(args.attachment.read_text()))
        before, flags, mixers = matrix.snapshot(), matrix.independent(), independent(session)
        report.update(before=before, mixersBefore=mixers, contextBefore=capture(session))
        if report["contextBefore"]["playing"] or report["contextBefore"]["recordMode"]:
            raise RuntimeError("Start stopped with recording Off")
        if bool(report["contextBefore"]["perSequenceMute"]) != (args.scope == "sequence"):
            raise RuntimeError("Prepare the matching native mute preference")
        if any(matrix.automation(i) != 1 for i in args.targets):
            raise RuntimeError("Prepare targets in Read; this runner never changes it")
        matrix.transport()
        if matrix.positions[-1]["pulse"]:
            raise RuntimeError("Start at the sequence beginning for exact restoration")
        report["sourcesBefore"] = sources(session, matrix)
        save()
        for screen in args.screens:
            args.screen = screen
            for playing in (False, True):
                matrix.screen()
                if playing:
                    matrix.ui("/button", name="PLAY")
                    matrix.require_playing = True
                for index in args.targets:
                    for selected in (True, False):
                        matrix.renew()
                        matrix.select(index if selected else (index + 1) % len(matrix.targets()))
                        print(
                            f"Read {screen} playing={playing} target={index} selected={selected}",
                            flush=True,
                        )
                        for prop in ("mute", "solo"):
                            # Restore the requested selection for each operation:
                            # native Single solo can select its target.
                            matrix.select(
                                index if selected else (index + 1) % len(matrix.targets())
                            )
                            original = matrix.independent()[index]["requested"][prop]
                            row = {
                                "screen": screen,
                                "playing": playing,
                                "index": index,
                                "selected": selected,
                                "property": prop,
                            }
                            report["rows"].append(row)
                            row["noop"] = matrix.command(
                                matrix.submission(index, prop, original, original)
                            )
                            require(row["noop"], "applied")
                            row["conflict"] = matrix.command(
                                matrix.submission(index, prop, original, not original)
                            )
                            require(row["conflict"], "rejected", "PRECONDITION_FAILED")
                            text = matrix.submission(index, prop, not original, original)
                            selection_offset = session.profile["offsets"]["project"][
                                "selectedTrackIndex"
                            ]
                            row["selectedAtInvocation"] = (
                                u32(session.read(matrix.project + selection_offset, 4)) == index
                            )
                            if row["selectedAtInvocation"] != selected:
                                raise RuntimeError("Target selection changed before invocation")
                            row["write"] = matrix.command(text)
                            require(row["write"], "applied", invoked=True)
                            row["independentDuring"] = matrix.settle(index, prop, not original)
                            row["restore"] = matrix.command(
                                matrix.submission(index, prop, original, not original)
                            )
                            require(row["restore"], "applied", invoked=True)
                            matrix.settle(index, prop, original)
                            row["replay"] = matrix.command(text)
                            if row["replay"] != row["write"]:
                                raise RuntimeError("Retry changed its retained result")
                            if matrix.independent() != flags:
                                matrix.restore(flags, before["selectedTrack"]["index"])
                        if before["tracks"][index]["type"] in (
                            "plugin",
                            "drum",
                            "keygroup",
                            "audio",
                        ):
                            matrix.select(
                                index if selected else (index + 1) % len(matrix.targets())
                            )
                            matrix.volume_row(
                                index, selected, report["rows"], report["sourcesBefore"][index]
                            )
                        if matrix.automation(index) != 1:
                            raise RuntimeError("Read mode changed")
                        save()
                if playing:
                    matrix.require_playing = False
                    matrix.ui("/button", name="STOP")
                    beginning(matrix)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            if matrix:
                matrix.require_playing = False
                if matrix.transport()["playing"]:
                    matrix.ui("/button", name="STOP")
                if before:
                    matrix.restore(flags, before["selectedTrack"]["index"])
                    for row in mixers:
                        if row["userIndex"] in args.targets and row["family"] in (
                            "plugin",
                            "drum",
                            "keygroup",
                            "audio",
                        ):
                            matrix.require(
                                matrix.send(
                                    matrix.request(row["userIndex"], "set_volume", row["volume"])
                                )
                            )
                    beginning(matrix)
                    args.screen = args.screens[0]
                    matrix.screen()
                    report["after"] = matrix.snapshot()
                    report["mixersAfter"] = independent(session)
                    report["sourcesAfter"] = sources(session, matrix)
                    report["restored"] = (
                        report["after"] == before and report["mixersAfter"] == mixers
                    )
                    report["timelinePreserved"] = report["sourcesAfter"] == report["sourcesBefore"]
                    if not report["restored"] or not report["timelinePreserved"]:
                        raise RuntimeError("State or timeline differs after restoration")
                report["handlersClosed"] = matrix.command("LAB_COMMAND_CLOSE").get("ok") is True
        except Exception as error:
            report["cleanupError"] = str(error)
        session.close()
        save()
    print(
        json.dumps(
            {
                key: report.get(key)
                for key in ("passed", "restored", "timelinePreserved", "error", "cleanupError")
            }
        )
    )
    return int(bool(report.get("error") or report.get("cleanupError")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--targets", type=int, nargs="+", required=True)
    parser.add_argument("--screens", nargs="+", choices=SCREENS, default=["mixer_volume"])
    parser.add_argument("--scope", choices=("global", "sequence"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.output.resolve().is_relative_to(ROOT / "build"):
        parser.error("Keep private device reports under ignored build/")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
