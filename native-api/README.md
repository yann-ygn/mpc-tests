# Native API: local state and command service

The implementation of the [accepted 0.1 contract](../findings/native-api-contract.md).
`describe` reads device-tree metadata, GNU ELF notes and verified adapter attachment status. With an
authoritative attachment, state queries capture current native objects through the UI executor.
The [persistent addin](INSTALL.md) also binds `set_mute`, `set_solo`, `set_record_armed` and `get_command`
to the retained [flag handlers](adapter/COMMANDS.md), with Unix peer authentication and a native live
mode/settings policy. [Local watch/replay](WATCH.md) is device-qualified for eight
subscribers on the declared eight-track workload and advertised when its embedded
backend is ready. The standalone binary can discover and read through
a temporary lab attachment; it cannot forward commands to the lab's diagnostic socket.
See [state qualification](../findings/native-state.md) and [command service qualification](../findings/native-service.md).

The local transport is a restricted Unix stream socket. Each connection carries **one compact JSON request plus
a newline**, then one JSON response plus a newline. Read through the response newline and close the connection.
`watch` instead keeps the connection open for newline-delimited frames and accepts `unwatch` to close it.
HTTP and MCP adapters follow in their own stories; this service has no TCP listener.

## Source layout

- `src/main.c`: command-line parsing and validation.
- `src/server.c`: socket ownership, peer admission, bounded client I/O and shutdown.
- `src/client.c`: the `request` command's delivery and response relay.
- `src/request.c`: bounded operation-specific JSON validation; `src/protocol.c` routes operations and writes discovery JSON.
  `src/discovery.c` reads process metadata and binds the command authority to the current service instance.
- `src/state.c`: validated snapshot serialization/projections; `src/attachment.c` checks the MPC peer and
  performs bounded metadata/state transfers.
- `src/watch.c`: shared observation worker, retained snapshot revisions, bounded replay and subscription recovery.
- `src/host.c` and `src/addin.c`: persistent in-process attachment, startup gate, lifecycle and management socket.
  `package.py` builds the checked ARM package with the shared addin installer; [installation guide](INSTALL.md).

`transport.h` is the private CLI/transport boundary; schemas cover discovery, state operations, flag requests, retained lookup and receipts.

## Build and test

```sh
uv sync --locked
uv run --no-sync meson setup builddir/host
uv run --no-sync meson compile -C builddir/host
uv run --no-sync meson test -C builddir/host --suite native-api --print-errorlogs

# ARM, using the pinned Zig compiler:
uv run --no-sync meson setup builddir/arm --cross-file build-support/cross/mpc-armhf.ini
uv run --no-sync meson compile -C builddir/arm
```

Zig's `ZIG_GLOBAL_CACHE_DIR` and `ZIG_LOCAL_CACHE_DIR` can be set to writable directories if its default cache
is unavailable. Builds go to ignored `builddir/{host,arm}/native-api/mpc-api`; they never deploy or restart MPC.
JSON Schema validation is included in the development environment. See the [root build guide](../README.md)
for requirements, all targets and dependency checks.

The host tests use synthetic proc/device-tree fixtures, including ELF32 and ELF64 note sections at unrelated
file offsets. They cover identity changes, firmware/model mismatches, malformed requests, connection bounds,
slow/disconnected clients, socket ownership/permissions and clean shutdown. `--root` is a server startup option
for these fixtures, never a public API argument.

To upload and smoke-test the ARM binary in a fresh private device directory:

```sh
python3 native-api/device_smoke.py --output build/native-api/device-smoke.json
```

This cleans up afterwards. Add `--keep` to leave the temporary service running for further requests.
Add `--state` with an existing authoritative attachment to check all read operations, compare them to the
independent observer and measure repeated native captures.
The report records its directory, socket and service PID. This does not install a startup job or addin.

## Start and query

On the machine running the compiled binary:

```sh
mkdir -m 700 /tmp/mpc-api
./mpc-api serve --socket /tmp/mpc-api/api.sock --device-id dev_studio
```

Run the request from another shell as the same user:

