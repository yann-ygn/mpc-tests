#!/usr/bin/env python3
"""US #47 device matrix using an existing authoritative temporary lab attachment.

Run stopped with automation Off, global mute and recognized solo/arm settings.
The operator owns project/settings preparation and the lab's attach/detach lifecycle.
Each row selects its baseline before opening the requested screen; commands use
only native handlers, then restore flags and selection. Never saves a project.
"""

import argparse
import json
import os
from pathlib import Path

from mpc_command_lab import HandlerMatrix
from mpc_command_policy import capture
from mpc_flag_semantics import PROPERTIES, STATE_KEYS
from mpc_live_state import MemorySession, u32
from mpc_native_lab import ROOT
from mpc_track_inventory import capture as inventory

SCREENS = {
    "main": ("MAIN", None, 0x2080),
    "mixer_volume": ("MIX", 107, 0x2083),
    "mixer_pan_volume": ("MIX", 320, 0x2083),
    "mixer_sends": ("MIX", 535, 0x2083),
    "mixer_effects": ("MIX", 748, 0x2083),
    "mixer_io": ("MIX", 959, 0x2083),
    "track_mute": ("TRACK-MUTE", None, 0x208D),
}


class ScreenMatrix(HandlerMatrix):
    def __init__(self, args, session, attachment):
        self.ui_indices = {
            t["index"]: t["uiIndex"]
            for t in inventory(session)["tracks"]
            if t["collection"] == "midi_triggered"
        }
        super().__init__(args, session, attachment)

    def submission(self, index, *args, **kwargs):
        return super().submission(self.ui_indices[index], *args, **kwargs)

    def screen(self):
        button, tab, view = SCREENS[self.args.screen]
        self.ui("/button", name=button)
        if tab is not None:
            self.ui("/tap", x=tab, y=770)
        if self.view() != (view, self.control["commandContext"]["noPopup"]):
            raise RuntimeError("Requested screen did not become the live native view")

    def view(self):
        identity = self.session.profile["identity"]
        p = self.control["commandContext"]
        ptr = lambda owner, offset: u32(self.session.read(owner + offset, 4))
        app = ptr(self.session.base, identity["applicationSlot"])
        window = ptr(app, identity["applicationWindow"])
        component = ptr(window, identity["windowComponent"])
        mode = ptr(component + identity["componentEngine"], p["engineMode"])
        data = self.session.read(mode, p["popupValue"] + 4)
        if u32(data) != self.session.base + p["modeVtable"]:
            raise RuntimeError("Live view owner changed")
        return u32(data, p["modeValue"]), u32(data, p["popupValue"])

    def select(self, index):
        ui_index = self.ui_indices[index]
        offset = self.session.profile["offsets"]["project"]["selectedTrackIndex"]
        current = u32(self.session.read(self.project + offset, 4))
        if current != ui_index:
            self.ui("/button", name="MAIN")
            for _ in range(abs(current - ui_index)):
                self.ui("/tap", x=770 if ui_index > current else 507, y=770)
            if u32(self.session.read(self.project + offset, 4)) != ui_index:
                raise RuntimeError("Canonical Track List selection did not match")
        self.screen()

    def overlap(self, prop, rows):
        first, second = self.args.targets
        before, flags = self.snapshot(), self.independent()
        context = capture(self.session)
        single = context["soloMode" if prop == "solo" else "recordArmMode"] == 0
        row = {"scenario": "overlap", "property": prop, "single": single}
        rows.append(row)
        try:
            for index in (first, second):
                current = self.independent()[index]["requested"][prop]
                if current and not self.set(index, prop, False, True)["ok"]:
                    raise RuntimeError("Cannot prepare the peer-effect baseline")
            row["first"] = self.set(first, prop, True, False)
            row["second"] = self.set(second, prop, True, False)
            if not row["first"]["ok"] or not row["second"]["ok"]:
                raise RuntimeError("Native peer-effect command rejected")
            observed = self.settle(second, prop, True)
            row["independentAfter"] = observed
            if observed[first]["requested"][prop] != (not single):
                raise RuntimeError("Native peer effect disagrees with the recognized setting")
            if observed[first]["audioSide"][prop] != (not single):
                raise RuntimeError("Peer audio readback disagrees with requested state")
            peer = next(
                t for t in self.identities["tracks"] if t["index"] == self.ui_indices[first]
            )
            if single and not any(
                c.get("trackId") == peer["id"] for c in row["second"]["receipt"]["observedChanges"]
            ):
                raise RuntimeError("Single peer change was absent from the retained receipt")
        finally:
            self.restore(flags, before["selectedTrack"]["index"])
            after = self.snapshot()
            row["restored"] = all(after[k] == before[k] for k in STATE_KEYS)
            if not row["restored"]:
                raise RuntimeError("Peer-effect restoration failed")

    def command(self, text):
        # Opening/closing handlers and reads do not navigate either. Check the
        # live screen around every submission, including no-ops and conflicts.
        if text.startswith("LAB_COMMAND_SET "):
            before = self.view()
            result = super().command(text)
            after = self.view()
            if before != after:
                raise RuntimeError("Native command changed the visible screen")
            return result
        return super().command(text)


