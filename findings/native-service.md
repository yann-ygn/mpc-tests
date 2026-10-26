# Local command service — US #32

Implemented on `us-32-local-command-service`, 2026-10-26, host package 0.2.0 / public API 0.1.
The embedded host exposes strict Unix `set_mute`, `set_solo`, `set_record_armed` and `get_command`
operations through the existing managed handlers. Discovery and authoritative state reads remain shared
with the standalone service. Streaming remains #33.

## Admission and delivery

`src/request.c` validates the complete operation-specific message before command enqueueing. It bounds
nesting, strings and integer syntax; decodes ASCII escapes before duplicate/canonical comparisons; requires
booleans and generation-bound references; and rejects caller fields, indices, arbitrary calls, memory/shell
operations, unknown fields and revision preconditions. Decimal object IDs must fit nonzero uint64.

The private directory is owned by the service UID with mode 0700; the socket is 0600. `SO_PEERCRED` admits
only the same UID and supplies the retained caller namespace. Processes of that UID share it across reconnects.
The caller cannot select an identity in JSON. This local identity is not future HTTP/MCP authorization.

The authority binds to the service instance and is retired on an observed attachment/instance change. It has
16 pending slots, 128 receipts and 600,000 ms minimum retention from acceptance, retaining pending work.
Canonical retries reuse the original receipt/deadline, including unknown results; conflicting reuse rejects.
Capacity pressure cannot evict an unexpired result. After retirement/expiry, lookup cannot invent an outcome.

Delivery waits are asynchronous socket state, polled every 10 ms while waiting. Idle service polling remains
100 ms. Waits and disconnects do not cancel handler-owned work or block socket admission. The original queued
start budget is 1–30,000 ms (default 1,000), delivery wait 0–30,000 ms (default 0), and verification budget 500 ms.
Shutdown joins socket callers, cancels/drains handlers, then drains the executor. A timeout retains ownership.

## Native live policy

The additive UI-only `mpc_executor_qualify_command` export preserves command ABI 1 and executor ABI 4 layouts.
It is valid inside an active managed-command policy callback. The persistent host supplies it automatically;
there is no configuration or request field that substitutes manual qualification. The lab can explicitly
select it with `LIVE_COMMAND_OPEN`; the old `LAB_COMMAND_OPEN` remains a private manual diagnostic.

The policy resolves the current request, verifies its target/family with the guarded adapter, and samples
its complete disposable context twice. It requires Main, no popup, stopped transport, recognized Single/Multi
settings, and automation Off on the target. Mute also requires global scope. Unsupported or unreadable
contexts reject before no-op/setter; differing captures return `STATE_UNSTABLE`. Identity, expected values,
filters, native/audio verification, observed peer/selection effects and retained uncertainty remain owned by #19.

The exact-firmware control profile now contains `commandContext` and sixteen additional 32-byte code
landmarks. The native sources were traced from ownership wiring, native getters and settings serialization:

| Source | Qualified layout / values |
|---|---|
| Application → Window → Component → Engine | Existing vtable-checked #20 root; project must match the resolved target |
| Engine + 0x114 → EngineMode | Vtable checked; mode +0x40 = 0x2080 (Main), popup +0x6c = 0x3080 (none) |
| Engine + 0x2e4 → MpcAudioThread + 0x3bc → Sequencer | Both vtables checked; canonical engine ownership wiring |
| Sequencer + 0x400 → SequencerTimeLine | Vtable checked; +0x114 is native playing, required zero |
| Sequencer + 0x3ec → SequenceRecorder + 0x98 → recording properties | Recorder vtable checked; properties +0x4ec is record-arm mode, recognized values 0/1 |
| Engine + 0x310 → RecArmFollowOrMultiBehaviour | Vtable checked; +0x50 must match the active Project and +0x54 the recorder's properties owner |
| Engine + 0x314 → SoloFollowOrMultiBehaviour + 0x38 → property | Behaviour vtable checked; property +0x10 is solo setting, 0 Single / 1 Multi |
| Engine + 0x2dc → Track Mute properties | +0x63 is per-sequence mute; 0 global / 1 sequence |
| Qualified target Program + 0x214 | Automation filter: 0 Off / 1 Read / 2 Write; only Off permitted |

These fields are policy inputs, not new public transport-state fields. Saved settings files and UI labels
cannot grant permission. `tools/mpc_command_policy.py` is a read-only external diagnostic, not an authority.
A final empty-project check caught an earlier incorrect arm-mode read from Sequencer +0x4ec. The corrected
recording-properties owner is cross-checked through the native arm behaviour, and fixtures poison the old slot.
The corrected owner was verified read-only on the device, including matching Project/properties identities and mode value 1; fixtures exercise both recognized modes.
The twelve live write rows and sixty policy checks preceded that correction; they establish flag behavior and
the other live gates. A later owner-loaded project run verified the corrected policy through the installed Unix
service: all three flag writes on a selected plugin passed with independent readback and complete captured
baseline restoration. See [installed live proof](native-service-live-20261026.json) for the narrower scope of that run.
The policy is restricted to this model/build and Main scope. Other modes/families and per-sequence command
references remain unqualified. Applied receipts do not establish audio activity, persistence or isolation.

