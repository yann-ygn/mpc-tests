# Transport and navigation, US #40

MPC Key 37, firmware 3.9.1.2, GNU build
`8371199f945ecc40f79cb19c17ff5de72810a91c`, 2026-10-28.
Package 0.14.0; the installed host remains 0.13.0.
Sanitized results are in `native-transport-navigation-proof-20261028.json`.

## Exposed operations

| Operation | Meaning and additional qualification | Completion |
|---|---|---|
| `play` | Desired playing state, recording Off | Native queue drained and model playing |
| `stop` | Desired stopped state, recording Off | Native queue drained and model stopped |
| `play_start` | Stopped only; locate zero, observe zero/stopped, then play once | Retained second phase reached, queue drained and model playing |
| `locate` | Stopped only, absolute native pulse string 0..2147483647 | Exact model pulse |
| `select_track` | Current user Track incarnation | Exact selected public Track ID |
| `select_sequence` | Stopped only, explicit Sequence ID and generation | Exact selected public Sequence ID |
| `set_sequence_bpm` | Stopped, used active sequence; numeric 20..999 | Stored sequence BPM equals requested binary32 |

All commands require stable application/project/native owners, sequence playback
mode, recording and pending record/overdub Off, local transport configuration,
an idle native queue, and no popup except Menu. View changes are fenced; a screen
is not selected as an implementation step. Main, all five Mixer tabs and Track
Mute are the tested view matrix. Track selection can run during ordinary
playback. Locate, Play Start, sequence selection and BPM changes cannot.

Transport targets contain instance/project IDs. All operations except track
selection also require a complete Sequence reference. Track selection instead
requires the full Track target. A discovered sequence `index` is the zero-based
native slot; its ID and generation are write authority. Inactive sequence IDs
are accepted only for stopped selection. No queued next-sequence command is
exposed; `queuedSequenceId` remains null/unsupported. An unused sequence is not
a qualified BPM write target.

Play/stop/select/locate/BPM are desired-state requests, including verified
no-ops. Play Start is an action. Every accepted command retains its outcome by
caller and request ID; replay never repeats an invocation, including either
Play Start phase. A new ID is a new action. This promise lasts only for the
advertised retention period. Expected values, mixer generations, mute scope and
`value` are invalid navigation arguments. Normal start deadlines, bounded
verification, cancellation, unknown running/partial outcomes and draining apply.

## Queue ownership: direct Sequencer calls are unsafe

Early disposable probes called the compiled core Sequencer/controller from the
UI executor. Four such probes caused SEGV and automatic MPC recovery. They are
not used by the implementation. A read-only all-thread fault observer located
one fault at RVA `0xf7efd8`, called from `0x1c103cc`, on MPC's audio thread while
removing scheduled sequencer events. It attempted to release an invalid event
pointer `0x208`. The evidence is consistent with UI/audio access racing the
native event vector. Raw traces, maps, project snapshots and addresses remain
ignored under `build/us40`; only firmware-relative findings are retained here.

The correct UI façade is reached through the validated application engine,
MpcAudioThread `+0x3d8` -> AsyncSequencer (vtable `0x689fdac`). Its `+0x40`
Sequencer must match MpcAudioThread `+0x3bc`, and the Sequencer's project must
match the active incarnation. The façade owns its native queue at `+8` and
pending counter at `+0xc`.

- Async play/stop entry `0x1bb87b8` captures a boolean and posts native callback
  `0x1bb7af0`; true plays and false stops. Native captures decrement the pending
  counter after execution. This is not a toggle.
- Async locate entry `0x1bb8e20` accepts an ABI-aligned signed 64-bit pulse. Its
  native callback `0x1bb7be0` uses Sequencer's safe stopped/continuation path
  `0x1c0cf44`, then core locate `0x1c06bb0` from the appropriate native context.