def run(args):
    attachment = json.loads(args.attachment.read_text())
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    session.ssh[-1:-1] = [
        "-o",
        "ControlMaster=auto",
        "-o",
        "ControlPersist=30",
        "-o",
        "ControlPath=/tmp/mpc-us47-matrix-" + str(os.getpid()),
    ]
    report = {"story": 47, "screen": args.screen, "rows": []}
    matrix, flags, before = None, None, None
    try:
        matrix = ScreenMatrix(args, session, attachment)
        matrix.screen()
        report["context"] = capture(session)
        before, flags = matrix.snapshot(), matrix.independent()
        report["before"] = before
        if report["context"]["playing"] or report["context"]["perSequenceMute"]:
            raise RuntimeError("Stopped/global-mute context is required")
        if (
            report["context"]["recordArmMode"] == 0
            and sum(f["requested"]["arm"] for f in flags) > 1
        ):
            flags = None  # No writes: do not normalize an inconsistent Single baseline.
            raise RuntimeError("Single arm baseline has multiple armed tracks")
        if args.scenario == "overlap":
            if len(args.targets) != 2:
                raise RuntimeError("Peer-effect qualification requires two targets")
            for prop in ("solo", "arm"):
                matrix.overlap(prop, report["rows"])
        else:
            for index in args.targets:
                if before["tracks"][index]["type"] not in ("plugin", "drum"):
                    raise RuntimeError("Only qualified plugin/drum targets may be changed")
                for selected in (True, False):
                    for prop in PROPERTIES:
                        print(f"{args.screen}: {index} {prop} selected={selected}", flush=True)
                        matrix.row(index, prop, selected, report["rows"])
                        args.output.write_text(json.dumps(report, indent=2) + "\n")
    except Exception as error:  # noqa: BLE001 - Retain evidence and attempt every cleanup step.
        report["error"] = str(error)
    finally:
        try:
            if flags is not None:
                matrix.restore(flags, before["selectedTrack"]["index"])
                after = matrix.snapshot()
                report["restored"] = all(after[k] == before[k] for k in STATE_KEYS)
                if not report["restored"]:
                    raise RuntimeError("Full matrix restoration failed")
        except Exception as error:  # noqa: BLE001 - Retain evidence and attempt every cleanup step.
            report["cleanupError"] = str(error)
        try:
            if matrix is not None:
                report["handlersClosed"] = matrix.command("LAB_COMMAND_CLOSE").get("ok") is True
                if not report["handlersClosed"]:
                    raise RuntimeError("Managed handlers have not drained")
        except Exception as error:  # noqa: BLE001 - Retain evidence and attempt every cleanup step.
            report["handlerCleanupError"] = str(error)
        try:
            session.close()
        except Exception as error:  # noqa: BLE001 - Retain evidence even if helper cleanup fails.
            report["probeCleanupError"] = str(error)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--screen", choices=SCREENS, required=True)
    parser.add_argument(
        "--targets", nargs="+", type=int, required=True, help="MIDI collection indices"
    )
    parser.add_argument("--scenario", choices=("matrix", "overlap"), default="matrix")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.live_policy = True
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = run(args)
    print(json.dumps({k: report[k] for k in ("restored", "error", "cleanupError") if k in report}))
    return int(
        any(
            k in report
            for k in ("error", "cleanupError", "handlerCleanupError", "probeCleanupError")
        )
    )


if __name__ == "__main__":
    raise SystemExit(main())
