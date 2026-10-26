# Firmware adapter ABI 1 and UI executor ABI 5

Taiga #29. `libmpc_native.so` contains firmware-specific state and property primitives extracted from the temporary
control lab. It has no constructor, socket, script frontend or public command endpoint. Its optional #30 executor
owns one scheduler thread after explicit open. Addresses in the trusted primitive C interface never become SDK
handles; executor requests carry opaque identities and generations.

## Guards and interface

The runtime backend checks 32-bit ARM hard-float/little-endian ABI, the device-tree model and the executable's
GNU build ID. It verifies the profiled code fingerprints before opening an adapter and again before target access
or native calls. These cover setters, dispatch, destruction, the application owner and sequence lifecycle
notifications. A code/read failure latches it unavailable until closed and reopened.

Private profiles in `tools/profiles/` use schema version 1. The generator rejects mismatched build/version/ABI,
unqualified families, invalid offsets, duplicate functions and bindings without fingerprints. They describe the
Key 37 / 3.9.1.2 research profile, including layouts and setter/filter paths. This is bounded lab qualification;
matching the profile is not release qualification for every MPC mode.

The [primitive header](include/mpc_native.h) defines the original eight functions:

- `mpc_native_open`, `mpc_native_close`, `mpc_native_get_status`
- `mpc_native_read_track`
- `mpc_native_set_mute`, `mpc_native_set_solo`, `mpc_native_set_record_armed`
- `mpc_native_error_name`

ABI 1 layouts/signatures are fixed; an incompatible future ABI needs separate entry points or a new library
SONAME. The build rejects other exported symbols, including the private fixture backend. The deployment SONAME
is `libmpc_native.so`; the lab host resolves it beside itself using `$ORIGIN`, never a workstation path.

Opening without a target validator permits status checks only. Every target operation must run on the MPC
main/UI thread and call a trusted `validate_target` hook immediately before traversal. The authoritative executor supplies this hook and checks
the active project, adapter instance and object generations. The adapter then checks vtables, normal-track ownership,
vector bounds/uniqueness, Program association and family. Primitive calls consume a resolved target; the authoritative executor performs root resolution and
handle creation. A missing hook or stale target fails before any property call.
`mpc_native_target` is a resolved view for one call: queue public handle IDs, resolve them at dispatch, and keep
the command's expected generations in the executor context consulted by the validator. Never queue raw targets.

The retained lab provides a private target registry with an ownership-only check, explicitly weaker than the
required production registry. It is not used to enable public commands. Direct primitive users must drain every
user before closing an adapter. The executor owns and closes its own adapter.

## UI executor (#30)

The [executor header](include/mpc_executor.h) uses ABI 5; ABI 1/2/3/4 configurations are rejected at open.
The primitive ABI remains 1. Requests support track reads/flags, identity snapshots and sequence-reference
validation and full live state snapshots. The authoritative mode owns the resolver; the original
host-resolver mode remains available for trusted experiments. Requests carry no arbitrary callback, native function address or raw target.

`mpc_executor_open` copies the configuration, opens its guarded adapter and starts a scheduler. A startup probe
uses the verified `callAsync` landmark and exact 16-byte GNU ARM function-object layout. Every callback checks
that its TID equals MPC's process/main-thread ID before model access. Until that probe runs, submissions return
`NOT_READY`. `wait_ready` uses a monotonic, bounded deadline; a failed/rejected/discarded probe is observable and
the instance must still be shut down and closed.

The host chooses 1–128 fixed job slots. Queued, running and completed-but-unreleased results all consume slots;
saturation returns `QUEUE_FULL` without accepting another request. Concurrent acceptance is serialized under a
short lock and requests execute in FIFO order on the UI thread. Each UI message processes at most the configured
capacity. Only one native callback capture group can be outstanding, including retained copies after invocation.
Cancelling/releasing jobs cannot grow JUCE's queue. The scheduler posts the next batch only after all copies of
the previous group are destroyed. A discarded callback fails pending work rather than silently retrying it.

Requests can carry a monotonic queue deadline relative to acceptance. A wait can shorten that deadline, and the
UI callback checks it before resolving a target. Expired queued work is cancelled even if its waiting client has
not been scheduled to observe the timeout. Wait/lifecycle durations and explicit queue deadlines are bounded to
30 seconds; running operations are never interrupted or rolled back.

