# Continuous mixer receipts (US #56)

Package 0.18.0 adds opt-in `continuous: true` for normalized volume, pan and send
updates. Ordinary commands retain their existing receipt and pending capacity.
The public contract is in [the service guide](../native-api/README.md#continuous-numeric-mixer-updates-us-56)
and [C integration guide](../native-api/adapter/COMMANDS.md#continuous-mixer-admission-us-56).

A continuous control is keyed by authenticated caller, instance, project, Track,
operation and send index. Its generation is still an execution fence, not a new
allocation. The authority owns a separate fixed pool of 64 records and eight
pending slots. One control owns at most one pending or completed record. The
configured 16 pending and 128 receipt slots remain exclusively for ordinary
requests, including all flags and numeric commands without opt-in.

A completed continuous receipt can be replaced at the next admission for its
control. At capacity a new control can replace the oldest admitted completed
continuous record. Pending jobs and active C waiters pin their storage; a newer
request for that control is refused with `QUEUE_FULL`. Pending work is not
coalesced or cancelled by a newer value. All admitted commands retain their
original global FIFO, deadlines, expected values, generation/source/automation
checks and completion timing. No native UI work or executor job layout changes.

`get_command` returns `OUTCOME_UNAVAILABLE` for superseded IDs. While a record is
live, canonical retries return the same result, and changing the mode or other
canonical fields returns `REQUEST_ID_CONFLICT`. Supersession removes the retry
key; submitting that ID again can execute again. Clients must issue new IDs for
new desired states and retain only their latest unsent value after backpressure.
Ordinary commands keep the existing ten-minute recovery contract. Same-UID
processes share both retry namespaces; client-controlled identities are rejected.

Command ABI 10 requires C callers to rebuild. Executor 12, primitive 2,
observation 2 and snapshot 8 / STATE_V8 remain unchanged. Discovery reports mode,
operations, capacities, replacement and lookup behavior. Strict schemas and
examples cover the option and the receipt's `retention` classification.

Validation includes authenticated socket integration, the real command worker
and policy/executor fixtures, ASan/UBSan and isolated ARM execution. A virtual
10 Hz stream executes 6,010 updates on one Track and another 6,010 on two Tracks,
cycling volume, pan and all four sends across 601 simulated seconds per phase.
Interleaved discrete commands, canonical retries, independent caller namespaces,
separate pool exhaustion, full continuous pending queues, completed replacement,
refused pending replacement, LRU replacement, FIFO with ordinary numeric writes,
expected values, original start deadlines, project replacement and stale mixer
generations are checked. Running uncertainty stays pinned past the retention
horizon and a timed-out shutdown/close cannot free its callback. Disconnect and
restart tests recover pending results or retire the old authority as appropriate.

The device qualification uses an owner-authorized disposable copy in Claude
Experiments. Three empty drum Tracks extend the loaded project to eight eligible
mixer targets, alongside four MIDI Tracks; the measured inventory is twelve user
Tracks. No effect, event or routing edit is needed. The independent memory reader
checks values/settings alongside the native receipts. All measurements are
aggregate traffic across the selected Tracks, not 10 Hz per Track or control.

The Automation Off workload executes 1,800 numeric updates on two Tracks and
1,800 on eight Tracks, each phase lasting 179.95 s (10.00 aggregate Hz). Volume,
pan and all four sends cycle through nineteen desired values. All 3,600 apply;
48 interleaved mute/solo/arm requests, including their restorations, apply.
Canonical retries and changed-payload conflicts are checked during the stream;
superseded keys become unavailable. All 48 numeric controls are restored and
independently checked. Single solo's native selection effect is restored using
`select_track`; the reusable runner now checks selection explicitly.

| Off workload | Count | Callback median / p95 / max ms | Round trip p95 ms |
|---|---:|---:|---:|
| Ordinary retained volume baseline | 12 | 28.624 / 29.672 / 29.672 | 105.621 |
| Continuous numeric updates | 3,600 | 26.871 / 28.958 / 38.896 | 93.353 |
| Interleaved flag changes | 24 | 26.451 / 28.750 / 48.802 | 100.755 |

Whole-process RSS ranges from 721,896 to 729,872 KiB during this run and finishes
at 725,900 KiB; the samples show no growth with cumulative update count. These
numbers include MPC, instruments, graphics and the temporary host, so they do
not attribute memory or CPU to command admission. The pool's allocation is fixed
at open; records are reused rather than appended. A host receipt is 27,016 bytes,
with a small additional private record/context header. The extra 64-record pool
is approximately 1.8 MB on that host layout.

An eight-client burst admits eight distinct continuous controls while mute,
solo and arm requests still apply. A deliberately discarded response recovers
through `get_command` and a canonical retry. Changed generation under a live ID
conflicts; a new stale-generation request rejects before invocation. Stopping
the host during an eight-control load drains it, rotates the API instance on
start and makes old receipt lookups unavailable; captured values, flags and
selection are restored afterward.

Under Automation Read, another 1,800 volume/pan updates on eight Tracks run for
179.95 s at 10.00 aggregate Hz. All apply, along with 24 interleaved flag
requests/restorations; numeric values, flags and selection are restored and
independently checked. Continuous callback median/p95/max are
26.585/27.229/39.819 ms, versus the same-project ordinary volume baseline's
26.789/27.098/27.098 ms. Round-trip p95 is 92.439 ms. The 12 non-restoration flag
probes have callback p95/max 60.799 ms and round-trip p95 141.004 ms. These are
model completion and transport measurements, not acoustic output verification.

A continuous request stream spans reloading the original backup. Its old
references yield 263 `STALE_PROJECT` rejections, 92 context `NOT_READY`
rejections and one queued cancellation; no native setter is invoked. The new
project has nine original user Tracks, retiring the three disposable drum
fixtures. The live native policy provides the context rejections during the
browser/modal phase; they are not capacity failures. The separate deterministic
fixture holds pending work through the actual project replacement.

The [sanitized evidence](native-continuous-mixer-proof-20261030.json) records
readbacks, metrics, final artifact hashes and restoration.

The measured UI work stays in the range of #39/#53/#54: #39's saturated maximum
was 145.804 ms, #53's Read matrix p95 51.960 ms and #54's pan matrix p95 32.307 ms.
Those inventories and workloads differ; this is not a causal overhead estimate
or hard latency bound. The continuous mode adds no UI callback operation. The
same-project retained baseline offers a narrower comparison for admission mode.
The #52 target of 25 ms callback p95 is still exceeded. Maximum-density,
per-Track 10 Hz fanout, arbitrary bursts and audio/sample completion remain
unqualified. The two classes reserve admission capacity, not CPU or socket
connections; ordinary clients can still exhaust their own retained pool.

Reproduce a prepared Off run with the temporary production host and SSH forward:

```sh
.venv/bin/python tools/mpc_continuous_mixer.py \
  --socket /tmp/mpc-us56-api.sock --tracks 2 8 --seconds 180 --sends \
  --output build/us56/device-off.json
```

Omit `--sends` for volume/pan under Read. The runner does not change projects,
automation settings or screens; the operator prepares and restores those and
owns temporary-host drain/unload. Raw captures remain under ignored `build/`.

All 30 host entries and 25 native API ASan/UBSan entries pass; final lifecycle
and schema changes also pass their sanitizer reruns. Nine isolated ARM fixtures
and the isolated ARM virtual stream pass, with ARM ELF/export/glibc guards.
Final loaded artifact hashes match the local ARM outputs. All six final reads
and schemas pass after restoration. Both temporary hosts drain/unload and their
private device files are removed. Installed 0.16.0 resumes ready with six reads.
The original user/bus flags, mixers, per-Track Read modes, selection, active
sequence, zero playhead and Mixer Pan & Volume match the independent initial
capture. MPC.settings bytes and saved backup bytes are unchanged. One Track's
Read mode is restored through its native UI button after reload. No addin
installation or MPC restart; 0.18.0 remains a locally built package.

The owner subsequently requested installation with permission to discard the
loaded test project. Version 0.18.0 from clean source `51685f4` is now installed
and ready. Four hundred installed continuous updates on one/two Tracks and
eight interleaved flag requests/restorations pass without capacity refusal;
Read volume watch/replay, all reads/schemas and host lifecycle checks pass.
Configuration, persistent identity and preload order are preserved. Test values
and Read context are restored on the fresh untitled project, Main, stopped at
zero. See [installed verification](native-continuous-mixer-installed-20261030.json).
