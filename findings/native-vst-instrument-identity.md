# VST instrument identity (US #57)

Package **0.17.0** extends US #55's read-only instrument metadata to the exact
native format name `VST`. The installed **0.16.0** host still qualifies only
`MPC`; this change is exercised through a temporary, drained lab host. See
[native-vst-instrument-identity-proof-20261029.json](native-vst-instrument-identity-proof-20261029.json)
for sanitized qualification results. No package upgrade is part of this story.

## Product identity

For the wrapper-hosted VST2 Midimix Editor, the instrument block is:

```json
{
  "status": "available",
  "reason": null,
  "displayName": "Midimix Editor",
  "productId": "VST:4d6d4564:/storage/Synths/ygn - VST - Midimix Editor/midimix_editor.so",
  "instanceId": null
}
```

The ID is `VST:<eight lowercase hexadecimal descriptor UID digits>:<exact native
fileOrIdentifier>`. The UID is interpreted as an unsigned 32-bit word, including
zero and high-bit values. The identifier must be an absolute library path. There
is no basename reduction, path normalization, display-name fallback, version or
address component. Consumers should compare the complete opaque ID.

| Format | Discovery `productIdScopes` | Source |
|---|---|---|
| `MPC` | `native_format_catalog` | Unchanged native `MPC:...` identifier |
| `VST` | `native_library_path_and_uid` | Native library identifier plus descriptor UID |

Discovery lists `formats: ["MPC", "VST"]`, `productIdScope: "per_format"`, and
the per-format scopes above. Torpedo Control reports descriptor UID `54704c76`
and `/storage/Synths/ygn - VST - Torpedo Control/torpedo_control.so`; Midimix Editor
reports `4d6d4564` (`MmEd`) and its own library path. Products built from the same
wrapper therefore differ. Even products reusing a UID differ when their library
paths differ. Separate tracks/instances of the same registered product share an
ID. Editable Track names and plugin display names do not enter the ID.

The scope is deliberately an installation path and native product ID, not a
vendor-global UUID, binary hash or version identity. Project save/reload and MPC
restart preserve it **if the native path and descriptor UID stay the same**.
Reinstalling a plugin at the same path with the same UID preserves it; updating
its binary/version/name also preserves it under those conditions. Moving or
renaming the library, or changing its UID, changes the ID. An unrelated binary
installed over that path while reusing the UID is indistinguishable by this
contract. Reinstall/update/restart behavior follows these defined inputs; no
physical reinstall or binary update was performed for qualification. JUCE's
zero-AEffect-ID fallback uses a path hash in the descriptor; the production API
reads that native descriptor word rather than computing another hash. The
independent device reader qualifies direct nonzero AEffect IDs for the two
installed products and explicitly rejects the fallback implementation.

Other format strings, including `VST3`, remain
`unsupported / UNQUALIFIED_PLUGIN_FORMAT`. Empty and assigned-but-unloaded
instruments retain `unassigned` and `unavailable`, respectively. All unsupported,
unavailable and unassigned names/IDs remain null. Instance identity, presets,
parameters and loading remain unsupported.

## Native trace and capture

