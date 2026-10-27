# Lightweight command and flag observations (US #52)

MPC Key 37, MPC 3.9.1.2, executable build
`8371199f945ecc40f79cb19c17ff5de72810a91c`. Package 0.8.0 contains executor ABI 8,
command ABI 6 and shared observation ABI 1. Primitive ABI 2, snapshot ABI 4,
STATE_V4 and public JSON API 0.1 remain unchanged. The package is built locally;
installed 0.7.0 is retained. No addin installation or MPC restart is needed for
the temporary lab qualification.

## Native work and correctness

Ordinary commands and delayed verification no longer allocate a 127824-byte
snapshot or read track names, buses, sequence names/BPM/usage. They sample live
user-track flags, selection and active sequence/mute context. All user peers are
included to retain Single/Multi solo/arm, follow-selection and silenced effects
across families. The command contract still qualifies the same screens,
recording/overdub Off, stopped or normal sequence playback and target automation
Off. Filters, expected values, no-ops, required family engine copies, FIFO,
start deadlines, disconnect ownership, retained receipts and draining shutdown
retain their existing rules. Optimization does not expand automation eligibility.

Identity refresh compares freshly read topology vectors and active application
owners on each traversal. Stable vectors with live destruction observers reuse
reconciliation. Destruction, Project reset/load and Sequence destructive-change
notifications retire those identities; reorder preserves individual Track IDs
while advancing the observation topology epoch. Every sampled Track/Program
association and family vtable is checked across the capture. Target traversals
still revalidate generations, live policy and the command-local context fence
before filters/setters and after native callbacks. No flag values or command
permissions are cached across UI messages.

