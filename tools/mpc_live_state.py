#!/usr/bin/env python3
"""Read experimental live MPC state through /proc/PID/mem, without injection.

Requires a compiled tools/mpc_mem_probe.c and the exact supported firmware build.
Only the temporary helper executable is written to the device. See findings/live-state.md.
"""

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import time
import uuid


class SnapshotChanged(RuntimeError):
    pass


def u32(data, offset=0):
    return struct.unpack_from("<I", data, offset)[0]


class MemorySession:
    def __init__(self, host, binary, profile):
        self.profile = profile
        self.ssh = [
            "ssh",
            "-F",
            "/dev/null",
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=7",
            "-o",
            "StrictHostKeyChecking=yes",
            "-o",
            "UpdateHostKeys=no",
            "root@" + host,
        ]
        pids = self.command("pidof MPC").decode().split()
        if len(pids) != 1 or not pids[0].isdigit():
            raise RuntimeError("Expected exactly one MPC process")
        self.pid = int(pids[0])
        self.refresh_maps()
        offset = int(profile["buildIdFileOffset"])
        expected = bytes.fromhex(profile["buildId"])
        actual = self.command(
            f"dd if=/proc/{self.pid}/exe bs=1 skip={offset} count={len(expected)} 2>/dev/null"
        )
        if actual != expected:
            raise RuntimeError("Unsupported MPC executable build ID; no object offsets were used")
        self.remote = "/tmp/mpc-memory-probe-" + uuid.uuid4().hex
        self.command(
            "cat > " + self.remote + " && chmod 700 " + self.remote, data=binary.read_bytes()
        )

    def command(self, command, data=None, timeout=15):
        result = subprocess.run(
            self.ssh + [command], input=data, capture_output=True, timeout=timeout
        )
        if result.returncode:
            raise RuntimeError(
                "Device command failed: " + result.stderr.decode(errors="replace")[:300]
            )
        return result.stdout

    def refresh_maps(self):
        lines = self.command(f"cat /proc/{self.pid}/maps").decode().splitlines()
        self.maps = []
        bases = []
        for line in lines:
            fields = line.split(maxsplit=5)
            start, end = (int(x, 16) for x in fields[0].split("-"))
            self.maps.append((start, end, fields[1]))
            if len(fields) == 6 and fields[5] == "/usr/bin/MPC" and int(fields[2], 16) == 0:
                bases.append(start)
        if len(bases) != 1:
            raise RuntimeError("Could not determine the MPC load address")
        self.base = bases[0]

    def readable(self, address, length):
        return (
            isinstance(address, int)
            and address > 0
            and length > 0
            and any(
                "r" in flags and start <= address < address + length <= end
                for start, end, flags in self.maps
            )
        )

    def read_many(self, jobs):
        if not jobs:
            return []
        if len(jobs) > 1024 or sum(n for _, n in jobs) > 8 * 1024 * 1024:
            raise RuntimeError("Diagnostic batch limit exceeded")
        for address, length in jobs:
            if not self.readable(address, length):
                raise SnapshotChanged("Object pointer is outside a readable mapping")
        script = "set -e\n" + "\n".join(f"{self.remote} {self.pid} read {a:x} {n}" for a, n in jobs)
        data = self.command("sh", data=script.encode(), timeout=30)
        if len(data) != sum(n for _, n in jobs):
            raise SnapshotChanged("Incomplete process-memory read")
        result, offset = [], 0
        for _, length in jobs:
            result.append(data[offset : offset + length])
            offset += length
        return result

    def read(self, address, length):
        return self.read_many([(address, length)])[0]

    def vector(self, data, offsets, prefix, limit=256):
        begin, end, capacity = (
            u32(data, offsets[prefix + key]) for key in ("Begin", "End", "Capacity")
        )
        if begin == end == capacity == 0:
            return []
        if not (
            begin <= end <= capacity
            and (end - begin) % 4 == 0
            and (capacity - begin) % 4 == 0
            and (capacity - begin) // 4 <= limit
        ):
            raise SnapshotChanged("Invalid track-vector bounds")
        count = (end - begin) // 4
        return list(struct.unpack("<" + "I" * count, self.read(begin, count * 4))) if count else []

    def valid_project(self, data):
        return u32(data) == self.base + self.profile["vtables"]["project"] and all(
            u32(data, int(off)) == self.base + table
            for off, table in self.profile["projectSecondaryVtables"].items()
        )

    def find_project(self):
        target = self.base + self.profile["vtables"]["project"]
        text = self.command(f"{self.remote} {self.pid} scan {target:x}", timeout=45).decode()
        candidates = [int(line.split()[0], 16) for line in text.splitlines()]
        if not candidates:
            raise RuntimeError("No Project object found in the main heap")
        if len(candidates) > 32:
            raise RuntimeError("Too many Project candidates; refusing an ambiguous layout")
        data = self.read_many([(a, 0xAA8) for a in candidates])
        valid = [a for a, d in zip(candidates, data) if self.valid_project(d)]
        if len(valid) != 1:
            raise RuntimeError("Expected one Project object with matching secondary virtual tables")
        return valid[0]

    def active_project(self):
        """Independent diagnostic read of the profiled owner path.

        This external reader is not a lifetime authority or an atomic UI read.
        The adapter re-resolves it on the UI thread and owns destruction watches.
        """
        layout = self.profile["identity"]
        app = u32(self.read(self.base + layout["applicationSlot"], 4))
        if u32(self.read(app, 4)) != self.base + layout["applicationVtable"]:
            raise SnapshotChanged("Active application owner changed")
        window = u32(self.read(app + layout["applicationWindow"], 4))
        if u32(self.read(window, 4)) != self.base + layout["windowVtable"]:
            raise SnapshotChanged("Active window owner changed")
        component = u32(self.read(window + layout["windowComponent"], 4))
        engine = component + layout["componentEngine"]
        if (
            u32(self.read(component, 4)) != self.base + layout["componentVtable"]
            or u32(self.read(engine, 4)) != self.base + layout["engineVtable"]
        ):
            raise SnapshotChanged("Active engine owner changed")
        project = u32(self.read(engine + layout["engineProject"], 4))
        if not self.valid_project(self.read(project, 0xAA8)):
            raise SnapshotChanged("Active project owner changed")
        return project

    def snapshot(self, project):
        for _ in range(3):
            try:
                return self._snapshot(project)
            except SnapshotChanged:
                self.refresh_maps()
        raise RuntimeError("Live state changed during all snapshot attempts")

    def _snapshot(self, project):
        started = time.monotonic()
        profile = self.profile
        po, to, so, pro, poolo = (
            profile["offsets"][k] for k in ("project", "track", "sequence", "program", "trackPool")
        )
        p = self.read(project, 0xAA8)
        if not self.valid_project(p):
            raise SnapshotChanged("Project object is no longer valid")
        current_track = u32(p, po["selectedTrack"])
        current_sequence = u32(p, po["selectedSequence"])
        if current_track != u32(p, po["selectedTrackMirror"]) or current_sequence != u32(
            p, po["selectedSequenceMirror"]
        ):
            raise SnapshotChanged("Selection update is in progress")
        pool_address = u32(p, po["trackPool"])
        pool = self.read(pool_address, 0x130)
        if u32(pool) != self.base + profile["vtables"]["trackPool"]:
            raise SnapshotChanged("TrackPool virtual table does not match")
        all_tracks = self.vector(pool, poolo, "all")
        normal_tracks = self.vector(pool, poolo, "normal")
        if len(set(all_tracks)) != len(all_tracks) or not set(normal_tracks) <= set(all_tracks):
            raise SnapshotChanged("Track vectors are inconsistent")

        count, capacity = u32(p, po["sequenceCount"]), u32(p, po["sequenceCapacity"])
        if not (0 < count <= capacity <= 2048):
            raise SnapshotChanged("Invalid sequence-array bounds")
        sequence_slots = list(
            struct.unpack("<" + "I" * count, self.read(u32(p, po["sequenceArray"]), count * 4))
        )
        if current_sequence not in sequence_slots:
            raise SnapshotChanged("Current sequence is outside the sequence array")
        sequence = self.read(current_sequence, 0xC8)
        if u32(sequence) != self.base + profile["vtables"]["sequence"]:
            raise SnapshotChanged("Sequence virtual table does not match")
        bpm = struct.unpack_from("<f", sequence, so["bpm"])[0]
        if not math.isfinite(bpm) or not 1 <= bpm <= 1000:
            raise SnapshotChanged("Invalid sequence tempo")

        addresses = list(all_tracks)
        if current_track not in addresses:
            addresses.append(current_track)
        track_data = self.read_many([(a, 0x64C) for a in addresses])
        for data in track_data:
            if u32(data) != self.base + profile["vtables"]["track"]:
                raise SnapshotChanged("Track virtual table does not match")
        programs = [u32(d, to["program"]) for d in track_data]
        program_data = self.read_many([(a, 0x270) for a in programs])
        string_pointers = [u32(d, to["name"]) for d in track_data] + [u32(sequence, so["name"])]
        strings = self.read_many([(a, 256) for a in string_pointers])
        names = []
        for raw in strings:
            if b"\0" not in raw:
                raise SnapshotChanged("Object name exceeds the diagnostic string limit")
            try:
                names.append(raw.split(b"\0", 1)[0].decode("utf-8"))
            except UnicodeDecodeError:
                raise SnapshotChanged("Object name is not valid UTF-8") from None

        rows = {}
        for address, data, pd, name in zip(addresses, track_data, program_data, names):
            kind = profile["programTypes"].get(str(u32(pd) - self.base))
            if kind is None:
                raise SnapshotChanged("Program class is outside the supported build profile")
            values = [data[to["recordArm"]], pd[pro["mute"]], pd[pro["solo"]], pd[pro["silenced"]]]
            if any(v not in (0, 1) for v in values):
                raise SnapshotChanged("Track flags are outside the expected boolean range")
            rows[address] = dict(
                name=name,
                type=kind,
                recordArmed=bool(values[0]),
                muted=bool(values[1]),
                soloed=bool(values[2]),
                silenced=bool(values[3]),
            )

        selected = dict(rows[current_track])
        selected["index"] = (
            normal_tracks.index(current_track) if current_track in normal_tracks else None
        )
        if selected["index"] is not None and selected["index"] != u32(p, po["selectedTrackIndex"]):
            raise SnapshotChanged("Selected track index and pointer disagree")
        after_p, after_pool, after_sequence = self.read_many(
            [(project, 0xAA8), (pool_address, 0x50), (current_sequence, 0xC8)]
        )
        checked = [
            po[k]
            for k in (
                "selectedTrack",
                "selectedTrackMirror",
                "selectedSequence",
                "selectedSequenceMirror",
                "selectedTrackIndex",
                "trackPool",
                "sequenceArray",
                "sequenceCapacity",
                "sequenceCount",
            )
        ]
        if (
            not self.valid_project(after_p)
            or any(u32(p, off) != u32(after_p, off) for off in checked)
            or pool[:0x50] != after_pool
            or u32(sequence, so["name"]) != u32(after_sequence, so["name"])
            or sequence[so["bpm"] : so["bpm"] + 4] != after_sequence[so["bpm"] : so["bpm"] + 4]
        ):
            raise SnapshotChanged("Selection or topology changed during the snapshot")
        return {
            "source": "process-memory",
            "experimental": True,
            "firmware": {"version": profile["mpcVersion"], "buildId": profile["buildId"]},
            "capturedAt": datetime.now(timezone.utc).isoformat(),
            "consistency": {
                "selectionChecked": True,
                "atomic": False,
                "readSeconds": round(time.monotonic() - started, 3),
            },
            "selectedTrack": selected,
            "selectedSequence": {
                "index": sequence_slots.index(current_sequence),
                "name": names[-1],
                "bpm": bpm,
            },
            "sequenceSlots": count,
            "tracks": [dict(index=i, **rows[a]) for i, a in enumerate(normal_tracks)],
            "buses": [rows[a] for a in all_tracks if a not in normal_tracks],
        }

    def close(self):
        self.command("rm -f " + self.remote)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--binary", type=Path, default=root / "builddir/arm/tools/mem_probe")
    parser.add_argument(
        "--profile", type=Path, default=root / "tools/profiles/key37-mpc3.9.1.2.json"
    )
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.binary.is_file():
        parser.error("Compile tools/mpc_mem_probe.c first; see findings/live-state.md")
    session = MemorySession(args.host, args.binary, json.loads(args.profile.read_text()))
    try:
        state = session.snapshot(session.find_project())
    finally:
        session.close()
    text = json.dumps(state, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
