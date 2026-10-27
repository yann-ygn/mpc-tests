# Native flag commands during sequence playback (US #50)

MPC Key 37, MPC 3.9.1.2, executable build
`8371199f945ecc40f79cb19c17ff5de72810a91c`. Package 0.7.0 adds playback policy
and command ABI 5. The initial qualification left installed 0.2.0 unchanged;
the owner subsequently authorized another project load and a 0.7.0 installation.

## Live qualification

The adapter permits stopped or ordinary sequence playback on the qualified
Main, Channel Mixer and Track Mute native views, with no popup, target automation
Off and recognized solo/arm settings. Mute must match the live global/sequence
preference. An explicit sequence reference must still name the active generation.
The six supported families retain their independent completion requirements:
plugin/drum/keygroup/audio solo and arm require requested and engine-copy values;
MIDI/CV require requested values only. Mute verifies the requested model in its
labeled scope. Physical output and `audioSide.muted` remain unqualified.

The live source chain is verified at every command traversal:

| Source | Native owner and value | Qualification |
|---|---|---|
| Playing | Engine `+0x2e4` audio thread, `+0x3bc` sequencer, `+0x400` timeline, timeline `+0x114` | Boolean 0 or 1 |
| Recording/overdub | Sequencer `+0x3ec` recorder, recorder `+0x98` SequenceRecorderProperties, properties `+0x528` | Exactly 0, recording Off |
| Recorder timeline | Recorder `+0x78` | Must equal the sequencer timeline |
| Native sequence/clip selector | Sequencer `+0x3e4` | Exactly the qualified zero context |
| Song source | Sequencer `+0x210` | Must be null |
| Automation | Current target Program `+0x214` | Exactly 0, Off |

Recorder and properties vtables are checked. Record-arm behavior must bind the
same properties object and active Project. The playing flag alone never grants
permission. Native recorder getters at `0x1c229c8` and `0x1c229f8` distinguish
Off, live Record/Overdub (1/2), and pending Record/Overdub (3/4). The
Overdub/Record getters at `0x1c22a14` / `0x1c22a30` confirm this encoding. Device
pending Record and Overdub observations independently establish 3/4 while stopped;
ordinary playback reports 0 while playing.

The native selector assignment at `0x1c059a4` and its Engine source at `0xe304f8`
are fingerprinted. It does not alone exclude Song: actual Song playback left that
selector zero. SongMode's native check at `0x195fe10` independently compares the
sequencer's `+0x210` source against the Song source. Actual Song entry/playback
made that source non-null; entering Mixer returned this session to ordinary
sequence playback and cleared it. The policy rejects any nonzero selector or
non-null Song source, regardless of screen; it does not infer context from the
screen name. Legacy Clip creation/output remains unqualified.

`RECORDING_UNQUALIFIED` rejects live, pending or unknown recording enum values.
`PLAYBACK_CONTEXT_UNQUALIFIED` rejects native Clip/other contexts and Song sources.
`AUTOMATION_UNQUALIFIED` rejects Read, Write and unknown automation values.
Unqualified screens/popups/settings return `NOT_READY`; unreadable/mismatched
owners return `READ_FAILED`. Stable repeated context samples are required even
for no-ops and expected conflicts.

## Timing, transitions and load

Acceptance retains a request without reserving a musical position. The initial
UI dispatch establishes its context fence, allowing a stable start/stop before
that dispatch. A subsequent sampled transition before invocation rejects; a
transition after a native filter/setter retains uncertainty with actual execution
metadata. Delayed verification is read-only and preserves the same fence. A full
ABA between observations is not detectable. Sequence mute never follows an old
reference into a new active sequence and never selects an inactive sequence.

These API property setters change the requested model immediately at UI dispatch.
They do not enqueue the Track Mute pad's quantized launch operation. There is no
beat/bar, sample or audio-buffer scheduling guarantee. Matching a native engine
copy does not establish acoustic output or persistence. Controller mute behavior
and queued musical scheduling are separate observations.