```sh
printf '%s\n' '{"apiVersion":"0.1","op":"describe"}' |
  ./mpc-api request --socket /tmp/mpc-api/api.sock
```

For a service left by the smoke test, use its reported directory in place of `.` and its reported socket path.
Over SSH, pipe the same request into the remote binary's `request` subcommand. Use the SSH options already
established for this device (`-F /dev/null`, batch mode, strict host-key checking).

`request` returns a nonzero exit code for delivery errors. A delivered API error is JSON in stdout with a zero
delivery exit code; inspect its `error.code`. Server shutdown uses SIGTERM or SIGINT and removes its own socket.
It refuses an existing socket/file rather than unlinking another service's path. Remove its temporary directory
after shutdown when no longer needed.

The configured `deviceId` must stay the same when this installation restarts. It is not derived from the hardware
serial. The smoke test stores its generated value in `device-id` beside the temporary executable. The opaque
`instanceId` stays stable between queries, changes on service restart and rotates on observed MPC process or
executable/model identity changes. Observed attachment identity/loss also changes the service instance.
State responses issue lifetime-bound project/track references for commands.

## Response semantics

- `firmware.model`: device-tree model, or `null` when unavailable.
- `firmware.buildId`: GNU build ID read from the live process's executable, or `null` when not reliably readable.
- `firmware.version`: version mapped from that exact known build ID, with `versionSource: build_profile`.
  An unknown build never inherits the research profile's version.
- `readiness`: distinguishes a matched research profile from unsupported/unknown firmware, and separately reports
  `nativeAdapter: not_attached` or `attached`. Status is `discovery_only`, or `ready` when authoritative
  snapshots are attached. Verified attachment metadata includes its ABI, incarnation ID and `bounded_lab_proof`
  qualification; clients still consult each operation's availability.
- `capabilities.describe.available`: true even when MPC is absent or its firmware is unsupported. Read operations
  become available only through an authoritative snapshot attachment. The embedded host enables command capabilities when its handler authority is ready; `conditions` lists live dispatch constraints.
  Streaming stays unavailable. Unsupported firmware and missing attachments fail closed.
- `readableFields`: qualified native state fields exposed by the attachment; empty when reads are unavailable.
- `limits`: enforced frame/connection limits. Native queue/receipt capacities are zero and command timing values
  are null while command operations are unavailable. Before enabling commands, receipt retention must meet the
  contract's ten-minute minimum.

Connections require a private parent directory owned by the service user, a socket with mode `0600`, and matching
Unix peer UID. Requests are limited to 1024 bytes excluding the framing newline; there are at most 16 active
connections. A request must finish within 1000 ms of acceptance, even if it trickles in. These are IPC resource
bounds, not native command execution limits. Each operation has a strict schema using bounded ASCII identifiers and ordinary JSON escaping. Duplicate or extra fields,
invalid types and unsupported API versions produce structured errors before native enqueueing. Unavailable operations return `NOT_READY`; unknown operations return `UNSUPPORTED_OPERATION`.
The response limit is 1 MiB. Snapshot transfer has a 200 ms total deadline and fails closed on malformed,
partial or stale data. Snapshot reads never serve a cached fallback.

The schemas are [request](schema/describe-request.schema.json) and [response/error](schema/describe-response.schema.json).
Requests reject unknown fields; responses permit additive optional fields as the contract requires.
Concrete examples are [request](examples/describe-request.json) and [response](examples/describe-response.json),
using illustrative installation/instance IDs. An [attached response](examples/describe-attached-response.json)
shows the adapter metadata while state/write capabilities remain unavailable.

## Device verification

Verified on 2026-10-24 against the running MPC Key 37 / 3.9.1.2, build ID
`8371199f945ecc40f79cb19c17ff5de72810a91c`. The ARM service read the model/build, preserved its instance ID between
queries, rejected unsupported versions/operations, and kept its socket at `0600`. MPC's PID stayed unchanged.
Process discovery uses the executable path: the actual main thread is named `MPC Main Thread`, so its thread name
cannot identify the process reliably. Host fixtures cover that case.

