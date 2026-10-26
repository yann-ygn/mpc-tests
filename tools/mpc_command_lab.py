#!/usr/bin/env python3
"""Bounded #19 handler qualification through the private diagnostic host.

Run stopped in Main with an existing disposable plugin/drum project. The operator
owns current-mode/settings qualification for the default manual lab policy.
--live-policy uses #32's production native callback and checks eligibility at dispatch.
Manual mute tests require --global-mute-confirmed after checking the CURRENT UI setting. Every row
restores requested/audio peer flags and selection; no project save/load/restart.
"""

import argparse
import json
import os
from pathlib import Path
import uuid

from mpc_flag_semantics import Matrix, PROPERTIES, STATE_KEYS
from mpc_live_state import MemorySession
from mpc_native_lab import ROOT, detach, experiment


class HandlerMatrix(Matrix):
    def __init__(self, args, session, attachment):
        self.args, self.session, self.attachment = args, session, attachment
        self.project = session.active_project()
        self._targets = None
        self.control = json.loads(
            (ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json").read_text()
        )
        self.identities = attachment["identities"]
        self.instance = "run_" + uuid.uuid4().hex
        opening = (
            "LIVE_COMMAND_OPEN " if getattr(args, "live_policy", False) else "LAB_COMMAND_OPEN "
        )
        if self.command(opening + self.instance).get("ok") is not True:
            raise RuntimeError("Could not open the managed lab command authority")

    def command(self, text):
        remote = self.attachment["remoteDirectory"]
        return json.loads(
            self.session.command(
                f"{remote}/native_client {self.session.pid}", data=(text + "\n").encode()
            )
        )

    def submission(self, index, prop, value, expected, request_id=None, wait_ms=1000):
        obj = next(t for t in self.identities["tracks"] if t["index"] == index)
        return (
            f"LAB_COMMAND_SET {prop} {int(value)} "
            f"{'-' if expected is None else int(expected)} {self.instance} "
            f"{self.identities['projectId']} {obj['id']} {request_id or 'req_' + uuid.uuid4().hex} "
            f"1000 {wait_ms}"
        )

    def set(self, index, prop, value, expected):
        receipt = self.command(self.submission(index, prop, value, expected))
        # Matrix.restore uses its established diagnostic success/error convention.
        return {
            "ok": receipt.get("status") == "applied",
            "receipt": receipt,
            "error": receipt.get("error"),
        }

    def row(self, index, prop, selected, rows):
        self.select(index if selected else (index + 1) % len(self.targets()))
        before, flags = self.snapshot(), self.independent()
        original = flags[index]["requested"][prop]
        row = {
            "index": index,
            "family": before["tracks"][index]["type"],
            "property": prop,
            "selected": selected,
        }
        rows.append(row)
        try:
            submission = self.submission(index, prop, not original, original)
            row["applied"] = self.command(submission)
            if row["applied"].get("status") != "applied":
                raise RuntimeError("Command did not apply: " + json.dumps(row["applied"]))
            row["independentDuring"] = self.settle(index, prop, not original)
            row["replay"] = self.command(submission)
            if row["replay"] != row["applied"]:
                raise RuntimeError("An identical retry did not return the retained receipt")
            row["noop"] = self.command(self.submission(index, prop, not original, not original))
            assert row["noop"]["status"] == "applied" and row["noop"]["changed"] is False
            assert row["noop"]["nativeInvoked"] is False
            row["conflict"] = self.command(self.submission(index, prop, not original, original))
            assert row["conflict"]["status"] == "rejected"
            assert row["conflict"]["error"]["code"] == "PRECONDITION_FAILED"
            assert row["conflict"]["nativeInvoked"] is False
            during = self.snapshot()
            if during["selectedTrack"]["index"] != before["selectedTrack"]["index"]:
                assert any(
                    c["path"] == "selectedTrackId" for c in row["applied"]["observedChanges"]
                )
        finally:
            self.restore(flags, before["selectedTrack"]["index"])
            after = self.snapshot()
            row["restored"] = all(after[key] == before[key] for key in STATE_KEYS)
            if not row["restored"]:
                raise RuntimeError("Full row restoration failed")
        # Retrying after restoration must not reapply the old desired value.
        row["replayAfterRestore"] = self.command(submission)
        assert row["replayAfterRestore"] == row["applied"]
        assert self.independent() == flags


def run(args):
    if "mute" in args.properties and not args.global_mute_confirmed and not args.live_policy:
        raise RuntimeError(
            "Mute qualification requires the current UI's per-sequence mute to be disabled"
        )
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    report = {
        "story": 32 if args.live_policy else 19,
        "context": args.context,
        "rows": [],
        "qualification": "native_live_policy" if args.live_policy else "bounded_lab_proof",
    }
    attachment_path = args.output.with_suffix(".attachment.json")
    attachment, matrix, flags, initial = None, None, None, None
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    try:
        attachment = experiment(
            argparse.Namespace(host=args.host, identities=True, pulse=None, keep=True)
        )
        attachment_path.write_text(json.dumps(attachment, indent=2) + "\n")
        if "error" in attachment or "cleanupError" in attachment:
            raise RuntimeError("Attachment failed: " + json.dumps(attachment))
        matrix = HandlerMatrix(args, session, attachment)
        initial, flags = matrix.snapshot(), matrix.independent()
        if len(initial["tracks"]) < 2 or any(
            t["type"] not in ("plugin", "drum") for t in initial["tracks"]
        ):
            raise RuntimeError("Need existing qualified plugin/drum tracks")
        report["before"] = initial
        for index in range(len(initial["tracks"])):
            for selected in (True, False):
                for prop in args.properties:
                    print(f"handler: {prop}, track {index}, selected={selected}", flush=True)
                    matrix.row(index, prop, selected, report["rows"])
                    args.output.write_text(json.dumps(report, indent=2) + "\n")
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            if matrix is not None and flags is not None:
                matrix.restore(flags, initial["selectedTrack"]["index"])
                report["after"] = matrix.snapshot()
                report["restored"] = all(report["after"][key] == initial[key] for key in STATE_KEYS)
                if not report["restored"]:
                    raise RuntimeError("Overall restoration failed")
            if matrix is not None:
                report["handlersClosed"] = matrix.command("LAB_COMMAND_CLOSE").get("ok") is True
            if attachment and attachment.get("bridgeRetained"):
                report["detach"] = detach(
                    argparse.Namespace(host=args.host, detach=attachment_path)
                )
        except Exception as error:
            report["cleanupError"] = str(error)
        session.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--context", required=True)
    parser.add_argument(
        "--properties", nargs="+", choices=list(PROPERTIES), default=["solo", "arm"]
    )
    parser.add_argument("--global-mute-confirmed", action="store_true")
    parser.add_argument(
        "--live-policy",
        action="store_true",
        help="Use the production native qualification callback",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = run(args)
    print(
        json.dumps(
            {
                key: report[key]
                for key in ("restored", "handlersClosed", "error", "cleanupError")
                if key in report
            }
        )
    )
    return int("error" in report or "cleanupError" in report)


if __name__ == "__main__":
    raise SystemExit(main())
