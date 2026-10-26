# Authoritative live snapshots (#18)

Implemented 2026-10-25 for Key 37 / MPC 3.9.1.2, build
`8371199f945ecc40f79cb19c17ff5de72810a91c`. The [local service](../native-api/README.md)
now exposes `get_state`, `list_tracks`, `current_track`, `current_sequence`, and
`recovery_snapshot` when an authoritative attachment is present. The recovery operation is a full fresh
snapshot; it carries no event replay or snapshot-to-subscription guarantee (#33).

US #46 extends these original findings to all user tracks, including audio, and
replaces the private transfer with snapshot ABI 2 / `STATE_V2`. The original
"normal" scope below was the MIDI-triggered collection; see the current
[inventory map, limits and evidence](native-track-inventory.md).

## Authority and sampling

Executor ABI 3 adds `GET_STATE` to the existing typed queue. Only authoritative executors accept it.
Its bounded, pointer-free buffer is allocated on the submitting thread, published only on successful completion,
and owned by the job until release. Cancellation, disconnect, shutdown and capture draining retain their
existing rules. Primitive adapter ABI 1 and the export allowlist are unchanged.

Each capture uses #20's application-owned active-project path and lifecycle observers. It samples identity,
normal-track topology, all-tracks topology and selections before and after reading metadata/fields. It checks
selection mirrors/index, native types, vector bounds/duplicates, normal-track membership, Program association,
sequence usage, finite BPM, boolean encoding and bounded names. A changed identity/topology/selection or Program
association retries up to three times, then returns `STATE_UNSTABLE`. A supported field that cannot be read fails
with `READ_FAILED`. A null active-project pointer produces an empty snapshot; an invalid owner path fails closed.
No heap/vtable scan, saved project, LED inference or cached fallback supplies a public snapshot.

Snapshots include the diagnostic wall-clock capture interval, monotonic duration, attempt count,
`atomic: false`, checked consistency and fresh/native-UI-thread provenance. Flags/audio copies are observations
within that interval, not a native transaction. The service assigns a new opaque observation revision per
successful read and rechecks process/attachment identity after transfer. Its instance rotates and revision resets
when the authority changes; an attachment lost during a read cannot publish its sample.

## Field meanings

- Normal tracks have #20 IDs/generations, zero-based indices, names and native Program types. Reorder/rename
  preserve the handle rules. The selected track/sequence comes from native Project properties, independent of view.
- Only plugin/drum normal-track flags have the current qualification: `requested` reads native mute/solo/arm
  properties, `audioSide` reads the separate solo/arm copies, and `silenced` is suppression rather than explicit
  mute or actual audio activity. The distinct audio-side mute copy and actual audio activity are unsupported.
- Buses are listed separately with names/types and no writable normal-track reference. Their flags are null.
  Other normal-track families expose metadata with null flags. Unknown native Program types are null.
- `sequenceSlotCount` counts slots; `sequenceObjectCount` counts non-null objects; `usedSequenceCount` sums the
  validated usage flag. Every existing sequence includes that flag, its identity/name and sequence-property BPM.
  A selected named sequence can still be unused. Effective transport BPM/source, playing/recording and project
  name are unqualified and null, with explicit unsupported-field metadata.

## Transport and limits

The restricted Unix service retains one JSON request/response per connection and same-UID admission.
A new private `STATE_V1` transfer accepts only the expected adapter incarnation; it transfers a versioned,
fixed-size pointer-free structure from the MPC-owned peer. Both status and state connections check peer PID/UID;
the service validates sizes, counts, IDs, UTF-8, values and attachment continuity. No raw target or lab write
command is accepted by the public service. The state transfer has a 200 ms total deadline (150 ms executor wait),
three capture attempts, 128 normal tracks, 128 buses, 128 sequence slots and a 1 MiB JSON response limit.
Capture names are bounded at 255 UTF-8 bytes; longer names fail rather than silently truncating.

The lab explicitly enables authoritative mode with `--identities --keep`. Its status advertises snapshot support
only in that mode. Discovery-only and unattached hosts keep reads gated. Public writes, receipts and `watch`
remain unavailable. Persistent packaging/attachment supervision and production command transport remain #21/#32;
HTTP, MCP and streaming remain #34/#35/#33. No startup service is installed by this work.

## Verification

[Sanitized evidence](native-state-proof-20261025.json) records:

- All ten host Meson entries and five ASan/UBSan native API entries pass.
- Isolated ARM adapter, executor and identity/snapshot fixtures pass, including 128 normal tracks plus 28 buses.
- Synthetic service tests exercise all read projections, revisions, empty projects, nullable fields, JSON/UTF-8,
  truncated/malformed/stalled transfers, attachment replacement, recovery and unsupported discovery.
- Native fixtures exercise transient/persistent selection races, project replacement mid-capture, failed supported
  fields, unsupported families, bus classification, empty projects and cancelled snapshot jobs.
- Real device snapshots matched the independent read-only observer: two normal tracks, 28 buses and 128 slots.
  All 128 Sequence objects had `used: false` in this empty project, including named `Sequence 01` at 120 BPM.
- Twenty repeated captures had a capture p95 of 9.058 ms. This measures the native observation interval for this
  small project, not full service round trips or a 128-track device performance qualification.
- A controller MUTE edit while Sounds remained open appeared in the next read without a save or mode switch.
  The track reference stayed stable; requested mute went false -> true -> false. All track flags and selection,
  and the original Main view, were restored.
- Removing the attachment invalidated the service instance and gated reads. Observers/captures drained, both
  libraries unloaded, temporary files were removed and MPC stayed at PID 10683. No restart/addin install/save/load.

Reproduce with the host/ARM builds from the root README, then:

```sh
python3 tools/mpc_native_lab.py --identities --keep --output build/native-api/state-attachment.json
python3 native-api/device_smoke.py --state --output build/native-api/state-smoke.json
python3 tools/mpc_native_lab.py --detach build/native-api/state-attachment.json \
  --output build/native-api/state-detached.json
```

Raw diagnostic reports stay under ignored `build/`; they are not public artifacts. Larger real projects, additional
families/modes, effective tempo and event delivery remain separate qualification work. The earlier
[live-state observer](live-state.md) is retained as independent evidence, with its original lifecycle limits.
