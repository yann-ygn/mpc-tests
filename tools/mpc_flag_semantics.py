#!/usr/bin/env python3
"""US #31 device matrix for the private native lab, never a public command service.

Save a disposable project under MPC Documents/Projects/Claude Experiments first.
Run while stopped in Main. Select settings/sequence in the UI between runs and
give each context a name. Each row changes one flag, repeats its desired value,
checks a conflicting precondition, restores flags and selection, then verifies
the whole snapshot. No project loads, setting changes or saves are automated.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import time
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET

from mpc_live_state import MemorySession, u32
from mpc_native_lab import ROOT, detach, experiment

PROPERTIES = {"mute": "muted", "solo": "soloed", "arm": "recordArmed"}
STATE_KEYS = ("selectedTrack", "selectedSequence", "tracks", "buses", "sequenceSlots")


class Matrix:
    def __init__(self, args, session, attachment):
        self.args, self.session, self.attachment = args, session, attachment
        # Reuse a short-lived SSH connection for the independent diagnostic reads.
        # A short unique /tmp path also fits OpenSSH's temporary socket suffix.
        session.ssh[-1:-1] = [
            "-o",
            "ControlMaster=auto",
            "-o",
            "ControlPersist=60",
            "-o",
            "ControlPath=/tmp/mpc-us31-" + uuid.uuid4().hex,
        ]
        self.project = session.find_project()
        self._targets = None
        self.control = json.loads(
            (ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json").read_text()
        )

    def snapshot(self):
        return self.session.snapshot(self.project)

    def targets(self):
        profile = self.session.profile["offsets"]
        project = self.session.read(self.project, 0xAA8)
        pool = self.session.read(u32(project, profile["project"]["trackPool"]), 0x130)
        tracks = self.session.vector(pool, profile["trackPool"], "normal")
        targets = [
            (track, u32(self.session.read(track, 0x64C), profile["track"]["program"]))
            for track in tracks
        ]
        if self._targets is not None and targets != self._targets:
            raise RuntimeError("Track topology changed; lab identities are no longer usable")
        self._targets = targets
        return targets

    def independent(self):
        """Separate requested booleans from the audio copies in the old snapshot reader."""
        targets = self.targets()
        jobs = []
        for track, program in targets:
            for binding in self.control["bindings"]:
                owner = track if binding["propertyOwner"] == "track" else program
                jobs.extend(
                    (owner + binding[k], 1) for k in ("scriptValueOffset", "observedValueOffset")
                )
        values = [v[0] for v in self.session.read_many(jobs)]
        if any(v not in (0, 1) for v in values):
            raise RuntimeError("Independent flag read was not boolean")
        return [
            {
                "requested": dict(zip(PROPERTIES, map(bool, values[i : i + 6 : 2]))),
                "audioSide": {
                    "mute": None,
                    "solo": bool(values[i + 3]),
                    "arm": bool(values[i + 5]),
                },
            }
            for i in range(0, len(values), 6)
        ]

    def command(self, text):
        remote = self.attachment["remoteDirectory"]
        reply = json.loads(
            self.session.command(
                f"{remote}/native_client {self.session.pid}", data=(text + "\n").encode()
            )
        )
        if reply.get("tid") != self.session.pid:
            raise RuntimeError("Lab request did not reach the verified UI thread")
        return reply

    def set(self, index, prop, value, expected):
        track, program = self.targets()[index]
        return self.command(
            f"SET {prop} {int(value)} {int(expected)} {self.project:x} {track:x} {program:x}"
        )

    def ui(self, path, **query):
        url = f"http://{self.args.host}:6720{path}?" + urllib.parse.urlencode(query)
        with urllib.request.urlopen(url, timeout=10) as response:
            response.read()
        time.sleep(0.15)

    def select(self, index):
        current = self.snapshot()["selectedTrack"]["index"]
        if current is None or abs(current - index) > 127:
            raise RuntimeError("Cannot restore selection through Main's track selector")
        if current != index:
            self.ui("/button", name="MAIN")
            for _ in range(abs(current - index)):
                self.ui("/tap", x=770 if index > current else 507, y=770)
            if self.snapshot()["selectedTrack"]["index"] != index:
                raise RuntimeError("Track selection did not match Main's selector")

    def settle(self, index, prop, desired):
        until = time.monotonic() + 2
        while True:
            state = self.independent()
            target = state[index]
            if target["requested"][prop] == desired and (
                prop == "mute" or target["audioSide"][prop] == desired
            ):
                return state
            if time.monotonic() >= until:
                raise RuntimeError("Independent requested/audio readback did not converge")

    def restore(self, before, selection):
        # Restore peer flags too: native arm/solo listeners can change other tracks.
        # This experiment requires an idle instrument and restores its full flag baseline.
        self.select(selection)
        current_flags = self.independent()
        for index, original in enumerate(before):
            for prop in PROPERTIES:
                current = current_flags[index]["requested"][prop]
                desired = original["requested"][prop]
                if current != desired:
                    reply = self.set(index, prop, desired, current)
                    if not reply["ok"]:
                        raise RuntimeError("Restoration rejected: " + json.dumps(reply))
                    current_flags = self.independent()
        self.select(selection)
        if self.independent() != before:
            raise RuntimeError("Requested/audio flags differ after restoration")

    def row(self, index, prop, selected, rows):
        self.select(index if selected else (index + 1) % len(self.targets()))
        before, flags = self.snapshot(), self.independent()
        old = flags[index]["requested"][prop]
        row = {
            "targetIndex": index,
            "family": before["tracks"][index]["type"],
            "property": prop,
            "selected": selected,
            "before": before,
            "independentBefore": flags,
        }
        rows.append(row)
        try:
            row["set"] = self.set(index, prop, not old, old)
            if not row["set"]["ok"]:
                row["constraint"] = row["set"]["error"]
                if row["constraint"] != "VALUE_REJECTED":
                    raise RuntimeError("Unexpected native failure: " + row["constraint"])
                row["independentDuring"] = self.independent()
            else:
                row["independentDuring"] = self.settle(index, prop, not old)
                row["repeat"] = self.set(index, prop, not old, not old)
                if (
                    not row["repeat"]["ok"]
                    or row["repeat"]["nativeInvoked"]
                    or row["repeat"]["changed"]
                    or not row["repeat"]["changedKnown"]
                    or not row["repeat"]["completed"]
                ):
                    raise RuntimeError("Already-desired request invoked a native setter")
                row["conflict"] = self.set(index, prop, not old, old)
                if (
                    row["conflict"]["error"] != "PRECONDITION_FAILED"
                    or row["conflict"]["nativeInvoked"]
                ):
                    raise RuntimeError(
                        "Conflicting precondition was not rejected before native invocation"
                    )
            row["during"] = self.snapshot()
        finally:
            try:
                self.restore(flags, before["selectedTrack"]["index"])
            finally:
                row["after"] = self.snapshot()
                row["restored"] = all(row["after"][k] == before[k] for k in STATE_KEYS)
            if not row["restored"]:
                raise RuntimeError("Full state differs after row restoration")

    def pulse(self, index, prop, manual, rows):
        """A transport disconnect does not turn the lab pulse into a rollback guarantee."""
        self.select(index)
        before, flags = self.snapshot(), self.independent()
        original = flags[index]["requested"][prop]
        track, program = self.targets()[index]
        remote = self.attachment["remoteDirectory"]
        row = {
            "scenario": "manual-edit" if manual else "disconnect",
            "property": prop,
            "before": before,
            "phases": [],
        }
        rows.append(row)
        process = subprocess.Popen(
            self.session.ssh + [f"{remote}/native_client {self.session.pid}"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            process.stdin.write(
                f"PULSE {prop} {int(not original)} {int(original)} 6000 "
                f"{self.project:x} {track:x} {program:x}\n"
            )
            process.stdin.close()
            for line in process.stdout:
                phase = json.loads(line)
                row["phases"].append(phase)
                if phase["phase"] == "applied":
                    row["during"] = self.snapshot()
                    row["independentDuring"] = self.settle(index, prop, not original)
                    if manual:
                        self.ui(
                            "/button", name={"mute": "MUTE", "solo": "SOLO", "arm": "REC-ARM"}[prop]
                        )
                        row["manualReadback"] = self.settle(index, prop, original)
                    else:
                        # Close the relay while the pulse is active, with no
                        # application completion response delivered to this client.
                        process.terminate()
                        process.wait(timeout=5)
                        row["clientDisconnectedAfterApplication"] = True
                        time.sleep(6.5)
                        break
            if manual:
                process.wait(timeout=15)
                restores = [p for p in row["phases"] if p["phase"] == "restoreAccepted"]
                if len(restores) != 1 or restores[0]["error"] != "PRECONDITION_FAILED":
                    raise RuntimeError(
                        "Pulse restoration did not reject the intervening manual edit"
                    )
            self.settle(index, prop, original)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
                time.sleep(6.5)
            self.restore(flags, before["selectedTrack"]["index"])
            row["after"] = self.snapshot()
            row["restored"] = all(row["after"][k] == before[k] for k in STATE_KEYS)
            if not row["restored"]:
                raise RuntimeError("Pulse scenario did not restore full state")

    def change_sequence(self, delta):
        index = self.snapshot()["selectedSequence"]["index"]
        self.ui("/button", name="MAIN")
        self.ui("/tap", x=420, y=165)
        self.session.command(
            "cat > /dev/midi_inject", data=bytes((0xB0, 0x64, 1 if delta > 0 else 127))
        )
        time.sleep(0.15)
        if self.snapshot()["selectedSequence"]["index"] != index + delta:
            raise RuntimeError("Main sequence selector did not move to the adjacent sequence")

    def overlap(self, prop, rows):
        """Enable two tracks in succession to observe native single/multi peers."""
        before, flags = self.snapshot(), self.independent()
        row = {"scenario": "overlap", "property": prop, "before": before}
        rows.append(row)
        try:
            for index in (0, 1):
                current = self.independent()[index]["requested"][prop]
                if current and not self.set(index, prop, False, True)["ok"]:
                    raise RuntimeError("Cannot prepare the two-track overlap test")
            row["first"] = self.set(0, prop, True, False)
            row["afterFirst"] = self.snapshot()
            row["independentFirst"] = self.independent()
            row["second"] = self.set(1, prop, True, False)
            row["afterSecond"] = self.snapshot()
            row["independentSecond"] = self.independent()
            if not row["first"]["ok"] or not row["second"]["ok"]:
                raise RuntimeError("Overlap write rejected; inspect native constraints")
        finally:
            self.restore(flags, before["selectedTrack"]["index"])
            row["after"] = self.snapshot()
            row["restored"] = all(row["after"][k] == before[k] for k in STATE_KEYS)
            if not row["restored"]:
                raise RuntimeError("Overlap experiment did not restore full state")

    def sequence_scope(self, index, prop, rows):
        """Observe both sequences before writing and verify both after restoring."""
        before, flags = self.snapshot(), self.independent()
        initial_sequence = before["selectedSequence"]["index"]
        if initial_sequence != 0:
            raise RuntimeError("Sequence-scope experiment starts in sequence 1")
        row = {
            "scenario": "sequence-scope",
            "targetIndex": index,
            "property": prop,
            "before": before,
        }
        rows.append(row)
        try:
            self.change_sequence(1)
            row["otherBefore"] = self.snapshot()
            self.change_sequence(-1)
            original = flags[index]["requested"][prop]
            row["set"] = self.set(index, prop, not original, original)
            if not row["set"]["ok"]:
                raise RuntimeError("Sequence-scope write failed")
            row["independentDuring"] = self.settle(index, prop, not original)
            self.change_sequence(1)
            row["otherDuring"] = self.snapshot()
            row["independentOther"] = self.independent()
            self.change_sequence(-1)
            row["returned"] = self.snapshot()
            row["independentReturned"] = self.independent()
        finally:
            current = self.snapshot()["selectedSequence"]["index"]
            if current == 1:
                self.change_sequence(-1)
            elif current != initial_sequence:
                raise RuntimeError("Unexpected sequence during restoration")
            self.restore(flags, before["selectedTrack"]["index"])
            if "otherBefore" in row:
                self.change_sequence(1)
                row["otherAfter"] = self.snapshot()
                self.change_sequence(-1)
                if any(row["otherBefore"][k] != row["otherAfter"][k] for k in STATE_KEYS):
                    raise RuntimeError("The adjacent sequence was not restored")
            row["after"] = self.snapshot()
            row["restored"] = all(row["after"][k] == before[k] for k in STATE_KEYS)
            if not row["restored"]:
                raise RuntimeError("Sequence-scope experiment did not restore full state")


def run(args):
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    report = {
        "story": 31,
        "context": args.context,
        "buildId": profile["buildId"],
        "rows": [],
        "startedAt": datetime.now(timezone.utc).isoformat(),
    }
    attachment_path = args.output.with_suffix(".attachment.json")
    attachment, matrix, initial_flags = None, None, None
    with_session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    try:
        current = ET.fromstring(with_session.command("cat /data/Settings/MPC/CurrentProject"))
        path = next((e.get("val", "") for e in current if e.get("name") == "lastProjectPath"), "")
        if not path.startswith("/storage/MPC Documents/Projects/Claude Experiments/"):
            raise RuntimeError("Save a disposable project in Claude Experiments before running")
        report["projectPath"] = path
        settings = ET.fromstring(with_session.command("cat /data/Settings/MPC/MPC.settings"))
        report["settings"] = {
            e.get("name"): e.get("val")
            for e in settings
            if e.get("name", "").startswith("TrackMute.")
            or e.get("name") in ("recArmMode", "TrackMixerSoloMode")
        }
        attachment = experiment(argparse.Namespace(host=args.host, pulse=None, keep=True))
        attachment_path.write_text(json.dumps(attachment, indent=2) + "\n")
        if "error" in attachment or "cleanupError" in attachment:
            raise RuntimeError("Attachment failed: " + json.dumps(attachment))
        matrix = Matrix(args, with_session, attachment)
        report["before"] = matrix.snapshot()
        initial_flags = matrix.independent()
        if (
            report["settings"].get("recArmMode") == "0"
            and sum(f["requested"]["arm"] for f in initial_flags) > 1
        ):
            initial_flags = None  # No writes occurred; do not normalize it implicitly.
            raise RuntimeError(
                "Single record-arm baseline still has multiple armed tracks; select another track and back in Main first"
            )
        if len(matrix.targets()) < 2:
            raise RuntimeError("Need two existing plugin/drum tracks for selection tests")
        if args.scenario == "matrix":
            for index, track in enumerate(report["before"]["tracks"]):
                if track["type"] not in ("plugin", "drum"):
                    continue
                for selected in (True, False):
                    for prop in args.properties:
                        print(
                            f"{args.context}: track {index}, {prop}, selected={selected}",
                            flush=True,
                        )
                        matrix.row(index, prop, selected, report["rows"])
                        args.output.write_text(json.dumps(report, indent=2) + "\n")
        elif args.scenario == "overlap":
            for prop in args.properties:
                if prop not in ("solo", "arm"):
                    raise RuntimeError("Overlap is for solo/arm only; use --properties solo arm")
                print(f"{args.context}: overlap, {prop}", flush=True)
                matrix.overlap(prop, report["rows"])
        elif args.scenario == "sequence-scope":
            for index, track in enumerate(report["before"]["tracks"]):
                if track["type"] in ("plugin", "drum"):
                    for prop in args.properties:
                        print(f"{args.context}: sequence-scope, track {index}, {prop}", flush=True)
                        matrix.sequence_scope(index, prop, report["rows"])
        else:
            index = report["before"]["selectedTrack"]["index"]
            for prop in args.properties:
                print(f"{args.context}: {args.scenario}, {prop}", flush=True)
                matrix.pulse(index, prop, args.scenario == "manual-edit", report["rows"])
        matrix.select(report["before"]["selectedTrack"]["index"])
        report["after"] = matrix.snapshot()
        report["restored"] = all(report["before"][k] == report["after"][k] for k in STATE_KEYS)
        if not report["restored"]:
            raise RuntimeError("Matrix did not restore its starting state")
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            if matrix and initial_flags is not None:
                matrix.restore(initial_flags, report["before"]["selectedTrack"]["index"])
        except Exception as error:
            report["restorationError"] = str(error)
        try:
            if attachment and attachment.get("bridgeRetained"):
                cleanup = detach(argparse.Namespace(host=args.host, detach=attachment_path))
                report["cleanup"] = {
                    k: cleanup[k] for k in ("bridgeUnloaded", "temporaryFilesRemoved")
                }
        except Exception as error:
            report["cleanupError"] = str(error)
        try:
            with_session.close()
        except Exception as error:
            report["probeCleanupError"] = str(error)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument(
        "--context", required=True, help="UI mode/settings/sequence label for this run"
    )
    parser.add_argument("--properties", nargs="+", choices=PROPERTIES)
    parser.add_argument(
        "--scenario",
        choices=("matrix", "disconnect", "manual-edit", "sequence-scope", "overlap"),
        default="matrix",
    )
    parser.add_argument(
        "--output", type=Path, required=True, help="Private report under ignored build/"
    )
    args = parser.parse_args()
    args.properties = args.properties or (
        ["solo", "arm"] if args.scenario == "overlap" else list(PROPERTIES)
    )
    if args.scenario == "overlap" and "mute" in args.properties:
        parser.error("Overlap tests use --properties solo arm")
    if not args.output.resolve().is_relative_to(ROOT / "build"):
        parser.error("Keep private addresses and captures under ignored build/")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = run(args)
    print(
        json.dumps(
            {
                k: report[k]
                for k in (
                    "context",
                    "restored",
                    "error",
                    "cleanupError",
                    "restorationError",
                    "probeCleanupError",
                )
                if k in report
            }
        )
    )
    return int(
        any(k in report for k in ("error", "cleanupError", "restorationError", "probeCleanupError"))
    )


if __name__ == "__main__":
    raise SystemExit(main())
