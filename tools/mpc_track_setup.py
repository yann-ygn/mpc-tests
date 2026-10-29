#!/usr/bin/env python3
"""Qualify retained track-setup commands in an owner-approved disposable project.

The attachment is a temporary lab running the production handlers and live
policy. This runner does not save/load projects, install addins or restart MPC.
Reports include private diagnostic data and belong under ignored build/.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import uuid

from mpc_live_state import MemorySession, SnapshotChanged, u32
from mpc_native_lab import ROOT
from mpc_screen_flags import SCREENS
from mpc_transport import TransportLab


OPERATIONS = {
    "set_track_colour": ("colour", [0, 0x123456, 0xFFFFFF]),
    "set_track_transpose": ("transposeSemitones", [-24, 12, 24]),
    "set_track_velocity_scale": ("velocityScale", [0.01, 1.23456, 2]),
    "set_midi_output_channel": ("midiOutputChannel", [0, 1, 16]),
    "set_midi_bank_enabled": ("midiBankEnabled", [True, False]),
    "set_midi_bank_msb": ("midiBankMsb", [0, 37, 127]),
    "set_midi_bank_lsb": ("midiBankLsb", [0, 91, 127]),
    "set_midi_program_enabled": ("midiProgramEnabled", [True, False]),
    "set_midi_program": ("midiProgram", [0, 63, 127]),
}


class TrackSetupReader:
    """One batched, read-only sample per check, with fixed topology/type fences.

    The constructor discovers diagnostic addresses. Every sample checks the
    Project, collection bounds/contents, Track vtables and Program pointers.
    These addresses are never used by a native setter or as write authority.
    """

    def __init__(self, session):
        self.session = session
        self.project = session.active_project()
        self.pool = u32(
            session.read(self.project + session.profile["offsets"]["project"]["trackPool"], 4)
        )
        data = session.read(self.pool, 0xA0)
        self.bounds = data[0x74:0x80]
        self.vector = u32(data, 0x74)
        self.addresses = session.vector(data, session.profile["offsets"]["trackPool"], "user")
        tracks = session.read_many([(address, 0x650) for address in self.addresses])
        self.programs = [u32(d, 0x648) for d in tracks]
        self.names = [u32(d, 0x468) for d in tracks]

    def read(self):
        session = self.session
        count = len(self.addresses)
        jobs = [(self.project, 0xAA8), (self.pool, 0xA0)]
        if count:
            jobs.append((self.vector, count * 4))
        jobs += [(address, 0x650) for address in self.addresses]
        jobs += [(address, 4) for address in self.programs]
        jobs += [(address, 256) for address in self.names]
        samples = session.read_many(jobs)
        if not session.valid_project(samples[0]) or samples[1][0x74:0x80] != self.bounds:
            raise SnapshotChanged("Project or track collection changed")
        if count and list(struct.unpack("<" + "I" * count, samples[2])) != self.addresses:
            raise SnapshotChanged("Track order changed")
        offset = 2 + bool(count)
        tracks = samples[offset : offset + count]
        programs = samples[offset + count : offset + 2 * count]
        names = samples[offset + 2 * count :]
        for data, program, name in zip(tracks, self.programs, self.names, strict=True):
            if u32(data, 0x648) != program or u32(data, 0x468) != name:
                raise SnapshotChanged("Track program or name allocation changed")
        result = []
        for index, (data, program, name) in enumerate(zip(tracks, programs, names, strict=True)):
            if u32(data) != self.session.base + self.session.profile["vtables"]["track"]:
                raise SnapshotChanged("Track incarnation changed")
            result.append(
                {
                    "uiIndex": index,
                    "name": name.split(b"\0", 1)[0].decode("utf-8"),
                    "family": self.session.profile["programTypes"].get(
                        str(u32(program) - self.session.base), "unknown"
                    ),
                    "colour": u32(data, 0x618),
                    "transposeSemitones": struct.unpack_from("<i", data, 0x4CC)[0],
                    "velocityScale": struct.unpack_from("<f", data, 0x5EC)[0],
                    "midiOutputChannel": u32(data, 0x3F8),
                    "midiBankEnabled": bool(data[0x30]),
                    "midiBankMsb": data[0x5C],
                    "midiBankLsb": data[0x88],
                    "midiProgramEnabled": bool(data[0xB4]),
                    "midiProgram": data[0xE0],
                }
            )
        return result


def independent(session):
    return TrackSetupReader(session).read()


class TrackSetupLab(TransportLab):
    def request(self, op, value=None, track=None, **fields):
        result = {
            "apiVersion": "0.1",
            "op": op,
            "requestId": "setup_" + uuid.uuid4().hex,
            "target": {
                "instanceId": self.instance,
                "projectId": self.identities["projectId"],
                "trackId": track or self.identities["selectedTrackId"],
            },
            "waitMs": 1000,
            **fields,
        }
        if op in OPERATIONS:
            result["value"] = value
        return result

    def check(self, request, rows, status="applied", error=None):
        reply = self.send(request)
        if reply.get("status") != status or (error and reply.get("error", {}).get("code") != error):
            raise RuntimeError("Unexpected setup receipt: " + json.dumps(reply))
        if status == "applied" and request["op"] in OPERATIONS:
            field = OPERATIONS[request["op"]][0]
            row = next(
                r for r in self.identities["tracks"] if r["id"] == request["target"]["trackId"]
            )
            observed = self.reader.read()[row["index"]][field]
            desired = request["value"]
            if field == "velocityScale":
                desired = struct.unpack("<f", struct.pack("<f", desired))[0]
            if observed != desired or reply["verification"]["requestedValue"] != observed:
                raise RuntimeError("Independent readback differs from receipt")
        else:
            observed = None
        rows.append({"request": request, "receipt": reply, "independentValue": observed})
        return reply


def run(args):
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    report = {"story": 41, "rows": [], "physicalOutputMeasured": False}
    lab = None
    failure = None

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    try:
        lab = TrackSetupLab(session, json.loads(args.attachment.read_text()))
        lab.reader = TrackSetupReader(session)
        report["before"] = lab.reader.read()
        report["transportBefore"] = lab.before
        if lab.before["playing"] or lab.before["recordMode"]:
            raise RuntimeError("Start stopped with recording Off")
        for screen in args.screens:
            lab.renew_authority()
            button, tab, _ = SCREENS[screen]
            lab.ui("/button", name=button)
            if tab is not None:
                lab.ui("/tap", x=tab, y=770)
            for target, before in zip(lab.identities["tracks"], report["before"], strict=True):
                if target["index"] != before["uiIndex"]:
                    raise SnapshotChanged("Track order changed")
                for op, (field, values) in OPERATIONS.items():
                    if op.startswith("set_midi_") and before["family"] != "midi":
                        continue
                    if field != "colour" and before["family"] == "audio":
                        continue
                    for value in [*values, before[field]]:
                        request = lab.request(op, value, track=target["id"])
                        first = lab.check(request, report["rows"])
                        if lab.send(request) != first:
                            raise RuntimeError("Retained replay changed")
                        report["rows"][-1]["screen"] = screen
                        save()
                    noop = lab.check(
                        lab.request(op, before[field], track=target["id"]), report["rows"]
                    )
                    if noop["nativeInvoked"] or noop["changed"]:
                        raise RuntimeError("No-op invoked a setter")
                    wrong = (
                        not before[field]
                        if type(before[field]) is bool
                        else (2 if before[field] != 2 else 1)
                        if field == "velocityScale"
                        else 0
                        if before[field]
                        else 1
                    )
                    lab.check(
                        lab.request(
                            op, before[field], track=target["id"], expected={"value": wrong}
                        ),
                        report["rows"],
                        "rejected",
                        "PRECONDITION_FAILED",
                    )
                    save()
                # Renew before bounded receipt capacity, without discarding any pending work.
                lab.renew_authority()
            print(screen, len(report["rows"]), "rows passed", flush=True)
        report["after"] = independent(session)
        report["transportAfter"] = lab.read()
        report["restored"] = (
            report["after"] == report["before"]
            and report["transportAfter"] == report["transportBefore"]
        )
        if not report["restored"]:
            raise RuntimeError("Track setup baseline was not restored")
    except (Exception, KeyboardInterrupt) as error:
        failure = error
        report["error"] = str(error)
    finally:
        try:
            if lab:
                if failure and "before" in report:
                    current = lab.command("IDENTITIES")
                    if (
                        current["projectId"] != lab.identities["projectId"]
                        or current["tracks"] != lab.identities["tracks"]
                    ):
                        raise SnapshotChanged("Project/topology changed; cleanup refuses writes")
                    lab.renew_authority()
                    lab.ui("/button", name="MAIN")
                    for target, before in zip(
                        lab.identities["tracks"], report["before"], strict=True
                    ):
                        for op, (field, _) in OPERATIONS.items():
                            if op.startswith("set_midi_") and before["family"] != "midi":
                                continue
                            if field != "colour" and before["family"] == "audio":
                                continue
                            lab.check(
                                lab.request(op, before[field], track=target["id"]), report["rows"]
                            )
                        lab.renew_authority()
                    report["restoredAfterFailure"] = independent(session) == report["before"]
                report["closed"] = lab.close()
        except Exception as error:
            report["cleanupError"] = str(error)
            failure = failure or error
        save()
        session.close()
    if failure:
        raise RuntimeError("Setup qualification failed; inspect " + str(args.output)) from failure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--screens", nargs="+", choices=list(SCREENS), default=["main"])
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    run(args)


if __name__ == "__main__":
    main()
