# Native flag qualification by track family (US #49)

MPC Key 37, MPC 3.9.1.2, build
`8371199f945ecc40f79cb19c17ff5de72810a91c`. Package 0.6.0 introduces a shared,
validated family matrix. The installed 0.2.0 package is not upgraded by this work.

## Qualification matrix

Each requested field has its own known bit. Read qualification, native operation
support and completion evidence are separate. Conditions below apply to every
operation, including expected-value conflicts and desired-state no-ops.

| User family | Requested mute / solo / arm | Solo / arm engine copies | Silenced | Set mute | Set solo / arm |
|---|---|---|---|---|---|
| Plugin | qualified independently | qualified independently | qualified | model | model + matching engine copy |
| Drum | qualified independently | qualified independently | qualified | model | model + matching engine copy |
| Keygroup | qualified independently | qualified independently | qualified | model | model + matching engine copy |
| Audio | qualified independently | qualified independently | qualified | model | model + matching engine copy |
| MIDI | qualified independently | unqualified, null | unqualified, null | model | model |
| CV | qualified independently | unqualified, null | unqualified, null | model | model |
| Legacy clip | unqualified, null | unqualified, null | unqualified, null | unavailable | unavailable |
| Unknown | unqualified, null | unqualified, null | unqualified, null | unavailable | unavailable |

`audioSide.muted` is unqualified for every family. `audioActivity` is never measured.
`silenced` describes effective native suppression; its inverse is neither activity
nor audibility. MIDI/CV native engine bytes exist diagnostically, but their meaning
for output was not qualified. They cannot satisfy an audio-copy acknowledgement.
The owner confirmed that no external endpoint was connected: MIDI note output,
external synthesizer behavior and CV/gate voltages remain unmeasured. Native CV
tracks can be created in this firmware's Add Track dialog; that is not proof of a
physical output endpoint. All matrix entries report `outputVerified: false`.

The Add Track dialog exposes audio, drum, plugin, keygroup, MIDI and CV. The legacy
clip Program class remains recognizable in the executable but has no qualified
creation/control fixture in that dialog. It remains `UNQUALIFIED_TRACK_TYPE`.
Unknown Programs likewise stay unavailable. Input/output/master-output/return/submix
objects are metadata-only buses (`BUS_METADATA_ONLY`), outside writable user references.

## Actual native owners and paths

Read-only traversal of each live family's subscriptions identified the actual
Muteable, Program solo, RecordArmable, per-sequence updater and peer behavior owners.
The audit included existing audio/plugin tracks and empty temporary keygroup, drum,
MIDI and CV tracks. Their subscriptions and native filter captures were inspected
before any native write; matching Program offsets were not used as qualification.

| Operation | Owner / requested field | Native operation | Qualified readback |
|---|---|---|---|
| Mute | Program property `+0x50`, value `+0x78` | locked notifying setter `0xe298bc` | requested model value in the addressed mute scope |
| Solo | Program property `+0x88`, value `+0x97` | notifying setter `0xe4aee8` | model, plus `Program +0xc0` for audio-producing families |
| Arm | Track property `+0x474`, value `+0x483` | captured filter `+0x484/+0x48c/+0x490`, invoker `0x257ba7c`, then `0xe4aee8` | model, plus `Track +0x494` for audio-producing families |

Fingerprint landmarks include the requested getters, each additional family class,
Muteable propagation (`0x2374cf0`), audio forwarding (`0x1dd0e04`), per-sequence mute
updater (`0x25b598c`), solo copy propagation (`0xea48e8`), Program solo/suppression
(`0x2513e14`), follow/peer behavior (`0x25849e0`) and RecordArmable propagation
(`0x257b3e4`). The existing automation getter/owner, notifying setters, arm invoker,
identity and screen/context fingerprints remain mandatory. Firmware/model mismatch,
any changed fingerprint or invalid native owner fails closed.

The captured arm member call was resolved for all six live families: each reached
Track virtual filter `0x2650488`, through the secondary vtable at `0x69355e0`.
That function is fingerprinted as `familyArmFilterTarget`.

The adapter preserves the captured native record-arm filter instead of bypassing
it. Filters may reject requested values; receipts distinguish rejection from work
that was never invoked. Native callbacks retain normal peer/selection effects.

## Context and stable references

Main, Channel Mixer and Track Mute are qualified native view IDs. All five Mixer
pages share the qualified view ID. A popup, playing transport, target automation
other than Off, or unrecognized solo/arm settings prevents execution. Mute additionally
requires the matching live global/sequence preference. Sequence mute addresses only
the explicitly named active generation; it never selects or edits an inactive sequence.

