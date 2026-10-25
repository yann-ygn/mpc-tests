# Track flag semantics on MPC Key 37 / 3.9.1.2

US #31. The native lab uses the guarded adapter and UI executor from #29/#30.
The tested executable build ID is `8371199f945ecc40f79cb19c17ff5de72810a91c`.
Public state and write operations remain unavailable: active-project generations
(#20), authoritative snapshots (#18) and command receipts (#19/#32) still apply.

## Values and completion

| Field | Meaning and verification |
|---|---|
| Requested mute | Explicit native Program mute property; independent memory readback |
| Requested solo | Native notifying Program property; separate requested and audio-side bytes |
| Requested record arm | Native filtered Track property; separate requested and audio-side bytes |
| Silenced | Effective suppression, including suppression by another solo; distinct from explicit mute |
| Audio-side mute | Unknown; no independently qualified copy exists |

A successful primitive result establishes requested-model readback. Solo/arm audio
copies can still lag. The experiment waits separately for those copies; future
public handlers must verify their required readbacks before reporting `applied`.
No result establishes audible output, a completed audio buffer, disk persistence
or an undo entry. These tests run stopped; playback, automation, clips and Song
remain outside this qualification.

A matching desired value returns a no-op without invoking the setter or record-arm
filter. An expected-value mismatch is rejected before the no-op check, including
when an intervening edit already produced the desired value. Expected values do
not detect an ABA edit (away and back) and do not isolate a group of commands.

## Selection and peer effects

Native listeners run normally. Soloing an unselected track can select that track.
Clearing its solo flag does not restore the previous selection. The API honors
this behavior and must report observed selection/peer changes. There is no
selection-preserving option or implicit selection rollback.

Record-arm filters and follow/multi-record behavior can reject a request or change
other tracks. A native constraint rejection is an observed result, not permission
to bypass the filter. Repeating an already-desired flag must not rerun listeners.

## Device matrix

The disposable project has one plugin and one drum track. Both selection positions
were exercised on each family. Requested booleans and the separate solo/arm audio
copies were read independently of the adapter. Every completed context checks
restoration and full library unload. The sanitized run record is
[native-flag-semantics-proof-20261025.json](native-flag-semantics-proof-20261025.json).
There are 42 passing device cases across nine contexts, plus the retained setting-transition
failure. All nine host Meson entries pass (26 Python cases and 34 native fixture cases);
the updated native/executor fixtures also pass as isolated ARM processes on the device.

| Context | Observed behavior |
|---|---|
| Multi record arm, Single solo; each flag, selected/unselected | Desired values applied, repeated desired values skipped native invocation, stale expected values rejected |
| Explicit mute versus solo suppression | Muting a track silenced it; soloing its peer silenced it while its explicit mute stayed false |
| Per-sequence mute enabled; sequence 1 → 2 → 1 | Mute belonged to the edited sequence, with its value recovered on returning |
| Per-sequence mute disabled; sequence 1 → 2 → 1 | Mute remained global; solo and arm also retained their requested/audio values across sequence changes |
| Single record arm; arm an unselected track | Selected the target and disarmed its peer; requested/audio arm converged |
| Single record arm; disarm the selected track | Allowed both tracks to be disarmed |
| Single record arm; select another track in Main | Arm followed the selected track |
| Single record arm; solo an unselected track | Follow-solo selected the target and moved arm to it |
| Multi record arm; arm two tracks successively | Both remained armed; arming an unselected target preserved selection |
| Single solo; solo two tracks successively | The second solo cleared the first solo and selected the second target |
| Multi solo; solo two tracks successively | Both remained soloed and selection was preserved |
| Intervening controller edit during each pulse | The pulse restoration rejected its stale expected value; the manual value remained |
| Disconnect during each applied pulse | The bridge continued its bounded experiment and restored; a lost connection alone did not cancel native work |

Switching the preference from Multi to Single did **not** immediately clear the
existing two armed flags. A subsequent arm operation enforced Single behavior and
cleared a peer. That deliberately retained transition report initially failed
restoration while Single was active. The runner now rejects that inconsistent
starting baseline before writes. Selecting another track and back established a
consistent Single baseline for the passing matrix; returning to Multi permits the
original two armed flags to be restored. A mode name alone does not establish the
current flag combination.

An immediate native arm response was also observed with its requested value false
and audio copy still true. Independent readback subsequently converged. The
requested-model result must not be promoted directly into a public `applied` receipt.

For the current public contract, per-sequence mute needs an explicit sequence
target/precondition before enabling that mode: a track reference alone cannot pin
the sequence that will be active at dispatch. No public writable capability is
enabled by this matrix. Playback/automation/clip/Song modes remain unqualified.

## Reproduction and restoration

Build the ARM bundle using the root build guide. Save a disposable two-track
plugin/drum project under `MPC Documents/Projects/Claude Experiments`, stop playback
and open Main. Do not load projects or edit topology during a run: this private
lab registry is deliberately weaker than #20 and is unsuitable for public writes.
Select the record-arm/mute settings in the UI between runs and record their values
with the context label. Settings captured from `MPC.settings` are saved settings;
the current project can override defaults. Verify the UI as well.

```sh
python3 tools/mpc_flag_semantics.py --context arm-mode-1-seq1 \
  --output build/native-control/flags-mode1.json
python3 tools/mpc_flag_semantics.py --context per-sequence-on \
  --scenario sequence-scope --output build/native-control/flags-sequences.json
python3 tools/mpc_flag_semantics.py --context disconnect \
  --scenario disconnect --output build/native-control/flags-disconnect.json
python3 tools/mpc_flag_semantics.py --context manual-edit \
  --scenario manual-edit --output build/native-control/flags-manual.json
python3 tools/mpc_flag_semantics.py --context single-solo-multi-arm \
  --scenario overlap --output build/native-control/flags-overlap.json
```

The matrix tests both selection positions, desired values, repeated no-ops and
expected-value conflicts. `--properties mute` or `--properties arm solo` narrows a
context. The sequence scenario observes both sequences before and after a native
flag write and checks both after restoration. Manual-edit tests use ordinary controller
buttons while a lab pulse is active; disconnect tests close the relay after the
setter's model readback. These pulse tests concern the experiment's conditional
restoration only: production commands leave their desired state in place.

Each row verifies requested/audio flags and the independent snapshot's track/bus
flags, selection, sequence and slot count. Restoration includes observed peer
flags and selection. A failed check aborts and preserves partial evidence. The
runner drains and unloads the bridge/adapter and removes temporary device files.
Private captures and native addresses stay under ignored `build/`; commit only
sanitized evidence. Restore every setting changed between contexts in the UI.

## Failure evidence

Host/isolated ARM fixtures deterministically cover an edit after queue acceptance
and before UI dispatch for all three flags, queued cancellation, an unknown running
outcome after timeout, running disconnect, eventual completion, and stale readback
after a native call. They use a synthetic native backend and fake message queue;
these are not claims of measured MPC UI stalls or project-load safety.

Only normal plugin and drum tracks have write evidence. Audio, MIDI, CV, keygroup,
clip/input families, buses and other firmware are unqualified. The adapter rejects
other families; their readable names or catalog bindings do not establish support.
