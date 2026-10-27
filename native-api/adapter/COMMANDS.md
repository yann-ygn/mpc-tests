# Native command handlers (#19)

`libmpc_native.so` now provides the transport-independent command API in
[`mpc_commands.h`](include/mpc_commands.h), ABI 7. It runs on executor ABI 9 and primitive adapter ABI 2.
The six handler exports are `mpc_commands_open`, `mpc_commands_submit`, `mpc_commands_get`,
`mpc_commands_shutdown`, `mpc_commands_close`, and `mpc_commands_receipt_json`.
The executor additionally exports a non-mutating `mpc_executor_poll` and the UI-only native policy helper
`mpc_executor_qualify_command`. US #46 expanded observations to 136 user tracks; US #48 adds
explicit sequence targets/context. US #49 adds per-family observations and versions
command ABI 4 / executor ABI 7 / primitive ABI 2. US #50 adds retained timing in command ABI 5.
Recompile trusted callers; configurations using the earlier ABIs are rejected.

US #51 retains primitive ABI 2, executor ABI 8, command ABI 6, observation ABI 1
and snapshot ABI 4 / STATE_V4: its invocation boundary and context remain private.
The additive error `AUTOMATION_READ_NOOP_ONLY` distinguishes Read mute/solo writes
from Write/unknown-mode rejection (`AUTOMATION_UNQUALIFIED`).

