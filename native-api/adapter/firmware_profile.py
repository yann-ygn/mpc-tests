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
    context = control["commandContext"]
    if set(context) != {
        "engineMode",
        "modeVtable",
        "modeValue",
        "popupValue",
        "mainMode",
        "mixerMode",
        "trackMuteMode",
        "noPopup",
        "engineAudioThread",
        "audioThreadVtable",
        "audioThreadSequencer",
        "sequencerVtable",
        "sequencerTimeline",
        "timelineVtable",
        "timelinePlaying",
        "sequencerRecorder",
        "recorderVtable",
        "recorderProperties",
        "engineArmBehaviour",
        "armBehaviourVtable",
        "armBehaviourProject",
        "armBehaviourProperties",
        "armModeValue",
        "engineMuteProperties",
        "perSequenceMute",
        "engineSoloBehaviour",
        "soloBehaviourVtable",
        "soloProperty",
        "soloValue",
        "programAutomation",
    }:
        raise ValueError("Invalid command context layout")
    for key, value in context.items():
        limit = 0xFFFFFFFF if key.endswith("Vtable") else 0x10000
        if not integer(value, key, limit) or (key.endswith("Vtable") and value % 4):
            raise ValueError("Invalid command context field")
    if len({context[k] for k in ("mainMode", "mixerMode", "trackMuteMode")}) != 3:
        raise ValueError("Aliased qualified screens")
    for name in ("project", "trackPool", "track", "sequence"):
        if not integer(state["vtables"][name], name) or state["vtables"][name] % 4:
            raise ValueError("Invalid vtable")
    for offset, vtable in state["projectSecondaryVtables"].items():
        if (
            integer(int(offset), "secondary offset", 0x10000) % 4
            or integer(vtable, "secondary vtable") % 4
        ):
            raise ValueError("Misaligned secondary vtable")
    for group in ("project", "track", "trackPool", "program", "sequence"):
        for key, value in state["offsets"][group].items():
            integer(value, group + "." + key, 0x10000)
    pool = state["offsets"]["trackPool"]
    starts = []
    for name in ("all", "normal", "user", "master", "audio"):
        start = pool.get(name + "Begin")
        if (
            type(start) is not int
            or start <= 0
            or start % 4
            or any(
                pool.get(name + suffix) != start + delta
                for suffix, delta in (("End", 4), ("Capacity", 8))
            )
        ):
            raise ValueError("Invalid inventory collection layout")
        starts.append(start)
    if len(set(starts)) != len(starts):
        raise ValueError("Aliased inventory collections")
    for name, limit in (("Midi", 128), ("Audio", 8), ("User", 136), ("Master", 128)):
        value = state.get("trackCollections", {}).get("max" + name + "Tracks")
        if type(value) is not int or value != limit:
            raise ValueError("Inventory capacity disagrees with native ABI")
    lifetime = state["identity"]
    for key in (
        "applicationSlot",
        "applicationVtable",
        "windowVtable",
        "componentVtable",
        "engineVtable",
        "projectListenerVtable",
    ):
        if not integer(lifetime[key], key) or lifetime[key] % 4:
            raise ValueError("Invalid identity anchor")
    for key in (
        "applicationWindow",
        "windowComponent",
        "componentEngine",
        "engineProject",
        "destructionOffset",
        "sequenceNotifier",
        "projectNotifier",
    ):
        if not integer(lifetime[key], key, 0x10000) or lifetime[key] % 4:
            raise ValueError("Invalid identity path")
    integer(lifetime["sequenceUsed"], "sequenceUsed", 0x10000)
    for vtable, name in state["programTypes"].items():
        if name not in {
            "audio",
            "clip",
            "cv",
            "drum",
            "keygroup",
            "input",
            "output",
            "master-output",
            "return",
            "submix",
            "midi",
            "plugin",
        }:
            raise ValueError("Unknown program type")
        if not integer(int(vtable), "program vtable") or int(vtable) % 4:
            raise ValueError("Invalid program vtable")
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
    if (
        not {
            "inventoryCollectionGetters",
            "inventoryUserCollection",
            "inventoryMasterCollection",
            "inventoryTypeCollection",
            "inventoryPartition",
            "inventoryTrackType",
        }
        <= names
    ):
        raise ValueError("Missing inventory landmarks")
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
    if (
        not {
            "lifetimeSubjectDestructor",
            "propertyDestructor",
            "ProjectDestructor",
            "TrackDestructor",
            "SequenceDestructor",
            "applicationDestructor",
            "projectOwnerConstructor",
            "sequenceListenerAdd",
            "sequenceListenerRemove",
            "sequenceUsageSetter",
            "sequenceUsageNotification",
            "sequenceReset",
            "projectListenerAdd",
            "projectListenerDetach",
            "projectResetBegin",
            "projectResetEnd",
            "projectDataInstalled",
        }
        <= names
    ):
        raise ValueError("Missing identity lifecycle landmarks")
    if (
        not {
            "commandModeGetter",
            "commandPopupGetter",
            "commandModeNames",
            "commandMainModeName",
            "commandMixerModeName",
            "commandTrackMuteModeName",
            "commandQualifiedModeCases",
            "commandTimelinePlaying",
            "commandSequencerPlaying",
            "commandSequencerOwner",
            "commandMuteScopeOwner",
            "commandMuteScopeValue",
            "commandSoloSettingsOwner",
            "commandSoloSettingsValue",
            "commandArmSettingsValue",
            "commandArmSettingsOwner",
            "commandArmBehaviourBinding",
            "commandAutomationValue",
            "commandAutomationNames",
        }
        <= names
    ):
        raise ValueError("Missing command qualification landmarks")
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
    for key, label in (
        ("project", "PROJECT"),
        ("trackPool", "POOL"),
        ("track", "TRACK"),
        ("sequence", "SEQUENCE"),
    ):
        lines.append(f"#define NATIVE_{label}_VTABLE {state['vtables'][key]}u")
    for family in control["supportedProgramFamilies"]:
        vtable = next(int(v) for v, name in state["programTypes"].items() if name == family)
        lines.append(f"#define NATIVE_{family.upper()}_VTABLE {vtable}u")
    lines.append(
        "static const struct { uint32_t vtable; const char *name; } native_program_types[]={"
        + ",".join(
            "{" + str(vtable) + "u," + json.dumps(name) + "}"
            for vtable, name in state["programTypes"].items()
        )
        + "};"
    )
    fields = {
        "PROJECT_LISTENER_ADD": next(
            f["rva"] for f in control["functions"] if f["name"] == "projectListenerAdd"
        ),
        "PROJECT_LISTENER_DETACH": next(
            f["rva"] for f in control["functions"] if f["name"] == "projectListenerDetach"
        ),
        "SEQUENCE_LISTENER_ADD": next(
            f["rva"] for f in control["functions"] if f["name"] == "sequenceListenerAdd"
        ),
        "SEQUENCE_LISTENER_REMOVE": next(
            f["rva"] for f in control["functions"] if f["name"] == "sequenceListenerRemove"
        ),
        "CALL_ASYNC": next(
            f["rva"] for f in control["functions"] if f["name"] == "juce::MessageManager::callAsync"
        ),
        "POOL_OFFSET": state["offsets"]["project"]["trackPool"],
        "MIDI_BEGIN": state["offsets"]["trackPool"]["normalBegin"],
        "MIDI_END": state["offsets"]["trackPool"]["normalEnd"],
        "MIDI_CAPACITY": state["offsets"]["trackPool"]["normalCapacity"],
        "ALL_BEGIN": state["offsets"]["trackPool"]["allBegin"],
        "ALL_END": state["offsets"]["trackPool"]["allEnd"],
        "ALL_CAPACITY": state["offsets"]["trackPool"]["allCapacity"],
        "USER_BEGIN": state["offsets"]["trackPool"]["userBegin"],
        "USER_END": state["offsets"]["trackPool"]["userEnd"],
        "USER_CAPACITY": state["offsets"]["trackPool"]["userCapacity"],
        "MASTER_BEGIN": state["offsets"]["trackPool"]["masterBegin"],
        "MASTER_END": state["offsets"]["trackPool"]["masterEnd"],
        "MASTER_CAPACITY": state["offsets"]["trackPool"]["masterCapacity"],
        "AUDIO_BEGIN": state["offsets"]["trackPool"]["audioBegin"],
        "AUDIO_END": state["offsets"]["trackPool"]["audioEnd"],
        "AUDIO_CAPACITY": state["offsets"]["trackPool"]["audioCapacity"],
        "TRACK_NAME": state["offsets"]["track"]["name"],
        "SEQUENCE_NAME": state["offsets"]["sequence"]["name"],
        "SEQUENCE_BPM": state["offsets"]["sequence"]["bpm"],
        "SELECTED_TRACK_MIRROR": state["offsets"]["project"]["selectedTrackMirror"],
        "SELECTED_SEQUENCE_MIRROR": state["offsets"]["project"]["selectedSequenceMirror"],
        "SELECTED_TRACK_INDEX": state["offsets"]["project"]["selectedTrackIndex"],
        "PROGRAM_OFFSET": state["offsets"]["track"]["program"],
        "SILENCED_OFFSET": state["offsets"]["program"]["silenced"],
        "SELECTED_TRACK": state["offsets"]["project"]["selectedTrack"],
        "SELECTED_SEQUENCE": state["offsets"]["project"]["selectedSequence"],
        "SEQUENCE_ARRAY": state["offsets"]["project"]["sequenceArray"],
        "SEQUENCE_CAPACITY": state["offsets"]["project"]["sequenceCapacity"],
        "SEQUENCE_COUNT": state["offsets"]["project"]["sequenceCount"],
    }
    for key, label in {
        "applicationSlot": "APPLICATION_SLOT",
        "applicationVtable": "APPLICATION_VTABLE",
        "windowVtable": "WINDOW_VTABLE",
        "componentVtable": "COMPONENT_VTABLE",
        "engineVtable": "ENGINE_VTABLE",
        "applicationWindow": "APPLICATION_WINDOW",
        "windowComponent": "WINDOW_COMPONENT",
        "componentEngine": "COMPONENT_ENGINE",
        "engineProject": "ENGINE_PROJECT",
        "destructionOffset": "DESTRUCTION_OFFSET",
        "sequenceNotifier": "SEQUENCE_NOTIFIER",
        "sequenceUsed": "SEQUENCE_USED",
        "projectNotifier": "PROJECT_NOTIFIER",
        "projectListenerVtable": "PROJECT_LISTENER_VTABLE",
    }.items():
        fields[label] = state["identity"][key]
    for key, value in fields.items():
        lines.append(f"#define NATIVE_{key} {value}u")
    lines.append("static const struct native_command_context_layout native_command_context={")
    lines.extend(f"  .{key}={value}u," for key, value in control["commandContext"].items())
    lines.append("};")
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