## Read current state

Enable the temporary authoritative host without installing an addin or restarting MPC:

```sh
python3 tools/mpc_native_lab.py --identities --keep --output build/native-api/state-attachment.json
```

Then send the same two-field request with `op` set to `get_state`, `list_tracks`, `current_track`,
`current_sequence`, or `recovery_snapshot`. `get_state` and `recovery_snapshot` both return a full fresh snapshot.
In the embedded host each read publishes a retained full baseline through the shared
publisher. Its `instanceId:revision` cursor can initialize [watch](WATCH.md) without
silently losing observations during setup. Standalone lab reads retain per-read
revisions without watch support. Capture intervals describe observations, not a native event log.
`current_track` and `current_sequence` project a full capture onto its selection; null selections return null.
`list_tracks` returns every user track, including audio, in canonical Track List order,
and a separate master-object metadata list in `buses` (submixes, returns and input/output objects).
Each user track has a zero-based `uiIndex`. `index` preserves the existing MIDI-triggered
collection index; audio indices are local to the audio collection. `collection` is
`midi_triggered`, `audio`, or `master`, so an index alone is neither unique nor a target.
Master entries have `uiIndex: null` and no writable reference. Rename/reorder preserves
track IDs; deletion, address reuse and project reload invalidate the old incarnation.
See [the native inventory map and device evidence](../findings/native-track-inventory.md).

The combined API limit is 136 user tracks (128 MIDI-triggered and 8 audio), 128 master
objects and 128 sequence slots. Discovery advertises these inventory limits and
`snapshotAbiVersion: 6`. The private transfer requires `STATE_V6`; older attachments
remain incompatible. No cached or partial list is published as a complete capture.

Plugin, drum, keygroup and audio tracks expose independently qualified requested flags,
solo/arm engine copies and effective `silenced` state. MIDI/CV tracks expose requested
mute/solo/arm only; engine copies and `silenced` remain null. Legacy clip/unknown tracks
and buses retain null flags. Project name, actual audio activity and effective transport
tempo remain null. Neither `!silenced` nor command completion establishes audibility.

Discovery's `flagFamilies` describes firmware qualification independently of attachment
readiness. Each track's `flagSupport` narrows it to that capture's known fields. Both
provide per-operation verification and context conditions, unsupported-field reasons and
`outputVerified: false`. `available` in this matrix is conditional qualification; callers
must also consult the top-level command capability and satisfy the listed live conditions.
MIDI/CV completion uses `verification.by: ["model"]` and `audioSideValue: null`.
Physical MIDI/CV output was not measured; no external endpoint was connected.
See [the family matrix](../findings/native-family-flags.md).
A sequence's `bpm` is its own property; `used` comes from the native usage flag rather than its name or slot.
Unsupported fields are identified explicitly. A supported field read failure returns `READ_FAILED`; three
unstable topology/identity/selection attempts return `STATE_UNSTABLE`.

Example: [full state response](examples/state-response.json).
Schemas: [state request](schema/state-request.schema.json), [state response](schema/state-response.schema.json).
A successful `describe` advertises each read independently of the still-unavailable write/watch operations.
The public service instance scopes all returned references; the adapter incarnation remains private attachment
metadata. No heap addresses appear in state responses.

Stop and unload the temporary host after testing:

```sh
python3 tools/mpc_native_lab.py --detach build/native-api/state-attachment.json \
  --output build/native-api/state-detached.json
```

On 2026-10-25 the read smoke matched an independent observer on the actual device. Twenty native captures
had p95 9.058 ms for two normal tracks, 28 buses and 128 sequence objects. A Sounds-mode controller mute edit
was visible immediately and restored. See [sanitized evidence](../findings/native-state-proof-20261025.json).

## Managed command handlers

US #19 provides the [typed command API](adapter/include/mpc_commands.h),
[integration guide](adapter/COMMANDS.md), [domain request schema](schema/command-request.schema.json),
and [receipt schema](schema/command-receipt.schema.json). The handler retains caller-scoped outcomes,
deduplicates retries, checks expected values at UI dispatch, verifies required native/audio readbacks,
and reports observed selection/peer changes. It keeps running work alive across client waits/disconnects.