[US #32](../../findings/native-service.md) binds the public Unix setters and `get_command` in the persistent
host. The socket authenticates callers by peer UID and validates strict schemas before enqueueing.
This library opens no sockets. Its production `mpc_executor_qualify_command` callback checks live native
Main/Mixer/Track Mute with no popup, stopped or normal sequence playback, recording/overdub Off, recognized solo/arm settings, target automation Off or qualified Read and a matching explicit mute scope.
The private lab can select this same policy with `LIVE_COMMAND_OPEN`; `LAB_COMMAND_OPEN` retains the older
explicitly manual experimental policy.

## Host contract

Open an authoritative executor, wait for readiness, then open one handler authority for it. Copy the current
service `run_<32 hex>` instance ID into its config. Restart/reattachment requires a fresh service instance and
new handles. The handler borrows its executor; close handlers before closing the executor. Executor close refuses
while a handler authority is attached. Only authoritative executors can host these commands.

A `mpc_command_policy.qualify` callback is mandatory for executing writes. It runs on the verified UI thread
at dispatch, before native filter/setter access and readback, and around every verification snapshot. The host must establish current mode/settings
from validated live sources, reject recording/overdub, automation Write/unknown, Clip/Song and unqualified contexts, and reject mute when
the live mute preference does not match the request scope. A saved setting, session label or null callback cannot establish production
qualification. Missing callbacks reject even no-ops with `NOT_READY`. The lab's manual policy is diagnostic only.
Family/ownership checks remain inside the guarded adapter regardless of host policy.

All client calls except `mpc_executor_qualify_command` are for non-UI, non-realtime threads.
The policy helper is UI-only and must run inside an active managed-command qualification callback. Caller identity is an authenticated host argument, never a
field clients may choose. The same authority can serve several caller IDs without sharing their deduplication keys.
UI hooks must be bounded and nonblocking; their userdata remains alive until successful handler close.

```c
struct mpc_commands_config config = {
  .abi_version = MPC_COMMANDS_ABI,
  .struct_bytes = sizeof config,
  .executor = executor,
  .pending_capacity = 16,
  .receipt_capacity = 128,
  .retention_ms = MPC_COMMAND_MIN_RETENTION_MS,
  .policy = {host_context, qualify_live_context},
};
memcpy(config.instance_id, service_instance_id, 37);
struct mpc_commands *commands;
/* Check every returned error before using an output. */
mpc_commands_open(&config, &commands);

struct mpc_command_request request = {
  .request_id = "req_example",
  .target = {.project = project_id, .track = track_id},
  .operation = MPC_COMMAND_SOLO,
  .value = true,
  .has_expected = true,
  .expected = false,
};
memcpy(request.target.instance_id, service_instance_id, 37);
struct mpc_command_receipt receipt;
mpc_commands_submit(commands, authenticated_caller_id, &request, 500, &receipt);
```

`qualify_live_context` can call `mpc_executor_qualify_command(executor, operation)` to use the exact-firmware
production policy. Supply the executor as callback userdata and keep it borrowed through handler close.
The helper resolves the active request, verifies its target/family, samples native owners/context twice and
rejects unreadable, unsupported or changing values before any setter or no-op. US #47 retains the initial
context through delayed verification: a context transition before invocation rejects; after native invocation
it yields retained uncertainty. The recursion guard allows policy reads to validate target lifetime without
recursively invoking the host callback. No native addresses or context samples enter public receipts.
An API return of `MPC_NATIVE_OK` means that a receipt was accepted/retrieved, not that its command applied.
Inspect `receipt.status`, `receipt.execution`, and `receipt.error`. Pre-acceptance errors create no receipt.
`mpc_commands_receipt_json` returns a heap string owned by the caller, matching the
[receipt schema](../schema/command-receipt.schema.json). The
[domain request schema](../schema/command-request.schema.json) defines strict boolean/handle inputs for the public Unix binding.
See the [service guide](../README.md#send-flag-commands) for delivery and authenticated caller semantics.

## Dispatch and outcomes

The handler resolves public project/track incarnation IDs through #20 on the UI thread. It binds current
generations and resolves again before each adapter traversal. Index/name/address lookup is never a write target.
Old instances fail admission; replaced projects and deleted/recreated tracks fail before a setter. Rename/reorder
preserve the reference rules. Qualified plugin/drum/keygroup/MIDI/CV/audio families use their traced native notifying
properties and Track record-arm filter. The family matrix determines required readbacks;
MIDI/CV completion verifies the requested model value only. See
[family qualification](../../findings/native-family-flags.md).
There is no toggle, scripting call, filter bypass, timed restoration or implicit selection rollback.

`expected.value` is checked against the native requested property in the setter callback, before the no-op check.
A failed expectation rejects without native invocation even if an intervening edit already produced the desired
value. Snapshot revisions are observations, not transaction counters: `has_expected_revision` is rejected with
`UNSUPPORTED_PRECONDITION` before acceptance. This follows the accepted #28 contract and refines #19's original
“expected-state/revision” wording; it does not promise ABA detection or group isolation.

- `accepted`: retained and queued/in progress; `wait_ms` ending does not cancel it.
- `applied`: a native setter or no-op completed and required live readbacks matched. Mute is model-only;
  solo/arm also require their qualified audio-side copies to equal the desired boolean.
- `rejected`: a definite validation/constraint failure. A filter rejection can occur after native execution
  and includes observed readback; it is not rolled back or bypassed.
- `cancelled`: expiry/shutdown before native execution. No setter/filter was invoked.
- `unknown`: execution/required verification could not be established. A running native callback is retained;
  later completion evidence can resolve the same receipt. Native invocation remains null while running.

A lagging audio copy causes bounded read-only verification, never another setter. Reads and qualification revalidate
the identity at each UI dispatch. Read failure, lost identity, or verification timeout after a native call cannot
be promoted to success. Verification does not prove audible output, an audio buffer boundary or persistence.
A verified no-op does not rerun filters/listeners. Successful setters leave the desired state in place.
`applied` verifies the required observation at completion. Later automation can
change that flag; the receipt does not promise a persistent or isolated value.

Before/after compact native observations report best-effort selection, peer requested/audio flags and suppression
changes, with project/track IDs and field paths. Single follow-solo can select a target and clear its peer;
Single arm can change selection and peer arms; Multi settings preserve the native behavior from #31. These
observations are not a complete causal history, isolation guarantee or promise to preserve selection.

## Playback and timing (US #50)

Normal sequence playback and stopped sequence mode use the same desired-state
requests. Recording and overdub, including pending modes, return
`RECORDING_UNQUALIFIED`; automation Write/unknown returns `AUTOMATION_UNQUALIFIED`.
Read permits verified no-ops and native arm writes; mute/solo writes return
`AUTOMATION_READ_NOOP_ONLY`.
Native Clip/other sequencer contexts or an active Song source return
`PLAYBACK_CONTEXT_UNQUALIFIED`. An unsupported screen/popup returns `NOT_READY`.
All checks also apply to no-ops and expected-value conflicts.

Acceptance retains a request; it does not reserve a playback position. The first
UI dispatch establishes the context fence, so a stable start/stop between
acceptance and dispatch is allowed. A sampled start/stop, recorder, automation,
owner, screen or settings transition after that baseline rejects before native
invocation or yields `unknown` after invocation. Delayed verification never
repeats the setter. Explicit sequence mute remains pinned to the named active
generation across loops and sequence changes. Sampling cannot detect a complete
intervening ABA transition or establish an audio-thread transaction.

These setters change the requested model immediately on UI dispatch. API mute
does not use the Track Mute pad's native quantized launch queue. There is no
beat/bar, sample, audio-buffer or audible-output scheduling guarantee. Mute
completion verifies the model in its labeled scope; solo/arm use the family's
model/engine-copy requirements. A matching engine copy is not audible output.

Receipts add a retained `timing` object. `queueMs` spans acceptance to first UI
dispatch; `dispatchMs` measures that complete callback, including observations;
`verificationMs` spans its completion to terminal receipt publication, including
worker scheduling and any later readbacks. `totalMs` spans acceptance to terminal
publication. Null means that stage is not yet established. `uiWorkUs` sums native
callback work and `verificationPasses` counts later read-only callbacks. These
monotonic durations do not timestamp the exact property change. Final retry
results preserve the same timing values.

Each UI message processes at most one executor job, even under replenishing
traffic. One non-preemptible bounded snapshot/command can still exceed a frame
budget; it is never multiplied by the slot capacity in one callback. The public
host retains 16 pending commands, 128 receipts and the existing 500 ms verification
budget. Work and waits stay on the caller/worker/UI threads; the adapter adds no
audio callback, socket I/O or wait to the audio thread. See
[device measurements and limits](../../findings/native-playback-flags.md).

## Delivery and lifetime

Deduplication keys are caller, authority instance and request ID. Canonical comparison includes operation, target,
desired boolean, expected value/presence and normalized start budget. Repeating a key returns its retained receipt
without reapplying, even after a manual edit or experiment restoration. Changing the command under that key returns
`REQUEST_ID_CONFLICT`. Only the delivery wait is excluded; changing it cannot extend the original start budget.

The handler serializes accepted writes in acceptance order, including verification, independently of physical edits.
Its original monotonic start deadline is carried into the executor; allocation, retries and queue residence cannot
renew it. Native access starts only before that deadline. A non-mutating executor poll preserves the budget,
unlike `wait(0)`, which can cancel a queued primitive job.

Limits are 1,000 ms default / 30,000 ms maximum start budget, 30,000 ms maximum delivery wait, a 500 ms verification
budget from UI dispatch, up to 128 pending commands and up to 256 retained receipts. The host configures actual
capacities. Results remain retained for at least 600,000 ms from acceptance; pending results never expire.
`QUEUE_FULL`/`RETENTION_CAPACITY` reject new work rather than evicting an unexpired result. After expiry/restart,
lookup returns `OUTCOME_UNAVAILABLE`. Deduplication is bounded and in-memory, not exactly-once delivery over crashes.

Disconnects require no handler action: the command and receipt belong to the authority. Shutdown stops acceptance,
cancels queued work and drains running jobs/verification. A timeout leaves everything alive; close refuses until
the worker and jobs drain. Stop/join API callers before close, then shut down/close the executor, establish external
UI/worker quiescence, and only then unload the DSO. Failed cleanup cannot free native callback storage.

## Validation

The shared fixture exercises eight concurrent duplicate submissions, retries after manual edits, all three no-ops
and expected conflicts, selection changes/reorder/ABA, project replacement before/after invocation, unsupported
families and absent/failing policies, Single/Multi peer effects, filtered rejection, audio lag/timeout, retained
unknown outcomes resolving later, independent caller namespaces, retention pressure/expiry, queued cancellation
and refused shutdown/close while a native callback is running. Its clock injection is private and not exported.
The same fixture runs on ARM; JSON Schema tests validate its actual receipts for every outcome.

The [device findings](../../findings/native-commands.md) and sanitized evidence distinguish these deterministic
failure cases from real stopped-mode plugin/drum experiments. Reproduce a manually qualified solo/arm run with:

```sh
python3 tools/mpc_command_lab.py --context stopped-sequence-multi-arm-single-solo \
  --output build/native-api/command-solo-arm.json
```

For mute, first disable per-sequence mute in the current UI, then pass `--properties mute --global-mute-confirmed`.
Restore the setting afterward. The runner restores all observed flags/selection, closes handlers, drains/unloads
the temporary attachment and preserves failure evidence under ignored `build/`. It performs no save/load/restart.

## Sequence-scoped mute (#48)

Zero-initialized `mpc_command_request.mute_scope` means `MPC_MUTE_GLOBAL`, preserving
existing calls. `MPC_MUTE_SEQUENCE` requires `operation = MPC_COMMAND_MUTE` and a complete
`sequence` with the service instance, same project, incarnation ID and generation.
Changing any of these canonical fields conflicts with a retained request ID. Other
operations cannot carry sequence scope. The command owner copies scope and sequence
into every initial/verification job; retries never rebind a sequence generation.

The authoritative resolver enforces both sequence lifetime and selection (including the
native selected-sequence mirror) before every traversal. The production policy compares
the request's scope with the live preference. Snapshot and narrow receipt observations
carry scope and the active sequence's generation; verification must match both. A lost
relationship after invocation remains unknown with actual invocation/execution metadata.
There is no inactive-sequence setter and no implicit selection change.

Use command ABI 7, executor ABI 9, snapshot ABI 5 and primitive ABI 2 together.
Older layouts are rejected at open/transfer or receipt serialization. The export set
is unchanged.
The [public service guide](../README.md#sequence-scoped-mute) defines wire shapes and errors.
`tools/mpc_sequence_mute.py` runs the bounded device matrix using the production live policy;
the operator owns lab attach/detach, existing sequences and automation/preference setup.

## Lightweight command observations (US #52)

Command ABI 6 / executor ABI 8 replace the preceding layouts. Primitive ABI 2,
snapshot ABI 4, STATE_V4 and public JSON API 0.1 are unchanged. Commands no longer
allocate a full snapshot. Before/after and delayed verification use the shared
[flag observation](include/mpc_observation.h) primitive, with live requested and
family-qualified engine/silenced flags for all user tracks, plus selection and
active sequence/mute context. All user peers remain covered because native
Single solo/arm and follow-selection effects can span families. Names, bus
metadata, sequence names/BPM/usage are excluded. No-op, expected values, filters,
FIFO, lifetime/Program/context fences, retained retries and uncertainty rules
remain in force. Automation Off still qualifies commands; reads are independent
of command eligibility. US #51 adds the qualified Read subset below.

## Automation Read (US #51)

Read permits matching desired-state requests for every qualified family and mute
scope. All identity, Program/family, screen, transport, scope and required
model/engine checks still apply. `expected.value` is checked before deciding a
no-op. Such requests report `nativeInvoked: false`, `changed: false`; even the
record-arm filter is skipped. A lagging required engine copy keeps the existing
bounded read-only verification and uncertainty rules.
The final native policy read must also agree with the completion snapshot's
required fields. A disagreement schedules read-only verification instead of
acknowledging the earlier snapshot.

Native record-arm writes qualify independently under Read. The Track-owned
RecordArmable/filter/Single-Multi path is outside Program automation parameters.
Read mute/solo writes remain `AUTOMATION_READ_NOOP_ONLY`, including without a
visible lane or with an empty cached event list. Their complete event/native
source ownership has not been qualified. No absence heuristic grants permission.

The adapter marks the boundary before a native filter and again before its
notifying setter. Production policy rechecks live context at each boundary and
after listener/filter work, as well as delayed verification. A mode/owner/scope
change rejects before native invocation or retains uncertainty afterward. The
policy never changes settings, event data or screen to gain permission.

Discovery and per-track/family support add `automation`: Off is
`qualified_writes`; Read is `verified_noop_only` for mute/solo and
`qualified_writes` for arm; Write/unknown are `unsupported`. This metadata is
conditional qualification, not a cached permission for a specific request.
See [native sources and device evidence](../../findings/native-automation-read.md).

`observationCoverage.before` / `.after` are null for absent samples, otherwise
`{kind: "user_track_flags", offset, count, totalTracks, topologyEpoch, durationUs}`.
Commands cover the complete user flag interval; the shared read primitive may
cover only a page. Missing rows or unknown fields never mean unchanged. Changes
compare only matching project/track generations and families with fields known
in both samples. Observation is best effort rather than an atomic causal log.
Topology epochs permit a producer to detect reordered/deleted/replaced objects
while paging; each sample has its own freshness and scope.

`timing.maxUiCallbackUs` retains the longest command callback, including bounded
observer fanout and executor work through publication. `uiWorkUs` sums initial
and delayed command callbacks; coalesced observers do not add native reads.
`stagesUs` provides exclusive identity, policy, filterSetter, targetReadback,
beforeObservation, afterObservation and other durations. Target validation inside
readback is charged to identity/policy, avoiding double counting. Delayed passes
contribute to the same stage totals and increment `verificationPasses`; their
elapsed worker/queue time is separately included in `verificationMs`.
`verificationUiUs` isolates the UI work of delayed read-only passes. Durations
end just before callback result notification; native message/destructor return
and OS scheduling introduce additional overhead. They are no hard real-time or
audio-completion guarantee.

See [measurements, supported limits and restoration](../../findings/native-ui-cost.md).

## Mixer commands (US #39)

Executor ABI 9 / command ABI 7 add volume, pan, four sends, rack enable and four
occupied insert slots. See [the mixer contract and qualification](../../findings/native-mixer-controls.md)
for normalized units, native binary32 expected comparisons, global scope,
Automation Off policy and model-only completion. Every mixer request includes
`mixerGeneration` from a fresh track observation; send/slot requests also include
their zero-based index. Desired numeric values live in `request.mixer.value` in C;
booleans use the existing `value`. Flag requests leave the mixer member zeroed.
Snapshots/observations use ABI 5/2. `MPC_EXECUTOR_SAMPLE_CONTROLS` aliases the
historical `SAMPLE_FLAGS` operation and includes independent `mixer.known` fields.
