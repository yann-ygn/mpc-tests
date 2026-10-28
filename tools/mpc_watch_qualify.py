#!/usr/bin/env python3
"""Qualify the C Unix watch service through an SSH Unix-socket forward.

Runs on the workstation, never on the instrument. Uses a temporary production
host loaded by the checked ARM lab loader. Raw reports stay under ignored build/.
Does not load/save projects, change automation, install or restart MPC. API flag
writes attempt restoration in finally and record any restoration failure. Use a
fresh temporary API instance per workload to preserve receipt capacity. Operator owns UI/manual
cases, playback start/stop, exact selection/playhead restoration and lab cleanup.
"""

import argparse
import copy
import json
import math
from pathlib import Path
import socket
import struct
import threading
import time
import uuid

import jsonschema
from referencing import Registry, Resource

from mpc_live_state import MemorySession
from mpc_track_inventory import capture as inventory
from mpc_mixer_controls import independent
from mpc_command_policy import capture as context

ROOT = Path(__file__).resolve().parents[1]
SCHEMAS = ROOT / "native-api/schema"


def distribution(values):
    values = sorted(values)
    return {
        "count": len(values),
        **{
            label: values[max(0, math.ceil(len(values) * fraction) - 1)] if values else None
            for label, fraction in (("p50", 0.50), ("p95", 0.95), ("p99", 0.99), ("max", 1))
        },
    }


def call(path, value):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(5)
        client.connect(str(path))
        client.sendall(value if isinstance(value, bytes) else json.dumps(value).encode() + b"\n")
        with client.makefile("rb") as reader:
            return json.loads(reader.readline(1048577))


class Stream:
    def __init__(self, path, cursor=None, baseline=None, consume=True, defer_validation=False):
        self.client = socket.socket(socket.AF_UNIX)
        self.client.settimeout(5)
        self.client.connect(str(path))
        self.reader = self.client.makefile("rb")
        self.frames, self.errors = [], []
        self.model = copy.deepcopy(baseline)
        self.cursor = cursor
        self.condition = threading.Condition()
        self.closed = False
        self.eof = False
        self.defer_validation = defer_validation
        request = {"apiVersion": "0.1", "op": "watch"}
        if cursor:
            request["cursor"] = cursor
        self.client.sendall(json.dumps(request).encode() + b"\n")
        registry = Registry().with_resources(
            (p.name, Resource.from_contents(json.loads(p.read_text())))
            for p in SCHEMAS.glob("*.schema.json")
        )
        self.validator = jsonschema.Draft202012Validator(
            json.loads((SCHEMAS / "watch-response.schema.json").read_text()), registry=registry
        )
        self.worker = threading.Thread(target=self.read, daemon=True) if consume else None
        if self.worker:
            self.worker.start()

    def read(self):
        try:
            while not self.closed:
                line = self.reader.readline(1048577)
                if not line:
                    break
                if not line.endswith(b"\n"):
                    raise RuntimeError("Incomplete frame")
                received = time.monotonic_ns()
                frame = json.loads(line)
                if "error" in frame:
                    raise RuntimeError(str(frame["error"]))
                if not self.defer_validation:
                    self.validator.validate(frame)
                with self.condition:
                    kind = frame["type"]
                    if kind in ("snapshot", "change"):
                        if self.model is not None and frame["previousRevision"] is not None:
                            assert frame["previousRevision"] == self.model["revision"], (
                                "Revision gap"
                            )
                        if kind == "snapshot":
                            self.model = copy.deepcopy(frame["state"])
                        elif self.model is not None:
                            tracks = {t["ref"]["trackId"]: t for t in frame["tracks"]}
                            self.model["tracks"] = [
                                tracks.get(t["ref"]["trackId"], t) for t in self.model["tracks"]
                            ]
                            self.model["selectedTrackId"] = frame["selectedTrackId"]
                            self.model["muteContext"] = frame["muteContext"]
                            self.model["revision"] = frame["revision"]
                        self.cursor = frame["cursor"]
                    self.frames.append({"atNs": received, "frame": frame})
                    self.condition.notify_all()
        except Exception as error:
            if not self.closed:
                self.errors.append(getattr(error, "message", str(error))[:600])
        finally:
            with self.condition:
                self.eof = True
                self.condition.notify_all()

    def wait(self, predicate, timeout=5):
        until = time.monotonic() + timeout
        with self.condition:
            while True:
                if self.errors:
                    raise RuntimeError(str(self.errors))
                for row in self.frames:
                    if predicate(row):
                        return row
                remaining = until - time.monotonic()
                if self.eof or remaining <= 0:
                    raise RuntimeError(
                        "Watch delivery ended/timed out: "
                        + str([r["frame"]["type"] for r in self.frames[-5:]])
                    )
                self.condition.wait(remaining)

    def state(self):
        with self.condition:
            return copy.deepcopy(self.model)

    def checkpoint(self):
        with self.condition:
            return self.cursor, copy.deepcopy(self.model)

    def ready(self):
        return self.wait(lambda r: r["frame"]["type"] == "snapshot")

    def unwatch(self):
        self.client.sendall(b'{"apiVersion":"0.1","op":"unwatch"}\n')
        return self.wait(lambda r: r["frame"]["type"] == "terminated")

    def close(self):
        self.closed = True
        try:
            self.client.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        if self.worker:
            self.worker.join(timeout=5)
            assert not self.worker.is_alive()
        self.reader.close()
        self.client.close()

    def validate_examples(self):
        """Check representative wire frames outside the delivery timing path."""
        counts = {}
        for row in self.frames:
            kind = row["frame"]["type"]
            if counts.get(kind, 0) < 2:
                self.validator.validate(row["frame"])
                counts[kind] = counts.get(kind, 0) + 1
        return sum(counts.values())


