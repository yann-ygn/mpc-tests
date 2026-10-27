#!/usr/bin/env python3
"""Bounded playback qualification through an existing temporary native lab.

Requires normal sequence mode, recording Off and an already looping sequence.
The operator owns project preparation and attach/detach. This runner starts and
stops ordinary playback through the controller, never recording or saving. It
restores flags, selection and target automation, including on failure. Reports
contain private diagnostic state and belong under ignored build/.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import struct
import time

from mpc_command_policy import capture
from mpc_command_lab import HandlerMatrix
from mpc_family_flags import FamilyMatrix
from mpc_flag_semantics import PROPERTIES, STATE_KEYS
from mpc_live_state import MemorySession, u32
from mpc_native_lab import ROOT
from mpc_screen_flags import SCREENS, ScreenMatrix


class PlaybackMatrix(FamilyMatrix):
    def __init__(self, args, session, attachment):
        self.require_playing = False
        super().__init__(args, session, attachment)
        p = self.control["commandContext"]
        identity = session.profile["identity"]
        ptr = lambda owner, offset: u32(session.read(owner + offset, 4))
        app = ptr(session.base, identity["applicationSlot"])
        component = ptr(ptr(app, identity["applicationWindow"]), identity["windowComponent"])
        engine = component + identity["componentEngine"]
        self.sequencer = ptr(ptr(engine, p["engineAudioThread"]), p["audioThreadSequencer"])
        self.timeline = ptr(self.sequencer, p["sequencerTimeline"])
        recorder = ptr(self.sequencer, p["sequencerRecorder"])
        self.record_properties = ptr(recorder, p["recorderProperties"])
        self.positions = []

    def transport(self):
        p = self.control["commandContext"]
        data = self.session.read_many(
            [
                (self.timeline + p["timelinePlaying"], 1),
                (self.record_properties + p["recordModeValue"], 4),
                (self.sequencer + p["sequencerMode"], 4),
                (self.sequencer + p["sequencerSongSource"], 4),
                # Sequencer::PlayheadPosition reads the timeline's signed pulse pair.
                (self.timeline + 0xA8, 8),
            ]
        )
        self.positions.append({"at": time.monotonic(), "pulse": struct.unpack("<q", data[4])[0]})
        return {
            "playing": data[0][0],
            "recordMode": u32(data[1]),
            "playbackMode": u32(data[2]),
            "songSourceActive": bool(u32(data[3])),
        }

    def assert_playback(self):
        observed = self.transport()
        if observed != {
            "playing": 1,
            "recordMode": 0,
            "playbackMode": 0,
            "songSourceActive": False,
        }:
            raise RuntimeError("Ordinary playback was lost: " + json.dumps(observed))

    def command(self, text):
        writing = text.startswith(("LAB_COMMAND_SET ", "LAB_COMMAND_SEQUENCE_SET "))
        if writing and self.require_playing:
            self.assert_playback()
        reply = super().command(text)
        if writing and self.require_playing:
            self.assert_playback()
        return reply

    def resources(self):
        # Process-wide CPU ticks and RSS are observations, not causal attribution.
        return self.session.command(
            f"cat /proc/{self.session.pid}/stat; "
            f"cat /proc/{self.session.pid}/status; "
            "for f in /proc/asound/card*/pcm*p/sub*/status; do "
            'if test -f "$f"; then cat "$f"; fi; done'
        ).decode()

    def modes(self, solo, arm):
        if arm == 0:
            armed = [i for i, row in enumerate(self.independent()) if row["requested"]["arm"]]
            if len(armed) == 1:
                self.select(armed[0])
        self.ui("/button", name="MENU")
        self.ui("/tap", x=1090, y=720)
        self.ui("/tap", x=150, y=485)
        context = capture(self.session)
        for key, desired, y in (("recordArmMode", arm, 500), ("soloMode", solo, 360)):
            if context[key] != desired:
                self.ui("/tap", x=930, y=y)
                self.session.command(
                    "cat > /dev/midi_inject",
                    data=bytes(
                        (
                            0xB0,
                            0x64,
                            1 if desired > context[key] else 127,
                        )
                    ),
                )
                context = capture(self.session)
        if context["recordArmMode"] != arm or context["soloMode"] != solo:
            raise RuntimeError("Live peer settings did not match")
        self.screen()

    def pressure(self, rows):
        self.renew()
        flags = self.independent()
        texts = [
            self.submission(
                self.args.targets[i % len(self.args.targets)],
                "mute",
                bool(i % 2),
                None,
                "pressure_" + str(i),
                wait_ms=0,
            )
            for i in range(32)
        ]

        def send(text):
            started = time.monotonic()
            reply = HandlerMatrix.command(self, text)
            return {"receipt": reply, "roundTripMs": (time.monotonic() - started) * 1000}

        self.assert_playback()
        with ThreadPoolExecutor(max_workers=8) as clients:
            replies = list(clients.map(send, texts))
        for i, row in enumerate(replies):
            receipt = row["receipt"]
            if receipt.get("status") == "accepted" or (
                receipt.get("status") == "unknown"
                and receipt.get("timing", {}).get("totalMs") is None
            ):
                for _ in range(100):
                    receipt = self.command(f"LAB_COMMAND_GET {self.instance} pressure_{i} 1000")
                    if receipt.get("status") != "accepted" and (
                        receipt.get("status") != "unknown"
                        or receipt.get("timing", {}).get("totalMs") is not None
                    ):
                        break
                row["terminal"] = receipt
            if (
                receipt.get("status") not in ("applied", "cancelled", "unknown")
                and receipt.get("error") != "QUEUE_FULL"
            ):
                raise RuntimeError("Unexpected pressure outcome: " + json.dumps(receipt))
            if receipt.get("status") in ("applied", "cancelled", "unknown"):
                row["replay"] = self.command(texts[i])
                if row["replay"] != receipt:
                    raise RuntimeError("Pressure retry did not retain its outcome")
            rows.append(row)
        self.assert_playback()
        self.restore(flags, self.snapshot()["selectedTrack"]["index"])


def run(args):
    session = MemorySession(
        args.host,
        ROOT / "builddir/arm/tools/mem_probe",
        json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text()),
    )
    report = {
        "story": 50,
        "screen": args.screen,
        "scope": args.scope,
        "rows": [],
        "audibleOutputMeasured": False,
        "underrunCounterAvailable": False,
    }
    matrix, flags, before, automation = None, None, None, {}
    started_playback = False
    try:
        matrix = PlaybackMatrix(args, session, json.loads(args.attachment.read_text()))
        report["contextBefore"] = capture(session)
        if matrix.transport() != {
            "playing": 0,
            "recordMode": 0,
            "playbackMode": 0,
            "songSourceActive": False,
        }:
            raise RuntimeError("Prepare stopped normal sequence mode with recording Off")
        if bool(report["contextBefore"]["perSequenceMute"]) != (args.scope == "sequence"):
            raise RuntimeError("Prepare a live mute preference matching --scope")
        before, flags = matrix.snapshot(), matrix.independent()
        if len(set(args.targets)) != len(args.targets) or any(
            i not in matrix.ui_indices for i in args.targets
        ):
            raise RuntimeError("Targets must be unique canonical indices from this inventory")
        report["before"] = before
        automation = {i: matrix.automation(i) for i in range(len(matrix.targets()))}
        report["automationBefore"] = automation
        # Keep the recovery baseline durable before the first UI mutation.
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        for index in automation:
            matrix.set_automation(index, 0)
        matrix.screen()
        started_playback = True
        matrix.ui("/button", name="PLAY")
        matrix.require_playing = True
        matrix.assert_playback()
        report["contextPlaying"] = capture(session)
        report["resourcesBefore"] = matrix.resources()
        report["startedAtMonotonic"] = time.monotonic()
        if args.scenario == "matrix":
            for index in args.targets:
                matrix.renew()
                for selected in (True, False):
                    for prop in args.properties:
                        print(
                            f"playback {args.screen} {args.scope}: {index} {prop} selected={selected}",
                            flush=True,
                        )
                        matrix.row(index, prop, selected, report["rows"])
                        args.output.write_text(json.dumps(report, indent=2) + "\n")
        elif args.scenario == "peers":
            targets = list(args.targets)
            for mode in (0, 1):
                matrix.modes(mode, mode)
                for i in range(0, len(targets), 2):
                    pair = targets[i : i + 2]
                    if len(pair) != 2:
                        raise RuntimeError("Peer targets must be pairs")
                    args.targets = pair
                    matrix.renew()
                    for prop in ("solo", "arm"):
                        ScreenMatrix.overlap(matrix, prop, report["rows"])
                        report["rows"][-1].update(pair=pair, mode=mode)
            matrix.modes(
                report["contextBefore"]["soloMode"], report["contextBefore"]["recordArmMode"]
            )
        else:
            matrix.pressure(report["rows"])
        report["finishedAtMonotonic"] = time.monotonic()
        report["resourcesAfter"] = matrix.resources()
        report["positions"] = matrix.positions
        report["observedLoopWraps"] = sum(
            b["pulse"] < a["pulse"] for a, b in zip(matrix.positions, matrix.positions[1:])
        )
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            if matrix is not None:
                matrix.require_playing = False
                if started_playback:
                    matrix.ui("/button", name="STOP")
                if flags is not None:
                    if args.scenario == "peers":
                        matrix.modes(
                            report["contextBefore"]["soloMode"],
                            report["contextBefore"]["recordArmMode"],
                        )
                    matrix.restore(flags, before["selectedTrack"]["index"])
                    for index, value in automation.items():
                        matrix.set_automation(index, value)
                    matrix.select(before["selectedTrack"]["index"])
                    report["after"] = matrix.snapshot()
                    report["restored"] = all(report["after"][k] == before[k] for k in STATE_KEYS)
                    report["automationRestored"] = all(
                        matrix.automation(i) == v for i, v in automation.items()
                    )
                    report["transportStopped"] = matrix.transport()["playing"] == 0
                    if not all(
                        report[k] for k in ("restored", "automationRestored", "transportStopped")
                    ):
                        raise RuntimeError("Playback matrix restoration failed")
                report["handlersClosed"] = matrix.command("LAB_COMMAND_CLOSE").get("ok") is True
        except Exception as error:
            report["cleanupError"] = str(error)
        try:
            session.close()
        except Exception as error:
            report["probeCleanupError"] = str(error)
            report.setdefault("cleanupError", "Memory probe cleanup failed; see probeCleanupError")
        finally:
            args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                k: report.get(k)
                for k in (
                    "error",
                    "cleanupError",
                    "restored",
                    "automationRestored",
                    "observedLoopWraps",
                )
            }
        )
    )
    return int("error" in report or "cleanupError" in report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--targets", type=int, nargs="+", required=True)
    parser.add_argument("--screen", choices=SCREENS, required=True)
    parser.add_argument("--scope", choices=("global", "sequence"), required=True)
    parser.add_argument("--properties", choices=PROPERTIES, nargs="+", default=list(PROPERTIES))
    parser.add_argument("--scenario", choices=("matrix", "peers", "pressure"), default="matrix")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.live_policy = True
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.resolve().is_relative_to(ROOT / "build"):
        parser.error("Keep diagnostic captures under ignored build/")
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
