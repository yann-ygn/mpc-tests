#!/usr/bin/env python3
"""Qualify native family flags through an existing temporary lab attachment.

Uses canonical user order, including audio. The independent reader is diagnostic:
raw engine copies are checked for restoration, but MIDI/CV completion requires
only qualified requested fields. No acoustic, MIDI-output or CV-output claim.
The operator owns project/settings preparation and attach/detach. Each row restores
flags and selection. Automation is temporarily turned Off and restored via the UI.
"""

import argparse
import json
import os
from pathlib import Path
import time
import uuid

from mpc_command_lab import HandlerMatrix
from mpc_flag_semantics import PROPERTIES, STATE_KEYS
from mpc_live_state import MemorySession, u32
from mpc_native_control_audit import listeners
from mpc_native_lab import ROOT
from mpc_screen_flags import SCREENS, ScreenMatrix
from mpc_track_inventory import capture as inventory


class FamilyMatrix(ScreenMatrix):
    def __init__(self, args, session, attachment):
        session.ssh[-1:-1] = [
            "-o",
            "ControlMaster=auto",
            "-o",
            "ControlPersist=60",
            "-o",
            "ControlPath=/tmp/mpc-us49-" + uuid.uuid4().hex[:12],
        ]
        self.metadata = inventory(session)
        qualified = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json").read_text())[
            "supportedProgramFamilies"
        ]
        if any(t["type"] not in qualified for t in self.metadata["tracks"]):
            raise RuntimeError("Prepare a project containing only qualified user-track families")
        HandlerMatrix.__init__(self, args, session, attachment)
        self.ui_indices = {t["uiIndex"]: t["uiIndex"] for t in self.metadata["tracks"]}
        self.identities = self.command("IDENTITIES")
        self.metadata = inventory(session)
        self.baseline = session.snapshot(self.project)
        offsets = session.profile["offsets"]
        self.pool = u32(session.read(self.project + offsets["project"]["trackPool"], 4))
        pool = session.read(self.pool, 0x130)
        self.all_addresses = session.vector(pool, offsets["trackPool"], "all")
        self.master_addresses = session.vector(pool, offsets["trackPool"], "master")
        self.sequence_address = u32(
            session.read(self.project + offsets["project"]["selectedSequence"], 4)
        )
        self.all_programs = [
            u32(d)
            for d in session.read_many(
                [(t + offsets["track"]["program"], 4) for t in self.all_addresses]
            )
        ]
        self.user_addresses = [t for t, _ in self.targets()]
        self.sequence = next(
            s
            for s in self.identities["sequences"]
            if s["id"] == self.identities["selectedSequenceId"]
        )

    def targets(self):
        if self._targets is not None:
            return self._targets
        offsets = self.session.profile["offsets"]
        pool = u32(self.session.read(self.project + offsets["project"]["trackPool"], 4))
        tracks = self.session.vector(self.session.read(pool, 0x130), offsets["trackPool"], "user")
        programs = self.session.read_many([(t + offsets["track"]["program"], 4) for t in tracks])
        targets = [(t, u32(p)) for t, p in zip(tracks, programs)]
        self._targets = targets
        return targets

    def renew(self):
        if self.command("LAB_COMMAND_CLOSE").get("ok") is not True:
            raise RuntimeError("Could not close the completed diagnostic authority")
        self.instance = "run_" + uuid.uuid4().hex
        if self.command("LIVE_COMMAND_OPEN " + self.instance).get("ok") is not True:
            raise RuntimeError("Could not open a fresh diagnostic authority")

    def read_flags(self, addresses, programs):
        blocks = self.session.read_many(
            [(t, 0x650) for t in addresses] + [(p, 0x270) for p in programs]
        )
        rows = []
        for index, (track, program) in enumerate(zip(addresses, programs)):
            td, pd = blocks[index], blocks[len(addresses) + index]
            if u32(td, self.session.profile["offsets"]["track"]["program"]) != program:
                raise RuntimeError("Program association changed during diagnostic capture")
            requested, copies = {}, {"mute": None}
            for prop, binding in zip(PROPERTIES, self.control["bindings"]):
                data = td if binding["propertyOwner"] == "track" else pd
                value, copy = (
                    data[binding[k]] for k in ("scriptValueOffset", "observedValueOffset")
                )
                if value > 1 or copy > 1:
                    raise RuntimeError("Independent field is not boolean")
                requested[prop] = bool(value)
                if prop != "mute":
                    copies[prop] = bool(copy)
            silenced = pd[self.session.profile["offsets"]["program"]["silenced"]]
            if silenced > 1:
                raise RuntimeError("Independent silenced field is not boolean")
            rows.append({"requested": requested, "audioSide": copies, "silenced": bool(silenced)})
        return rows

    def independent(self):
        targets = self.targets()
        return self.read_flags([t for t, _ in targets], [p for _, p in targets])

    def snapshot(self):
        offsets = self.session.profile["offsets"]
        project = self.session.read(self.project, 0xAA8)
        pool = self.session.read(self.pool, 0x130)
        if (
            u32(project, offsets["project"]["trackPool"]) != self.pool
            or self.session.vector(pool, offsets["trackPool"], "all") != self.all_addresses
            or self.session.vector(pool, offsets["trackPool"], "user") != self.user_addresses
        ):
            raise RuntimeError("Inventory changed during the family matrix")
        flags = dict(
            zip(self.all_addresses, self.read_flags(self.all_addresses, self.all_programs))
        )
        selection = u32(project, offsets["project"]["selectedTrackIndex"])
        if u32(project, offsets["project"]["selectedTrack"]) != self.user_addresses[selection]:
            raise RuntimeError("Canonical selection changed during diagnostic capture")
        users = [{**t, **flags[a]} for t, a in zip(self.metadata["tracks"], self.user_addresses)]
        if (
            u32(project, offsets["project"]["selectedSequence"]) != self.sequence_address
            or u32(project, offsets["project"]["selectedSequenceMirror"]) != self.sequence_address
        ):
            raise RuntimeError("Sequence changed during the family flag matrix")
        buses = [{**t, **flags[a]} for t, a in zip(self.metadata["buses"], self.master_addresses)]
        return {
            "selectedTrack": {**self.metadata["tracks"][selection], "index": selection},
            "selectedSequence": self.baseline["selectedSequence"],
            "tracks": users,
            "buses": buses,
            "sequenceSlots": u32(project, offsets["project"]["sequenceCount"]),
        }

    def select(self, index):
        return ScreenMatrix.select(self, index)

    def screen(self):
        return ScreenMatrix.screen(self)

    def view(self):
        return ScreenMatrix.view(self)

    def command(self, text):
        writing = text.startswith(("LAB_COMMAND_SET ", "LAB_COMMAND_SEQUENCE_SET "))
        before = self.view() if writing else None
        result = HandlerMatrix.command(self, text)
        if writing and self.view() != before:
            raise RuntimeError("Native command changed the visible screen")
        return result

    def submission(self, index, prop, value, expected, request_id=None, wait_ms=1000):
        if prop != "mute" or self.args.scope == "global":
            return super().submission(index, prop, value, expected, request_id, wait_ms)
        return self.addressed_submission(index, value, expected, self.sequence, request_id, wait_ms)

    def addressed_submission(self, index, value, expected, sequence, request_id=None, wait_ms=1000):
        track = next(t for t in self.identities["tracks"] if t["index"] == index)
        return (
            f"LAB_COMMAND_SEQUENCE_SET {int(value)} "
            f"{'-' if expected is None else int(expected)} {self.instance} "
            f"{self.identities['projectId']} {track['id']} {request_id or 'family_' + uuid.uuid4().hex} "
            f"1000 {wait_ms} {sequence['id']} {sequence['generation']}"
        )

    def change_sequence(self, delta):
        index = self.baseline["selectedSequence"]["index"]
        self.ui("/button", name="MAIN")
        self.ui("/tap", x=420, y=165)
        self.session.command(
            "cat > /dev/midi_inject", data=bytes((0xB0, 0x64, 1 if delta > 0 else 127))
        )
        time.sleep(0.15)
        self.sequence_address = u32(
            self.session.read(
                self.project + self.session.profile["offsets"]["project"]["selectedSequence"], 4
            )
        )
        self.baseline = self.session.snapshot(self.project)
        if self.baseline["selectedSequence"]["index"] != index + delta:
            raise RuntimeError("Adjacent sequence selection did not match")
        self.identities = self.command("IDENTITIES")
        self.sequence = next(
            s
            for s in self.identities["sequences"]
            if s["id"] == self.identities["selectedSequenceId"]
        )
        self.screen()

    def settle(self, index, prop, desired):
        family = self.snapshot()["tracks"][index]["type"]
        operation = {"mute": "set_mute", "solo": "set_solo", "arm": "set_record_armed"}[prop]
        required = self.control["flagFamilies"][family]["operations"][operation]
        audio_required = any(f.startswith("audioSide.") for f in required)
        until = time.monotonic() + 2
        while True:
            flags = self.independent()
            if flags[index]["requested"][prop] == desired and (
                not audio_required or flags[index]["audioSide"][prop] == desired
            ):
                return flags
            if time.monotonic() >= until:
                raise RuntimeError("Required native observation did not converge")

    def automation(self, index):
        program = self.targets()[index][1]
        return u32(
            self.session.read(program + self.control["commandContext"]["programAutomation"], 4)
        )

    def set_automation(self, index, desired):
        self.select(index)
        for _ in range(4):
            if self.automation(index) == desired:
                return
            if desired == 0 or self.automation(index) == 0:
                self.ui("/button", name="SHIFT", action="down")
                try:
                    self.ui("/button", name="AUTOMATION")
                finally:
                    self.ui("/button", name="SHIFT", action="up")
            else:
                self.ui("/button", name="AUTOMATION")
        raise RuntimeError("UI automation did not reach its requested state")

    def audit(self):
        rows = []
        types = self.snapshot()["tracks"]
        for index, (track, program) in enumerate(self.targets()):
            row = {"family": types[index]["type"], "uiIndex": index, "bindings": []}
            for binding in self.control["bindings"]:
                owner = track if binding["propertyOwner"] == "track" else program
                row["bindings"].append(
                    {
                        "name": binding["name"],
                        "listeners": listeners(self.session, owner, binding["propertyOffset"]),
                    }
                )
            rows.append(row)
        return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument(
        "--targets", type=int, nargs="+", required=True, help="Canonical user indices"
    )
    parser.add_argument("--screen", choices=SCREENS, required=True)
    parser.add_argument("--scope", choices=("global", "sequence"), required=True)
    parser.add_argument("--audit", action="store_true")
    parser.add_argument("--properties", nargs="+", choices=PROPERTIES, default=list(PROPERTIES))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.live_policy = True
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    report = {
        "story": 49,
        "screen": args.screen,
        "scope": args.scope,
        "outputVerified": False,
        "rows": [],
    }
    matrix, flags, before, automation = None, None, None, {}
    try:
        matrix = FamilyMatrix(args, session, json.loads(args.attachment.read_text()))
        if len(set(args.targets)) != len(args.targets) or any(
            index not in matrix.ui_indices for index in args.targets
        ):
            raise RuntimeError("Targets must be unique canonical user indices from this inventory")
        matrix.screen()
        before, flags = matrix.snapshot(), matrix.independent()
        report["before"] = before
        if args.audit:
            report["audit"] = matrix.audit()
        automation = {i: matrix.automation(i) for i in range(len(matrix.targets()))}
        for index in automation:
            matrix.set_automation(index, 0)
        for index in args.targets:
            matrix.renew()
            for selected in (True, False):
                for prop in args.properties:
                    print(f"{args.screen}: {index} {prop} selected={selected}", flush=True)
                    matrix.row(index, prop, selected, report["rows"])
                    args.output.write_text(json.dumps(report, indent=2) + "\n")
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            if matrix is not None and flags is not None:
                matrix.restore(flags, before["selectedTrack"]["index"])
                for index, value in automation.items():
                    matrix.set_automation(index, value)
                matrix.select(before["selectedTrack"]["index"])
                report["after"] = matrix.snapshot()
                report["restored"] = all(report["after"][key] == before[key] for key in STATE_KEYS)
                report["automationRestored"] = all(
                    matrix.automation(i) == v for i, v in automation.items()
                )
                if not report["restored"] or not report["automationRestored"]:
                    raise RuntimeError("Family matrix restoration failed")
            if matrix is not None:
                report["handlersClosed"] = matrix.command("LAB_COMMAND_CLOSE").get("ok") is True
        except Exception as error:
            report["cleanupError"] = str(error)
        session.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {k: report.get(k) for k in ("error", "cleanupError", "restored", "automationRestored")}
        )
    )
    if "error" in report or "cleanupError" in report:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
