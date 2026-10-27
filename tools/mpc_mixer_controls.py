#!/usr/bin/env python3
"""Bounded US #39 mixer qualification through a retained native lab.

Use a disposable project in Claude Experiments. The runner uses production live
policy and generation-bound managed commands; it never saves, loads, installs,
restarts or edits effects/routing. Raw reports belong in ignored build/.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import threading
import json
import os
from pathlib import Path
import struct
import time
import uuid
import xml.etree.ElementTree as ET

from mpc_command_lab import HandlerMatrix
from mpc_command_policy import capture
from mpc_live_state import MemorySession, SnapshotChanged, u32
from mpc_native_lab import ROOT
from mpc_playback_flags import PlaybackMatrix
from mpc_screen_flags import SCREENS


OPERATIONS = (
    "set_volume",
    "set_pan",
    "set_send_level",
    "set_insert_rack_enabled",
    "set_insert_enabled",
)


def independent(session):
    """External process-memory observations, independent of adapter snapshots."""
    p = session.profile
    project = session.active_project()
    pool = u32(session.read(project + p["offsets"]["project"]["trackPool"], 4))
    pool_data = session.read(pool, 0x130)
    vectors = {
        k: session.vector(pool_data, p["offsets"]["trackPool"], k, limit=512)
        for k in ("all", "user", "master")
    }
    tracks = vectors["all"]
    blocks = session.read_many([(t, 0x650) for t in tracks])
    programs = [u32(d, p["offsets"]["track"]["program"]) for d in blocks]
    data = session.read_many([(pg, 0x700) for pg in programs])
    racks = [u32(d, 0x5E0) for d in data]
    rack_data = session.read_many([(r, 0xD8) for r in racks])
    slot_data = session.read_many(
        [job for rd in rack_data for job in [(u32(rd, 0x94), 16), (u32(rd, 0xB0), 4)]]
    )
    out = []
    for row_index, (track, td, d, rd) in enumerate(
        zip(tracks, blocks, data, rack_data, strict=True)
    ):
        begin, end = u32(rd, 0x94), u32(rd, 0x98)
        if end - begin != 16:
            raise SnapshotChanged("Unexpected insert count")
        slots = struct.unpack("<4I", slot_data[2 * row_index])
        bits = u32(slot_data[2 * row_index + 1]) & 15
        out.append(
            {
                "userIndex": vectors["user"].index(track) if track in vectors["user"] else None,
                "family": p["programTypes"].get(str(u32(d) - session.base)),
                "volume": struct.unpack_from("<f", d, 0x5D8)[0],
                "pan": struct.unpack_from("<f", d, 0x3F8)[0],
                "sends": [struct.unpack_from("<f", d, 0x450 + 56 * i)[0] for i in range(4)],
                "rack": bool(rd[0x38]),
                "slotBits": bits,
                "slots": [bool(bits & (1 << i)) if slots[i] else None for i in range(4)],
                "automation": u32(d, 0x214),
            }
        )
    if session.active_project() != project:
        raise SnapshotChanged("Project changed")
    return out


def field(row, op, index):
    return (
        row[
            {
                "set_volume": "volume",
                "set_pan": "pan",
                "set_send_level": "sends",
                "set_insert_rack_enabled": "rack",
                "set_insert_enabled": "slots",
            }[op]
        ][index]
        if op in ("set_send_level", "set_insert_enabled")
        else row[{"set_volume": "volume", "set_pan": "pan", "set_insert_rack_enabled": "rack"}[op]]
    )


class MixerMatrix(PlaybackMatrix):
    def observed(self, index):
        # A diagnostic cache identifies the row only. The adapter independently
        # resolves public IDs for every call; these addresses never target writes.
        track, program = self.targets()[index]
        association, data = self.session.read_many(
            [(track + self.session.profile["offsets"]["track"]["program"], 4), (program, 0x700)]
        )
        if u32(association) != program:
            raise SnapshotChanged("Program changed during independent readback")
        rack = u32(data, 0x5E0)
        rd = self.session.read(rack, 0xD8)
        if u32(rd, 0x98) - u32(rd, 0x94) != 16:
            raise SnapshotChanged("Insert topology changed")
        slots, bits = self.session.read_many([(u32(rd, 0x94), 16), (u32(rd, 0xB0), 4)])
        pointers = struct.unpack("<4I", slots)
        mask = u32(bits)
        return {
            "volume": struct.unpack_from("<f", data, 0x5D8)[0],
            "pan": struct.unpack_from("<f", data, 0x3F8)[0],
            "sends": [struct.unpack_from("<f", data, 0x450 + 56 * i)[0] for i in range(4)],
            "rack": bool(rd[0x38]),
            "slots": [bool(mask & (1 << i)) if pointers[i] else None for i in range(4)],
        }

    def controls(self, index):
        sample = self.command(f"FLAGS {index} 1 0")
        if not sample.get("ok") or len(sample["tracks"]) != 1:
            raise RuntimeError("Mixer sampler failed: " + json.dumps(sample))
        return sample["tracks"][0]

    def request(
        self, index, op, value, control_index=0, expected=None, request_id=None, wait_ms=1000
    ):
        track = self.controls(index)
        request = {
            "apiVersion": "0.1",
            "op": op,
            "requestId": request_id or "m_" + uuid.uuid4().hex,
            "target": {
                "instanceId": self.instance,
                "projectId": self.identities["projectId"],
                "trackId": track["id"],
            },
            "mixerGeneration": track["mixer"]["generation"],
            "value": value,
            "startWithinMs": 1000,
            "waitMs": wait_ms,
        }
        if op == "set_send_level":
            request["sendIndex"] = control_index
        if op == "set_insert_enabled":
            request["slotIndex"] = control_index
        if expected is not None:
            request["expected"] = {"value": expected}
        return request

    def send(self, request):
        if self.require_playing:
            self.assert_playback()
        view = self.view()
        started = time.monotonic()
        result = HandlerMatrix.command(
            self, "LAB_COMMAND_JSON " + json.dumps(request, separators=(",", ":"))
        )
        result["diagnosticRoundTripMs"] = (time.monotonic() - started) * 1000
        if self.view() != view:
            raise RuntimeError("Mixer request navigated the UI")
        if self.require_playing:
            self.assert_playback()
        return result

    @staticmethod
    def require(result, status="applied", error=None, invoked=None):
        if (
            result.get("status") != status
            or (error and result.get("error", {}).get("code") != error)
            or (invoked is not None and result.get("nativeInvoked") is not invoked)
        ):
            raise RuntimeError("Unexpected mixer receipt: " + json.dumps(result))

    def row(self, index, op, control_index, selected, rows):
        original = field(self.observed(index), op, control_index)
        row = {
            "target": index,
            "op": op,
            "index": control_index,
            "selected": selected,
            "playing": self.require_playing,
            "screen": self.args.screen,
            "before": original,
            "commands": [],
        }
        rows.append(row)
        if original is None:
            request = self.request(index, op, False, control_index)
            rejected = self.send(request)
            self.require(rejected, "rejected", "EMPTY_INSERT", False)
            row["commands"].append(rejected)
            row["restored"] = True
            return
        numeric = op in OPERATIONS[:3]
        try:
            values = (
                ((0.25 if original != 0.25 else 0.75,) if numeric else (not original,))
                if self.args.quick
                else ((0.0, 1.0, 0.123456789) if numeric else (False, True))
            )
            for n, value in enumerate(values):
                request = self.request(index, op, value, control_index)
                result = self.send(request)
                row["commands"].append(result)
                self.require(result)
                actual = field(self.observed(index), op, control_index)
                expected = struct.unpack("<f", struct.pack("<f", value))[0] if numeric else value
                if actual != expected or result["verification"]["requestedValue"] != expected:
                    raise RuntimeError("Independent mixer observation disagreed")
                if self.args.quick:
                    continue
                replay = self.send(request)
                if {k: v for k, v in replay.items() if k != "diagnosticRoundTripMs"} != {
                    k: v for k, v in result.items() if k != "diagnosticRoundTripMs"
                }:
                    raise RuntimeError("Retry changed a retained receipt")
                noop = self.send(self.request(index, op, value, control_index, expected=value))
                self.require(noop, invoked=False)
                conflict = self.send(
                    self.request(
                        index,
                        op,
                        value,
                        control_index,
                        expected=(0.75 if value != 0.75 else 0.25) if numeric else not value,
                    )
                )
                self.require(conflict, "rejected", "PRECONDITION_FAILED", False)
                row["commands"].extend((noop, conflict))
        finally:
            restored = self.send(self.request(index, op, original, control_index))
            self.require(restored)
            row["restore"] = restored
            row["restored"] = field(self.observed(index), op, control_index) == original
            if not row["restored"]:
                raise RuntimeError("Mixer restoration failed")

    def pressure(self, rows):
        self.renew()
        baseline = {i: self.observed(i) for i in self.args.targets}
        requests = [
            self.request(
                self.args.targets[i % len(self.args.targets)],
                "set_volume" if i % 2 else "set_pan",
                (i % 7) / 6,
                request_id=f"rapid_{i}",
                wait_ms=0,
            )
            for i in range(32)
        ]
        samples, errors = [], []
        stop = threading.Event()

        def observe():
            while not stop.is_set():
                try:
                    sample = HandlerMatrix.command(self, "FLAGS 0 16 0")
                    if not sample.get("ok"):
                        raise RuntimeError(str(sample))
                    samples.append(
                        {
                            key: sample[key]
                            for key in ("durationUs", "callbackUs", "shared", "capturedMs")
                        }
                    )
                except Exception as error:
                    errors.append(str(error))
                stop.wait(0.1)

        producer = threading.Thread(target=observe)
        producer.start()
        evidence = {
            "scenario": "rapid_numeric_with_10hz_observer",
            "clients": 8,
            "requests": [],
            "samples": samples,
            "observerErrors": errors,
        }
        rows.append(evidence)
        try:
            evidence["startedAtMonotonic"] = time.monotonic()
            evidence["resourcesBefore"] = self.resources()

            def rapid(request):
                started = time.monotonic()
                reply = HandlerMatrix.command(
                    self, "LAB_COMMAND_JSON " + json.dumps(request, separators=(",", ":"))
                )
                reply["diagnosticRoundTripMs"] = (time.monotonic() - started) * 1000
                return reply

            if self.require_playing:
                self.assert_playback()
            with ThreadPoolExecutor(max_workers=8) as clients:
                replies = list(clients.map(rapid, requests))
            evidence["initialReplies"] = replies
            if self.require_playing:
                self.assert_playback()
            for request, reply in zip(requests, replies, strict=True):
                if reply.get("status") == "accepted" or (
                    reply.get("status") == "unknown"
                    and reply.get("timing", {}).get("totalMs") is None
                ):
                    reply = HandlerMatrix.command(
                        self, f"LAB_COMMAND_GET {self.instance} {request['requestId']} 2000"
                    )
                error = reply.get("error", {})
                error_code = error.get("code") if isinstance(error, dict) else error
                if reply.get("status") not in (
                    "applied",
                    "cancelled",
                    "unknown",
                    "rejected",
                ) and error_code not in (
                    "QUEUE_FULL",
                    "RETENTION_CAPACITY",
                ):
                    raise RuntimeError(
                        "Pressure command lost its retained outcome: " + json.dumps(reply)
                    )
                evidence["requests"].append(reply)
            evidence["resourcesAfter"] = self.resources()
            evidence["finishedAtMonotonic"] = time.monotonic()
        finally:
            stop.set()
            producer.join()
            for i, original in baseline.items():
                for op in ("set_volume", "set_pan"):
                    self.require(self.send(self.request(i, op, field(original, op, 0))))
        if errors:
            raise RuntimeError("Observation failed under pressure")


def run(args):
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    session.ssh[-1:-1] = [
        "-o",
        "ControlMaster=auto",
        "-o",
        "ControlPersist=30",
        "-o",
        "ControlPath=/tmp/mpc-us39-" + uuid.uuid4().hex[:12],
    ]
    report = {"story": 39, "rows": [], "screen": args.screen}
    matrix = None
    before = None
    automation = {}
    try:
        current = ET.fromstring(session.command("cat /data/Settings/MPC/CurrentProject"))
        project = next(e.get("val", "") for e in current if e.get("name") == "lastProjectPath")
        if not project.startswith("/storage/MPC Documents/Projects/Claude Experiments/"):
            raise RuntimeError("Use a disposable Claude Experiments project")
        matrix = MixerMatrix(args, session, json.loads(args.attachment.read_text()))
        matrix.screen()
        before = independent(session)
        report["before"] = before
        report["flagsBefore"] = matrix.snapshot()
        report["contextBefore"] = capture(session)
        initial_selection = matrix.snapshot()["selectedTrack"]["index"]
        report["selectionBefore"] = initial_selection
        automation = {index: matrix.automation(index) for index in range(len(matrix.targets()))}
        for index in args.targets:
            matrix.set_automation(index, 0)
        matrix.screen()
        if args.playing:
            matrix.ui("/button", name="PLAY")
            matrix.require_playing = True
            matrix.assert_playback()
        for index in [] if args.pressure_only else args.targets:
            for selected in (True, False):
                matrix.renew()
                matrix.select(index if selected else (index + 1) % len(matrix.targets()))
                for op in OPERATIONS:
                    count = 4 if op in ("set_send_level", "set_insert_enabled") else 1
                    for control_index in range(count):
                        matrix.row(index, op, control_index, selected, report["rows"])
                        args.output.write_text(json.dumps(report, indent=2) + "\n")
        if args.pressure_only:
            for _ in range(args.bursts):
                matrix.pressure(report["rows"])
        report["passed"] = True
    finally:
        if matrix:
            if matrix.require_playing:
                matrix.require_playing = False
                matrix.ui("/button", name="STOP")
                matrix.ui("/button", name="STOP")
            for index, value in automation.items():
                matrix.set_automation(index, value)
            if before is not None:
                matrix.select(report["selectionBefore"])
                report["after"] = independent(session)
                report["allMixerValuesRestored"] = report["after"] == before
                report["flagsAfter"] = matrix.snapshot()
                report["allFlagsRestored"] = report["flagsAfter"] == report["flagsBefore"]
                report["contextAfter"] = capture(session)
            report["handlersClosed"] = matrix.command("LAB_COMMAND_CLOSE").get("ok") is True
        session.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    if not report.get("allMixerValuesRestored"):
        raise RuntimeError("Restoration audit failed")
    print(
        json.dumps({k: report[k] for k in ("passed", "allMixerValuesRestored", "handlersClosed")})
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--screen", choices=SCREENS, required=True)
    parser.add_argument("--targets", type=int, nargs="+", required=True)
    parser.add_argument("--playing", action="store_true")
    parser.add_argument("--pressure-only", action="store_true")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--bursts", type=int, default=1)
    parser.set_defaults(live_policy=True)
    run(parser.parse_args())


if __name__ == "__main__":
    main()