The public Unix binding uses the same receipts and lifecycle rules. Only the embedded host supplies its
production live policy; the older manual lab policy cannot enable public commands.

## Send flag commands

Use a fresh `target.ref` from `list_tracks` or `get_state`, an application-chosen request ID and an explicit
boolean. See the [setter example](examples/command-request.json), [setter schema](schema/command-request.schema.json),
[lookup example](examples/get-command-request.json) and [lookup schema](schema/get-command-request.schema.json).
For example, with real instance/project/track IDs substituted:

```json
{"apiVersion":"0.1","op":"set_solo","requestId":"solo_1","target":{"instanceId":"run_0123456789abcdef0123456789abcdef","projectId":"1","trackId":"2"},"value":true,"expected":{"value":false},"waitMs":500}
```

Read the returned receipt's `status`, `execution`, `nativeInvoked`, `verification` and `error`.
`accepted` means retained work is pending; `applied` requires native readback, plus the family's
qualified engine copies for solo/arm. Mute and MIDI/CV commands verify the model only.
`unknown` preserves execution uncertainty.
`get_command` takes `instanceId`, `requestId` and optional `waitMs`; it never executes a command.

The native live policy permits **Main, Mixer or Track Mute, no popup, stopped or normal sequence playback, recording/overdub Off, automation Off or qualified Read**, on qualified plugin, drum, keygroup, MIDI, CV and audio tracks.
Mute requires the live **Track Mute State Per Sequence** preference to match the requested scope. Single/Multi solo and arm settings are
read live and must be recognized. Native follow/filter effects are retained and reported in `observedChanges`.
The policy re-resolves owners and samples its context twice at dispatch, before native invocation and readback,
and around each owned verification snapshot. Its initial context is retained through delayed audio verification.
Unsupported or unreadable contexts reject even no-ops. A context change before native invocation rejects; after
a filter/setter it retains `unknown`. A queued command cannot reuse earlier eligibility.
Playback requests use immediate requested-model changes at UI dispatch; mute is not beat/bar quantized. Receipts distinguish acceptance, dispatch and verification with retained monotonic durations. Recording/overdub (including pending), automation Write/unknown, Clip and Song remain unavailable. See [playback qualification and limits](../findings/native-playback-flags.md).

`describe` lists the qualified `screens` and the separate transport/recording/automation/settings/mute-scope `conditions`.
Mixer Volume, Pan & Volume, Sends, Effects and I/O pages address the same model tracks; commands do not navigate
the UI. Pad Mixer, Song, Clip and other views/popups remain unqualified. See [screen qualification](../findings/native-command-screens.md).
Under Automation Read, mute, solo, volume and pan can make actual desired-state changes through qualified live native sources. Both mute scopes qualify independently; volume and pan keep global scope and their mixer generation. Single solo requires qualified Off/Read user peers. Expected values precede no-op detection; verified no-ops invoke neither setter nor arm filter. Unknown sources reject an actual write with `AUTOMATION_SOURCE_UNQUALIFIED`; source replacement during a command uses `AUTOMATION_SOURCE_CHANGED`; an unqualified Single peer uses `AUTOMATION_PEER_UNQUALIFIED`. Later automation can supersede an applied manual value, or prevent verification and leave an invoked command unknown. Write/unknown modes remain gated, and sends/inserts remain Off-only. Commands preserve Read and timeline data. See [native Read routes](../findings/native-automation-read-writes.md) and [pan qualification](../findings/native-automation-read-pan.md).

### Sequence-scoped mute

Omitting `muteScope` preserves global mute semantics. Explicit `"muteScope":"global"`
is canonically identical and requires per-sequence mute to be disabled. It cannot carry
`sequence`. Only `set_mute` accepts these fields.

For sequence mute, send `"muteScope":"sequence"` and copy a complete `sequence` reference
from `selectedSequence.ref` (or `current_sequence.sequence.ref`). It contains `instanceId`,
`projectId`, `sequenceId` and `generation`; the instance/project must match the track target.
See [the request example](examples/sequence-mute-request.json). All handles and generations
are decimal strings. A sequence reference is never a slot number or a sequence name.

