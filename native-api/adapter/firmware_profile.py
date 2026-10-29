"""Validate the private v1 firmware profiles and generate a bounded C table."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATE = ROOT / "tools/profiles/key37-mpc3.9.1.2.json"
CONTROL = ROOT / "tools/profiles/key37-mpc3.9.1.2-control.json"
FAMILIES = ["unknown", "plugin", "drum", "keygroup", "midi", "cv", "audio", "clip"]
FIELDS = [
    "requested.muted",
    "requested.soloed",
    "requested.recordArmed",
    "audioSide.soloed",
    "audioSide.recordArmed",
    "silenced",
]
OPERATIONS = ["set_mute", "set_solo", "set_record_armed"]


def family_matrix(control):
    matrix = control.get("flagFamilies", {})
    if set(matrix) != set(FAMILIES):
        raise ValueError("Incomplete family qualification matrix")
    for name, row in matrix.items():
        if set(row) != {"readableFields", "operations", "reason"}:
            raise ValueError("Invalid family qualification")
        fields, operations = row["readableFields"], row["operations"]
        if (
            not isinstance(fields, list)
            or len(set(fields)) != len(fields)
            or not set(fields) <= set(FIELDS)
            or not isinstance(operations, dict)
            or not set(operations) <= set(OPERATIONS)
        ):
            raise ValueError("Invalid family field/operation")
        qualified_fields = FIELDS[:3] if name in ("midi", "cv") else FIELDS
        if name in ("unknown", "clip"):
            qualified_fields = []
        if not set(fields) <= set(qualified_fields):
            raise ValueError("Unqualified family observation")
        for operation, required in operations.items():
            property_index = OPERATIONS.index(operation)
            expected = {FIELDS[property_index]}
            if property_index and name in ("plugin", "drum", "keygroup", "audio"):
                expected.add(FIELDS[property_index + 2])
            if (
                not isinstance(required, list)
                or not required
                or len(set(required)) != len(required)
                or not set(required) <= set(fields)
                or set(required) != expected
            ):
                raise ValueError("Unverifiable family operation")
        if row["reason"] not in (None, "UNQUALIFIED_TRACK_TYPE", "HARDWARE_UNAVAILABLE"):
            raise ValueError("Invalid family qualification reason")
        if bool(fields) == bool(row["reason"]):
            raise ValueError("Missing family qualification reason")
    qualified = [name for name in FAMILIES if matrix[name]["operations"]]
    if control.get("supportedProgramFamilies") != qualified:
        raise ValueError("Family list disagrees with operation qualification")
    if matrix["unknown"]["readableFields"] or matrix["clip"]["readableFields"]:
        raise ValueError("Unqualified legacy/unknown family")
    return matrix


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
    family_matrix(control)
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
        "recorderTimeline",
        "recordPropertiesVtable",
        "recordModeValue",
        "sequencerMode",
        "normalSequenceMode",
        "sequencerSongSource",
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
        if key == "normalSequenceMode":
            if type(value) is not int or value != 0:
                raise ValueError("Unqualified sequence playback context")
            continue
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
    navigation = control.get("navigation")
    if navigation != {
        "schemaVersion": 1,
        "timelinePulse": 0xA8,
        "sequencerProject": 0x1E4,
        "sequencerSequence": 0x208,
        "sequencerCountIn": 0x3E8,
        "sequenceProperty": 0xBC,
        "trackProperty": 0x78,
        "menuPopup": 12544,
        "sequenceTempoProperty": 0x9C,
        "sequenceTempoStorage": 0xC8,
        "sequenceTempoManager": 0xD0,
        "sequenceTempoInvoker": 0xD4,
        "masterOwner": 0x1E8,
        "masterEnabled": 0x2C,
        "masterBpm": 0x58,
        "engineSurface": 0x388,
        "surfaceController": 0xF0,
        "controllerVtable": 0x68959FC,
        "controllerSequencer": 0x1F0,
        "controllerTime": 0x1EC,
        "controllerReady": 0x232,
        "sequencerTime": 0x3FC,
        "timeSource": 0x258,
        "audioThreadAsync": 0x3D8,
        "asyncVtable": 0x689FDAC,
        "asyncSequencer": 0x40,
        "asyncQueue": 8,
        "asyncPending": 12,
    } or any(type(v) is not int for v in navigation.values()):
        raise ValueError("Unqualified navigation layout")
    required_navigation = {
        "navigationAsyncPlay": 0x1BB87B8,
        "navigationAsyncLocate": 0x1BB8E20,
        "navigationSequence": 0x10457CC,
        "navigationTrack": 0xE5AD58,
        "navigationTempoFilter": 0x25CA4E4,
        "navigationTempoNotification": 0x25BC2FC,
    }
    if any(
        not any(f["name"] == n and f["rva"] == r for f in control["functions"])
        for n, r in required_navigation.items()
    ):
        raise ValueError("Missing navigation call fingerprints")
    required_setup = {
        "trackSetupColourSetter": 0xE5243C,
        "trackSetupIntSetter": 0xE4FB24,
        "trackSetupChannelSetter": 0x1462C7C,
        "trackSetupByteSetter": 0x1AC2E0C,
        "trackSetupColourBinding": 0x287061C,
        "trackSetupTransposeBinding": 0x286FD4C,
        "trackSetupVelocityBinding": 0x286CDF0,
        "trackSetupOutputChannelBinding": 0x286F92C,
        "trackSetupBankBinding": 0x286D754,
        "trackSetupMsbBinding": 0x286E7B8,
        "trackSetupLsbBinding": 0x286E9C0,
        "trackSetupProgramEnabledBinding": 0x286D95C,
        "trackSetupProgramBinding": 0x286EBC8,
    }
    if any(not any(f["rva"] == r for f in control["functions"]) for r in required_setup.values()):
        raise ValueError("Missing track setup call fingerprints")
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
            "playbackRecordModeGetters",
            "playbackLiveRecordMode",
            "playbackOverdubMode",
            "playbackRecorderTimeline",
            "playbackContextSource",
            "playbackContextAssignment",
            "playbackSongSource",
        }
        <= names
    ):
        raise ValueError("Missing command qualification landmarks")
    if (
        not {
            "familyRequestedMute",
            "familyRequestedSolo",
            "familyRequestedArm",
            "familyMutePropagation",
            "familyMuteForwarding",
            "familySequenceMutePropagation",
            "familySoloCopyPropagation",
            "familySoloSuppression",
            "familySoloPeerBehaviour",
            "familyArmCopyPropagation",
            "familyArmFilterTarget",
            "familyAudioClass",
            "familyKeygroupClass",
            "familyMidiClass",
            "familyCvClass",
        }
        <= names
    ):
        raise ValueError("Missing family qualification landmarks")
    evidence = control.get("familyEvidence", {})
    if set(evidence) != set(control["supportedProgramFamilies"]) or any(
        row
        != {
            "nativeMatrix": "findings/native-family-flags.md",
            "listenerTrace": "live_property_subscriptions",
            "outputVerified": False,
        }
        for row in evidence.values()
    ):
        raise ValueError("Missing family qualification evidence")
    mixer = control.get("mixer", {})
    if mixer.get("schemaVersion") != 1 or mixer.get("families") != [
        "plugin",
        "drum",
        "keygroup",
        "audio",
    ]:
        raise ValueError("Invalid mixer qualification matrix")
    for group in ("volume", "pan", "sends", "rack", "slot"):
        for key, value in mixer[group].items():
            integer(value, "mixer." + group + "." + key)
    if mixer["sends"]["count"] != 4 or mixer["rack"]["count"] != 4:
        raise ValueError("Invalid mixer addressing bounds")
    if (
        mixer["volume"]["value"] != mixer["volume"]["property"] + 40
        or mixer["volume"]["format"] != mixer["volume"]["value"] + 4
        or mixer["pan"]["value"] != mixer["pan"]["property"] + 40
        or mixer["pan"]["filterStorage"] != mixer["pan"]["property"] + 44
        or mixer["pan"]["filterManager"] != mixer["pan"]["filterStorage"] + 8
        or mixer["pan"]["filterInvoker"] != mixer["pan"]["filterStorage"] + 12
        or mixer["sends"]["stride"] != 56
        or mixer["sends"]["value"] != 48
        or mixer["sends"]["format"] != 52
        or mixer["volume"]["requiredFormat"] != 3
        or mixer["sends"]["requiredFormat"] != 1
    ):
        raise ValueError("Inconsistent mixer property/scaling ABI")
    if (
        not {
            "mixerVolumeSetter",
            "mixerSendSetter",
            "mixerPanFilter",
            "mixerRackSetter",
            "mixerSlotSetter",
        }
        <= names
    ):
        raise ValueError("Missing mixer call fingerprints")
    sources = control.get("readSources", {})
    expected_sources = {
        "schemaVersion": 1,
        "sequenceClipTree": 0x524,
        "updaterProject": 0x30,
        "listenerLimit": 64,
        "muteListenerVtable": 110315468,
        "muteListenerCallback": 39541132,
        "clipVtable": 110293008,
        "eventListVtable": 109670140,
        "listenerOwner": 32,
        "listenerCallback": 36,
        "listenerAdjustment": 40,
        "listenerParameterGroup": 44,
        "listenerParameterId": 48,
        "listenerClip": 52,
        "updaterApplying": 100,
        "clipEventList": 904,
        "eventTree": 956,
        "eventTreeWords": 5,
        "clipParameterMap": 1120,
        "sequencerPlaybackProperties": 764,
        "muteSoloPlayValue": 84,
        "levelPanPlayValue": 216,
    }
    if sources != expected_sources:
        raise ValueError("Unqualified Automation Read source layout")
    if sources.get("schemaVersion") != 1 or sources.get("listenerLimit") != 64:
        raise ValueError("Missing Automation Read source qualification")
    for key, value in sources.items():
        integer(value, "readSources." + key)
    if sources.get("eventTreeWords") != 5:
        raise ValueError("Invalid Automation Read event fence")
    if (
        not {
            "readSequenceClipLookup",
            "readSequenceSourceBinding",
            "readScheduledParameterDispatch",
            "readScheduledEventDispatch",
            "readParameterApplication",
            "readParameterMuteCase",
            "readParameterSoloCase",
            "readParameterVolumeCase",
            "readParameterPanDispatch",
            "readParameterPanCase",
            "readPanFilteredApplication",
            "readPanFilterInvocation",
            "readPanNotifyingApplication",
            "readMuteClipListener",
            "familySequenceMutePropagation",
            "readClipParameterNotify",
            "readSoloParameterNotify",
            "readEventReplacement",
            "readEventInvalidation",
            "readMuteSoloPlaybackSwitch",
            "readVolumePlaybackSwitch",
        }
        <= names
    ):
        raise ValueError("Missing Automation Read route fingerprints")
    plugin = control.get("pluginIdentity", {})
    if plugin != {
        "schemaVersion": 1,
        "programInstance": 0x1B68,
        "programOwner": 0x1B6C,
        "descriptionName": 0x1BD0,
        "descriptionFormat": 0x1BD8,
        "descriptionIdentifier": 0x1BE8,
        "supportedFormat": "MPC",
    }:
        raise ValueError("Unqualified instrument identity layout")
    plugin_functions = {
        "pluginIdentityDescriptionFromInstance": 0x8ADA4C,
        "pluginIdentityAssignmentDescription": 0x2502384,
        "pluginIdentityAssignmentProduct": 0x25023D4,
        "pluginIdentityDescriptionCopy": 0x24FCAEC,
        "pluginIdentityInstanceRead": 0x24F8BFC,
        "pluginIdentityAssignment": 0x2502540,
        "pluginIdentityInstanceDescriptor": 0x226AC00,
    }
    if any(
        not any(f["name"] == name and f["rva"] == rva for f in control["functions"])
        for name, rva in plugin_functions.items()
    ):
        raise ValueError("Missing instrument identity fingerprints")
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
    for key, value in control["pluginIdentity"].items():
        if type(value) is int:
            lines.append(f"#define PLUGIN_{key} {value}u")
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
    for key, value in control.get("navigation", {}).items():
        lines.append(f"#define NAV_{key} {integer(value, key)}u")
    for function in control["functions"]:
        if function["name"].startswith("navigation"):
            lines.append(f"#define NAV_{function['name']} {function['rva']}u")
    lines.append("#define NAV_navigationTempoSetter 0xE5AF48u")
    (out / "mpc_adapter_profile.h").write_text("\n".join(lines) + "\n")
    (out / "mpc_read_profile.h").write_text(
        "/* Generated verified Automation Read sources. */\n"
        + "\n".join(f"#define READ_{key} {value}u" for key, value in control["readSources"].items())
        + "\n"
    )
    mixer = control["mixer"]
    macros = {
        "VOLUME_VALUE": mixer["volume"]["value"],
        "VOLUME_FORMAT": mixer["volume"]["format"],
        "VOLUME_PROPERTY": mixer["volume"]["property"],
        "RECEIVER": mixer["volume"]["receiver"],
        "PAN_PROPERTY": mixer["pan"]["property"],
        "PAN_VALUE": mixer["pan"]["value"],
        "PAN_MANAGER": mixer["pan"]["filterManager"],
        "PAN_INVOKER": mixer["pan"]["filterInvoker"],
        "PAN_STORAGE": mixer["pan"]["filterStorage"],
        "SEND_BASE": mixer["sends"]["base"],
        "SEND_STRIDE": mixer["sends"]["stride"],
        "SEND_VALUE": mixer["sends"]["value"],
        "SEND_FORMAT": mixer["sends"]["format"],
        "RACK_OFFSET": mixer["rack"]["programOffset"],
        "RACK_VTABLE": mixer["rack"]["vtable"],
        "RACK_SLOTS": mixer["rack"]["slots"],
        "RACK_ENABLED": mixer["rack"]["enabled"],
        "RACK_BITS": mixer["rack"]["slotBits"],
        "RACK_BIT_COUNT": mixer["rack"]["slotBitCount"],
        "SLOT_VTABLE": mixer["slot"]["vtable"],
    }
    for label, name in {
        "VOLUME_SETTER": "mixerVolumeSetter",
        "FLOAT_SETTER": "mixerFloatPropertySetter",
        "SEND_SETTER": "mixerSendSetter",
        "PAN_FILTER": "mixerPanFilter",
        "RACK_SETTER": "mixerRackSetter",
        "SLOT_SETTER": "mixerSlotSetter",
    }.items():
        macros[label] = next(f["rva"] for f in control["functions"] if f["name"] == name)
    (out / "mpc_mixer_profile.h").write_text(
        "/* Generated verified mixer layout. */\n"
        + "\n".join(f"#define MIXER_{key} {value}u" for key, value in macros.items())
        + "\n"
    )
    matrix = family_matrix(control)
    lines = [
        "/* Generated from the qualified firmware family matrix. */",
        "#ifndef MPC_FAMILY_PROFILE_H",
        "#define MPC_FAMILY_PROFILE_H",
        '#include "mpc_native.h"',
        "#include <string.h>",
        "struct mpc_family_support {",
        "  unsigned family, vtable, fields, operations, required[3];",
        "  const char *type, *reason;",
        "};",
        "static const struct mpc_family_support mpc_family_matrix[]={",
    ]

    def mask(fields):
        return sum(1 << FIELDS.index(field) for field in fields)

    for family, name in enumerate(FAMILIES):
        row = matrix[name]
        vtable = next((int(v) for v, n in state["programTypes"].items() if n == name), 0)
        ops = sum(1 << OPERATIONS.index(op) for op in row["operations"])
        required = ",".join(str(mask(row["operations"].get(op, []))) for op in OPERATIONS)
        reason = json.dumps(row["reason"]) if row["reason"] else "NULL"
        lines.append(
            f"  {{{family}u,{vtable}u,{mask(row['readableFields'])}u,{ops}u,"
            f"{{{required}}},{json.dumps(name)},{reason}}},"
        )
    lines.extend(
        [
            "};",
            "static inline const struct mpc_family_support *mpc_family_by_type(const char *type) {",
            "  for (unsigned i=0; i<sizeof mpc_family_matrix/sizeof *mpc_family_matrix; i++)",
            "    if (!strcmp(type,mpc_family_matrix[i].type)) return &mpc_family_matrix[i];",
            "  return &mpc_family_matrix[0];",
            "}",
            "static inline const struct mpc_family_support *mpc_family_by_id(unsigned family) {",
            "  return &mpc_family_matrix[family < sizeof mpc_family_matrix/sizeof *mpc_family_matrix ? family : 0];",
            "}",
            "#endif",
        ]
    )
    (out / "mpc_family_profile.h").write_text("\n".join(lines) + "\n")
    return out
