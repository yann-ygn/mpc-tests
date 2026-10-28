#!/usr/bin/env python3
"""Qualify US #40 through a retained lab in a disposable experiment project.

Uses the production policy and managed receipts. Does not save/load projects,
install addins, restart MPC, or record. Raw diagnostic reports belong in build/.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import time
import uuid

from mpc_live_state import MemorySession, SnapshotChanged, u32
from mpc_native_lab import ROOT
from mpc_screen_flags import SCREENS


class TransportLab:
    def __init__(self, session, attachment):
        self.session = session
        self.attachment = attachment
        self.instance = "run_" + uuid.uuid4().hex
        self.identities = self.command("IDENTITIES")
        self.project = session.active_project()
        p = session.profile["identity"]
        ptr = lambda a: u32(session.read(a, 4))
        app = ptr(session.base + p["applicationSlot"])
        engine = (
            ptr(ptr(app + p["applicationWindow"]) + p["windowComponent"]) + p["componentEngine"]
        )
        self.sequencer = ptr(ptr(engine + 0x2E4) + 0x3BC)
        self.before = self.read()
        self.sequence = next(
            row
            for row in self.identities["sequences"]
            if row["id"] == self.identities["selectedSequenceId"]
        )

        if not self.command("LIVE_COMMAND_OPEN " + self.instance).get("ok"):
            raise RuntimeError("Could not open live command authority")

    def renew_authority(self):
        if not self.close().get("ok"):
            raise RuntimeError("Command authority did not close")
        self.instance = "run_" + uuid.uuid4().hex
        if not self.command("LIVE_COMMAND_OPEN " + self.instance).get("ok"):
            raise RuntimeError("Could not renew command authority")

    def command(self, text):
        remote = self.attachment["remoteDirectory"]
        return json.loads(
            self.session.command(
                f"{remote}/native_client {self.session.pid}", data=(text + "\n").encode()
            )
        )

    def ui(self, path, **params):
        return self.session.command(
            "wget -qO /dev/null 'http://127.0.0.1:6720"
            + path
            + "?"
            + "&".join(f"{k}={v}" for k, v in params.items())
            + "'"
        )

    def read(self):
        """Read-only observations from a separate process, never write targets."""
        s = self.session
        if s.active_project() != self.project:
            raise SnapshotChanged("Project changed")
        d, pd = s.read_many([(self.sequencer, 0x410), (self.project, 0x168)])
        if u32(d, 0x1E4) != self.project:
            raise SnapshotChanged("Sequencer owner changed")
        timeline, recorder, sequence = u32(d, 0x400), u32(d, 0x3EC), u32(pd, 0xE4)
        props = u32(s.read(recorder + 0x98, 4))
        td, rd, sd, master = s.read_many(
            [
                (timeline + 0xA8, 0x70),
                (props + 0x528, 4),
                (sequence, 0x260),
                (u32(d, 0x1E8), 0x64),
            ]
        )
        if u32(pd, 0xE4) != u32(pd, 0xF4):
            raise SnapshotChanged("Sequence mirror changed")
        return {
            "playing": bool(td[0x114 - 0xA8]),
            "recordMode": u32(rd),
            "pulse": struct.unpack_from("<q", td)[0],
            "countInEnabled": bool(d[0x3E8]),
            "trackIndex": u32(pd, 0x15C),
            "sequenceIndex": u32(sd, 0x250),
            "sequenceBpm": struct.unpack_from("<f", sd, 0xC4)[0],
            "masterBpm": struct.unpack_from("<f", master, 0x58)[0],
            "masterTempoEnabled": bool(master[0x2C]),
        }

    def request(self, op, sequence=None, track=None, **fields):
        target = {"instanceId": self.instance, "projectId": self.identities["projectId"]}
        result = {
            "apiVersion": "0.1",
            "op": op,
            "requestId": "nav_" + uuid.uuid4().hex,
            "target": target,
            "waitMs": 1000,
            **fields,
        }
        if op == "select_track":
            target["trackId"] = track
        else:
            row = sequence or self.sequence
            result["sequence"] = {
                **target,
                "sequenceId": row["id"],
                "generation": row["generation"],
            }
        return result

    def send(self, request):
        return self.command("LAB_COMMAND_JSON " + json.dumps(request, separators=(",", ":")))

    def require(self, request, rows, status="applied", error=None):
        result = self.send(request)
        row = {"request": request, "receipt": result, "independent": self.read()}
        rows.append(row)
        if result.get("status") != status or (
            error and result.get("error", {}).get("code") != error
        ):
            raise RuntimeError("Unexpected navigation receipt: " + json.dumps(result))
        if status == "applied":
            state = row["independent"]
            op = request["op"]
            if op in ("play", "play_start", "stop") and state["playing"] != (op != "stop"):
                raise RuntimeError("Playback differs from independent readback")
            if op == "locate" and state["pulse"] != int(request["pulse"]):
                raise RuntimeError("Playhead differs from independent readback")
            if op == "select_track" and state["trackIndex"] != next(
                row["index"]
                for row in self.identities["tracks"]
                if row["id"] == request["target"]["trackId"]
            ):
                raise RuntimeError("Track selection differs from independent readback")
            if op == "select_sequence" and state["sequenceIndex"] != next(
                row["index"]
                for row in self.identities["sequences"]
                if row["id"] == request["sequence"]["sequenceId"]
            ):
                raise RuntimeError("Sequence selection differs from independent readback")
            if (
                op == "set_sequence_bpm"
                and state["sequenceBpm"]
                != struct.unpack("<f", struct.pack("<f", request["bpm"]))[0]
            ):
                raise RuntimeError("Tempo differs from independent readback")
        return result

    def close(self):
        return self.command("LAB_COMMAND_CLOSE")


def run(args):
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    report = {"story": 40, "rows": [], "audibleOutputMeasured": False}
    lab = None
    failure = None

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    try:
        lab = TransportLab(session, json.loads(args.attachment.read_text()))
        report["before"] = lab.before
        save()
        if lab.before["playing"] or lab.before["recordMode"]:
            raise RuntimeError("Start with stopped playback and recording Off")
        for screen in args.screens:
            lab.renew_authority()
            button, tab, _ = SCREENS[screen]
            lab.ui("/button", name=button)
            if tab is not None:
                lab.ui("/tap", x=tab, y=770)
            for op, fields in [
                ("stop", {}),
                ("locate", {"pulse": "960"}),
                ("play", {}),
                ("play", {}),
                ("stop", {}),
                ("play_start", {}),
                ("stop", {}),
                ("locate", {"pulse": "0"}),
                ("set_sequence_bpm", {"bpm": 123.456}),
                ("set_sequence_bpm", {"bpm": lab.before["sequenceBpm"]}),
            ]:
                lab.require(lab.request(op, **fields), report["rows"])
                report["rows"][-1]["screen"] = screen
                print(screen, op, report["rows"][-1]["receipt"]["status"], flush=True)
                save()
            for target in lab.identities["tracks"]:
                lab.require(lab.request("select_track", track=target["id"]), report["rows"])
                report["rows"][-1]["screen"] = screen
                save()
            lab.require(
                lab.request("select_track", track=lab.identities["selectedTrackId"]), report["rows"]
            )
            for index in args.sequence_indices:
                target = next(row for row in lab.identities["sequences"] if row["index"] == index)
                for sequence in (target, lab.sequence):
                    lab.require(lab.request("select_sequence", sequence=sequence), report["rows"])
                    report["rows"][-1]["screen"] = screen
                    save()
        report["after"] = lab.read()
    except Exception as error:
        report["error"] = str(error)
        failure = error
    finally:
        try:
            if lab:
                report["processUnchanged"] = session.command("pidof MPC").decode().split() == [
                    str(session.pid)
                ]
                if report["processUnchanged"]:
                    lab.renew_authority()
                    current = lab.command("IDENTITIES")
                    if current["projectId"] != lab.identities["projectId"]:
                        raise SnapshotChanged("Project replaced; cleanup refuses writes")
                    sequence = next(
                        row
                        for row in current["sequences"]
                        if row["id"] == current["selectedSequenceId"]
                    )
                    # Each cleanup step has a fresh request ID. A retry would
                    # return the retained receipt instead of restoring state.
                    lab.require(lab.request("stop", sequence=sequence), report["rows"])
                    lab.require(lab.request("select_sequence"), report["rows"])
                    for op, fields in [
                        ("locate", {"pulse": str(lab.before["pulse"])}),
                        ("set_sequence_bpm", {"bpm": lab.before["sequenceBpm"]}),
                    ]:
                        lab.require(lab.request(op, **fields), report["rows"])
                    lab.require(
                        lab.request("select_track", track=lab.identities["selectedTrackId"]),
                        report["rows"],
                    )
                    report["restored"] = lab.read() == lab.before
                    button, tab, _ = SCREENS[args.restore_screen]
                    lab.ui("/button", name=button)
                    if tab is not None:
                        lab.ui("/tap", x=tab, y=770)
                    report["closed"] = lab.close()
                else:
                    report["restored"] = False
        except Exception as error:
            report["cleanupError"] = str(error)
            failure = failure or error
        finally:
            save()
            session.close()
    if failure:
        raise RuntimeError("Qualification failed; inspect " + str(args.output)) from failure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--screens", nargs="+", choices=list(SCREENS), default=["main"])
    parser.add_argument("--restore-screen", choices=list(SCREENS), default="mixer_volume")
    parser.add_argument("--sequence-indices", nargs="*", type=int, default=[])
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    run(args)


if __name__ == "__main__":
    main()
