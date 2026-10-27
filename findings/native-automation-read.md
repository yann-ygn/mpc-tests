# Automation Read flag qualification (US #51)

US #53 subsequently extends this baseline with [actual native Read mute/solo/volume
changes](native-automation-read-writes.md). The qualification below records the earlier release.

Package 0.9.0, MPC Key 37 / MPC 3.9.1.2, GNU build ID
`8371199f945ecc40f79cb19c17ff5de72810a91c`. The installed 0.7.0 host is
retained; acquiring this code requires a separately approved installation/restart.
The [sanitized evidence](native-automation-read-proof-20261027.json) distinguishes
the supported subset from remaining event-source research.

## Qualified subset

These conditions apply to plugin, drum, keygroup, MIDI, CV and audio user tracks,
in Main, Mixer or Track Mute, stopped or playing an ordinary sequence, with
recording/overdub Off and recognized native Single/Multi settings.

| Operation | Off | Read, matching desired state | Read, actual change | Write / unknown |
|---|---|---|---|---|
| Global mute | existing qualified writes | verified no-op | `AUTOMATION_READ_NOOP_ONLY` | `AUTOMATION_UNQUALIFIED` |
| Named active-sequence mute | existing qualified writes | verified no-op | `AUTOMATION_READ_NOOP_ONLY` | `AUTOMATION_UNQUALIFIED` |
| Solo | existing qualified writes | verified no-op | `AUTOMATION_READ_NOOP_ONLY` | `AUTOMATION_UNQUALIFIED` |
| Record arm | existing qualified writes | verified no-op | native filter/setter, including Single/Multi peers | `AUTOMATION_UNQUALIFIED` |

A mute request must still match the live scope preference. Sequence mute requires
the named active generation; the API never selects a sequence to satisfy a request.
Legacy clip/unknown families and buses retain their existing exclusions.

No Read mute/solo write is enabled, even with no visible automation, an empty lane,
or cached absence of events. The actual-write subset is record arm. Exhaustive
mute/solo event ownership and event-free writes remain unqualified; automation
event editing is still US #43.

## Native ownership and applicable sources

