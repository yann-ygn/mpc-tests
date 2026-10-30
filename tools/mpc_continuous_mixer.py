#!/usr/bin/env python3
"""Qualify continuous mixer admission on an already prepared disposable project.

Use a temporary production host and an SSH Unix forward. No project/UI changes,
installation or restart. Restores numeric values and flag probes in finally;
operator owns project/context setup and teardown. Raw evidence stays in build/.
The requested Hz is aggregate traffic across the chosen tracks, not per Track.
"""

import argparse
import json
from pathlib import Path
import time
import uuid

import jsonschema

from mpc_watch_qualify import call, distribution
from mpc_live_state import MemorySession
from mpc_mixer_controls import independent
from mpc_command_policy import capture

ROOT = Path(__file__).resolve().parents[1]


def run(args):
    schema = json.loads((ROOT / "native-api/schema/command-receipt.schema.json").read_text())
    validator = jsonschema.Draft202012Validator(schema)
    profile = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    session = MemorySession(args.host, ROOT / "builddir/arm/tools/mem_probe", profile)
    report = {
        "aggregateHz": args.hz,
        "phaseSeconds": args.seconds,
        "rows": [],
        "phases": [],
        "resources": [],
    }
    baseline = None
    prefix = "gesture_" + uuid.uuid4().hex[:10]
    serial = 0
    targets = []

    def save():
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    def request(op, target, value, continuous=False, **extra):
        nonlocal serial
        serial += 1
        return {
            "apiVersion": "0.1",
            "op": op,
            "requestId": prefix + "_" + str(serial),
            "target": target["ref"],
            "value": value,
            "waitMs": 2000,
            **(
                {"continuous": True, "mixerGeneration": target["mixer"]["generation"]}
                if continuous
                else {}
            ),
            **extra,
        }

    def send(req, category):
        started = time.monotonic()
        out = call(args.socket, req)
        if "status" not in out:
            raise RuntimeError(f"{req['op']}: {out.get('error')}")
        validator.validate(out)
        report["rows"].append(
            {
                "category": category,
                "request": req,
                "receipt": out,
                "elapsedMs": (time.monotonic() - started) * 1000,
            }
        )
        if out.get("status") != "applied":
            save()
            raise RuntimeError(f"{req['op']}: {out.get('error')}")
        return out

    def resources():
        raw = session.command(
            f"cat /proc/{session.pid}/stat; cat /proc/{session.pid}/status"
        ).decode()
        lines = raw.splitlines()
        fields = lines[0].split(") ", 1)[1].split()
        keys = {"VmSize", "VmRSS", "RssAnon", "Threads"}
        report["resources"].append(
            {
                "elapsedMs": (time.monotonic() - epoch) * 1000,
                "utimeTicks": int(fields[11]),
                "stimeTicks": int(fields[12]),
                **{
                    line.split(":")[0]: line.split(":", 1)[1].strip()
                    for line in lines[1:]
                    if line.split(":")[0] in keys
                },
            }
        )

    epoch = time.monotonic()
    try:
        report["describe"] = call(args.socket, {"apiVersion": "0.1", "op": "describe"})
        assert report["describe"]["continuousMixer"]["available"]
        baseline = call(args.socket, {"apiVersion": "0.1", "op": "get_state"})
        report["before"] = baseline
        report["beforeIndependent"] = independent(session)
        report["beforeContext"] = capture(session)
        targets = [
            t for t in baseline["tracks"] if t["type"] in ("plugin", "drum", "keygroup", "audio")
        ]
        assert len(targets) >= max(args.tracks)
        resources()
        # Same-project retained baseline isolates admission-mode UI cost.
        for i in range(12):
            t = targets[i % len(targets)]
            send(
                request("set_volume", t, 0.2 + i / 100, mixerGeneration=t["mixer"]["generation"]),
                "retained_baseline",
            )
        for count in args.tracks:
            chosen = targets[:count]
            start = time.monotonic()
            phase = {
                "tracks": count,
                "startedMs": (start - epoch) * 1000,
                "firstRow": len(report["rows"]),
            }
            last_ids = {}
            for i in range(round(args.seconds * args.hz)):
                due = start + i / args.hz
                delay = due - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                track = chosen[i % count]
                # Each track sweeps volume, pan and four independent sends.
                control = (i // count) % (6 if args.sends else 2)
                op = (
                    "set_volume"
                    if control == 0
                    else "set_pan"
                    if control == 1
                    else "set_send_level"
                )
                extra = {"sendIndex": control - 2} if control >= 2 else {}
                value = ((i // (count * (6 if args.sends else 2))) % 19 + 1) / 20
                req = request(op, track, value, True, **extra)
                result = send(req, "continuous")
                key = (track["ref"]["trackId"], control)
                if i % 100 == 0:
                    assert call(args.socket, {**req, "waitMs": 0}) == result
                    conflict = call(args.socket, {**req, "value": 0 if value else 1})
                    assert conflict["error"]["code"] == "REQUEST_ID_CONFLICT"
                    if key in last_ids:
                        gone = call(
                            args.socket,
                            {
                                "apiVersion": "0.1",
                                "op": "get_command",
                                "instanceId": req["target"]["instanceId"],
                                "requestId": last_ids[key],
                            },
                        )
                        assert gone["error"]["code"] == "OUTCOME_UNAVAILABLE"
                last_ids[key] = req["requestId"]
                if i % round(15 * args.hz) == 0:
                    t = targets[0]
                    property_index = (i // round(15 * args.hz)) % 3
                    prop = ("set_mute", "set_solo", "set_record_armed")[property_index]
                    field = ("muted", "soloed", "recordArmed")[property_index]
                    original = t["requested"][field]
                    flag_extra = {}
                    if prop == "set_mute" and baseline["muteContext"]["scope"] == "sequence":
                        flag_extra = {
                            "muteScope": "sequence",
                            "sequence": baseline["selectedSequence"]["ref"],
                        }
                    try:
                        send(request(prop, t, not original, **flag_extra), "discrete")
                    finally:
                        send(request(prop, t, original, **flag_extra), "discrete_restore")
                    resources()
                    save()
                if i % round(30 * args.hz) == 0:
                    print(
                        f"tracks={count} updates={i} elapsed={time.monotonic() - start:.1f}s",
                        flush=True,
                    )
            phase.update(elapsedSeconds=time.monotonic() - start, lastRow=len(report["rows"]))
            report["phases"].append(phase)
            report["independentAfterPhase" + str(count)] = independent(session)
            resources()
            save()
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            if baseline:
                for t in targets:
                    for op, value, extra in [
                        ("set_volume", t["mixer"]["volume"], {}),
                        ("set_pan", t["mixer"]["pan"], {}),
                    ] + (
                        [
                            ("set_send_level", level["value"], {"sendIndex": i})
                            for i, level in enumerate(t["mixer"]["sends"])
                        ]
                        if args.sends
                        else []
                    ):
                        send(request(op, t, value, True, **extra), "numeric_restore")
                current = call(args.socket, {"apiVersion": "0.1", "op": "get_state"})
                if current["selectedTrackId"] != baseline["selectedTrackId"]:
                    selected = next(
                        t
                        for t in baseline["tracks"]
                        if t["ref"]["trackId"] == baseline["selectedTrackId"]
                    )
                    req = request("select_track", selected, None)
                    del req["value"]
                    send(req, "selection_restore")
                report["after"] = call(args.socket, {"apiVersion": "0.1", "op": "get_state"})
                report["afterIndependent"] = independent(session)
                report["afterContext"] = capture(session)
                assert report["afterIndependent"] == report["beforeIndependent"]
                # Sources, settings and peers are not silently changed for eligibility.
                assert report["afterContext"] == report["beforeContext"]
                for original, current in zip(
                    baseline["tracks"], report["after"]["tracks"], strict=True
                ):
                    assert original["ref"] == current["ref"]
                    assert original["requested"] == current["requested"]
                assert report["after"]["selectedTrackId"] == baseline["selectedTrackId"]
                report["restored"] = True
            rows = report["rows"]
            report["measurements"] = {
                category: {
                    "callbackUs": distribution(
                        [
                            r["receipt"]["timing"]["maxUiCallbackUs"]
                            for r in rows
                            if r["category"] == category
                        ]
                    ),
                    "roundTripMs": distribution(
                        [r["elapsedMs"] for r in rows if r["category"] == category]
                    ),
                }
                for category in ("retained_baseline", "continuous", "discrete")
            }
            resources()
        except BaseException as error:
            report["restorationError"] = str(error)
            raise
        finally:
            save()
            session.close()
    print(
        json.dumps(
            {
                "passed": report["passed"],
                "restored": report.get("restored"),
                "measurements": report["measurements"],
            }
        ),
        flush=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="mpc-key-37.lan.arpa")
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tracks", type=int, nargs="+", default=[2, 8])
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--hz", type=int, default=10)
    parser.add_argument(
        "--sends", action="store_true", help="Requires Automation Off on every target"
    )
    args = parser.parse_args()
    if args.seconds < 1 or not 1 <= args.hz <= 10 or not all(1 <= n <= 8 for n in args.tracks):
        parser.error("Use positive duration, 1–10 aggregate Hz and 1–8 tracks")
    run(args)


if __name__ == "__main__":
    main()
