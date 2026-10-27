# Native API contract

Design baseline 0.1, accepted by the owner on 2026-10-24. Taiga #28, on branch `native-api-contract`.

This is the agreed design; implementation and qualification proceed endpoint by endpoint. The existing lab proves
bounded native mute, solo and record-arm writes on one firmware. It does not yet provide the lifetime, concurrency,
delivery or qualification guarantees below. State/command examples describe the service after those requirements
are met. The first implementation is the [discovery-only service](../native-api/README.md).

## Shape of the library

Expose MPC objects and explicit desired-state setters. Keep firmware offsets, object discovery, native calling
conventions and UI-thread dispatch inside the adapter. One local service owns identities, validation, command
receipts and state observations. HTTP, MCP and SDKs share those semantics.

```python
mpc = MPC.connect(endpoint, credentials=credentials)
track = mpc.current_track()

receipt = track.set_mute(True)
track.set_solo(False)
track.set_record_armed(True)

state = mpc.get_state()
```

This is illustrative Python, not an importable SDK yet. The synchronous setters wait for a verified result within
a bounded timeout and return a receipt; unsuccessful outcomes raise typed exceptions carrying that receipt.
An asynchronous submission option returns the accepted receipt and uses `get_command()` for follow-up.
The SDK generates a request ID once per submission and preserves it on transport retries.

`current_track()` binds the track selected when the read occurs. Its later setters keep targeting that track if
the user selects another one. There is no delayed, selection-relative write and no name-based write lookup.

## First public surface

| Operation | Result |
|---|---|
| `describe()` | API version, device/service identity, firmware, qualified capabilities and limits |
| `get_state()` | Fresh live project, selected track/sequence and all user-track state |
| `current_track()` | The selected track and its reference, or `null` when none is selected |
| `set_mute(track, value)` | Command receipt for explicit mute |
| `set_solo(track, value)` | Command receipt for explicit solo |
| `set_record_armed(track, value)` | Command receipt for track record arm |
| `get_command(instance_id, request_id)` | Latest retained receipt for an earlier submission |