Program/family changes preserve the Track reference. Every dispatch uses the current
Program and current support; an old family cannot lend its eligibility to queued work.
The context fence now includes Program address and vtable across the arm filter,
setter and delayed verification. Loss before invocation rejects; loss after invocation
retains an unknown outcome and actual invocation/execution metadata. Destruction,
replacement, address reuse and project reload retain the existing generation rules.

## Public and trusted interfaces

Discovery `flagFamilies` reports firmware qualification. Per-track `flagSupport`
reports fields actually known in that capture, per-operation availability, required
verification, explicit conditions and reasons for unsupported fields. These are
conditional qualifications, not a replacement for top-level attachment readiness or
the live dispatch policy. Unsupported observations remain null.

MIDI/CV solo and arm receipts use `verification.by: ["model"]` with
`audioSideValue: null`. Other qualified families require the corresponding engine
copy. Mute always uses the model in its labeled scope. Peer changes compare only
individual fields known in both same-family observations. Unsupported fields do not
hide independently known requested changes.

Recompile trusted C hosts for **primitive ABI 2, executor ABI 7, command ABI 4 and
snapshot ABI 4**. The snapshot still occupies 127824 bytes and track rows 344 bytes,
but known bits now independently represent mute, solo, arm, solo copy, arm copy and
silenced (1, 2, 4, 8, 16, 32). Old `known == 15` is incompatible. Command observations
also carry family qualification; receipts carry a checked ABI/size prefix before serialization.
Status-only primitive opens never touch typed output buffers, including older caller
layouts. Transfers use `STATE_V4` / `MPC_ADAPTER_STATE_V4`;
primitive attachments use `MPC_ADAPTER_V2`. The export set and JSON API 0.1 are retained.

`tools/mpc_family_flags.py` uses canonical user indices, including audio, and an
existing temporary authoritative attachment. It checks native commands with an
independent memory reader and restores flags, selection and automation through the
UI. The operator retains ownership of preparation and attach/detach. Example:

```sh
python3 tools/mpc_family_flags.py --attachment build/us49/attachment.json \
  --targets 4 8 10 11 --screen mixer_effects --scope sequence \
  --output build/us49/mixer-sequence.json
```

## Validation and restoration

The [sanitized evidence](native-family-flags-proof-20261026.json) records:

- All 18 host Meson entries and 13 ASan/UBSan native entries pass. The five isolated
  ARM fixtures and ARM deployment/export/layout guards pass.
- 96 new-family command rows cover audio/keygroup/MIDI/CV, selected/unselected,
  Main/Mixer Effects/Track Mute, all three operations and both mute scopes. Every
  row verifies an independent readback, retained retry after restoration, a no-op
  and an expected conflict. Twelve final plugin/drum regression rows also pass.
- 48 cross-family peer rows cover Single/Multi solo and arm on all three native
  views, including audio/MIDI, keygroup/CV and plugin interactions. Peer changes,
  follow-selection and complete restoration agree with independent observations.
- Twelve controller-edit cases and twelve discarded-response recovery cases cover
  every new family/operation. Eight Main cases establish different patterns on
  two adjacent sequences, reject inactive/stale references without invocation,
  retain results while inactive and restore both sequences. Separate scoped-write
  rows cover Mixer and Track Mute; those views inherit the independently qualified
  screen/context policy. New-family Mixer device rows use the Effects page.
- All five public read projections match the independent inventory, requested
  fields, qualified engine copies, silenced values and unsupported nulls. Twenty
  fresh final captures have native capture p95 **11.911 ms**.

The first large diagnostic batch reached the bounded retained-receipt capacity
while checking an existing plugin after the new families had completed. The
runner now opens a fresh diagnostic command authority between family batches;
it preserves the production retention guarantee. Original state was restored
on that failure path. Raw intermediate and final reports remain in ignored
`build/us49/`.

Four empty temporary keygroup/drum/MIDI/CV tracks were removed. The original
eight-track inventory, requested/engine/silenced flags, selection, sequence,
automation, native/saved command preferences and Mixer Effects view were restored.
All saved settings match the pre-global-test capture, with no differing keys.
The project path and MPC PID are unchanged. Both lab mappings were drained and
unloaded, temporary files were removed, and the installed 0.2.0 host resumed ready
with a successful state read. No project save/load, MPC restart, addin upgrade or
physical output experiment was performed. Package 0.6.0 is built, not installed.
