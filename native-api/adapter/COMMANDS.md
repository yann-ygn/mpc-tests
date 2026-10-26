# Native command handlers (#19)

`libmpc_native.so` now provides the transport-independent command API in
[`mpc_commands.h`](include/mpc_commands.h), ABI 2. It runs on executor ABI 5 and primitive adapter ABI 1.
The six handler exports are `mpc_commands_open`, `mpc_commands_submit`, `mpc_commands_get`,
`mpc_commands_shutdown`, `mpc_commands_close`, and `mpc_commands_receipt_json`.
The executor additionally exports a non-mutating `mpc_executor_poll` and the UI-only native policy helper
`mpc_executor_qualify_command`. US #46 expands observations to 136 user tracks and versions command ABI 2 / executor ABI 5.
Recompile trusted callers; configurations using the earlier ABIs are rejected.

[US #32](../../findings/native-service.md) binds the public Unix setters and `get_command` in the persistent
host. The socket authenticates callers by peer UID and validates strict schemas before enqueueing.
This library opens no sockets. Its production `mpc_executor_qualify_command` callback checks live native
Main/no-popup, stopped transport, recognized solo/arm settings, target automation Off and global mute scope.
The private lab can select this same policy with `LIVE_COMMAND_OPEN`; `LAB_COMMAND_OPEN` retains the older
explicitly manual experimental policy.

## Host contract

Open an authoritative executor, wait for readiness, then open one handler authority for it. Copy the current
service `run_<32 hex>` instance ID into its config. Restart/reattachment requires a fresh service instance and
new handles. The handler borrows its executor; close handlers before closing the executor. Executor close refuses
while a handler authority is attached. Only authoritative executors can host these commands.

A `mpc_command_policy.qualify` callback is mandatory for executing writes. It runs on the verified UI thread
at initial dispatch and every later verification, before native access. The host must establish current mode/settings
from validated live sources, reject playback/automation/Clip/Song and unqualified contexts, and reject mute when
per-sequence mute is enabled. A saved setting, session label or null callback cannot establish production
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
rejects unreadable, unsupported or changing values before any setter or no-op.
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
preserve the reference rules. Normal plugin/drum families use the existing notifying setters and record-arm filter.
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

Before/after compact native observations report best-effort selection, peer requested/audio flags and suppression
changes, with project/track IDs and field paths. Single follow-solo can select a target and clear its peer;
Single arm can change selection and peer arms; Multi settings preserve the native behavior from #31. These
observations are not a complete causal history, isolation guarantee or promise to preserve selection.

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