- Play Start never calls core `0x1c0eec4`. The retained command enqueues locate,
  then a later UI job validates unchanged ownership/context, stopped playback,
  exact zero and native queue drain before enqueueing play once. A failed seek
  cannot be promoted just because playback was already true.

There is no adapter callback on the realtime thread, no direct memory write,
no UI button injection in public handlers, no wait on the UI thread and no
replay of a setter during ordinary verification. A native queue that remains
pending produces a bounded unknown result even if the model appears correct.
Play Start's single remaining phase is an explicit exception to read-only
verification; it is recorded in private retained command context. An invoked
queued operation does not alone establish `changed`: uncertainty retains null
until matching completion confirms the change.

## Selection, tempo and identities

Track selection uses notifying pointer property setter `0xe5ad58` at
Project `+0x78`. Sequence selection uses notifying setter `0x10457cc` at
Project `+0xbc`. Selected pointer mirrors and incarnation/generation checks
survive every traversal. The command does not use UI text or an index as a
pointer source.

Stored Sequence BPM is the float at Sequence `+0xc4`. The verified native
filter lives at `+0xc8` with invoker `0x25ca4e4`; notifying float setter
`0xe5af48` receives Sequence `+0x9c`. The filter/owner are checked again before
the setter. Requests and no-ops compare the resulting binary32 value.

Native BPM listener callback `0x25bc2fc` dispatches virtual offset `0x10`
(listener slot 4). That event preserves Sequence generations. Destructive
reset/Clear All still emit slots 2 and 3, including unchanged unused slots,
and continue to retire references. Tests exercise the actual production
callback table rather than a mirrored test implementation.

Master tempo owner is Sequencer `+0x1e8`, with enabled byte `+0x2c` and BPM
float `+0x58`. These are separate observations, not the Sequence property.
No generic `set_tempo`, master-tempo setter, effective BPM or effective source
is advertised. `transport.effectiveBpm` and `tempoSource` stay null. External
transport configuration is rejected rather than inferred from a stable tempo
label. No external clock generator or acoustic timing qualification is claimed.

## Recording, count-in, punch and sync probes

Pending Record/Overdub are native modes 3/4; the stopped hardware arm probe
rejected all seven navigation operations before invocation in both states.
Hardware REC/OVERDUB plus PLAY START then showed playing true with pulse held
at zero during count-in, followed by modes 1/2 and advancing pulse. A further
navigation request in each live recording state rejected before invocation.
Hardware Stop cleared recording. No musical note injection, record API, overdub API or
punch API is part of the supported surface.

Changing Click/Metro Count-in from Record to Off removed the observed pending
interval: the first sample was active mode 1 at a positive pulse. Restoring
Record restored the preference. Sequencer `+0x3e8` stayed true in both UI modes;
it is a distinct recorder flag, not a qualified count-in preference getter.
The public API does not expose it. Native `recording` follows the compiled
playing/nonzero-record-mode predicate, including pending count-in. It is not
an assertion that an event or audio buffer was written.

Punch qualification was negative. Holding REC opens Start Recording -> Record
Config, with automation/arm controls but no punch interval. A balanced
Shift+REC probe did not establish a punch UI. Compiled registration
`0x284b054` advertises EnablePunchIn and start/end BBP methods, with enable
callback `0x284a540`; no qualified live wrapper receiver was found. No punch
setter or interval write was attempted. These bindings remain unsupported;
this evidence does **not** establish functioning punch recording.

Menu Sync selections were independently observed as Off=0, MIDI Clock=1,
MTC=2 and Ableton Link=3 at SequencerTime `+0x258`. All seven navigation
operations rejected before invocation for each configured external source,
with unchanged model state. Off was restored. No external clock generator,
externally clocked playback or effective-tempo measurement was tested.

## Qualification results

The Main matrix has 29 command rows; the other six views have 126. Every
matrix command passed independent observations, with all eight tracks and
five used sequences. A later 42-row matrix exercised the final staged Play
Start on all seven views, including seven playing-state rejections and seven
retained action replays after Stop. Master-enabled playback covered eight
track selections, three stopped-only rejections and eight concurrent duplicate
play responses. Independent selection comparisons total 96.