The resolver runs when the UI callback starts each job, and again immediately before each adapter traversal,
including post-setter readback. The adapter compares the fresh resolved target against the current call's view
and retains all existing ownership, family, firmware and precondition guards. Native targets live only for that
UI call. Resolver userdata must remain alive until successful executor close.

| Event | Result and ownership |
|---|---|
| Deadline/cancel before execution | `CANCELLED`; no target resolution or native call occurs |
| Deadline/cancel after execution starts | `OUTCOME_UNKNOWN`, job remains `RUNNING`; wait again for the eventual result |
| Client releases queued work | Removed from the queue, slot reusable immediately |
| Client releases running work | Detached; callback retains the slot until execution returns |
| Completed work | Result retained until the client releases its handle |
| Shutdown | Stop acceptance, cancel queued work, wait for callbacks/captures and scheduler exit |
| Shutdown deadline | `TIMED_OUT`; keep the instance mapped and retry shutdown later |
| Close with callbacks, worker or handles remaining | `NOT_READY`; nothing is freed |

Final observation/write fields are meaningful only for `COMPLETED` results. A running job with an unknown outcome
does not claim whether a setter has begun. An internal successful setter still means requested-model readback,
not audio propagation or a public applied receipt. All client API use is for non-realtime threads. Blocking waits,
shutdown and close reject the UI thread. The callback and resolver perform no client I/O or condition waits;
posting, startup waits and lifecycle coordination belong to the scheduler/client workers.

Before closing, stop/join API callers and release every job handle. Successful shutdown/close establishes data
ownership draining, but the host must also establish **external UI quiescence before `dlclose`**: a final capture
destructor can still be returning through DSO instructions after decrementing its count. The lab loader checks
that both lab and executor workers have exited and that the main UI thread has returned to its normal poll loop.
If callbacks never drain, keep the DSO mapped; no forced unload is supported.

## Primitive results

Reads distinguish requested flags, separate audio-side solo/arm copies and effective silencing. There is no
separate qualified audio-side mute copy. Plugin and drum are the only accepted target families.

Setters compare the optional expected value, skip already-desired values and invoke MPC's notifying property
setters. Record arm first calls the validated native filter; its manager must exist and its invoker must match
the profile. The adapter never stores directly into MPC's flag bytes; its memory descriptor is read-only.

