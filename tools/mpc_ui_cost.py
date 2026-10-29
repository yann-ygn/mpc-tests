#!/usr/bin/env python3
"""Compare #52 rapid command bursts with a single 10 Hz observation producer.

Use an existing temporary lab on the same stopped project before and after the
optimization. The playback runner captures/restores flags, automation and selection.
Baseline observation uses authoritative full reads; optimized observation uses
flags-only samples. Raw reports contain private state and stay in ignored build/.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import threading
import time

from mpc_native_lab import ROOT
from mpc_playback_flags import PlaybackMatrix, run


original_pressure = PlaybackMatrix.pressure


def pressure_with_observation(self, rows):
    samples, failures = [], []
    stop = threading.Event()

    def observe():
        while not stop.is_set():
            started = time.monotonic()
            try:
                verb = (
                    "FLAGS 0 16 0"
                    if self.args.lightweight
                    else ("STATE_V7 " + self.attachment["adapter"]["instanceId"])
                )
                data = self.session.command(
                    f"{self.attachment['remoteDirectory']}/native_client {self.session.pid}",
                    data=(verb + "\n").encode(),
                )
                if self.args.lightweight:
                    sample = json.loads(data)
                    if not sample.get("ok"):
                        raise RuntimeError(str(sample))
                    duration = sample["durationUs"]
                else:
                    abi, size, error = struct.unpack_from("<III", data)
                    if abi != 5 or size != 140496 or error or len(data) != size + 12:
                        raise RuntimeError("Full observation failed")
                    duration = struct.unpack_from("<Q", data, 12 + 96)[0] / 1000
                samples.append(
                    {
                        "nativeUs": duration,
                        "callbackUs": sample["callbackUs"] if self.args.lightweight else None,
                        "shared": sample["shared"] if self.args.lightweight else False,
                        "roundTripMs": (time.monotonic() - started) * 1000,
                    }
                )
            except Exception as error:
                failures.append(str(error))
            stop.wait(max(0, 0.1 - (time.monotonic() - started)))

    producer = threading.Thread(target=observe)
    producer.start()
    try:
        for _ in range(self.args.bursts):
            original_pressure(self, rows)
    finally:
        stop.set()
        producer.join()
        evidence = {"observationSamples": samples, "observationErrors": failures}
        rows.append(evidence)
    if self.args.lightweight and not failures:
        sample = self.command("FLAGS 0 16 0")
        raw = self.session.command(
            f"{self.attachment['remoteDirectory']}/native_client {self.session.pid}",
            data=("STATE_V7 " + self.attachment["adapter"]["instanceId"] + "\n").encode(),
        )
        if struct.unpack_from("<III", raw) != (5, 140496, 0):
            raise RuntimeError("Authoritative comparison failed")
        independent = self.independent()
        for i, track in enumerate(sample["tracks"]):
            identity = struct.unpack_from("<Q", raw, 12 + 128 + i * 392)[0]
            flags = struct.unpack_from("<7I", raw, 12 + 128 + i * 392 + 312)
            if str(identity) != track["id"] or flags != tuple(
                track[k]
                for k in ("known", "mute", "solo", "arm", "audioSolo", "audioArm", "silenced")
            ):
                raise RuntimeError("Flags differ from authoritative full state")
            expected = independent[i]
            for key, mask, value in (
                ("mute", 1, expected["requested"]["mute"]),
                ("solo", 2, expected["requested"]["solo"]),
                ("arm", 4, expected["requested"]["arm"]),
                ("audioSolo", 8, expected["audioSide"]["solo"]),
                ("audioArm", 16, expected["audioSide"]["arm"]),
                ("silenced", 32, expected["silenced"]),
            ):
                if track["known"] & mask and bool(track[key]) != value:
                    raise RuntimeError("Flags differ from independent state")
        evidence.update(flagsEquivalentToFull=True, flagsEquivalentToIndependent=True)
    if failures:
        raise RuntimeError("Concurrent observation failed: " + failures[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("MPC_HOST", "mpc-key-37.lan.arpa"))
    parser.add_argument("--attachment", type=Path, required=True)
    parser.add_argument("--targets", type=int, nargs="+", required=True)
    parser.add_argument("--screen", default="mixer_effects")
    parser.add_argument("--scope", choices=("global", "sequence"), required=True)
    parser.add_argument("--bursts", type=int, default=3)
    parser.add_argument("--lightweight", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.bursts <= 8:
        parser.error("Use 1..8 bursts")
    if not args.output.resolve().is_relative_to(ROOT / "build"):
        parser.error("Keep private reports under ignored build/")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.scenario, args.live_policy = "pressure", True
    PlaybackMatrix.pressure = pressure_with_observation
    raise SystemExit(run(args))


if __name__ == "__main__":
    main()
