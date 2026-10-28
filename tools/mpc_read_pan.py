#!/usr/bin/env python3
"""Reversible US #54 Read pan matrix against an authoritative temporary lab.

Targets must already be in Read, stopped at zero with recording Off. Never
changes automation modes, timeline data, projects, routing or installed addins.
Reports independent model values and scheduled-source replacement separately.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import urllib.request

from mpc_automation_flags import beginning
from mpc_command_policy import capture
from mpc_live_state import MemorySession
from mpc_mixer_controls import MixerMatrix, independent
from mpc_native_lab import ROOT
from mpc_read_writes import sources
from mpc_screen_flags import SCREENS


class PanMatrix(MixerMatrix):
    def pan(self, index):
        return struct.unpack("<f", self.session.read(self.targets()[index][1] + 0x3F8, 4))[0]

    def pan_row(self, index, selected, rows, source):
        original = self.pan(index)
        row = {
            "screen": self.args.screen,
            "playing": self.require_playing,
            "track": index,
            "selected": selected,
            "op": "set_pan",
            "commands": [],
        }
        rows.append(row)
        # Native Mixable automation parameter 10 in the Program namespace is normalized pan.
        populated = [257, 10] in source.get("automationParameters", []) or (257, 10) in source.get(
            "automationParameters", []
        )
        row["populatedPanSource"] = populated
        for value in (0.0, 0.5, 1.0, 0.123456789):
            request = self.request(index, "set_pan", value)
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
            actual = self.pan(index)
            row["commands"].append(result)
            row.setdefault("independentReadback", []).append(actual)
            if actual != expected and not (self.require_playing and populated):
                raise RuntimeError(
                    "Independent pan readback disagreed without an applicable native source"
                )
            if value == 1.0 and self.args.screenshots:
                name = f"{self.args.screen}-{int(self.require_playing)}-{index}-{int(selected)}.png"
                directory = self.args.output.parent / (self.args.output.stem + "-screens")
                directory.mkdir(exist_ok=True)
                with urllib.request.urlopen(
                    f"http://{self.args.host}:6720/screen.png", timeout=10
                ) as response:
                    (directory / name).write_bytes(response.read())
                row["uiEvidence"] = str(directory / name)
            replay = self.send(request)
            if {k: v for k, v in replay.items() if k != "diagnosticRoundTripMs"} != {
                k: v for k, v in result.items() if k != "diagnosticRoundTripMs"
            }:
                raise RuntimeError("Numeric replay changed its retained result")
            if not (self.require_playing and populated):
                noop = self.send(self.request(index, "set_pan", value, expected=value))
                self.require(noop, invoked=False)
                conflict = self.send(self.request(index, "set_pan", value, expected=0.75))
                self.require(conflict, "rejected", "PRECONDITION_FAILED", False)
                row["commands"].extend([noop, conflict])
        restored = self.send(self.request(index, "set_pan", original))
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
    report = {"story": 54, "scope": "global", "rows": []}

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    try:
        args.screen = args.screens[0]
        args.live_policy, args.quick = True, False
        matrix = PanMatrix(args, session, json.loads(args.attachment.read_text()))
        before, flags, mixers = matrix.snapshot(), matrix.independent(), independent(session)
        report.update(before=before, mixersBefore=mixers, contextBefore=capture(session))
        if report["contextBefore"]["playing"] or report["contextBefore"]["recordMode"]:
            raise RuntimeError("Start stopped with recording Off")
        # Pan always has global scope. The inherited flag cleanup uses the
        # captured native mute preference only if a flag needs restoration.
        args.scope = "sequence" if report["contextBefore"]["perSequenceMute"] else "global"
        report["nativeMuteScope"] = args.scope
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
                        if before["tracks"][index]["type"] in (
                            "plugin",
                            "drum",
                            "keygroup",
                            "audio",
                        ):
                            matrix.select(
                                index if selected else (index + 1) % len(matrix.targets())
                            )
                            matrix.pan_row(
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
                    beginning(matrix)
                    matrix.restore(flags, before["selectedTrack"]["index"])
                    for row in mixers:
                        if row["userIndex"] in args.targets and row["family"] in (
                            "plugin",
                            "drum",
                            "keygroup",
                            "audio",
                        ):
                            matrix.require(
                                matrix.send(matrix.request(row["userIndex"], "set_pan", row["pan"]))
                            )
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
        except Exception as error:
            report["cleanupError"] = str(error)
        if matrix:
            try:
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
    parser.add_argument("--screenshots", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.output.resolve().is_relative_to(ROOT / "build"):
        parser.error("Keep private device reports under ignored build/")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