Command receipts retain `timing.queueMs`, `dispatchMs`, `verificationMs`, `totalMs`,
`uiWorkUs` and `verificationPasses`. Discovery reports a one-job UI limit only
when the current command authority establishes it; an external read-only
attachment reports that limit as null. Dispatch duration includes the complete first
UI job, including before/after observations; verification duration includes worker
scheduling and later readbacks. Null means that a stage has not been established.
No field timestamps the exact property change. Final retries retain the same
measurements. See [the handler contract](../native-api/adapter/COMMANDS.md).

At most one executor job runs per JUCE message. A non-preemptible snapshot can
still exceed a display frame, but client pressure cannot multiply it by the slot
capacity within that message. Only a bounded number of jobs/captures is owned.
Caller/worker waits and socket I/O remain outside the UI/audio callback; no new
audio-thread callback is installed. The production host retains 16 pending
commands, 128 receipts for at least ten minutes, a 1 s default start budget and
500 ms verification budget. Queue/retention pressure rejects new work; expiry
never implies rollback after invocation.

## Reproduction

Prepare an already looping sequence, stopped and recording Off, then attach the
rebuilt temporary lab with the persistent host stopped. No addin install or MPC
restart is needed. The runner turns automation Off through the UI and restores
its captured values, flags and selection on completion or failure:

```sh
python3 tools/mpc_playback_flags.py --attachment build/us50/attachment.json \
  --targets 0 4 8 9 10 11 --screen main --scope sequence \
  --output build/us50/main-sequence.json
```

`--scenario peers` checks Single/Multi pairs; `--scenario pressure` submits a
32-request burst from eight callers. Reports remain private under ignored
`build/`. The operator owns preparation and attachment cleanup. The runner stops
playback before restoring automation, avoiding unintended automation recording.

## Device evidence and restoration

The [sanitized proof](native-playback-flags-proof-20261026.json) records tests on
2026-10-26 and final restoration on 2026-10-27. Raw state, settings, project names,
addresses and screenshots remain under ignored `build/us50/`.

All 18 host Meson entries, 13 native ASan/UBSan entries, five isolated ARM fixtures
and ARM build/deployment guards pass. Fixtures cover 55 context transitions at
five execution stages, native arm-filter rejection on selected/unselected targets,
family/screen/Single/Multi combinations, sequence fences and bounded delivery
failures. Live recording/overdub and automation Write while playing were not used
to mutate the owner's project; their transitions run in isolated fixtures.

The device matrix contains 168 passing command rows, each with no-op, conflict,
independent readback, restoration and retained retries before/after restoration.
All six families cover selected/unselected targets on Main, Mixer Effects and
Track Mute. Mute covers 36 sequence-scoped and 44 global rows. Four other Mixer
pages add plugin regression rows; all five pages share the same native view ID.
There are also 36 Single/Multi peer rows, 18 controller-edit cases, 18 discarded
response recoveries, three playing sequence transitions and 88 sampled loop wraps.
Old explicit sequence references reject without native invocation after a switch.
All five read projections match the independent reader while playing; 20 fresh
captures have native capture p95 14.451 ms.

A 32-request burst from eight clients produced ten applied, six cancelled by
their queue deadline and sixteen `QUEUE_FULL` outcomes. Ten separate 1 ms queue
budgets cancelled without invocation. Command authority shutdown completed while
playback continued. Pending Record/Overdub, automation Read during playback and
Write while stopped rejected as expected. Song entry/playback established its
separate source; unsupported Song view commands rejected before invocation.

The first pressure setup requested sequence scope while the live preference was
global and correctly rejected before a setter. The runner now checks that
precondition before starting. An SSH connection dropped during a controller-case
cleanup. Reconnection found the same MPC process; recovery restored the durable
baseline, and the affected family cases were rerun successfully. These diagnostic
interruptions are retained in the evidence.

## Measured workload and limits

