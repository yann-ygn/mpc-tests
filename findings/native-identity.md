# Active project and native handle lifetimes

US #20, Key 37 / MPC 3.9.1.2, build `8371199f945ecc40f79cb19c17ff5de72810a91c`.
The shared adapter now owns an authoritative project/track/sequence registry through
`mpc_executor_open_authoritative`. Public service state and commands remain unavailable pending #18/#19/#32.

## Authoritative root and project incarnation

Each UI-thread resolver traverses this private, profiled owner path:

| Owner | Member |
|---|---|
| JUCE application slot, RVA `0x06b21d88` | current `MpcApplication` |
| `MpcApplication + 0xc4` | owned `StandaloneFilterWindow` |
| Window `+ 0x264` | owned `MpcJuceFilter` |
| Filter `+ 0xd0` | embedded `MpcEngine` |
| Engine `+ 0x24c` | owned active `Project` |

Every owner has its own checked vtable. The Project's secondary vtables, normal-track pool and sequence array
are also validated. No heap scan, unique candidate, retained pointer or pool-membership check establishes the root.
The independent external diagnostic reader follows the same path, but is explicitly not a lifetime authority.

Loading the same saved file can keep the Project object and MPC PID unchanged while replacing its contents.
Root-address checks and Project destruction alone therefore miss a project incarnation boundary. The registry
also subscribes to `Notifier<ProjectListener>` at Project `+ 0x448`: virtual slots 5/6 bracket reset, and slot 7
reports data installation. The pinned reset code emits these notifications at `0x02561004`, `0x025611dc` and
`0x0256169c`. Observed reset phases return `NOT_READY`; the next capture allocates a new project ID/generation.
An attachment made between reset and installation also observes the installation boundary.

## Object and sequence lifetimes

Native destruction observers attach to the subject at object `+ 4`. The native destructor unlinks an observer
before invoking its third virtual entry. Our observer records an atomic expired flag and owns no model object.
This catches destruction/recreation between reads even when the allocator reuses the same address. Registry
entries also retire when an object leaves the active normal-track pool or sequence slots.

Track names and positions are metadata. Rename/reorder preserves the handle while that native track remains
alive. Replacing its Program resolves the current association for each call; a cached Program address is never
part of a queued command.

Sequence slots need additional observation: Clear All can reset a Sequence in place. In the first slot, even the
used flag can stay unchanged. `Notifier<SequenceListener>` at `+ 0x30` supplies structural notifications in virtual
slots 2–4. Those retire sequence references conservatively, including destructive content/length edits. Slot 5
also fires for name changes, so its callback only retires a reference when the native used flag at `+ 0x258`
actually changes. Rename/reorder of an in-use sequence alone preserves its reference. Clients must reacquire a
sequence reference after destructive edits; they must not infer lifetime from its name or slot number.

The adapter uses qualified native registration/removal routines for SequenceListener. ProjectListener removal invokes its qualified native, non-deleting listener destructor after checking that
notifier dispatch depth is zero. This unlinks the receiver lifetime and marks its native registration dead. The
notifier subsequently unlinks and frees its own node. Directly freeing a still-linked registration is unsafe.
The firmware profile fingerprints all these paths; there are 24 checked code landmarks in total.

## References, execution and reconnects

Identity snapshots contain an opaque adapter instance, project ID/generation, object IDs/generations, selection
IDs and mutable zero-based indices. They contain no native addresses. IDs are monotonically allocated and never
reused within an authority. Exhaustion gates further work instead of wrapping. A fresh adapter gets a random
instance identity, so a process restart or reattachment cannot revive an old reference even if its numbers repeat.

Executor ABI 2 adds the instance and optional sequence precondition to requests. The original primitive ABI
remains 1; old executor configurations fail ABI negotiation. Typed identity capture and sequence validation join
the existing track operations. A track request resolves at dispatch and again at every adapter traversal, including
post-setter readback. A sequence precondition, when present, is checked each time too.