Receipts explicitly label flags-only coverage, sampled interval, total tracks,
topology epoch and capture duration. Diffs compare matching generations/families
and individually known fields. Missing rows and unsupported fields do not mean
unchanged. Full reads and recovery retain authoritative metadata coverage.
See [the handler integration](../native-api/adapter/COMMANDS.md#lightweight-command-observations-us-52).

## Shared sampling and limits

`MPC_EXECUTOR_SAMPLE_FLAGS` reads at most 16 tracks. Its borrowed result includes
coverage, selection, active sequence generation/mute scope, freshness and known
field masks. Continuing a page with a retired topology epoch returns
STATE_UNSTABLE. There is one bounded attempt per sampling request; failed partial
buffers are not exposed. Reads remain available independently of command policy.
Native flag dirty signals have not been qualified; this is targeted live sampling,
using the existing verified lifecycle notifications for identity invalidation.

One native job executes per UI message. Up to 16 consecutive covered sampling
jobs can share its fresh result, including a command's after-observation. Sharing
cannot cross another command in FIFO order. There is no reuse between callbacks,
implicit high-frequency full-snapshot fallback, serializer or subscriber I/O on
the UI/audio thread. Fixed slots bound queued jobs and owned buffers to executor
capacity (at most 128). An unreleased slow consumer occupies its own slot; new
work receives QUEUE_FULL instead of unbounded retention. Shutdown drains all
owned results. This adds no audio-thread callback.

US #33 owns the single producer, subscriber transport, replay and reconnect
policy. Initialize/resync with full reads and reconcile metadata when needed;
ordinary LED refresh should use these samples. The measured producer is 10 Hz
on eight tracks. The structural limit is 136 tracks; nine 16-track pages would
be required at that bound, and maximum-density device performance is unqualified.
There is no automatic full fallback here, so its implicit frequency is zero.

The workload acceptance budgets are command UI p95 <=25 ms, maximum <=40 ms,
unpressured terminal receipt p95 <=50 ms, and observation callback p95 <=3 ms,
maximum <=10 ms. They are measured-workload targets, not enforced deadlines or
hard real-time guarantees. Queue deadlines and pressure outcomes retain the
1 s default start and 500 ms verification budgets. A native callback cannot be
preempted. Maximum-density release qualification remains US #38.

## Workloads and measurement boundaries

The baseline is the 0.7.0 command path at `8d96ee7`, with exclusive stage timers
and a larger receipt ABI for instrumentation, before the observation/topology
optimizations. Its retained ARM bundle and raw reports stay in ignored
`build/us52/`. The optimized measurements use the final 0.8.0 ARM lab. Both use
the same MPC process, owner-loaded project, eight user tracks (six plugins and
two audio), 28 buses, 128 sequence slots and the same 70 BPM four-bar sequence.
Playback phases are not synchronized; starting pulses are retained in the proof.
No project is loaded, saved or replaced for the comparison.

The matrix has twelve actual writes per build: mute/solo/arm on plugin and audio
targets, each selected/unselected, in Mixer Effects with sequence-scoped mute.
Every row also verifies no-op/conflict behavior, independent required readback,
retained retries and restoration. The second workload is three 32-request mute
bursts from eight clients while one observation producer runs at 10 Hz. Desired
values alternate for each target across the burst. Baseline observation captures
full state; optimized observation samples only flags. Each burst drains terminal
receipts and restores flags. A final optimized sample is compared with full state
and the independent memory reader.

Command timings separate acceptance/queue, initial dispatch, terminal verification,
cumulative UI work and longest callback. Exclusive UI stages cover identity and
dispatch overhead, live policy, native filter/setter, target readback, before/after
observations and callback publication overhead. Nested target validation is
charged to identity/policy rather than counted twice. `verificationUiUs` isolates
delayed verification UI work. Native callbacks are measured through bounded
fanout/publication, just before notification/return; message destruction, final
lock release and OS scheduling add overhead outside that boundary. Baseline
snapshot timings measure the native capture, while optimized observation reports
both native capture and complete callback; coalesced deliveries add no independent
native job. JSON, socket/SSH transfer and subscriber I/O remain off the UI thread.

The [sanitized proof](native-ui-cost-proof-20261027.json) contains nearest-rank
p50/p95/p99/max, sample counts, phase totals, process CPU/RSS observations and
restoration checks. Twelve-row matrix tail percentiles are maxima and cannot
characterize rare release tails. Sampled ALSA RUNNING states cannot rule out
intervening underruns; no reliable xrun counter, acoustic observation or isolated
API CPU attribution is available. The UI tests, audio state and engine work vary
within each run. Process CPU/RSS comparisons are descriptive rather than causal.

## Reproduction

Prepare the same stopped sequence with recording Off, retain its flags/settings,
stop the installed host, then attach the built temporary lab with
`tools/mpc_native_lab.py --identities --keep`. The lab uses the existing privtap
worker; it does not install an addin or restart MPC. The runners temporarily set
automation Off through the UI and restore its captured values on exit:

```sh
uv run --no-sync python tools/mpc_playback_flags.py \
  --attachment build/us52/measured-attachment.json --targets 0 4 \
  --screen mixer_effects --scope sequence --output build/us52/final-matrix.json
uv run --no-sync python tools/mpc_ui_cost.py \
  --attachment build/us52/measured-attachment.json --targets 0 4 \
  --scope sequence --lightweight --output build/us52/final-bursts.json
```

For the retained instrumented baseline, attach its bundle using
`--tools-directory build/us52/baseline-tools`, and omit `--lightweight` for burst
observation. After all runners stop and restore, use native Locate to restore the original zero
playhead, detach the exact recorded lab, and start the installed host again.
The raw reports are private diagnostics and must not be committed.

## Host and ARM verification

All 19 host Meson entries, 14 ASan/UBSan native entries, six isolated ARM fixture
executables and ARM ELF deployment guards pass. The new observation fixture
compares flags with authoritative full reads across qualified/unknown families,
manual edits, partial coverage, coalescing, reorder/deletion/address reuse,
Project replacement, selection/scope/family/Program changes, capacity pressure,
cancellation/disconnect and shutdown. Existing command cases retain cross-family
Single/Multi peer effects, delayed engine copies, sequence/context transitions,
no-op/conflict, FIFO, receipt ownership and draining behavior. New command cases
verify command-to-observer sharing and an observer preceding a setter in FIFO.

## Before/after results

Durations below are milliseconds. The matrix's nearest-rank p50 is 52.021 ->
12.669 ms; the arithmetic median is 52.767 -> 12.694 ms, a 76% reduction. The
longest matrix callback falls 45%, from 61.106 to 33.838 ms.

| Matrix stage | p50 | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|
| Queue / baseline | 1.000 | 14.000 | 14.000 | 14.000 |
| Queue / optimized | 2.000 | 10.000 | 10.000 | 10.000 |
| Complete UI callback / baseline | 52.021 | 61.106 | 61.106 | 61.106 |
| Complete UI callback / optimized | 12.669 | 33.838 | 33.838 | 33.838 |
| Post-dispatch verification elapsed / baseline | 2.000 | 5.000 | 5.000 | 5.000 |
| Post-dispatch verification elapsed / optimized | 2.000 | 5.000 | 5.000 | 5.000 |
| Terminal receipt / baseline | 57.000 | 70.000 | 70.000 | 70.000 |
| Terminal receipt / optimized | 21.000 | 43.000 | 43.000 | 43.000 |

| Exclusive UI stage | Baseline p50 | Optimized p50 | Baseline maximum | Optimized maximum |
|---|---:|---:|---:|---:|
| identity | 15.788 | 6.081 | 29.777 | 6.436 |
| policy | 1.647 | 1.627 | 1.979 | 2.191 |
| filterSetter | 0.123 | 0.113 | 21.069 | 20.809 |
| targetReadback | 2.502 | 2.525 | 2.793 | 3.118 |
| beforeObservation | 9.811 | 1.019 | 10.020 | 1.403 |
| afterObservation | 9.921 | 1.077 | 10.098 | 1.336 |
| other | 0.000 | 0.010 | 0.000 | 0.010 |

The native unselected Single-solo and audio-arm listener paths still take
approximately 12.5 and 20.8 ms respectively; the same paths appear in baseline
and optimized measurements. The matrix therefore **misses the 25 ms command
p95 target**, while meeting its 40 ms maximum and 50 ms terminal-receipt targets.
Those native listeners cannot be skipped to meet a budget. No delayed engine
verification was needed in the device matrix; isolated fixtures exercise it.

| Eight-client burst stage | p50 | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|
| Queue / baseline | 591.000 | 947.000 | 981.000 | 981.000 |
| Queue / optimized | 325.000 | 486.000 | 496.000 | 496.000 |
| Complete command callback / baseline | 66.284 | 121.634 | 138.121 | 138.121 |
| Complete command callback / optimized | 15.621 | 68.438 | 91.367 | 91.367 |
| Applied terminal receipt / baseline | 660.000 | 1007.000 | 1049.000 | 1049.000 |
| Applied terminal receipt / optimized | 348.000 | 498.000 | 514.000 | 514.000 |

Saturation also **exceeds the 40 ms maximum callback target**: optimized
callbacks reach 91.367 ms, compared with baseline 138.121 ms. Its wall-clock
phase expansion is in identity/validation and observation reads, not the mute
setter alone; those timings do not separate scheduling stalls from active CPU
work. The change materially reduces occupied UI time but does not establish a
frame-time or hard real-time guarantee under eight-client saturation.

Each build receives 96 requests. Baseline produces 44 applied (31 actual changes),
12 deadline cancellations and 40 QUEUE_FULL outcomes. Optimized produces 58 applied
(40 actual changes) and 38 QUEUE_FULL outcomes, with no deadline cancellations.
Applied terminal receipt p95 falls from 1007 to 498 ms. Different accepted and
completed counts mean this is not an equal-completion-count throughput trial.

The measured cumulative command plus independent observation UI occupancy falls
from 6.010 to 1.524 seconds over 26.134 and 21.864 second workload windows. This
counts each coalesced sample only in its originating command. Baseline full-read
observation measures native capture; optimized independent observation measures
its complete callback, a conservative difference in measurement boundaries.

| Observation stage | p50 | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|
| Baseline full native capture | 11.832 | 13.552 | 14.179 | 15.630 |
| Optimized flag native capture | 1.399 | 2.382 | 3.167 | 7.470 |
| Optimized independent callback | 1.410 | 2.427 | 3.050 | 3.181 |

There are 255 baseline and 218 optimized observations, all successful. Nineteen
optimized observations share a command's already captured flags; the remaining
199 execute independent sampling callbacks. Their callback p95 2.427 ms and
maximum 3.181 ms meet the 3/10 ms sampling targets. A shared sample's originating
command callback remains accounted for in the command distribution, including
the saturation tail. The final flags-only sample matches authoritative full
state and the independent reader.

Whole-process CPU over the burst windows is 77.75% baseline and 64.31% optimized
of one core, using the device's AT_CLKTCK=100. Sampled RSS is 1,009,004 ->
1,005,004 KiB baseline and 1,009,016 -> 1,005,016 KiB optimized. These are process
observations, without an API-only or idle subtraction claim. Each burst window's
two sampled ALSA playback states are RUNNING; there is no xrun counter or acoustic
measurement. Twenty fresh authoritative full captures after the optimization
have p95 8.691 ms; all five read projections match independent state.

## Final restoration

Independent final comparison matches the original eight user tracks and 28 buses,
all requested/engine/silenced flags, selection, sequence metadata and slot count.
All eight automation modes and native mode/popup/recording/playback/mute/solo/arm
context match the pre-benchmark baseline. The stopped playhead was returned to
its original zero pulse through the native Locate UI; repeated Stop presses do
not reset this firmware's displayed position. Mixer Effects and its eight-track
viewport are restored. No loop or sequence-content control was changed.

The temporary authority drained, the exact lab unloaded and its files were
removed. Installed host 0.7.0 resumed ready/OK; all five installed reads pass and
match independent state. The MPC PID and project stayed unchanged. No project
was loaded or saved, and MPC was not restarted. These checks cover observed
state, not identical undo history or heap contents. Future installation of the
prepared 0.8.0 package requires the owner's restart approval.