The SDK methods on a track call the corresponding service operation. The track list covers every user track
in canonical Track List order (US #46), including audio;
buses need their own qualified entity and capability later. Playback, tempo writes, selection writes, track
creation/deletion, volume, plugins and event editing remain expansion work. Catalog names alone expose nothing.
Each setter accepts only a boolean. Track record arm does not mean global transport recording.

## Identity and lifetime

- `deviceId` is an opaque configured installation identity, stable across ordinary restarts. It need not expose a
  hardware serial number.
- `instanceId` identifies one service/adapter authority lifetime. It changes after service restart, MPC process
  restart or replacement/reattachment of the adapter. Authentication credentials have a separate lifetime.
- `projectId` identifies one active project incarnation. Loading the same file again creates a new identity.
- `trackId` and `sequenceId` identify object incarnations within that project. Rename and reorder preserve them;
  deletion/recreation invalidates them. IDs are never reused within an instance.
- A writable track reference contains `instanceId`, `projectId` and `trackId`. Names and zero-based indices are
  display metadata. No address, heap fingerprint, name or index can substitute for a reference.

Resolve and revalidate the reference against the authoritative active project immediately before execution on the
UI thread. A queued command cannot follow a replacement project or a recycled object into a different target.
If identity/lifetime cannot be established, reject the command. The #20 adapter implements these guarantees through native ownership and lifecycle notifications; the external
heap-discovery prototype remains diagnostic only. Sequence references are conservatively retired on destructive
content/length notifications as well as clear/recreation. Reacquire them after those edits. Rename/reorder of an
in-use sequence alone preserves its reference. Same-file project loading creates a new project generation even
when the firmware retains its Project object.

## Track inventory and ordering (US #46)

Every user track appears once in `tracks`, including plugin, drum, keygroup, clip,
MIDI, CV and audio. `uiIndex` is its zero-based canonical Track List position, read
from the native normal/user collection; array order follows `uiIndex`. `index`
retains the previously exposed MIDI-triggered collection index. New audio entries
use their audio-collection index. `collection` (`midi_triggered` or `audio`) makes
that scope explicit; the two indices can differ. For example, a plugin after two
audio tracks can have `index: 4, uiIndex: 6`. Use `ref`, never either index, for writes.

The separate `buses` list contains explicit native master-collection objects,
including typed submix, return, input, output and master-output objects. It has
`collection: "master"`, master-relative `index`, `uiIndex: null`, and no writable
reference. Unknown Program types remain null. Listing a family does not enable
its flags or setters; US #49 owns additional per-family qualification.

The API bounds inventory at 128 MIDI-triggered plus 8 audio tracks and 128 master
objects. Capture fences inventory, ordering, Program association and selection.
The public 0.1 extension preserves existing MIDI indices and adds the two ordering
fields. Private snapshot ABI 2, executor ABI 5 and command ABI 2 replace their
older layouts; trusted C consumers must recompile and older attachments fail
negotiation. See [the firmware map](native-track-inventory.md).

## Reads and state meaning

Read requests use `apiVersion` and an operation name:

```json
{"apiVersion":"0.1","op":"get_state"}
```

Example response with one user track, shortened in scope rather than containing an incomplete track record:

```json
{
  "apiVersion": "0.1",
  "deviceId": "dev_example",
  "instanceId": "run_a",
  "projectId": "prj_b",
  "revision": "42",
  "sample": {
    "startedAt": "2026-10-24T18:00:00.000Z",
    "finishedAt": "2026-10-24T18:00:00.010Z",
    "atomic": false
  },
  "project": {"name": "Example"},
  "selectedTrackId": "trk_c",
  "selectedSequence": {
    "id": "seq_d", "index": 0, "name": "Sequence 01", "bpm": 120.0
  },
  "sequenceSlotCount": 128,
  "tracks": [
    {
      "ref": {"instanceId":"run_a","projectId":"prj_b","trackId":"trk_c"},
      "index": 0,
      "uiIndex": 0,
      "collection": "midi_triggered",
      "name": "Plugin 001",
      "type": "plugin",
      "requested": {"muted":false,"soloed":false,"recordArmed":true},
      "audioSide": {"muted":null,"soloed":false,"recordArmed":true},
      "silenced": false
    }
  ]
}
```

`requested` contains values read from MPC's native control properties, not an echo of the last API request.
`audioSide` contains separately observed copies where mapped and qualified. Mute has no distinct qualified copy
in the present evidence, so its audio-side value is `null`. Unsupported or unavailable values never become
`false`, zero or an empty string. Capabilities identify unsupported fields; a temporary failure to read a required,
supported field fails the read with `READ_FAILED` rather than silently serving an old value.

`silenced` reports MPC's suppression state, which can result from another track's solo. It is distinct from
explicit mute; `silenced: false` does not establish actual audio activity. Actual audio activity is outside this
first contract. Sequence `bpm` is the sequence property, not a claim about effective tempo under master/external
sync. `sequenceSlotCount` counts allocated slots, not used sequences. No active project is represented by
`projectId: null`, `project: null`, empty tracks and null selections.

The snapshot covers an observation interval. Check identity, topology and selection around sampling; retry a
bounded number of times or return `STATE_UNSTABLE`. Do not claim all fields were captured atomically. A
`revision` is an opaque, instance-scoped identifier for a published service observation, not a native transaction
counter or proof that every intervening hardware edit was observed. Timestamps are diagnostic wall-clock values.

`current_track()` is the same live sampling operation projected onto the selected track:

```json
{"apiVersion":"0.1","op":"current_track"}
```

```json
{
  "apiVersion": "0.1",
  "instanceId": "run_a",
  "projectId": "prj_b",
  "revision": "42",
  "sample": {
    "startedAt":"2026-10-24T18:00:00.000Z",
    "finishedAt":"2026-10-24T18:00:00.010Z",
    "atomic":false
  },
  "track": {
    "ref": {"instanceId":"run_a","projectId":"prj_b","trackId":"trk_c"},
    "index":0, "name":"Plugin 001", "type":"plugin",
    "requested":{"muted":false,"soloed":false,"recordArmed":true},
    "audioSide":{"muted":null,"soloed":false,"recordArmed":true},
    "silenced":false
  }
}
```

## Capabilities and compatibility

`describe()` reports exact model, firmware version and GNU build ID, instance identity, readable fields and each
operation's availability, target types, verification level and limits. An unavailable capability includes a reason
such as `UNSUPPORTED_FIRMWARE`, `UNQUALIFIED_TRACK_TYPE` or `NOT_IMPLEMENTED`. Limits include queue capacity,
request-retention duration and command timing bounds. Clients consult discovery rather than a hardcoded list.

The implemented `describe` request is `{"apiVersion":"0.1","op":"describe"}`. Its
[request schema](../native-api/schema/describe-request.schema.json) and
[response/error schema](../native-api/schema/describe-response.schema.json) define the concrete discovery shape.
Readiness separates matching a research profile from attaching/qualifying the native adapter. Only discovery is
currently available. Native queue/receipt capacities are zero and command timing bounds are null until those
features are implemented; they must meet the requirements below before writable capabilities become available.

The research profile is MPC Key 37 / 3.9.1.2 / build ID
`8371199f945ecc40f79cb19c17ff5de72810a91c`. This is evidence scope, not a release support claim. Operations become
available only after their firmware, object types, mode constraints and verification paths pass qualification.
Live readiness checks can still reject an otherwise supported operation. A failed firmware/profile guard disables
native access; there is no best-effort fallback to nearby offsets.

The [#31 stopped-mode matrix](native-flag-semantics.md) verifies global mute when
per-sequence mute is disabled, and sequence-specific mute when enabled, on plugin/drum tracks.
US #48 adds the explicit `muteScope: sequence` request and complete generation-bound sequence reference.
The named sequence must remain active and the live per-sequence preference must match at dispatch,
every native traversal and verification. Inactive editing returns `SEQUENCE_NOT_ACTIVE`; no command
switches selection. Omitted scope retains global semantics. Authoritative state and receipt observations
label the active mute scope; reads do not claim independent inactive/global copies.
See [the implemented scope contract](../native-api/README.md#sequence-scoped-mute).
Clip/Song modes, playback and automation remain unqualified.

All examples use agreed API/schema version `0.1`, independently of MPC firmware.
Additive optional fields/capabilities remain compatible; changing field meaning or
required fields needs a new major version. A client requests a supported version explicitly. Unknown optional
response fields are ignored; unknown operations, input fields and invalid values are rejected.

## Command requests and receipts

Every setter carries a unique request ID and a complete track reference. `expected.value` is an optional
precondition on that operation's native control property, checked immediately before the setter in the same
UI-thread callback. An observation revision is not accepted as a transaction precondition in this first version.
It cannot detect all concurrent/manual edits with the evidence currently available.

These are three alternative submissions, not an atomic batch:

```json
[
  {
    "apiVersion":"0.1", "op":"set_mute", "requestId":"req_mute_1",
    "target":{"instanceId":"run_a","projectId":"prj_b","trackId":"trk_c"},
    "value":true, "expected":{"value":false}, "startWithinMs":1000, "waitMs":500
  },
  {
    "apiVersion":"0.1", "op":"set_solo", "requestId":"req_solo_1",
    "target":{"instanceId":"run_a","projectId":"prj_b","trackId":"trk_c"},
    "value":true, "startWithinMs":1000, "waitMs":500
  },
  {
    "apiVersion":"0.1", "op":"set_record_armed", "requestId":"req_arm_1",
    "target":{"instanceId":"run_a","projectId":"prj_b","trackId":"trk_c"},
    "value":false, "startWithinMs":1000, "waitMs":500
  }
]
```

`startWithinMs` bounds queue residence from first acceptance using the service's monotonic clock. The service
must not begin an expired command. `waitMs` controls how long this delivery waits for a receipt; it does not cancel
the command or extend its execution budget. Both values are bounded by advertised limits.

All three setters return the same receipt shape. Example verified mute result:

```json
{
  "apiVersion":"0.1",
  "requestId":"req_mute_1",
  "instanceId":"run_a",
  "status":"applied",
  "execution":"completed",
  "target":{"instanceId":"run_a","projectId":"prj_b","trackId":"trk_c"},
  "op":"set_mute",
  "value":true,
  "changed":true,
  "verification":{
    "by":["model"], "requestedValue":true, "audioSideValue":null,
    "observedAt":"2026-10-24T18:00:00.020Z"
  },
  "observedChanges":[],
  "error":null
}
```

For successful solo and arm results, `op`/`value` reflect their requests and `verification.by` includes
`audio_copy` as well as `model` when that readback is qualified. `audioSideValue` must then equal the desired
value. Empty `observedChanges` means none observed, not proof that no surrounding state changed.

| Status | Meaning |
|---|---|
| `accepted` | Recorded and queued/in progress; application is not yet established |
| `applied` | Setter completed, or verified no-op, and the required live readbacks matched the desired value |
| `rejected` | A definite validation, precondition or native constraint failure; error and execution stage explain which |
| `cancelled` | Expired or stopped before native execution; no setter was invoked |
| `unknown` | Execution may have occurred but completion could not be established |

`execution` is `not_started`, `started` or `completed`. Before acceptance, validation/authentication failures
return a structured error without creating a receipt. Once accepted, outcomes are retained for `get_command()`.
If the HTTP/client wait ends while work continues, return/retain `accepted`; a lost connection alone says nothing
about execution. If execution started and verification times out or the adapter disappears, report `unknown`.
An unknown receipt can later resolve if retained execution evidence becomes available.

Applied means the required values were observed at that time. It does not promise continued state, audible sound,
a completed audio buffer, persistence to disk or an undo entry. `changed: false` is a verified already-desired
state, not a repeated native toggle. The service's per-operation verification timeout is advertised separately;
model-only verification for mute is explicit and cannot silently be described as audio verification.

The production setters leave the desired state in place. They do not inherit the lab `PULSE` command's timed
restoration. Native follow-solo behavior is honored: solo can select its target. Record-arm filters and MultiRec
can constrain outcomes. Report observed selection/other state changes in `observedChanges` with field paths,
before/after values and project context; these observations are best-effort and not a complete causal history.
Do not promise selection preservation, isolated side effects, rollback, atomic batches or uniform undo behavior.
A constraint rejection after execution may have side effects and must include its observed readback.

The #31 device matrix observes Single solo selecting its target and clearing a peer's solo;
Multi solo retains both solos and selection. Single record arm can select an armed target and disarm
its peer; track selection and follow-solo also move arm. Multi arm permits multiple armed tracks.
Changing a setting does not establish that existing flags already satisfy it. Report these live
effects instead of inferring them from a setting name or restoring selection implicitly.

## Retries, ordering and errors

Deduplication is scoped to the authenticated caller and `instanceId`. The same request ID with the same canonical
command returns the existing receipt and never re-enqueues it during the advertised retention interval. A
different operation, target, value, precondition or start budget under that ID is `REQUEST_ID_CONFLICT`.
Changing only `waitMs` does not create a new command or renew its budget. Retained unknown results are also
deduplicated. Capacity pressure rejects new work instead of evicting an unexpired receipt.

Propose at least ten minutes of receipt retention from acceptance, also retaining commands while pending.
After expiry or restart, `get_command()` returns `OUTCOME_UNAVAILABLE` rather than inventing success/failure.
Resubmission after the retention window has no deduplication guarantee. SDKs stop automatic resubmission once
that window is no longer known to be active, and do not replay an uncertain command with a new ID. They refresh
the session/state and let the caller decide.
This provides bounded deduplication, not exactly-once delivery across process failure.

Serialize accepted native writes in service acceptance order on the MPC UI thread. This orders service commands,
not every physical edit. Recheck identity, support and preconditions at dispatch; do not hold native object
pointers across queue waits. Repeated desired-state writes are safe to reconcile but can still have native
mode/filter effects; avoid invoking the setter when a qualified read establishes the desired state already holds.

Errors contain a stable `code`, readable `message` and structured `details`; accepted-command errors also retain
their execution stage in the receipt. Initial codes include:

| Group | Codes |
|---|---|
| Input/access | `INVALID_ARGUMENT`, `UNSUPPORTED_API_VERSION`, `UNAUTHORIZED`, `FORBIDDEN` |
| Support/readiness | `UNSUPPORTED_FIRMWARE`, `UNSUPPORTED_OPERATION`, `UNQUALIFIED_TRACK_TYPE`, `NOT_READY` |
| Playback policy | `RECORDING_UNQUALIFIED`, `PLAYBACK_CONTEXT_UNQUALIFIED`, `AUTOMATION_UNQUALIFIED` |
| Identity/state | `STALE_INSTANCE`, `STALE_PROJECT`, `TARGET_NOT_FOUND`, `NO_ACTIVE_PROJECT`, `PRECONDITION_FAILED` |
| Delivery | `QUEUE_FULL`, `REQUEST_ID_CONFLICT`, `RETENTION_CAPACITY`, `OUTCOME_UNAVAILABLE` |
| Observation/execution | `READ_FAILED`, `STATE_UNSTABLE`, `VALUE_REJECTED`, `VERIFICATION_TIMEOUT`, `ADAPTER_LOST` |

For example, a rejected mute precondition returns error details containing the expected and observed native
values and `execution: not_started`. Error text is never parsed for control flow. An error's code alone is not
permission to retry an operation whose execution began.

## Subscriptions and transports

Reserve `watch()` for #33; advertise it unavailable until implemented. Subscribe from a snapshot revision using
a bounded replay cursor. Subscription setup must bridge the snapshot-to-watch interval or explicitly require a
fresh snapshot. Events carry instance/project context and ordered service revisions. A gap/expired cursor produces
`resync_required`; a new instance invalidates old handles. These are observed state changes, not a lossless native
event log. The first contract does not select a WebSocket transport or promise to observe every brief flag change.

The domain contract is shared across local IPC, paired HTTP, MCP and SDKs. Transports map structured errors and
receipts without changing completion, lifetime or retry semantics. The service starts on a restricted local
endpoint; remote adapters enforce their authorization before queuing work. Akai HTTP integration preserves its
pairing/authentication settings; MCP does not bypass them. No generic memory, address-call or shell operation is
part of this API. Concrete HTTP paths and MCP tool schemas follow in #34/#35.

## Proposed measurement budgets

These are initial qualification targets, not measured guarantees or hard real-time scheduling promises. Measure
client-observed latency and queue/dispatch/readback times separately on the device, under idle and playback load,
and report distributions and failures rather than averages alone.

| Measurement | Proposed first target |
|---|---|
| Local receipt acceptance | p95 <= 20 ms for one client with no queue backlog |
| Single flag to required readback | p95 <= 100 ms; verification observation bounded to 500 ms after dispatch |
| Fresh snapshot, up to 128 normal tracks | p95 <= 100 ms with stable topology |
| Queued start | Default expiry 1000 ms; cancellation if not started in that interval |

The current SSH reader takes roughly 2.5 seconds per snapshot plus discovery; it does not meet these targets.
The in-process proof did not establish a latency distribution or measure audio disruption. Profile and publish
those results in qualification (#38); change targets openly if measurement warrants it. Queue size, memory use
and supported client concurrency also need explicit measured limits before release.

## Implementation gates

The accepted decisions are: object references bound to lifetimes; explicit boolean setters; native
follow behavior with observed effects; and readback-based completion with uncertainty preserved. The first SDK
should be Python, with TypeScript consuming the same contract later.

Add machine-readable schemas and shared examples as each endpoint is implemented; discovery has its schemas
above. Authoritative read operations, including `list_tracks`, `current_sequence` and the full
`recovery_snapshot` alias, now have [implementation findings](native-state.md) and
[state schemas/examples](../native-api/README.md#read-current-state) under #18. Before exposing writable capabilities, finish authoritative project lifetime (#20),
UI execution/lifecycle (#30), flag semantics across modes/types (#31) and failure/concurrency qualification (#38).
These requirements describe the intended product, not additional proof already obtained from the lab.

Evidence: [live state](live-state.md), [native control](native-control.md),
[sanitized write proof](native-control-proof-20261024.json), [capability catalog](native-control-catalog.md),
[delivery roadmap](native-api-roadmap.md).

## Managed command implementation (#19)

The [transport-independent handler API](../native-api/adapter/COMMANDS.md) implements typed desired-state
requests, retained receipts, caller-scoped retries, dispatch preconditions, native/audio verification and
observed peer/selection changes. [Domain request](../native-api/schema/command-request.schema.json) and
[receipt](../native-api/schema/command-receipt.schema.json) schemas describe that boundary. The [#32 Unix binding and native live policy](native-service.md) now expose these commands in the embedded host.
`describe` advertises its actual authority limits and qualified Main/Mixer/Track Mute, no-popup, sequence transport, recording-Off and automation-Off conditions; mute requires a matching explicit scope.

## Family qualification (#49)

[The firmware family matrix](native-family-flags.md) now separates recognized
families, individually readable flags and per-operation completion evidence.
`describe.flagFamilies` reports conditional firmware qualification; each state
track's `flagSupport` narrows it to fields known in that capture and names
unsupported reasons, verification and live context requirements. Top-level
attachment/command readiness still applies.

Plugin, drum, keygroup and audio solo/arm require model and engine-copy agreement.
MIDI/CV commands qualify native requested state only and return model verification
with a null audio-side value. Physical output remains unmeasured. Legacy clip and
unknown flags remain unavailable, and buses remain metadata-only. `silenced`
never implies audio activity or audibility.

Trusted clients must recompile for primitive ABI 2, executor ABI 7, command ABI 5
and snapshot ABI 4 (`STATE_V4` / `MPC_ADAPTER_STATE_V4`). Old layouts are rejected;
receipts carry an ABI/size prefix. The JSON API version remains 0.1. Updated strict
schemas and examples are in `native-api/schema` and `native-api/examples`.

US #50 extends flag commands to normal sequence playback, initially with
recording/overdub and automation Off. `describe` names transport states, excluded
contexts and immediate model timing semantics. A command receipt includes retained
monotonic acceptance-to-dispatch, dispatch, verification and total durations.
Acceptance does not reserve a musical position; model completion and engine-copy
readback do not establish audibility or sample timing. See the
[qualified subset and performance limits](native-playback-flags.md) and
[handler semantics](../native-api/adapter/COMMANDS.md#playback-and-timing-us-50).
