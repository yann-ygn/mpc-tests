# Explicit sequence-scoped mute on MPC Key 37

US #48, MPC 3.9.1.2, executable build ID
`8371199f945ecc40f79cb19c17ff5de72810a91c`. The public JSON version remains 0.1;
package 0.5.0 contains the implementation. The installed 0.2.0 host was resumed
without upgrade or MPC restart after qualification.

## Address and read boundaries

`set_mute` accepts optional `muteScope`, either `global` (the existing default) or
`sequence`. Sequence scope requires a complete `sequence` reference containing
`instanceId`, `projectId`, `sequenceId` and `generation`. Instance/project must
match the track target. Scope is mute-only; global requests cannot carry a sequence.
The [service guide](../native-api/README.md#sequence-scoped-mute),
[request schema](../native-api/schema/command-request.schema.json) and
[example](../native-api/examples/sequence-mute-request.json) define the wire shape.
Omitted and explicit global scope are canonically identical. Changing scope,
sequence incarnation or generation under a retained request ID conflicts.

This increment qualifies only the **named active sequence**. A live inactive
reference returns `SEQUENCE_NOT_ACTIVE`; a missing/retired generation returns
`STALE_SEQUENCE`; a mismatched native preference returns `MUTE_SCOPE_MISMATCH`.
There is no qualified inactive-sequence setter. Commands never select sequences
or change preferences. Existing qualified plugin/drum, Main/Mixer/Track Mute,
no-popup, stopped and target-automation-Off restrictions still apply.

All authoritative read projections label `requested.muted` with `muteContext`,
including a complete sequence reference in sequence mode. Sequence metadata also
provides `ref` directly. Reads cover the active sequence's live Program mute;
inactive-sequence values and simultaneous independent global/sequence copies are
unavailable. Global mode labels the same native property as global. `silenced`
remains effective suppression, including solo effects; `audioSide.muted` remains
null. A null scope means no active project. Read requests take no sequence selector.

## Execution and uncertainty

The handler retains the original scope/reference and copies them into the initial
and verification jobs. Binding a track does not replace the supplied sequence
generation with a current one. Resolution checks lifetime, active selection and
its native mirror at dispatch and every adapter traversal. The production policy
checks the live per-sequence preference against the request and retains the
existing context fence. Expected values and no-ops use the addressed live property.

Switching sequence or scope before execution rejects without a setter. Loss after
invocation preserves `nativeInvoked: true` and `execution: completed`, returning
`unknown`; the active-only binding cannot independently verify the former sequence.
The receipt echoes the original scope/reference. Verification and observed mute
changes carry scope context, and different sequence generations/scopes are never
compared as changes to one mute property. Receipt lookup and canonical retries
return the retained outcome even after selection or manual edits change.

Snapshot captures fence the native mute preference as well as identity, topology
and selection. Three unstable attempts fail instead of publishing mixed context.
Trusted hosts must recompile for **executor ABI 6, command ABI 3 and snapshot ABI 3**.
The private wire/attachment tags are `STATE_V3` / `MPC_ADAPTER_STATE_V3`; old transfers
are rejected. Primitive adapter ABI 1 and the exported function set are unchanged.

## Verification and device restoration

The [sanitized proof](native-sequence-mute-proof-20261026.json) records:

- All 17 host Meson entries and 11 ASan/UBSan entries pass. Five isolated ARM
  fixtures pass on the instrument; ARM deployment/export/layout guards pass.
- The native command fixture covers both families and selection positions on
  Main/Mixer/Track Mute, 1 → 2 → 1 patterns, no-ops, expected values, canonical
  conflicts and retained replay. Deterministic races cover queued setting/sequence
  changes, destruction/reset, same-object project reload, post-setter selection,
  preference loss and generation retirement, plus a selection change during
  pre-invocation qualification. Actual invocation/execution stages are asserted.
- Public transport tests cover complete/consistent references, escaped duplicate
  fields, overflow, stale generations, scope mismatches, active-only reads, no
  selection effects on inactive rejection, retries, disconnects and queued/
  post-invocation races. Snapshot tests reject corrupted scope/generation context.
- Twelve live rows cover plugin/drum × selected/unselected × Main/Mixer Effects/
  Track Mute. Every row establishes different patterns on two adjacent existing
  sequences, verifies an edit only in the addressed sequence, rejects inactive and
  stale targets, returns retained results while inactive, and restores both
  sequences. Eight rows additionally cover ordinary controller edits and discarded
  response recovery. Native receipts and an independent memory reader agree.
- All five public read projections match the independent inventory/flag reader.
  Twenty final captures have native capture p95 **11.032 ms**; scope/reference
  observations match the independent live preference. No audible-completion claim.

`tools/mpc_sequence_mute.py` reproduces the bounded device matrix using a retained
lab attachment and the production live callback. The operator owns settings setup,
existing adjacent sequences and attach/detach. `--targets` uses MIDI collection
indices and resolves their canonical Track List positions before selection.
For example, with an appropriate prepared project:

```sh
python3 tools/mpc_sequence_mute.py --attachment build/native-api/attachment.json \
  --targets 0 1 --screen mixer_effects --output build/native-api/sequence-mixer.json
```

Sequence navigation restored automation Read on some tracks even after automation
had been turned Off. The live policy rejected such a target before invocation;
passing rows use targets that remain qualified. This behavior does not relax the
policy. Preliminary runner assertions and the rejected attempt are retained in
ignored raw reports. Project/sequence destructive races are synthetic fixture
injections, not destructive device experiments.

The scratch installer fixture also previously called host-wide `sync`, which
stalled on unrelated host writeback during final checks. Its private test PATH now
stubs only that call; installation/preload/configuration checks still run normally,
and the production installer is unchanged.

Original requested/audio flags, effective silencing, sequence, selection, eight-track
inventory, native command settings/automation and Mixer Effects viewport were
restored. One empty temporary drum track was removed after its tests. The lab DSOs
were drained/unloaded and temporary files removed; the original installed host
resumed ready with a successful read. MPC's PID remained unchanged. No project
save/load, addin installation, MPC restart or firmware change was performed.