An internal `OK` means model readback matched the desired value. It is not a public `applied` receipt: audio-side
propagation can still be pending. Results include observations and whether a native call began; post-call
failures preserve uncertainty. The command layer (#19/#32) must implement completion, timeout and retry rules.
There is no rollback, auto-restoration or undo guarantee.

## Build and verification

```sh
uv sync --locked
uv run --no-sync meson setup builddir/host
uv run --no-sync meson test -C builddir/host --suite native-api --print-errorlogs
uv run --no-sync meson setup builddir/arm --cross-file build-support/cross/mpc-armhf.ini
uv run --no-sync meson compile -C builddir/arm
```

Builds produce `builddir/{host,arm}/native-api/libmpc_native.so`, the service and
`tests/{native_fixture,executor_fixture,identity_fixture}`.
See the [root build guide](../../README.md) for requirements and all targets.
The isolated fixture uses synthetic addresses and private I/O callbacks. No MPC entry point is called by those
tests. It exercises 51 cases, including each fingerprint mismatch, wrong thread, missing validator, stale
ownership, unknown family, expected-value rejection, filtered arm rejection and pending audio propagation.
Python tests also reject 14 malformed profile variants and check exports and the service attachment boundary.

Verified on 2026-10-24: 22 host tests with JSON Schema validation; the 28-case isolated fixture also passed on
ARM. Rebuilding the adapter with the same compiler, profiles and inputs produced identical bytes. Build paths
are normalized in debug information. These checks do not establish behavior across untested firmware, models,
modes or concurrent project replacement.

Verified on 2026-10-25 for #30: all eight host Meson test entries pass (including the existing 22 Python API/adapter
cases, ELF guards, privtap and formatting). The executor fixture also passes with AddressSanitizer and
UndefinedBehaviorSanitizer, and both native/executor fixtures pass as isolated ARM processes on the MPC.
Executor tests pump an isolated fake message queue and exercise delayed/discarded callbacks, capture copies,
queue/result saturation, FIFO/preconditions, simultaneous clients, queued and running disconnects, timeout
uncertainty, stale generations, dispatch-time revalidation, wrong threads, startup/post failures and shutdown.
The [sanitized evidence](../../findings/native-executor-proof-20261025.json) separates these failure tests from
the real JUCE read-only smoke test. This does not establish authoritative project lifetime tracking (#20).

## Active projects and handles (#20)

`mpc_executor_open_authoritative(capacity, &executor)` opens a managed authority. Submit
`MPC_EXECUTOR_IDENTITIES` to capture the current project, all user tracks, sequence slots and selection on the
verified UI thread. The [snapshot header](include/mpc_identity.h) contains IDs/generations and mutable indices,
with no pointers. Copy its `instance_id`, project identity and chosen object's ID/generation into a request.
An optional sequence identity is a precondition on a track request; `MPC_EXECUTOR_VALIDATE_SEQUENCE` validates
one independently. `MPC_EXECUTOR_READ_TRACK` targets the captured track even after selection changes.

The fixed, fingerprinted owner path starts at JUCE's active application, follows its standalone window and
filter, and reads the embedded MpcEngine's owned Project. It is traversed afresh for every resolver call.
Native destruction observers distinguish deletion/recreation at the same address, even between captures.
Project reset-begin/end and data-installed notifications retire the project generation when loading reuses its
Project object; observed reset phases return `NOT_READY`.
Normal-track rename/reorder preserves IDs. Sequence metadata changes preserve IDs while the sequence remains
in use; used/unused transitions and structural reset/content/length notifications retire sequence references
conservatively. In particular, Clear All on the first sequence can keep its object and used flag unchanged.
Those reset notifications must also be observed. Reacquire a sequence reference after destructive edits.

Project replacement or an unreadable/malformed topology retires old references. A new adapter/process lifetime
gets a fresh instance ID; IDs are never reused within it. Old references return `STALE_TARGET` before native
setters. During unavailable model transitions, reads/commands can return `NOT_READY`/`READ_FAILED` instead.
Queued requests resolve at dispatch and every adapter traversal. Socket reconnection alone preserves references
while the same authority survives; service/adapter reattachment requires discovery and fresh references.

Shutdown cancels queued jobs and posts UI-thread observer removal before capture/scheduler drain. Failed or
discarded dispatch, or a still-owned native listener, keeps the authority alive and makes close refuse; retry
shutdown after resolving the failure. The host must still establish external UI quiescence before `dlclose`.
The identity fixture covers forced project/track/sequence ABA, retained old roots, queued stale writes,
reconnection and refused/dropped cleanup. See [lifecycle findings](../../findings/native-identity.md) for device
validation, the conservative sequence rule and a repeatable manual-test runner. Public service reads require
#18’s authoritative snapshot host; writes remain gated by #19/#32.

## Temporary device host

The lab consumes the shared executor and retains its bounded loader and root-only diagnostic socket. Its READ
and PULSE reports contain the requested track's flags. The default address registry remains lab-only.
`--identities` switches to the authoritative resolver and compares handle reads with an independent diagnostic
read of the application-owned root. It can retain an attachment for manual lifecycle experiments.

```sh
uv run --no-sync meson compile -C builddir/arm
python3 tools/mpc_native_lab.py --output build/native-api/adapter-device-unload.json
```

The default test checks the loader on a disposable child, loads the adapter, verifies status, reads flags on
the UI thread, compares them to the independent reader, then drains/stops/unloads both libraries. It performs
no flag write, verifies that observed flags/selection remain unchanged and removes temporary files.

To retain the tested host for discovery, and later remove exactly that attachment:

```sh
python3 tools/mpc_native_lab.py --keep --output build/native-api/adapter-device-retained.json
python3 tools/mpc_native_lab.py --detach build/native-api/adapter-device-retained.json \
  --output build/native-api/adapter-device-detached.json
```

`--keep` cannot be combined with `--pulse`. Detach verifies the recorded adapter instance, drains callbacks and
uses the loader's worker/UI quiescence checks before unloading. It refuses a different attachment. Diagnostic
reports contain private lab details and stay under ignored `build/`. No addin installation or MPC restart occurs.

The service's private status probe requires the peer PID/UID to match the current MPC, the expected ABI tag,
profile/build, a valid adapter incarnation ID and successful code verification. It has a 50 ms total deadline.
Attachment identity/loss changes invalidate the service instance. Public `describe` reports `readiness.adapter`
with `qualification: bounded_lab_proof`. Authoritative mode now advertises the private snapshot transfer
for #18; other lab attachments remain discovery-only. Public write/watch capabilities remain unavailable. Device tests verified loading, typed readback, full unload,
and a retained attachment visible through `describe`, with MPC PID and observed state unchanged.

The #30 read-only smoke test on 2026-10-25 confirmed the executor's callback on UI TID/PID 365, typed readback
matching the independent snapshot, unchanged observed flags/selection and complete unload of both DSOs. The
temporary files were removed and MPC's PID stayed unchanged. No flag write, addin installation or restart was
performed. The private report is `build/native-api/executor-device-unload.json`.

## Flag semantics (#31)

The [device matrix](../../findings/native-flag-semantics.md) extends the initial
plugin/drum proof to selected/unselected targets, repeated desired values, conflicts,
sequence mute scope, Single/Multi arm and solo behavior, and intervening controller
edits/disconnects. The private lab adds a desired-state `SET` diagnostic and reports
completion, native invocation and changed/readback metadata. `SET` has no timed
restoration; the experiment runner owns restoration and verification. It is not a
public endpoint and retains the lab registry's #20 limitations.

A non-completed lab response reports `nativeInvoked: null`; it cannot claim that no
native call began. Completed failures include observations only when known.
The matrix runner preserves failure evidence, checks peer flags and selection, and
aborts on restoration mismatch. Host tests cover those failure paths and manual
edits between queue acceptance and dispatch for every flag. The primitive ABI,
firmware qualification level and public discovery capabilities are unchanged.

## Live state (#18)

`MPC_EXECUTOR_GET_STATE` requires authoritative mode and returns a borrowed
[`mpc_state_snapshot`](include/mpc_state.h) only on successful completion. Keep the job alive while reading
that pointer; release frees its buffer. Allocation occurs during submission, outside the UI callback.
Cancelled/running/failed jobs expose no snapshot pointer. Capacity still bounds outstanding job storage.

The capture reads native metadata, qualified plugin/drum requested/audio flags, bus metadata and all existing
sequences with their usage flag/BPM. It fences project generations, topology, Program associations and selection,
retries instability three times, and fails supported field read errors rather than caching. The result reports
capture interval/duration and attempts. No atomicity or effective transport tempo is implied.
The local service supplies JSON projections, service-scoped references and observation revisions; see
[findings](../../findings/native-state.md) and [service schemas](../schema/state-response.schema.json).

## Managed flag handlers (#19)

The [command API](include/mpc_commands.h) adds retained, idempotent desired-state handlers over an authoritative
executor. It checks expected values at dispatch, verifies solo/arm audio copies, reports peer/selection changes,
and preserves accepted/unknown outcomes across disconnects. See [integration and lifecycle](COMMANDS.md),
[domain schemas](../schema/command-request.schema.json), and [device findings](../../findings/native-commands.md).
A required UI-thread host policy gates unqualified contexts; the private lab policy is manually qualified.
US #32 now supplies [public Unix write/receipt binding and production live qualification](../../findings/native-service.md).
The production policy is exposed by the additive UI-only `mpc_executor_qualify_command` helper.

## Mixed track inventory (#46)

Executor ABI 5 expands identity snapshots to 136 user tracks. Snapshot ABI 2 adds
collection metadata and preserves the distinction between the legacy MIDI-triggered
index and canonical user order. Command ABI 2 expands receipt observations to the
same capacity; recompile trusted hosts against these headers. Old executor/command
configurations are rejected at open. Primitive adapter ABI 1 and its eight layouts
remain unchanged. The private transfer and attachment tag are `STATE_V2` and
`MPC_ADAPTER_STATE_V2`; an older snapshot attachment cannot enable state reads.

The registry uses the native normal/user collection at TrackPool `+0x74`, including
all audio tracks. The profile's historical `normal*` offsets at `+0x44` describe
MIDI-triggered tracks. Master objects come from `+0x84`, audio from `+0x94`.
Snapshots validate these partitions against all tracks, then fence collection order,
selection, Program pointers and Program types. The 128 MIDI-triggered / 8 audio /
128 master limits bound the API; vector storage may have spare capacity beyond its
current element count. `mpc_identity_object.index` is canonical user order internally;
`mpc_state_track.collection_index` supplies public `index` and the collection enum
supplies public `collection`. Buses have no user `uiIndex` or writable reference.

See [inventory findings](../../findings/native-track-inventory.md) for the native map,
UI evidence and validation. Listing a family does not qualify its flags or setters.