Firmware remains build `8371199f945ecc40f79cb19c17ff5de72810a91c`. The authoritative
Track → PluginProgram path, Program vtable and descriptor strings are unchanged
from [US #55](native-plugin-identity.md). The added descriptor UID is at Program
`+0x1c00`, or PluginDescription `+0x30`; name, format and fileOrIdentifier remain
at `+0x1bd0`, `+0x1bd8` and `+0x1be8`.

The VST instance description-fill virtual at RVA `0x8d9574` copies its name from
instance `+0xdc`. At `0x8d9640`, it copies the module path from the module at
instance `+0xd0`, module `+8`, and reads AEffect.uniqueID at instance `+0xcc` →
AEffect `+0x48`. A zero ID takes the native file-name hash path. The code at
`0x8d96a0` writes the literal `VST`. Program assignment copies the UID at
`0x25023f8`–`0x250240c`; the independent Program descriptor-copy path reads it at
`0x24fcb74`. These five additional landmarks are fingerprinted and required by
the validated firmware profile. The independent reader verifies all identity
fingerprints before reading the loaded instance/module/AEffect path. Production
captures never dereference the instance or invoke its description-fill virtual.

The disposable capture fence now includes the descriptor UID. Both passes copy
and compare the owned output and associations, including the UID, under the
existing Track/Program destruction and project-generation fences. VST↔MPC
replacement and UID-only mutation cannot publish a mixed capture. There is no
persistent plugin cache or borrowed pointer outside the callback.

Both output strings still have a 256-byte bound including NUL. The complete VST
ID must fit, so its native path can occupy at most **242 UTF-8 bytes**. A longer
path, malformed UTF-8, unreadable storage or invalid qualified descriptor fails
the capture explicitly; no identifier is silently truncated. Unknown formats
remain unsupported without interpreting their product fields.

Public C layouts and ABI versions are unchanged: executor 12, snapshot 8,
commands 9, observations 2, primitive 2 and private STATE_V8. Only a private
capture fence grows. Existing socket clients that ignore new format/ID values
continue working. The service independently validates VST ID syntax on attachment
transfers; the JSON schemas and discovery examples reflect the new values.

`get_state`, `list_tracks`, `current_track` and `recovery_snapshot` use the same
capture/serializer. Instrument scans remain restricted to full metadata captures
and the five-second watch metadata audit. Controls samples and commands do not
scan plugins. Watch changes use existing replacement snapshots, cursors, replay
and explicit resync rules; a native load can exceed a bounded capture and require
recovery rather than uninterrupted streaming.

## Verification

Native fixtures cover mixed MPC/VST descriptors, two products on one wrapper,
duplicate products on distinct Programs, display-name independence, unsigned
UIDs, exact length bounds, malformed UTF-8, unload/unassigned status, unknown
formats, VST↔MPC replacement and UID mutation during capture. Existing lifecycle,
address reuse, conversion and no-plugin-scan controls cases continue to pass.
Attachment validation also rejects malformed UID syntax, relative VST paths,
invalid UTF-8 and unterminated wire strings.
Real Unix integration validates all four projections, metadata audit delivery,
retained VST replay and replacement back to MPC. Host, ASan/UBSan and isolated
ARM results, device comparisons, load continuity and restoration are recorded
in the proof file.

Device changes are confined to an owner-approved disposable copy under Claude
Experiments. A fresh session backup is verified before loading it. Main's PLUGIN
field and an independent loaded-instance reader are compared with the temporary
0.17.0 API for both VST products alongside Hype and TubeSynth. The original
session is reloaded afterward and the installed 0.16.0 host is resumed. Saved
projects are used for preservation and reload tests, never as an identity source.

The completed runs pass **27 host**, **22 native API ASan/UBSan** and **eight
isolated ARM** entries. The final attachment validator additions also pass on
host, sanitizer and isolated ARM. Both VST products match Main and the independent
AEffect/module path. The duplicate Midimix tracks share an ID, and saving/reloading
the disposable project preserves every instrument block while retiring the project
generation.

The ten-track workload had two eight-subscriber watch interruptions around
native plugin browsing/replacement; all peers resynced, and the captured second
interruption explicitly reports `CONTINUITY_LOST`. Fresh recovery agrees with
all eight subscribers; retained reconnect succeeds after freeing one subscriber
slot. No uninterrupted replacement latency is claimed. Host integration separately
verifies five-second metadata replacement delivery and retained replay. Across
the broader temporary-host run, four captures were cancelled (three controls,
one full, including project transitions). Completed callback p95 is **18.555 ms**
for 112 full captures and **4.821 ms** for 2,925 controls samples, with respective
maxima **27.459 ms** and **9.510 ms**. These mixed nine/ten-track observations are
not maximum-density or marginal VST cost qualification.

The owner-approved session backup is reloaded byte-for-byte unchanged. Independent
inventory/flags, all user/bus mixer values, automation/settings context, instruments,
selection and sequences match the captured baseline. The selected fifth Track,
zero playhead and original Mixer Pan & Volume screen are restored. The added Track
exists only in the retained disposable copy. No MPC restart or addin installation
occurred. A final smoke check uploads the final ARM artifacts, verifies their hashes
and passes six discovery/read/schema checks on the restored session. Both temporary
hosts drain/unload and their device files are removed; installed 0.16.0 resumes ready
and passes the same six operations, with VST still truthfully unsupported there.