Across 216 applied receipts (including no-ops and cleanup), command total p95
is 47 ms and maximum 60 ms; maximum UI callback p95 is 27.245 ms and observed
maximum 35.543 ms. These are model/control measurements, not acoustic timing.
264 recorded device receipts validate against the new request/receipt schemas.
A final read-only 0.14.0 service also validates ABI 6 live transport fields and
all five read projections against their schemas and independent observations;
its lab/service and five task-owned orphan diagnostic helpers were removed.
One early six-view runner hit its 128-receipt authority limit during cleanup,
after all matrix rows passed. Closing that authority and using a fresh one
completed restoration. The runner now rotates authorities per view and before
cleanup. That admission failure is retained in the evidence, not counted as a
successful command.

All 24 host Meson entries, 19 ASan/UBSan entries, eight isolated ARM fixtures,
format checks and ELF/deployment guards pass. Tests cover parser/target domains,
replay and concurrent duplicates, native pending drain, stopped-only starts,
failed locate, context loss before the play phase, stale identities, binary32
BPM and actual Sequence metadata/reset callbacks. Receipt serialization rejects
invalid native navigation enum/BPM observations. Package 0.14.0 is built locally.

The queued implementation remained in one MPC process through all these
probes and the backup reload. Old Play and BPM references then rejected with
STALE_PROJECT before invocation. All 36 independent mixer/automation rows,
user/bus flags, selection, sequence and command settings match the baseline;
persisted settings have zero changed keys. The source backup and both copies
have unchanged hashes. Stopped/record Off, pulse zero, sequence 4, track 2,
sequence BPM 70 and master BPM 120 disabled are restored. Original native
Menu context `(0x2084, 0x3100)` is restored (Menu over Track View). Temporary
labs drained/unloaded; installed 0.13.0 resumed ready and all five read
projections plus discovery match independent state and their installed schemas.

## Read and wire contracts

Full snapshots add model playing, recording, native record-mode enum, absolute
signed pulse, stored Sequence BPM and separate master BPM/enabled. Missing
observations are null and listed in `unsupportedFields`. The native record mode
is independent of per-track arm flags. Navigation receipts include
`verification.transport`; `by: ["model"]` does not establish acoustic output or
sample scheduling. Flag/mixer observations still report their own side effects.

The lightweight watch sampler still covers flags/mixer/selection, not transport
or playhead changes. Full reads and recovery captures refresh transport; a
subscription snapshot does not promise subsequent transport change frames.

Rebuild C integrations for executor ABI 10, command ABI 8, snapshot ABI 6 and
STATE_V6. Primitive and observation ABIs remain 2. Strict request/receipt/state
schemas, current examples and discovery describe the new shape and gates.

## Reproduction and restoration

The owner authorized backup/disposable copies under Claude Experiments and
exclusive device access. `US40 Session Backup.xpj` and `US40 Disposable
Session.xpj` were copied with their own project data from the verified US54
installation backup; existing experiments were not overwritten. All playback
and recording probes use the disposable project. No addin install or deliberate
MPC restart is part of qualification.

`tools/mpc_transport.py --attachment REPORT --output build/us40/REPORT.json`
uses production parser/managed receipts and an independent read-only memory
observer. `--screens` chooses views; `--sequence-indices` names already-used
slots. Each view gets a separate test authority so receipt retention cannot
consume cleanup capacity. The runner checks the process and project before
cleanup and refuses to target a replacement process.

The final evidence records test counts, exact recording/count-in/punch limits,
backup reload and independent restoration checks. Original user/bus flags,
mixer values, automation modes, selection, sequence BPM, master observations,
transport/record state and settings are compared after reload. The temporary
lab must drain/unload before the installed host resumes ready. Experimental
backup/disposable projects remain in Claude Experiments for inspection.
