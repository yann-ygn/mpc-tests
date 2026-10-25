# Firmware adapter and UI executor, C ABI 1

Taiga #29. `libmpc_native.so` contains firmware-specific state and property primitives extracted from the temporary
control lab. It has no constructor, socket, script frontend or public command endpoint. Its optional #30 executor
owns one scheduler thread after explicit open. Addresses in the trusted primitive C interface never become SDK
handles; executor requests carry opaque identities and generations.

## Guards and interface

The runtime backend checks 32-bit ARM hard-float/little-endian ABI, the device-tree model and the executable's
GNU build ID. It verifies seven 32-byte code prefixes before opening an adapter and again before target access
or native calls. A code/read failure latches it unavailable until closed and reopened. The seventh prefix covers
the actual record-arm filter invoker, in addition to the original six lab landmarks.

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
main/UI thread and call a trusted `validate_target` hook immediately before traversal. The production hook must
check the active project and object generations from #20. The adapter then checks vtables, normal-track ownership,
vector bounds/uniqueness, Program association and family. It does not discover an authoritative active root or
create public handles. A missing hook or stale target fails before any property call.
`mpc_native_target` is a resolved view for one call: queue public handle IDs, resolve them at dispatch, and keep
the command's expected generations in the executor context consulted by the validator. Never queue raw targets.

The retained lab provides a private target registry with an ownership-only check, explicitly weaker than the
required production registry. It is not used to enable public commands. Direct primitive users must drain every
user before closing an adapter. The executor owns and closes its own adapter.

## UI executor (#30)

The [executor header](include/mpc_executor.h) adds nine exports to the same allowlisted library without changing
the existing primitive ABI. It accepts only read-track, set-mute, set-solo and set-record-armed requests. Its
trusted host supplies a bounded resolver for opaque project/track IDs and their expected generations; there is
no arbitrary callback, native function address or raw target in a queued request. #20 must supply the production
resolver before these operations can be exposed publicly.

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
`tests/{native_fixture,executor_fixture}`.
See the [root build guide](../../README.md) for requirements and all targets.
The isolated fixture uses synthetic addresses and private I/O callbacks. No MPC entry point is called by those
tests. It exercises 28 cases, including each fingerprint mismatch, wrong thread, missing validator, stale
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

## Temporary device host

The lab consumes the shared executor and retains its bounded loader and root-only diagnostic socket. Its READ
and PULSE reports contain the requested track's flags; the independent memory reader still compares the project
snapshot before and after the experiment. Its address registry remains lab-only and does not establish #20.

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
with `qualification: bounded_lab_proof`; all state/write/watch capabilities remain unavailable. This handshake
is diagnostic, not the future command transport. Device tests verified loading, typed readback, full unload,
and a retained attachment visible through `describe`, with MPC PID and observed state unchanged.

The #30 read-only smoke test on 2026-10-25 confirmed the executor's callback on UI TID/PID 365, typed readback
matching the independent snapshot, unchanged observed flags/selection and complete unload of both DSOs. The
temporary files were removed and MPC's PID stayed unchanged. No flag write, addin installation or restart was
performed. The private report is `build/native-api/executor-device-unload.json`.