## Validation and deployment boundary

All 16 host Meson entries pass, including 12 public wire cases and five production-policy cases. All eleven
native-api entries also pass with ASan/UBSan. Tests cover malformed/duplicate/coerced/overflow inputs,
caller spoofing, canonical concurrent retries, dispatch-time manual edits and policy changes, no-ops,
verification uncertainty, disconnects, start expiry, queue/retention pressure and shutdown during a long wait.
Existing handler fixtures cover independent caller namespaces, lifecycle/ABA, running callbacks and audio lag.
Schemas validate actual receipts and the new request/discovery examples.

ARM compilation and ELF/ABI/glibc guards pass. The isolated ARM host exercises the real public socket, all three
setters, verified receipts, lookup/replay, stop/start and stalled-UI draining. It changes no native MPC object.

Live MPC testing used an unsaved disposable two-track drum/plugin project created from the initially empty
project. Twelve write rows cover all flags on selected/unselected targets, no-ops, expected conflicts, replay
before/after restoration and independent native/audio reads. Every row and the overall flag baseline restored.
Ten further contexts supplied sixty no-op policy checks across automation Off/Read/Write, global/sequence mute,
Single/Multi solo, playback, Menu/Track Mute settings popups, Sounds and Track Mute. UI values matched the external
reader. Main/global/Off allowed the qualified commands; the other contexts rejected before native invocation.

Cleanup has a separate caveat: the persistent read host's executor made the lab's conservative unload guard
refuse `dlclose`. After draining that host and selecting New Project / Don't Save to discard the test project,
MPC exited cleanly and its normal service restarted. The journal reports `Deactivated successfully`; no crash
signal was reported. New Project as the trigger is inferred from timing. Final state is empty (zero tracks,
buses and used sequences), with the original settings and healthy installed 0.1.0 read service. Temporary lab
mappings ended with that process, and its files were removed. This run does **not** claim graceful lab DSO unload
or unchanged MPC PID through cleanup. Treat New Project as a possible MPC restart in future device work.

The lab now refuses attachment while another native executor runs: stop the installed host before lab work
and start it after cleanup. `tools/mpc_command_lab.py --live-policy` selects the production policy for future
requalification on a disposable project.

The owner approved installation on 2026-10-26. The 0.2.0 package from implementation commit `40f1b13` is now
installed at `/data/mpc-addins/mpc-native-api`. The standard upgrade drained the old 0.1.0 host and restarted
MPC once. Package checksums matched before installation; installed binary hashes match the package.
The configuration, device identity and exact existing preload list were preserved. The original installation
was backed up locally in ignored `build/native-service/` before the upgrade.

The installed host reports `ready` / `OK`, and all five public state operations pass. Command discovery exposes
all three setters and receipt lookup under their documented conditions. Each setter rejected a nonexistent track
without a native setter call; canonical retry and retained lookup returned the same rejection. Host stop/start
drained successfully without another MPC restart, preserving installation identity and rotating service/adapter
instances. Both privtap and MPC Remote remain mapped, and Remote screenshots work. The instrument remains at
the same Select Demo startup screen with zero tracks, buses and used sequences. No project was saved or changed.

This initial installation smoke check made no actual flag writes. [Installation evidence](native-service-installed-20261026.json)
records those bounds and the package identity. Future upgrades/restarts need owner approval as specified in `CLAUDE.md`.
Production uses the existing NODELETE host lifecycle. Latency/load qualification remains #38.

After the owner loaded a project, the installed service passed actual mute, solo and record-arm writes on the
selected plugin track, restoring each flag through the same public setters. Independent process-memory reads
confirmed requested values and the required solo/arm audio copies. Each case also verified retained lookup,
canonical retry before and after restoration, no-op suppression and rejection of a stale expected value.
Fifteen no-op policy checks covered the original mixer view, Main with Automation Read, Main/Off with sequence
mute, Main/Off with global mute, and the restored mixer/settings. Unsupported contexts rejected before setters.
The run used record-arm mode 1 and Single solo; automation and mute scope were restored after testing.

The final public and independent baselines matched across six normal tracks and thirty bus/other entries,
including flags, silencing, selection and sequence state. The original Mixer Effects view and native settings
were restored; MPC's PID and the service instance were unchanged, with the host still `ready` / `OK`.
No project load/unload, save, addin upgrade or restart was performed during this run. This establishes the
corrected installed path on one selected plugin in stopped Main mode; it does not expand the qualified modes,
families or persistence/audio claims. Sanitized details are in [the live proof](native-service-live-20261026.json).

See [sanitized evidence](native-service-proof-20261026.json), [wire guide](../native-api/README.md#send-flag-commands)
and [C integration](../native-api/adapter/COMMANDS.md). Private captures, addresses and logs remain in ignored
`build/native-service/`.
