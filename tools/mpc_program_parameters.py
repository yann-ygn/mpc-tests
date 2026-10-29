#!/usr/bin/env python3
"""Validate the research-only parameter metadata or emit its private lab header.

No public parameter operation is enabled by this catalog. Device qualification
uses the separately built fixed-allowlist program_parameter_lab.so.
"""

import argparse
import json
import math
from pathlib import Path
import struct

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "native-api/parameters/key37-3.9.1.2.json"


def validate(profile):
    schema = json.loads((ROOT / "native-api/schema/parameter-metadata.schema.json").read_text())
    Draft202012Validator(schema).validate(profile)
    state = json.loads((ROOT / "tools/profiles/key37-mpc3.9.1.2.json").read_text())
    if profile["firmware"] != {
        "buildId": state["buildId"],
        "version": state["mpcVersion"],
        "model": state["deviceTreeModel"],
    }:
        raise ValueError("Parameter metadata must match the pinned firmware/model")
    parameters = profile["parameters"]
    if len({p["key"] for p in parameters}) != len(parameters):
        raise ValueError("Duplicate parameter key")
    if len({p["nativeId"] for p in parameters}) != len(parameters):
        raise ValueError("Duplicate parameter ID in this namespace")
    functions = profile["functions"]
    if len({f["name"] for f in functions}) != len(functions):
        raise ValueError("Duplicate function name")
    if len({f["rva"] for f in functions}) != len(functions):
        raise ValueError("Duplicate function RVA")
    required = {
        "programGet",
        "programSet",
        "programDispatch",
        "drumGet",
        "drumCurrentGet",
        "drumSet",
        "instrumentGet",
        "instrumentSet",
        "realtimeGet",
        "realtimeSet",
        "isRealtimeParameter",
        "parameterRange",
        "parameterDefault",
        "layerSemiTuneGet",
        "pluginGet",
        "pluginSet",
    }
    if {f["name"] for f in functions} != required:
        raise ValueError("Missing or unexpected native parameter landmark")
    lab_subset = {514, 519, 1089, 1039, 1040, 1043}
    controls = {
        p["nativeId"] for p in parameters if p["classification"] in ("native_control", "candidate")
    }
    if controls != lab_subset:
        raise ValueError("Changing the lab subset requires explicit native qualification")
    for parameter in parameters:
        verified = parameter["qualification"] == "device_verified"
        if verified != parameter["evidence"]["device"]:
            raise ValueError("Qualification must agree with device evidence")
        if verified and parameter["classification"] != "native_control":
            raise ValueError("Only a native control can qualify")
        if parameter["classification"] == "native_control" and not verified:
            raise ValueError("A native control needs device qualification")
        if parameter["value"]:
            value = parameter["value"]
            if (value["minimum"], value["maximum"]) != (0, 1):
                raise ValueError("The qualified native input range is normalized 0..1")
            tuning = parameter["nativeId"] == 1043
            if value["getterCanonicalization"] != ("binary32_endpoint_clamp" if tuning else "none"):
                raise ValueError("Getter canonicalization is qualified only for integer tuning")
        if parameter["classification"] in ("native_control", "candidate"):
            if parameter["value"] is None or parameter["readback"] is None:
                raise ValueError("A control needs a typed value and independent readback")
            if parameter["families"] != ["drum"]:
                raise ValueError("The lab subset qualifies only drum Programs")
    return profile


def header(profile):
    validate(profile)
    lines = ["/* Generated research profile; never a public command allowlist. */"]
    for name, value in profile["layout"].items():
        lines.append(f"#define PARAM_{name} {value}u")
    for function in profile["functions"]:
        lines.append(f"#define PARAM_{function['name']} {function['rva']}u")
    lines.append("static const struct { unsigned rva; unsigned char bytes[32]; } param_code[] = {")
    for function in profile["functions"]:
        data = ",".join(str(b) for b in bytes.fromhex(function["prefixHex"]))
        lines.append(f"{{{function['rva']}u, {{{data}}}}},")
    lines.append("};")
    lines.append("static const unsigned param_ids[] = {")
    for parameter in profile["parameters"]:
        if parameter["classification"] in ("candidate", "native_control"):
            lines.append(f"{parameter['nativeId']}u,")
    lines.append("};")
    return "\n".join(lines) + "\n"


def independent(session, profile, track_index):
    """Experimental external readback, independent of the native getters.

    Never a lifetime authority. The lab resolves generations on the UI thread;
    this reader detects owner changes and refuses a mixed observation.
    """
    from mpc_live_state import SnapshotChanged, u32

    project = session.active_project()
    pool = u32(session.read(project + 2708, 4))
    vector = session.read(pool + 116, 12)
    begin, end, capacity = struct.unpack("<III", vector)
    if not (0 <= track_index < (end - begin) // 4 <= 136 and end <= capacity):
        raise SnapshotChanged("Invalid user-track index/vector")
    track = u32(session.read(begin + 4 * track_index, 4))
    program = u32(session.read(track + 0x648, 4))
    layout = profile["layout"]
    if u32(session.read(program, 4)) != session.base + layout["drumVtable"]:
        raise SnapshotChanged("The parameter lab requires a drum Program")
    instruments = session.read(program + layout["instruments"], 12)
    first, last, limit = struct.unpack("<III", instruments)
    if not (last - first == limit - first == 128 * 4):
        raise SnapshotChanged("Invalid drum instrument vector")
    instrument = u32(session.read(first, 4))
    if u32(session.read(instrument, 4)) != session.base + layout["instrumentVtable"]:
        raise SnapshotChanged("Unknown Instrument receiver")
    pointers = session.read_many(
        [(instrument + layout[k], 4) for k in ("realtimeParameters", "layers")]
    )
    owners = dict(zip(("realtime", "layer"), (u32(p) for p in pointers), strict=True))
    parameters = [
        p for p in profile["parameters"] if p["classification"] in ("candidate", "native_control")
    ]
    values = session.read_many(
        [(owners[p["readback"]["owner"]] + p["readback"]["offset"], 4) for p in parameters]
    )
    result = {}
    for parameter, data in zip(parameters, values, strict=True):
        value = parameter["value"]
        native = struct.unpack("<i" if value["nativeType"] == "int" else "<f", data)[0]
        normalized = struct.unpack(
            "<f", struct.pack("<f", native * value["nativeScale"] + value["nativeBias"])
        )[0]
        if not math.isfinite(normalized) or not 0 <= normalized <= 1:
            raise SnapshotChanged("Invalid native parameter value")
        result[parameter["key"]] = {"native": native, "normalized": normalized}
    if (
        session.active_project() != project
        or session.read(pool + 116, 12) != vector
        or u32(session.read(begin + 4 * track_index, 4)) != track
        or u32(session.read(track + 0x648, 4)) != program
        or session.read(program + layout["instruments"], 12) != instruments
        or u32(session.read(first, 4)) != instrument
        or session.read_many(
            [(instrument + layout[k], 4) for k in ("realtimeParameters", "layers")]
        )
        != pointers
    ):
        raise SnapshotChanged("Parameter owner changed during independent capture")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("validate", "header"))
    parser.add_argument("--profile", type=Path, default=PROFILE)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    profile = validate(json.loads(args.profile.read_text()))
    if args.action == "header":
        if not args.output:
            parser.error("header requires --output")
        args.output.write_text(header(profile))
    else:
        print(f"Parameter metadata {profile['metadataVersion']}: valid (public writes disabled)")


if __name__ == "__main__":
    main()
