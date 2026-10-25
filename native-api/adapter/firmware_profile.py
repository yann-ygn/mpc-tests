"""Validate the private v1 firmware profiles and generate a bounded C table."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATE = ROOT / "tools/profiles/key37-mpc3.9.1.2.json"
CONTROL = ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json"


def integer(value, label, limit=0xFFFFFFFF):
    if type(value) is not int or not 0 <= value <= limit:
        raise ValueError(f"Invalid {label}")
    return value


def validate(state, control):
    if any(
        type(p.get("profileSchemaVersion")) is not int or p["profileSchemaVersion"] != 1
        for p in (state, control)
    ):
        raise ValueError("Unsupported firmware profile schema")
    if state.get("abi") != {
        "machine": "arm",
        "pointerBits": 32,
        "byteOrder": "little",
        "floatAbi": "hard",
    }:
        raise ValueError("Unsupported native ABI")
    try:
        build_id = bytes.fromhex(state["buildId"])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError("Invalid build identity") from error
    if not 1 <= len(build_id) <= 64 or state["buildId"] != control.get("buildId"):
        raise ValueError("State/control build identity mismatch")
    if (
        not isinstance(state.get("mpcVersion"), str)
        or not state["mpcVersion"]
        or state["mpcVersion"] != control.get("firmwareVersion")
    ):
        raise ValueError("State/control version mismatch")
    if (
        not isinstance(state.get("deviceTreeModel"), str)
        or not 1 <= len(state["deviceTreeModel"]) <= 255
        or not all(32 <= ord(char) < 127 for char in state["deviceTreeModel"])
    ):
        raise ValueError("Missing device model")
    if control.get("supportedProgramFamilies") != ["plugin", "drum"]:
        raise ValueError("Unqualified program family")
    if control.get("capabilityQualification") != "bounded_lab_proof":
        raise ValueError("Unexpected qualification claim")
    for name in ("project", "trackPool", "track"):
        if not integer(state["vtables"][name], name) or state["vtables"][name] % 4:
            raise ValueError("Invalid vtable")
    for offset, vtable in state["projectSecondaryVtables"].items():
        if (
            integer(int(offset), "secondary offset", 0x10000) % 4
            or integer(vtable, "secondary vtable") % 4
        ):
            raise ValueError("Misaligned secondary vtable")
    for group in ("project", "track", "trackPool", "program"):
        for key, value in state["offsets"][group].items():
            integer(value, group + "." + key, 0x10000)
    for family in control["supportedProgramFamilies"]:
        candidates = [
            integer(int(v), "program vtable")
            for v, name in state["programTypes"].items()
            if name == family
        ]
        if len(candidates) != 1 or not candidates[0] or candidates[0] % 4:
            raise ValueError("Ambiguous program family")
    fingerprints = {}
    names = set()
    for function in control["functions"]:
        rva = integer(function["rva"], "function RVA")
        if not rva or rva in fingerprints or function["name"] in names:
            raise ValueError("Duplicate/invalid function identity")
        code = bytes.fromhex(function["prefixHex"])
        if len(code) != 32:
            raise ValueError("Expected a 32-byte code fingerprint")
        fingerprints[rva] = code
        names.add(function["name"])
    bindings = control["bindings"]
    if [b["name"] for b in bindings] != ["SetMute", "SetSolo", "SetTrackRecordArmed"]:
        raise ValueError("Unexpected typed operation bindings")
    for i, binding in enumerate(bindings):
        if binding["propertyOwner"] != ("track" if i == 2 else "program"):
            raise ValueError("Wrong property owner")
        for key in ("propertyOffset", "scriptValueOffset", "observedValueOffset"):
            integer(binding[key], key, 0x10000)
        if binding["propertyOffset"] % 4 or binding["setterRva"] not in fingerprints:
            raise ValueError("Unverified setter binding")
        if binding["wrapperRva"] not in fingerprints:
            raise ValueError("Unverified wrapper landmark")
        if i == 2:
            for key in ("filterFunctionOffset", "filterManagerOffset", "filterInvokerOffset"):
                if integer(binding[key], key, 0x10000) % 4:
                    raise ValueError("Misaligned filter binding")
            if binding["filterInvokerRva"] not in fingerprints:
                raise ValueError("Unverified arm filter invoker")
    if "juce::MessageManager::callAsync" not in names:
        raise ValueError("Missing executor landmark")
    return state, control


def generate(out, state_path=STATE, control_path=CONTROL):
    state, control = validate(
        json.loads(Path(state_path).read_text()), json.loads(Path(control_path).read_text())
    )
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    lines = ["/* Generated from validated private profile schema v1. */"]
    values = {
        "NATIVE_PROFILE_ID": Path(state_path).stem,
        "NATIVE_PROFILE_BUILD": state["buildId"],
        "NATIVE_PROFILE_MODEL": state["deviceTreeModel"],
    }
    for key, value in values.items():
        lines.append(f"#define {key} {json.dumps(value)}")
    for key, label in (("project", "PROJECT"), ("trackPool", "POOL"), ("track", "TRACK")):
        lines.append(f"#define NATIVE_{label}_VTABLE {state['vtables'][key]}u")
    for family in control["supportedProgramFamilies"]:
        vtable = next(int(v) for v, name in state["programTypes"].items() if name == family)
        lines.append(f"#define NATIVE_{family.upper()}_VTABLE {vtable}u")
    fields = {
        "POOL_OFFSET": state["offsets"]["project"]["trackPool"],
        "VECTOR_BEGIN": state["offsets"]["trackPool"]["normalBegin"],
        "VECTOR_END": state["offsets"]["trackPool"]["normalEnd"],
        "VECTOR_CAPACITY": state["offsets"]["trackPool"]["normalCapacity"],
        "PROGRAM_OFFSET": state["offsets"]["track"]["program"],
        "SILENCED_OFFSET": state["offsets"]["program"]["silenced"],
    }
    for key, value in fields.items():
        lines.append(f"#define NATIVE_{key} {value}u")
    entries = ",".join(
        "{" + str(offset) + "u," + str(vtable) + "u}"
        for offset, vtable in state["projectSecondaryVtables"].items()
    )
    lines.append("static const struct native_secondary native_secondary[]={" + entries + "};")
    entries = ",".join(
        "{" + str(f["rva"]) + "u,{" + ",".join(str(b) for b in bytes.fromhex(f["prefixHex"])) + "}}"
        for f in control["functions"]
    )
    lines.append("static const struct native_fingerprint native_fingerprints[]={" + entries + "};")
    entries = []
    for i, binding in enumerate(control["bindings"]):
        row = [
            binding[k]
            for k in ("propertyOffset", "scriptValueOffset", "observedValueOffset", "setterRva")
        ]
        row += [
            binding.get(k, 0)
            for k in (
                "filterFunctionOffset",
                "filterManagerOffset",
                "filterInvokerOffset",
                "filterInvokerRva",
            )
        ]
        entries.append(
            "{" + ",".join(str(v) + "u" for v in row) + "," + ("true" if i == 2 else "false") + "}"
        )
    lines.append(
        "static const struct native_binding native_bindings[]={" + ",".join(entries) + "};"
    )
    (out / "mpc_adapter_profile.h").write_text("\n".join(lines) + "\n")
    return out
