#!/usr/bin/env python3
"""US #48 bounded sequence-mute qualification through a retained native lab.

Use existing adjacent sequences, stopped transport and automation Off. The operator
owns settings preparation and lab attach/detach. Each row restores both sequences,
all flags and selection. No project save/load, implicit sequence switch in a command,
or setting write is performed. Raw reports belong under ignored build/.
"""

import argparse
import json
import os
from pathlib import Path
import traceback
import uuid

from mpc_command_policy import capture
from mpc_flag_semantics import STATE_KEYS
from mpc_live_state import MemorySession
from mpc_native_lab import ROOT
from mpc_screen_flags import SCREENS, ScreenMatrix


class SequenceMatrix(ScreenMatrix):
    def __init__(self, args, session, attachment):
        super().__init__(args, session, attachment)
        self.identities = self.identities_now()

    def identities_now(self):
        result = self.command("IDENTITIES")
        if not result.get("ok"):
            raise RuntimeError("Authoritative identities unavailable")
        return result

    def submission(self, index, prop, value, expected, request_id=None, wait_ms=1000):
        if prop != "mute":
            return super().submission(index, prop, value, expected, request_id, wait_ms)
        identities = self.identities_now()
        sequence = next(
            s for s in identities["sequences"] if s["id"] == identities["selectedSequenceId"]
        )
        return self.addressed_submission(index, value, expected, sequence, request_id, wait_ms)

    def addressed_submission(self, index, value, expected, sequence, request_id=None, wait_ms=1000):
        track = next(t for t in self.identities["tracks"] if t["index"] == self.ui_indices[index])
        return (
            f"LAB_COMMAND_SEQUENCE_SET {int(value)} "
            f"{'-' if expected is None else int(expected)} {self.instance} "
            f"{self.identities['projectId']} {track['id']} {request_id or 'seq_' + uuid.uuid4().hex} "
            f"1000 {wait_ms} {sequence['id']} {sequence['generation']}"
        )

    def row(self, index, selected, rows):
        self.select(index if selected else (index + 1) % len(self.targets()))
        before, flags = self.snapshot(), self.independent()
        identities = self.identities_now()
        sequence = next(
            s for s in identities["sequences"] if s["id"] == identities["selectedSequenceId"]
        )
        row = {
            "index": index,
            "family": before["tracks"][index]["type"],
            "selected": selected,
            "screen": self.args.screen,
            "before": before,
        }
        rows.append(row)
        other_before = None
        try:
            self.change_sequence(1)
            other_before = self.snapshot()
            other_flags = self.independent()
            self.change_sequence(-1)
            self.screen()
            original = flags[index]["requested"]["mute"]
            text = self.addressed_submission(index, not original, original, sequence)
            row["applied"] = self.command(text)
            if row["applied"].get("status") != "applied":
                raise RuntimeError("Sequence write failed: " + json.dumps(row["applied"]))
            assert row["applied"]["muteScope"] == "sequence"
            assert (
                row["applied"]["verification"]["muteContext"]["sequence"]["sequenceId"]
                == sequence["id"]
            )
            row["independentDuring"] = self.settle(index, "mute", not original)
            row["noop"] = self.command(
                self.addressed_submission(index, not original, not original, sequence)
            )
            assert row["noop"]["status"] == "applied" and not row["noop"]["nativeInvoked"]
            row["conflict"] = self.command(
                self.addressed_submission(index, not original, original, sequence)
            )
            assert row["conflict"]["error"]["code"] == "PRECONDITION_FAILED"
            assert not row["conflict"]["nativeInvoked"]
            row["stale"] = self.command(
                self.addressed_submission(
                    index, original, None, {**sequence, "generation": "18446744073709551615"}
                )
            )
            assert (
                row["stale"]["error"]["code"] == "STALE_SEQUENCE"
                and not row["stale"]["nativeInvoked"]
            )
            self.change_sequence(1)
            self.screen()
            row["otherDuring"] = self.snapshot()
            assert self.independent() == other_flags
            assert all(row["otherDuring"][k] == other_before[k] for k in STATE_KEYS)
            row["differentPatterns"] = other_flags[index]["requested"]["mute"] != (not original)
            row["inactive"] = self.command(
                self.addressed_submission(index, original, None, sequence)
            )
            assert row["inactive"]["error"]["code"] == "SEQUENCE_NOT_ACTIVE"
            assert (
                not row["inactive"]["nativeInvoked"]
                and row["inactive"]["execution"] == "not_started"
            )
            row["replayWhileInactive"] = self.command(text)
            assert row["replayWhileInactive"] == row["applied"]
            assert self.independent() == other_flags
            self.change_sequence(-1)
            self.screen()
            row["returned"] = self.settle(index, "mute", not original)
            row["replayReturned"] = self.command(text)
            assert row["replayReturned"] == row["applied"]
            self.select(index)
            self.ui("/button", name="MUTE")
            row["manualEdit"] = self.settle(index, "mute", original)
            row["manualConflict"] = self.command(
                self.addressed_submission(index, original, not original, sequence)
            )
            assert row["manualConflict"]["error"]["code"] == "PRECONDITION_FAILED"
            assert not row["manualConflict"]["nativeInvoked"]
            assert self.command(text) == row["applied"]
            # Discard the response, then retrieve/replay the retained no-op.
            lost_id = "lost_" + uuid.uuid4().hex
            lost_text = self.addressed_submission(index, original, original, sequence, lost_id)
            remote = self.attachment["remoteDirectory"]
            self.session.command(
                f"{remote}/native_client {self.session.pid} > /dev/null",
                data=(lost_text + "\n").encode(),
            )
            row["lostResponse"] = self.command(f"LAB_COMMAND_GET {self.instance} {lost_id} 1000")
            assert (
                row["lostResponse"]["status"] == "applied"
                and not row["lostResponse"]["nativeInvoked"]
            )
            assert self.command(lost_text) == row["lostResponse"]
            # Observation waits never cancel or retarget accepted work.
            request_id = "disconnect_" + uuid.uuid4().hex
            row["accepted"] = self.command(
                self.addressed_submission(index, original, original, sequence, request_id, 0)
            )
            row["retained"] = self.command(f"LAB_COMMAND_GET {self.instance} {request_id} 1000")
            assert row["retained"]["status"] == "applied" and not row["retained"]["nativeInvoked"]
        finally:
            current = self.snapshot()["selectedSequence"]["index"]
            initial = before["selectedSequence"]["index"]
            if current == initial + 1:
                self.change_sequence(-1)
            elif current != initial:
                raise RuntimeError("Unexpected sequence: restoration requires operator review")
            self.screen()
            self.restore(flags, before["selectedTrack"]["index"])
            row["restored"] = all(self.snapshot()[k] == before[k] for k in STATE_KEYS)
            if other_before is not None:
                self.change_sequence(1)
                row["otherRestored"] = all(
                    self.snapshot()[k] == other_before[k] for k in STATE_KEYS
                )
                self.change_sequence(-1)
                self.screen()
            assert row["restored"] and row.get("otherRestored", True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--targets", type=int, nargs="+", required=True)
    parser.add_argument("--screen", choices=SCREENS, default="main")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.live_policy = True
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    attachment = json.loads(args.attachment.read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    session.ssh[-1:-1] = [
        "-o",
        "ControlMaster=auto",
        "-o",
        "ControlPersist=30",
        "-o",
        "ControlPath=/tmp/mpc-us48-" + str(os.getpid()),
    ]
    matrix = None
    report = {"story": 48, "screen": args.screen, "rows": []}
    try:
        matrix = SequenceMatrix(args, session, attachment)
        matrix.screen()
        report["context"] = capture(session)
        if report["context"]["playing"] or not report["context"]["perSequenceMute"]:
            raise RuntimeError("Stopped/per-sequence-mute context required")
        for index in args.targets:
            if matrix.snapshot()["tracks"][index]["type"] not in ("plugin", "drum"):
                raise RuntimeError("Only qualified plugin/drum targets may be changed")
            for selected in (True, False):
                print(f"{args.screen}: {index} mute selected={selected}", flush=True)
                matrix.row(index, selected, report["rows"])
                args.output.write_text(json.dumps(report, indent=2) + "\n")
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        report["traceback"] = traceback.format_exc()
    finally:
        try:
            if matrix:
                report["commandsClosed"] = matrix.command("LAB_COMMAND_CLOSE").get("ok")
        finally:
            session.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    if "error" in report:
        raise SystemExit(report["error"])
    print("All rows restored both sequences, peer flags and selection")


if __name__ == "__main__":
    main()