The exact firmware fingerprints and six-family owner/filter traces established
by [US #49](native-family-flags.md) remain mandatory. Qualification does not infer
an owner from a matching boolean offset alone.

- Mute's requested Program property is `+0x50`, with value `+0x78` and notifying
  setter `0xe298bc`. Native Muteable propagation, audio forwarding and the live
  global/per-sequence preference participate. The per-sequence listener captures
  a Clip and AutomationParameter; `PerSequenceParameterValueUpdater` at
  `0x25b598c` updates that Clip's parameter map at `+0x460`. Native source ownership
  is per Track/sequence Clip, rather than a visible lane on just the selected
  Program. A live source audit found key `[0x100, 0x101]` with value `1.0` on an
  already-muted unselected track, while two other targets had `0.0`. Source maps
  also contained `[0x100, 0x100]`. These are native parameter values, not a claim
  that a timeline lane is empty or that event automation is absent.
- Solo uses the Program property `+0x88`, requested value `+0x97`, engine copy
  `+0xc0`, and setter `0xe4aee8`. Program solo/suppression (`0x2513e14`), native
  follow/Single-Multi behavior (`0x25849e0`), and per-sequence/native parameter
  interactions can affect peers and effective suppression. A target-only empty
  lane or Program automation mode cannot establish permission for these writes.
  Their complete applicable event/peer source set is not qualified.
- Record arm is the separate Track-owned RecordArmable property `+0x474`,
  requested value `+0x483`, with engine copy `+0x494`. The captured native filter
  (`+0x484/+0x48c/+0x490`, invoker `0x257ba7c`) resolves to Track virtual filter
  `0x2650488` across all six qualified families. RecordArmable propagation
  (`0x257b3e4`), RecordArmableConnector, ClipPlayer and native follow/peer listeners
  govern arm state. This property is outside Program's Automatable parameter
  route: neither a track/program/instrument automation lane nor the Clip mute/solo
  parameter map supplies an arm value. Single mode can select/arm the target and
  disarm peers; Multi preserves the native filter behavior. The implementation
  retains these calls and observes all user peers. Record/overdub remains Off.

The record-arm qualification reuses those independently traced owners, not an
absence scan. Manual changes to event data cannot turn a rejected Read mute/solo
write into an allowed one. Arm eligibility does not depend on event absence.
The positive device control combines a populated native per-sequence mute source,
rejected mute writes, successful matching mute observations, and successful arm
writes on the same track without changing the mute source.

## No-ops, invocation boundaries and completion

The live policy now permits Read observations. The desired-state primitive first
checks live identity/family and `expected.value`; only then can a matching value
return without either native setter or arm filter. Completion still requires the
family's mask: mute and MIDI/CV requested flags use the model, while audio-producing
families' solo/arm also require the corresponding engine copy. No-op receipts
report `nativeInvoked: false`, `changed: false`.

Immediately before a native filter, and again before its notifying setter, a
private invocation marker causes production policy to requalify actual-write
eligibility. Read mute/solo cannot cross either boundary. This also prevents an
observation permission from becoming a write permission when the value changes.
The marker is cleared on every return; it is not a wire or caller-controlled field.

The existing command-local fence retains Program address/vtable, native context
owners, mode, transport/recorder, settings and scope. Dispatch, post-filter,
post-listener and delayed verification recheck it. Loss before native execution
rejects; loss after invocation preserves uncertainty and actual invocation metadata.
Write/unknown modes are rejected even for no-ops. No command changes automation
settings/data or navigates the screen to gain permission.

The final policy read is compared with the completion snapshot for the addressed
operation's required fields. A disagreement, lagging copy or nonmatching value
continues the existing bounded 500 ms read-only verification. It never repeats the
setter. Failure to converge remains `unknown`. Expected values are not reapplied
as preconditions to these later observations. `applied` describes a verified
observation at completion; later automation may change the flag. Sampling cannot
establish a transaction or detect an entire intervening ABA change.

## Discovery and compatibility

Top-level capabilities, `flagFamilies`, and per-track `flagSupport` now expose:

```json
{"automation":{"off":"qualified_writes","read":"verified_noop_only","write":"unsupported","unknown":"unsupported"}}
```

Record arm reports `read: "qualified_writes"`. These are conditional firmware
qualifications; a live dispatch must still satisfy all context and identity checks.
`AUTOMATION_READ_NOOP_ONLY` is distinct from `AUTOMATION_UNQUALIFIED`.
`excludedContexts.automationWriteUnknown` replaces the earlier blanket
`automationReadWrite` label. Schemas and examples include both Read no-op and
rejected-write receipts. Expected-value semantics and caller/request-ID retention
are unchanged, including retries after state restoration or disconnection.

Primitive ABI 2, executor ABI 8, command ABI 6, observation ABI 1, snapshot ABI 4 /
STATE_V4 and JSON API 0.1 retain their layouts. The invocation marker, final native
sample and consistency flag remain private. No transfer tag or export is added.

## Validation and restoration

The evidence records 19 passing host entries, 14 passing ASan/UBSan native entries,
six isolated ARM fixtures and the ARM compatibility/export guards. New host/ARM
cases cover all six families, both mute scopes, selected/unselected targets,
stopped/playing, matching/nonmatching requests, expected conflicts, Write/unknown
refusal, engine lag, final-sample drift, and retained retries. Read and Off context
transitions are forced before dispatch, before invocation, inside the arm filter,
after the setter and during verification. The full lifecycle, Program replacement,
stale-reference, sequence/scope, disconnect and deduplication suites also pass.

Live matrices exercise plugin/audio targets in Main, Mixer Effects and Track Mute,
stopped and playing, with independent requested/engine readback. Both global and
active-sequence mute observations are covered. A populated per-sequence source
provides a positive rejection/control case. Native Single/Multi arm peer behavior
is tested separately under Read. The other families retain US #49's exact owner
qualification plus the new host/ARM Read matrix; this story makes no new claim
about physical MIDI/CV output or audio audibility.

The record contains 120 preliminary flag rows, followed by 36 rows on the final
completion-consistency build and four final-build Single/Multi arm peer rows.
All passed. Matrix command total p95 was 37 ms for the recorded no-op/write mix;
this was not a saturation or audio-timing qualification. The evidence retains
the failed initial UI preparation and the subsequent corrected peer run.

`tools/mpc_automation_flags.py` is the repeatable private runner. Prepare an existing
authoritative temporary lab, Read targets, the requested live mute scope, and zero
playhead, then run:

```sh
python3 tools/mpc_automation_flags.py --attachment build/us51/final-attachment.json \
  --targets 0 4 --screen mixer_effects --scope sequence \
  --output build/us51/read-matrix.json
```

`--peers` qualifies two targets' native Single/Multi arm behavior. The runner does
not change automation settings or events. It restores flags, selection, tested
screen, peer settings and zero playhead using the ordinary Locate UI; STOP alone
can leave a residual position. Private reports remain under ignored `build/`.

The final restoration audit checks the original flags/buses, inventory, automation,
selection, active sequence, command settings, zero playhead and Mixer Effects view.
Both temporary lab DSOs are unloaded, temporary files removed, and installed 0.7.0
resumed. No project is saved/loaded and MPC is not restarted. The local 0.9.0 package
is prepared for review; it is not installed by this work.