The first workload is an owner-loaded project: eight original user tracks, four
empty temporary tracks for the additional families, 28 buses and 128 sequence
slots. The main loop was four bars at 70 BPM; sequence-transition tests used an
adjacent one-bar sequence. The second workload is the factory DnB Demo Reactor,
imported without saving: four user tracks (drum, two plugins and audio), 28 buses
and a 32-bar sequence at 174 BPM. The owner authorized discarding unsaved changes
and loading that project after the initial restoration checks. These two projects
do not establish maximum-density release performance.

The first project's 168 applied matrix rows yielded these monotonic durations in milliseconds:

| Stage | p50 | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|
| Acceptance to UI dispatch | 1.5 | 11 | 23 | 28 |
| Complete initial UI job | 56 | 84 | 102 | 107 |
| Dispatch completion to terminal verification | 3 | 8 | 10 | 11 |
| Acceptance to terminal receipt | 64.5 | 97 | 114 | 129 |

Cumulative UI work p95 was 84.185 ms, maximum 107.151 ms. This includes snapshots
and native traversal, not just the setter. Pressure request round trips, including
SSH and the diagnostic client, had p95 100.651 ms and maximum 102.335 ms. The
implementation does not timestamp client parsing/authentication separately from
acceptance, or isolate the exact property-change instant.

Whole-process CPU samples ranged from approximately 183.5–196.1% of one core,
using the device's `AT_CLKTCK=100`. A separate 20 s playing-without-API sample was
59.3%. Different UI, flags and audio states prevent attributing that difference
to API overhead. Sampled RSS ranged from 1,041,696–1,068,916 KiB. All 28 sampled
ALSA playback states were `RUNNING`; no reliable xrun counter or acoustic
observation was available. These samples cannot exclude intervening underruns.

The second project's 18 command rows cover all flags on selected/unselected
drum/plugin/audio targets in Mixer Effects with sequence mute, independent
required readbacks, no-ops, conflicts, restoration and retained retries. Two loop
wraps were sampled. Its stage durations in milliseconds were:

| Stage | p50 | p95 / maximum |
|---|---:|---:|
| Acceptance to UI dispatch | 1 | 12 |
| Complete initial UI job | 54 | 74 |
| Dispatch completion to terminal verification | 2.5 | 5 |
| Acceptance to terminal receipt | 59.5 | 78 |

Whole-process CPU was 115.4% of one core during that matrix and 105.9% in its
separate 20 s playing-without-API sample; sampled RSS was 924,804 KiB. Its
32-request/eight-client pressure burst produced twelve applied, four cancelled
and sixteen `QUEUE_FULL` outcomes, with external round-trip p95 100.577 ms. All
five read projections matched the independent reader; 20 fresh captures had
native capture p95 12.843 ms. Sampled PCM states were `RUNNING`, subject to the
same underrun-observation limits. Flags, automation and selection were restored;
the lab and temporary service files were removed before installation.

The measured UI work can exceed a display frame. One-job dispatch and fixed
capacities bound owned work, but establish no hard latency, scheduling or
audibility guarantee. Physical MIDI/CV output remains unmeasured. Recording,
overdub, automation Read/Write and Clip/Song remain unavailable.

## Final restoration

Before the additional owner-authorized project load, the four temporary tracks
were deleted through the UI. Independent reads matched
the original eight-track inventory, requested/engine/silenced flags, automation,
selection, sequence and playhead. Native and saved command preferences were
restored, including the saved mute preference whose disk value lagged the live
value. Mixer Effects and its original viewport were restored. No saved settings
differed at the final check.

The lab drained and unloaded, its files and a probe left by the dropped connection
were removed, and installed host 0.2.0 resumed ready with a successful public read.
The MPC process and project path stayed unchanged. No project save/load, addin
installation or MPC restart occurred. Restoration claims cover the observed
fields; they do not assert identical undo history or heap contents. The subsequent
owner-authorized installation and restart are recorded separately in
`native-playback-installed-20261027.json` after verification.