`STALE_TARGET` rejects old instances, project incarnations and object incarnations before a setter runs.
An unavailable transition can return `NOT_READY` or `READ_FAILED`. Malformed topology retires issued references;
repairing memory does not revive them. A socket reconnect to the same authority preserves valid references.
After service/adapter/process restart, rediscover and capture new references; never retry an old command using a
new object looked up by name/index. Unknown running write outcomes retain the executor's existing uncertainty rules.

Shutdown stops acceptance, cancels queued jobs and removes native observers on the UI thread before scheduler
and capture drain. Refused/discarded dispatch or a still-owned registration keeps the authority alive; close
refuses until cleanup succeeds. Code-fingerprint failures also prevent unsafe native cleanup. Successful drain
still requires the loader's external UI-quiescence check before DSO unload.

## Verification and repeatable experiments

The isolated identity fixture uses the real executor and resolver with synthetic native ownership/lifetime events.
It covers rename/reorder, selection outside Main, track/sequence/project ABA, a valid retained old Project,
in-place project loading, in-place sequence reset, queued stale writes, malformed topology, reattachment and
refused/discarded/retained-listener shutdown. Forced ABA belongs to the fixture evidence; device tests do not
claim allocator-address reuse that was not observed. Host checks include ASan/UBSan and existing API/ELF/privtap
coverage. All three isolated ARM fixtures can run on the device without loading code into MPC.

With the owner's approval and a saved project, use disposable projects under `Claude Experiments`:

```sh
uv run --no-sync meson compile -C builddir/arm
python3 tools/mpc_native_lab.py --identities --keep --output build/identity-attachment.json
python3 tools/mpc_identity_lab.py --attachment build/identity-attachment.json \
  --report build/identity-cases.json --checkpoint before --read-track 0
# Keep the device idle during a checkpoint. Make the manual edit between invocations.
python3 tools/mpc_identity_lab.py --attachment build/identity-attachment.json \
  --report build/identity-cases.json --checkpoint reordered --compare before --same-track 0:1
python3 tools/mpc_identity_lab.py --attachment build/identity-attachment.json \
  --report build/identity-cases.json --checkpoint replaced --compare before \
  --new-project --stale-track 0 --stale-sequence 0
python3 tools/mpc_native_lab.py --detach build/identity-attachment.json \
  --output build/identity-detached.json
```

`--stale-track` sends an expected-state mute request and requires `STALE_TARGET` with no native setter invocation.
The runner independently compares track counts and selected track/sequence against native memory. The comparison
is not atomic across the two readers: moving selection during a checkpoint invalidates that attempt. A changed
MPC PID requires a new attachment report. `--new-instance` checks adapter reconnect behavior against an earlier
checkpoint in the same report. Reports include failed attempts, so an unsafe candidate cannot appear to pass.

The [sanitized evidence](native-identity-proof-20261025.json) records accepted device cases, earlier rejected
candidates and restoration. The important findings from failed candidates were sequence reset without destruction
or a used-flag transition, and project reload without Project destruction. Both now have native notifications.

Final evidence: all ten host Meson entries, five ASan/UBSan entries and all three isolated ARM fixtures pass
(51 native guard cases and 19 identity lifecycle groups). Nine accepted device checkpoints contain 73 passing
assertions across the development iterations. The corrected cleanup separately passed same-file reload with a
new project generation and unchanged PID, reconnect/stale-reference rejection, guarded unload, and another file
load after both libraries were unmapped. One rejected cleanup candidate required an explicitly approved MPC
application restart. The saved experiment's observed selection, names/order, flags, buses and sequence-slot count
were restored; temporary attachments and owned files were removed. This is firmware-specific foundation evidence,
not release qualification for the later public state/write service.


US #40 refines Sequence metadata handling: native BPM notification callback
`0x25bc2fc` dispatches virtual offset `0x10` (listener slot 4). It now preserves
the Sequence generation. Destructive reset/Clear All still notify slots 2 and
3, including an unchanged unused slot, and continue to retire generations.
Host fixtures invoke the actual production callback table for both cases.