The named sequence must remain active, with per-sequence mute enabled, at dispatch,
every native traversal and verification. A live but inactive reference returns
`SEQUENCE_NOT_ACTIVE`; a retired/missing generation returns `STALE_SEQUENCE`; a wrong
preference returns `MUTE_SCOPE_MISMATCH`. Malformed/inconsistent references return
`INVALID_ARGUMENT`. No command selects a sequence or changes preferences. Inactive-sequence
editing has no qualified direct native path. A sequence change while queued rejects before
the setter; a change/lost scope after invocation retains `execution: completed`,
`nativeInvoked: true`, and `status: unknown`. Retry the original request ID or use `get_command`
to recover the retained result; neither operation follows the newly selected sequence.

All authoritative read projections include `muteContext: {scope, sequence}`. The context
labels `tracks[].requested.muted`, and `sequence` is the complete active sequence reference
when scope is `sequence`, otherwise null. No active project gives a null scope. Reads cover
only the active sequence's live Program mute; inactive-sequence and simultaneous independent
global/sequence copies are unavailable. `silenced` remains separate effective suppression,
including solo effects; `audioSide.muted` stays null. Sequence metadata still lists all slots.
Read requests accept no sequence selector. This API does not claim audible completion.

Mute receipts echo canonical `muteScope` and `sequence`, including while pending or rejected.
`verification.muteContext` labels its requested-value observation; mute entries in
`observedChanges` carry the same context. Mute changes across different scopes or sequence
generations are not compared as if they were edits to one property. Expected-value checks
and desired-state no-ops use the addressed live scope and still require full qualification.

US #50 adds command ABI **5** with retained receipt timing; recompile trusted command callers.
Primitive ABI **2**, executor ABI **7**, snapshot ABI **4** and private
`STATE_V4` / `MPC_ADAPTER_STATE_V4` retain US #49's layouts and independent known-field bits.
Command observations retain family qualification. The C export set and public JSON
API version 0.1 remain unchanged. Package 0.7.0 contains this increment.
See [playback qualification and device restoration evidence](../findings/native-playback-flags.md).

Project and track IDs are decimal strings representing nonzero unsigned 64-bit values, at most
18446744073709551615. Names, indices, addresses, numeric coercions, caller IDs and revision preconditions are rejected.
`expected.value` is checked against the live native property immediately before the setter, before the no-op check.

The authenticated caller is the Unix peer **UID**, obtained with `SO_PEERCRED`. Same-UID processes share a retry
namespace across connections and PIDs. Caller identity cannot be supplied in JSON. Request IDs are scoped to that
caller and service instance. A canonical retry returns the retained result; changing operation, reference, value,
expectation, mute scope, sequence identity/generation or start budget gives `REQUEST_ID_CONFLICT`. Changing only `waitMs` does not re-enqueue or renew work.

There are 16 pending slots and 128 retained receipts. Retention is at least 600,000 ms from acceptance, and pending
commands remain retained. Pressure returns `QUEUE_FULL` or `RETENTION_CAPACITY` without evicting live receipts.
`startWithinMs` defaults to 1000, ranges from 1 to 30000, and bounds the original monotonic queued-start deadline.
`waitMs` defaults to 0 and ranges from 0 to 30000. Delivery waits are polled independently of socket admission;
a waiting or disconnected client does not block other clients or cancel native work. Receipt lookup after expiry
or a new authority returns `OUTCOME_UNAVAILABLE`. Retention is in memory and does not survive restart.

Shutdown first stops and joins socket callers, then cancels/drains command handlers, then detaches/drains the
executor. A stalled native callback retains its authority and reports draining; a timeout cannot free it.
See [implementation evidence](../findings/native-service.md) for fixture, sanitizer, ARM and live-device coverage.

## UI work and incremental observations (US #52)

