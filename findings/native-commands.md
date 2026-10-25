# Managed native flag commands (#19)

Implemented 2026-10-25 for Key 37 / MPC 3.9.1.2, build
`8371199f945ecc40f79cb19c17ff5de72810a91c`. The
[command API](../native-api/adapter/COMMANDS.md) is transport independent and lives in the guarded native DSO.
It implements explicit desired-state mute, solo and record-arm handlers over #20's authoritative identities,
#30's UI executor and #18's native observations. Primitive ABI 1 is unchanged; executor ABI is now 4 and
command ABI is 1. Six handler exports and a non-mutating executor poll are allowlisted.

## Behavior

The handler resolves incarnation IDs at dispatch, checks the trusted host's current-context qualification,
uses the existing notifying setters/record-arm filter, and verifies live readback. Expected native values
are checked immediately before the native call and before a no-op check. Revisions are explicitly rejected
as write preconditions, following #28; they cannot detect every physical edit or act as transaction counters.
A repeat with an already-matching desired value does not rerun filters or listeners.

Applied mute establishes requested-model readback. Applied solo/arm also require the separately qualified
audio copies to equal the desired boolean. Lag triggers read-only UI verification, never another setter.
Readback failure, lost identity and verification timeout after execution produce an unknown outcome.
Receipts distinguish accepted/applied/rejected/cancelled/unknown and execution stage. A running callback
cannot establish whether a native call began; that field stays null. Its receipt and storage survive client
wait/disconnect and can resolve when execution evidence arrives later.

Caller-scoped request IDs deduplicate canonical commands for at least ten minutes from acceptance, including
unknown results. A wait timeout does not cancel work or renew its original monotonic start deadline. Changed
payloads conflict; capacity pressure rejects new work; pending receipts never expire. Expired/restarted outcomes
are unavailable. Shutdown cancels unstarted work and retains running jobs until drain; close refuses premature
freeing. Executor close also refuses an attached handler authority.

Compact before/after native observations report best-effort selected-track/sequence, peer requested/audio flag
and silencing changes using opaque IDs and field paths. Native follow-solo and Single/Multi arm/solo listeners
run normally. Commands leave their desired state in place; there is no implicit selection preservation, rollback,
pulse restoration, atomic batch or claim of actual audio activity.

## Boundary with #32

This story implements the typed handler/session layer, receipt serialization and domain schemas. The existing
Unix service continues to gate public setters and `get_command`: authenticated wire binding, command routing,
and validated live mode/settings qualification remain #32. HTTP, MCP, packaging and streaming are separate work.

A missing host qualification callback rejects even a no-op before native access. The callback runs on the UI
thread at dispatch/verification and must reject unsupported current contexts. The private lab callback is an
explicit manual assertion for bounded experiments, not production mode detection. Current qualification remains
stopped sequence-mode plugin/drum tracks; playback/automation/Clip/Song and other families stay unqualified.
Per-sequence mute stays gated until an explicit sequence precondition is defined. Device mute tests temporarily
disabled that setting in the current UI and restored it afterward; saved settings alone were not treated as authority.

## Verification

[Sanitized evidence](native-commands-proof-20261025.json) distinguishes synthetic failures from real device results.

- All 12 host Meson entries pass, including schema validation of actual fixture receipts for all five outcomes.
- All seven native API ASan/UBSan entries pass. ARM compilation and ELF/export/SONAME/RUNPATH/glibc guards pass.
- All four isolated ARM fixtures pass, including 36 handler receipts and the existing 128-track snapshot case.
- Deterministic cases cover expected conflicts/no-ops for every flag, physical edits between acceptance and
  dispatch, eight concurrent retries, post-edit replay, changed-payload conflicts, delayed audio copies,
  verification timeout, queued expiry, retention pressure/expiry, caller isolation, reorder/ABA, stale projects,
  project loss after invocation, unsupported family/context and filtered rejection.
- A blocked native callback reports unknown after the wait/verification deadline. Shutdown/close refuse to free
  it; its receipt later resolves applied after resuming, and the queued following command is cancelled.
- Twelve real device rows cover each flag on plugin/drum targets, selected/unselected. Every row checks applied
  readback against independent requested/audio reads, exact retained replay, a new-ID native no-op, a conflicting
  expected value, full restoration, and replay after restoration without reapplication.
- Two solo rows observed native selection changes and reported them in `observedChanges`. The existing #31
  Single/Multi matrix remains the broader mode evidence; synthetic handlers also check selection/peer arm effects.
- Both device runs closed handler workers, removed observers, drained/unloaded both libraries and removed
  temporary files. All flags/selection and Main were restored; per-sequence mute was restored to enabled.
  MPC remained at PID 10683. No addin install, application restart, project save or project load occurred.

The repeatable private runner is [`tools/mpc_command_lab.py`](../tools/mpc_command_lab.py). See the
[host/lifecycle guide](../native-api/adapter/COMMANDS.md) before integrating the C API. Raw diagnostic captures
remain under ignored `build/native-api/`. These tests do not qualify production mode discovery, transport latency,
large-project command performance, audio disruption or additional firmware/families.
