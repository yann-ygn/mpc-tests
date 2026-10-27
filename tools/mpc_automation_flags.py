#!/usr/bin/env python3
"""Qualify #51 Read commands using an existing temporary native lab.

Requires stopped normal sequence transport, recording Off and targets already in
Read. No automation setting or event data is edited. Tests no-ops, conflicts,
retained outcomes and rejected mute/solo writes, then actual arm writes with an
independent reader. Restores flags, selection, screen and zero playhead.
Private reports must stay in ignored build/. No project save/load or restart.
"""

import argparse
import json
import os
from pathlib import Path

from mpc_command_policy import capture
from mpc_flag_semantics import PROPERTIES, STATE_KEYS
from mpc_live_state import MemorySession
from mpc_native_lab import ROOT
from mpc_playback_flags import PlaybackMatrix
from mpc_screen_flags import ScreenMatrix


def require(receipt, status, error=None, invoked=False):
    if (
        receipt.get("status") != status
        or receipt.get("nativeInvoked") is not invoked
        or (error and receipt.get("error", {}).get("code") != error)
    ):
        raise RuntimeError("Unexpected command outcome: " + json.dumps(receipt))


def beginning(matrix):
    """Restore the qualified zero playhead through the ordinary Locate keypad."""

    def double(x, y):
        matrix.ui("/tap", x=x, y=y)
        matrix.ui("/tap", x=x, y=y)

    matrix.ui("/button", name="MAIN")
    double(851, 65)
    for x, value in ((550, 1), (638, 1), (730, 0)):
        double(x, 300)
        matrix.ui("/tap", x=904 if value else 1040, y=362 if value else 500)
        matrix.ui("/tap", x=1154, y=743)
    matrix.ui("/tap", x=1106, y=76)
    matrix.screen()
    matrix.transport()
    if matrix.positions[-1]["pulse"] != 0:
        raise RuntimeError("Locate did not restore the sequence beginning")


def run(args):
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    matrix, before, flags = None, None, None
    report = {"story": 51, "rows": [], "screen": args.screen, "scope": args.scope}

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    try:
        matrix = PlaybackMatrix(args, session, json.loads(args.attachment.read_text()))
        matrix.screen()
        context = capture(session)
        report["contextBefore"] = context
        if context["playing"] or context["recordMode"] or context["playbackMode"]:
            raise RuntimeError("Start stopped in normal sequence mode with recording Off")
        if bool(context["perSequenceMute"]) != (args.scope == "sequence"):
            raise RuntimeError("Live mute scope must match --scope")
        if any(matrix.automation(i) != 1 for i in args.targets):
            raise RuntimeError("Prepare targets in Read; this runner never changes automation")
        matrix.transport()
        report["initialPulse"] = matrix.positions[-1]["pulse"]
        if report["initialPulse"]:
            raise RuntimeError("Start at the sequence beginning for exact playhead restoration")
        before, flags = matrix.snapshot(), matrix.independent()
        report["before"], report["independentBefore"] = before, flags
        save()
        if args.peers:
            if len(args.targets) != 2:
                raise RuntimeError("Peer qualification requires exactly two targets")
            for playing in (False, True):
                if playing:
                    matrix.ui("/button", name="PLAY")
                    matrix.require_playing = True
                for mode in (0, 1):
                    matrix.modes(context["soloMode"], mode)
                    ScreenMatrix.overlap(matrix, "arm", report["rows"])
                    report["rows"][-1].update(mode=mode, playing=playing)
                    matrix.modes(context["soloMode"], context["recordArmMode"])
                    matrix.restore(flags, before["selectedTrack"]["index"])
                    save()
                if playing:
                    matrix.require_playing = False
                    matrix.ui("/button", name="STOP")
        for playing in () if args.peers else (False, True):
            if playing:
                matrix.ui("/button", name="PLAY")
                matrix.require_playing = True
            for index in args.targets:
                matrix.renew()
                for selected in (True, False):
                    matrix.select(index if selected else (index + 1) % len(matrix.targets()))
                    for prop in PROPERTIES:
                        print(
                            f"Read {args.screen} playing={playing}: {index} {prop} {selected}",
                            flush=True,
                        )
                        current = matrix.independent()[index]["requested"][prop]
                        row = {
                            "index": index,
                            "family": before["tracks"][index]["type"],
                            "selected": selected,
                            "playing": playing,
                            "property": prop,
                        }
                        report["rows"].append(row)
                        text = matrix.submission(index, prop, current, current)
                        row["noop"] = matrix.command(text)
                        require(row["noop"], "applied")
                        if row["noop"]["changed"] is not False:
                            raise RuntimeError("A no-op reported a change")
                        row["retry"] = matrix.command(text)
                        if row["retry"] != row["noop"]:
                            raise RuntimeError("No-op retry changed its retained result")
                        row["conflict"] = matrix.command(
                            matrix.submission(index, prop, current, not current)
                        )
                        require(row["conflict"], "rejected", "PRECONDITION_FAILED")
                        text = matrix.submission(index, prop, not current, current)
                        row["write"] = matrix.command(text)
                        if prop == "arm":
                            require(row["write"], "applied", invoked=True)
                            row["independentDuring"] = matrix.settle(index, prop, not current)
                            row["restore"] = matrix.command(
                                matrix.submission(index, prop, current, not current)
                            )
                            require(row["restore"], "applied", invoked=True)
                            matrix.settle(index, prop, current)
                        else:
                            require(row["write"], "rejected", "AUTOMATION_READ_NOOP_ONLY")
                        if matrix.command(text) != row["write"]:
                            raise RuntimeError("Retry after restoration changed its result")
                        row["independentAfter"] = matrix.independent()
                        row["automationReadPreserved"] = matrix.automation(index) == 1
                        if not row["automationReadPreserved"]:
                            raise RuntimeError("Target automation mode changed")
                        save()
            if playing:
                matrix.require_playing = False
                matrix.ui("/button", name="STOP")
                matrix.ui("/button", name="STOP")
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            if matrix is not None:
                matrix.require_playing = False
                if matrix.transport()["playing"]:
                    matrix.ui("/button", name="STOP")
                    matrix.ui("/button", name="STOP")
                if flags is not None:
                    if args.peers:
                        matrix.modes(context["soloMode"], context["recordArmMode"])
                    matrix.restore(flags, before["selectedTrack"]["index"])
                    beginning(matrix)
                    report["zeroPlayheadRestored"] = True
                    report["after"] = matrix.snapshot()
                    report["restored"] = all(report["after"][k] == before[k] for k in STATE_KEYS)
                    report["contextAfter"] = capture(session)
                    if not report["restored"] or report["contextAfter"] != report["contextBefore"]:
                        raise RuntimeError("State or command context was not restored")
                report["handlersClosed"] = matrix.command("LAB_COMMAND_CLOSE").get("ok") is True
        except Exception as error:
            report["cleanupError"] = str(error)
        session.close()
        save()
    print(json.dumps({k: report.get(k) for k in ("restored", "error", "cleanupError")}))
    return int(bool(report.get("error") or report.get("cleanupError")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--targets", type=int, nargs="+", required=True)
    parser.add_argument(
        "--screen", choices=("main", "mixer_effects", "track_mute"), default="mixer_effects"
    )
    parser.add_argument("--scope", choices=("global", "sequence"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--peers", action="store_true", help="Qualify native Single/Multi arm effects"
    )
    args = parser.parse_args()
    if not args.output.resolve().is_relative_to(ROOT / "build"):
        parser.error("Keep private device reports under ignored build/")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.live_policy = True
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