Package 0.8.0 replaces full-project command observations with live user-track
flag samples. `get_state`, `list_tracks`, `current_track`, `current_sequence` and
`recovery_snapshot` remain authoritative full reads. Public watch is still
unavailable; its shared native sampling primitive now exists in executor ABI 8.
Trusted C callers must rebuild for executor ABI 8 / command ABI 6; primitive ABI 2,
snapshot ABI 4 and STATE_V4 remain unchanged.

Receipts add `observationCoverage` and retained `timing.maxUiCallbackUs` /
`timing.stagesUs`. Coverage is explicitly flags-only, with sampled intervals,
topology epoch and duration; missing rows/unknown fields cannot establish
unchanged state. Completion still uses the required live family fields and
context fence. US #51 subsequently qualifies Read as described above.
See [native integration](adapter/COMMANDS.md#lightweight-command-observations-us-52)
and [device measurements and limits](../findings/native-ui-cost.md).

## Mixer extension (US #39)

The five desired-state operations `set_volume`, `set_pan`, `set_send_level`,
`set_insert_rack_enabled` and `set_insert_enabled` extend the same authenticated
Unix command/receipt path. Numeric values use normalized 0–1 units; pan is centred
at 0.5. Requests require the generation in the track's `mixer` observation, plus
`sendIndex`/`slotIndex` where relevant. Expected comparisons use native binary32.
Plugin, drum, keygroup and audio qualify under Automation Off on Main/Mixer/Track
Mute, stopped or ordinary sequence playback. MIDI/CV mixer controls, bus setters,
empty-slot enables and Automation Write remain unavailable. US #53 additionally qualifies
volume under Read; US #54 also qualifies pan. Sends and inserts remain Off-only. Completion is
model-only; no engine-buffer or audibility claim is made.

See [the full contract, native paths and evidence](../findings/native-mixer-controls.md).
All authoritative reads and lightweight observations include per-field known/null
mixer coverage. Trusted C layouts change to executor 9, commands 7, observation 2,
snapshot 5 / STATE_V5; primitive ABI 2 stays unchanged. Rebuild C integrations.


## Transport and selection (0.14.0 / US #40)

Conditional authenticated commands now include `play`, `stop`, `play_start`,
`locate`, `select_track`, `select_sequence` and `set_sequence_bpm`. Play/stop
request desired states. Play Start, locate, sequence selection and stored BPM
writes require stopped playback. Recording/pending record, Song and external
transport remain gated. Track and Sequence references, dispatch checks and
retained request IDs apply equally to navigation; changing a UI view is not a
write prerequisite. See [wire details](adapter/COMMANDS.md#transport-and-navigation-us-40)
and [qualification](../findings/native-transport-navigation.md).

Full state adds a `transport` object: model playing, record mode/recording,
absolute pulse, stored sequence BPM, master BPM and master-tempo enabled.
Effective BPM, tempo source and queued sequence remain null and explicitly
unsupported. These fields are snapshots, not lightweight watch change fields.
`snapshotAbiVersion` is 6, private transfer STATE_V6; executor and command ABI
versions are 10 and 8. C consumers must rebuild. Primitive/observation ABIs stay
2. No new version is installed by the build or qualification runner.

Example using references from `get_state`:

```json
{"apiVersion":"0.1","op":"locate","requestId":"locate_1","target":{"instanceId":"run_0123456789abcdef0123456789abcdef","projectId":"1"},"sequence":{"instanceId":"run_0123456789abcdef0123456789abcdef","projectId":"1","sequenceId":"14","generation":"14"},"pulse":"0","waitMs":1000}
```

For `set_sequence_bpm` replace `pulse` with `bpm: 120`. For `play`, `stop`,
`play_start` and `select_sequence`, omit both fields. `select_track` uses the
full Track target and omits `sequence`. No navigation request accepts `value`.


The [navigation request](examples/navigation-request.json) and
[applied receipt](examples/navigation-applied.json) show the retained Play Start
shape. Native `recording` means playing with a nonzero record mode, including
pending count-in. Modes 1/2 distinguish active Record/Overdub from pending 3/4;
it does not report whether an audio buffer or event was written.