class Qualification:
    def __init__(self, args):
        self.args = args
        self.attachment = json.loads(args.attachment.read_text())
        profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
        self.session = MemorySession(
            self.attachment["host"], ROOT / "builddir/arm/tools/mem_probe", profile
        )
        self.session.ssh[-1:-1] = [
            "-o",
            "ControlMaster=auto",
            "-o",
            "ControlPersist=60",
            "-o",
            "ControlPath=/tmp/mpc-us33-qualify",
        ]
        assert self.session.pid == self.attachment["pid"]
        self.path = self.attachment["localSocket"]
        auxv = self.session.command(f"cat /proc/{self.session.pid}/auxv")
        self.clock_ticks = dict(struct.iter_unpack("<II", auxv))[17]
        self.report = {
            "story": 33,
            "scenario": args.scenario,
            "clockTicks": self.clock_ticks,
            "phases": [],
            "rows": [],
            "transport": "Unix through SSH forwarding",
            "beforeContext": context(self.session),
            "beforeInventory": inventory(self.session, flags=True),
            "beforeMixers": independent(self.session),
        }
        if args.scenario in ("stopped", "playing"):
            c = self.report["beforeContext"]
            assert c["playing"] == (args.scenario == "playing")
            assert c["recordMode"] == 0 and c["playbackMode"] == 0 and not c["songSourceActive"]

    def public(self, op, **extra):
        return call(self.path, {"apiVersion": "0.1", "op": op, **extra})

    def metrics(self):
        raw = self.session.command(
            "cat " + self.attachment["remoteDirectory"] + "/metrics.jsonl"
        ).decode()
        return [json.loads(line) for line in raw.splitlines() if line.endswith("}")]

    def resources(self):
        raw = self.session.command(
            f'cat /proc/{self.session.pid}/stat; cat /proc/{self.session.pid}/status; for f in /proc/asound/card*/pcm*p/sub*/status; do if test -f "$f"; then cat "$f"; fi; done'
        ).decode()
        fields = raw.splitlines()[0].rsplit(")", 1)[1].split()
        rss = next(int(line.split()[1]) for line in raw.splitlines() if line.startswith("VmRSS:"))
        return {
            "at": time.monotonic(),
            "cpuTicks": int(fields[11]) + int(fields[12]),
            "rssKiB": rss,
            "raw": raw,
        }

    def phase(self, name, seconds):
        first = len(self.metrics())
        before = self.resources()
        time.sleep(seconds)
        after = self.resources()
        metrics = self.metrics()[first:]
        row = {
            "name": name,
            "seconds": after["at"] - before["at"],
            "before": before,
            "after": after,
            "metrics": metrics,
            "cpuPercentOneCore": (after["cpuTicks"] - before["cpuTicks"])
            / self.clock_ticks
            / (after["at"] - before["at"])
            * 100,
        }
        self.report["phases"].append(row)
        self.save()
        print(
            name,
            "samples",
            sum(not m["full"] for m in metrics),
            "full",
            sum(m["full"] for m in metrics),
            flush=True,
        )

    def save(self):
        self.args.output.write_text(json.dumps(self.report, indent=2) + "\n")

    def request(self, track, op, value):
        request = {
            "apiVersion": "0.1",
            "op": op,
            "requestId": "w_" + uuid.uuid4().hex,
            "target": track["ref"],
            "value": value,
            "waitMs": 1000,
        }
        if op == "set_mute" and self.baseline["muteContext"]["scope"] == "sequence":
            request.update(muteScope="sequence", sequence=self.baseline["muteContext"]["sequence"])
        if op == "set_volume":
            request["mixerGeneration"] = track["mixer"]["generation"]
        receipt = call(self.path, request)
        if receipt.get("status") != "applied":
            raise RuntimeError("Command failed: " + json.dumps(receipt))
        return receipt

    def verify(self, stream):
        self.session.refresh_maps()
        expected = inventory(self.session, flags=True)
        model = stream.state()
        assert model is not None
        assert len(model["tracks"]) == len(expected["tracks"])
        for actual, want in zip(model["tracks"], expected["tracks"], strict=True):
            for key in (
                "name",
                "type",
                "uiIndex",
                "collection",
                "index",
                "requested",
                "audioSide",
                "silenced",
            ):
                assert actual[key] == want[key], (key, actual, want)
        mixers = sorted(
            (r for r in independent(self.session) if r["userIndex"] is not None),
            key=lambda r: r["userIndex"],
        )
        model = stream.state()
        after_mixers = (
            sorted(
                (r for r in independent(self.session) if r["userIndex"] is not None),
                key=lambda r: r["userIndex"],
            )
            if self.args.scenario == "playing"
            else mixers
        )
        stable, changing = 0, 0
        for actual, want, after in zip(model["tracks"], mixers, after_mixers, strict=True):
            for key in ("volume", "pan"):
                if actual["mixer"][key] is not None:
                    if want[key] == after[key]:
                        assert abs(actual["mixer"][key] - want[key]) < 1e-7, (
                            actual["uiIndex"],
                            key,
                            actual["mixer"][key],
                            want[key],
                            after[key],
                        )
                        stable += 1
                    else:
                        changing += 1
        self.report.setdefault("mixerComparisonWindows", []).append(
            {
                "stableFieldsVerified": stable,
                "changingFields": changing,
                "before": mixers,
                "after": after_mixers,
            }
        )
        assert model["selectedTrackId"] == next(
            t["ref"]["trackId"]
            for t in model["tracks"]
            if t["uiIndex"] == expected["selectedTrack"]["uiIndex"]
        )
        return True

    def exercise(self, stream, count, reconnect=True):
        values = {i: self.baseline["tracks"][i]["requested"]["muted"] for i in (0, 4)}
        for i in range(count):
            target = (0, 4)[i % 2]
            value = not values[target]
            track = self.baseline["tracks"][target]
            bookmark, old_model = stream.checkpoint()
            started = time.monotonic_ns()
            receipt = self.request(track, "set_mute", value)
            assert receipt["changed"]

            def matches(row):
                if row["atNs"] < started:
                    return False
                f = row["frame"]
                tracks = f.get("tracks", f.get("state", {}).get("tracks", []))
                return any(
                    t["ref"]["trackId"] == track["ref"]["trackId"]
                    and t["requested"]["muted"] == value
                    for t in tracks
                )

            delivered = stream.wait(matches)
            row = {
                "target": target,
                "desired": value,
                "updateMs": (delivered["atNs"] - started) / 1e6,
                "receipt": receipt,
                "frame": delivered["frame"],
            }
            values[target] = value
            if reconnect and i % 4 == 0:
                before = time.monotonic_ns()
                replay = Stream(self.path, bookmark, old_model, defer_validation=True)
                try:
                    ack = replay.wait(lambda r: r["frame"]["type"] == "ack")
                    assert ack["frame"]["resumed"]
                    frame = replay.wait(
                        lambda r: (
                            r["frame"].get("cursor") == delivered["frame"]["cursor"]
                            and r["frame"]["type"] in ("change", "snapshot")
                        )
                    )
                    assert frame["frame"] == delivered["frame"]
                    row["reconnectMs"] = (frame["atNs"] - before) / 1e6
                finally:
                    replay.close()
            self.report["rows"].append(row)
            if i % 10 == 0:
                row["independentReadback"] = self.verify(stream)
                self.save()

    def restore(self):
        now = self.public("get_state")
        for track, baseline in zip(now["tracks"], self.baseline["tracks"], strict=True):
            for key, op in (
                ("muted", "set_mute"),
                ("soloed", "set_solo"),
                ("recordArmed", "set_record_armed"),
            ):
                if track["requested"][key] != baseline["requested"][key]:
                    self.request(track, op, baseline["requested"][key])
        after = self.public("get_state")
        for actual, want in zip(after["tracks"], self.baseline["tracks"], strict=True):
            assert actual["requested"] == want["requested"]

    def run(self):
        streams = []
        self.baseline = None
        try:
            if self.args.scenario == "stopped":
                self.public("describe")
                if self.metrics():
                    print("Waiting for the previous observation lease to expire", flush=True)
                    time.sleep(32)
                self.phase("cold_idle", 5)
                assert not self.report["phases"][-1]["metrics"]
            self.baseline = self.public("get_state")
            self.report["baseline"] = self.baseline
            streams = [Stream(self.path, defer_validation=True)]
            streams[0].ready()
            self.phase("one_subscriber", 8)
            self.exercise(streams[0], 24)
            # Eight total: seven new consumers share the same source.
            for _ in range(7):
                cursor, baseline = streams[0].checkpoint()
                stream = Stream(self.path, cursor, baseline, defer_validation=True)
                stream.wait(lambda r: r["frame"]["type"] == "ack")
                streams.append(stream)
            self.phase("eight_subscribers", 8)
            self.exercise(streams[0], 24, reconnect=False)
            self.report["allSubscribersEquivalent"] = all(self.verify(s) for s in streams)
            self.restore()
            for stream in streams:
                stream.unwatch()
                assert not stream.errors
                stream.close()
            self.report["schemaValidatedFrames"] = sum(
                stream.validate_examples() for stream in streams
            )
            streams = []
            if self.args.scenario == "stopped":
                print("Waiting for the 30-second producer lease to expire", flush=True)
                time.sleep(32)
                self.phase("expired_lease_idle", 5)
                assert not self.report["phases"][-1]["metrics"]
            self.report["afterInventory"] = inventory(self.session, flags=True)
            self.report["afterMixers"] = independent(self.session)
            self.report["flagsAndInventoryRestored"] = all(
                self.report["afterInventory"][k] == self.report["beforeInventory"][k]
                for k in ("selectedTrack", "tracks", "buses", "counts")
            )
            self.report["mixersRestored"] = (
                (self.report["afterMixers"] == self.report["beforeMixers"])
                if self.args.scenario != "playing"
                else None
            )
            self.report["mixerRestorationDeferredUntilStopped"] = self.args.scenario == "playing"
            assert self.report["flagsAndInventoryRestored"] and (
                self.report["mixersRestored"] or self.args.scenario == "playing"
            )
            self.report["updatesMs"] = distribution([r["updateMs"] for r in self.report["rows"]])
            self.report["reconnectMs"] = distribution(
                [r["reconnectMs"] for r in self.report["rows"] if "reconnectMs" in r]
            )
            self.report["passed"] = True
        except Exception as error:
            self.report["error"] = str(error)
            raise
        finally:
            try:
                if self.baseline:
                    self.restore()
            except Exception as error:
                self.report["restoreError"] = str(error)
                raise
            finally:
                for stream in streams:
                    stream.close()
                self.save()
                self.session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--scenario", choices=("stopped", "playing"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.output.resolve().is_relative_to(ROOT / "build"):
        parser.error("Keep private qualification reports under ignored build/")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    Qualification(args).run()


if __name__ == "__main__":
    main()
