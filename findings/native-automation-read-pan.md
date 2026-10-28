# Pan under Automation Read (US #54)

Version 0.13.0 adds native `set_pan` writes under Read for plugin, drum,
keygroup and audio tracks. The owner-requested 0.13.0 installation is verified;
the earlier 0.12.0 host did not support Read pan. Implementation and device
qualification are recorded in the [sanitized proof](native-automation-read-pan-proof-20261028.json).
The subsequent installation has its own [verification record](native-pan-installed-20261028.json).

Pan retains global scope and its complete track reference and mixer generation.
Units are finite normalized 0–1: left 0, centre 0.5, right 1. Receipts retain the
original numeric request, while expected values, no-ops and completion use the
native binary32 value. Expected values precede no-op detection. A verified no-op
skips both the float filter and notifying setter, including when an unknown
source would reject an actual write.

Native parameter application dispatches case `0x205` at `0x242ef0c` to
`0x2430784`. Its receiver is Mixable `+0x394`, which is Program `+0x3d0`.
`0x243b014` invokes the property filter using storage `+0x2c`, manager `+0x34`
and invoker `+0x38`, then reaches notifying float setter `0xe5af48`. The manual
wrapper is `0x286cffc`; the native filter `0x1844614` clamps to the captured
float limits. Readback is Program `+0x3f8`. The new dispatch/case/application
fingerprints supplement the existing wrapper/filter/setter fingerprints.

The policy resolves Project → active Sequence → Track-keyed shared Clip through
the native map and validates optional subscription ownership, Clip/event-list
vtables, structural event metadata and level/pan playback preferences. Populated
and absent sources use the same route. No visible lane or shared volume state
establishes pan permission. Pan additionally captures its two filter storage
words, manager and exact invoker. These values join the private context fence;
public C layouts and transfer versions are unchanged.

Policy is sampled twice at each qualification boundary, including dispatch,
pre-filter, post-filter/pre-setter and verification. Replacement of a qualified
source or filter capture returns `AUTOMATION_SOURCE_CHANGED`. An initially
unrecognized source returns `AUTOMATION_SOURCE_UNQUALIFIED` before invocation.
A filter callback counts as native invocation even if a subsequent fence stops
the setter. Context loss preserves that execution/uncertainty evidence.

The existing native filter is called once and its finite normalized result is
passed to the notifying setter once. Native rejection remains visible. If a
filter normalizes to a different value, bounded read-only verification cannot
claim the original desired value applied unless it is subsequently observed.
It can finish unknown with `VERIFICATION_TIMEOUT`; verification never repeats a
setter. Completion remains model-only. Later native automation can replace an
applied manual value; completion promises neither a persistent override,
audibility, saved state nor sample-accurate position.

Discovery, per-track operations and receipt mixer state advertise Read behavior
and source conditions for pan. Sends, rack/slot enables, Write/unknown modes,
unqualified families, buses and unsupported transport/recording contexts retain
their gates. Caller namespaces, FIFO, canonical deduplication/conflicts, original
deadlines, disconnect ownership and shutdown use the existing managed path.
The shared lightweight observation/watch producer carries pan without a new
full snapshot for every change.

Host fixtures cover all four families, selected/unselected, three native view
classes and stopped/playback, endpoints/intermediate/binary32 values, no-ops,
conflicts and retained retries. Additional pan cases mutate filter/source,
playback preference and automation context inside the native filter callback,
exercise normalization/rejection, malformed sources and concurrent receipts.
The Unix watch regression observes a Read pan command and subsequent independent
native pan update, then reconnects from the retained cursor and replays the
original command receipt. Existing Off, Read flag/volume, identity, stale mixer,
pressure and shutdown tests remain in the suites.

Reproduce the reversible device matrix with targets already in Read, recording
Off and stopped at zero. The runner does not prepare timeline data or modes:

```sh
.venv/bin/python tools/mpc_read_pan.py --attachment build/us54/attachment.json \
  --targets 0 4 \
  --screens main mixer_volume mixer_pan_volume mixer_sends mixer_effects mixer_io track_mute \
  --screenshots --output build/us54/screens.json
```

## Qualification results

All 23 host entries, 18 ASan/UBSan entries and eight isolated ARM fixtures pass,
including the final source/profile pair and ARM ELF/deployment guards. Formatting
and strict example/schema validation pass. The actual superseded receipt is
preserved as [a wire example](../native-api/examples/pan-read-superseded.json).

The canonical device matrix contains 112 event-free contexts across all four
families and 28 populated-source keygroup contexts. Every screen, playback and
selection combination is covered independently. Four actual values per context
produce 560 writes: 559 applied and one completed native invocation finished
unknown with `VERIFICATION_TIMEOUT`. No-ops, expected conflicts and retained
retries pass. Independent reads and 140 UI captures support the contexts; raw
reports and screenshots remain under ignored `build/us54/`.

The native UI's TRK: Pan lane created five scheduled events with parameter key
`{instrumentIndex: 257, parameterId: 10}` on a disposable keygroup track.
Scheduled events and native parameter maps remain unchanged during commands.
An independent stopped/playback test observed manual pan `0.12345679104328156`
and later native values `0.8777777552604675` and `0.23333333432674408`, with Read
remaining enabled. The original receipt stayed unchanged across retries.

The device timeout occurred on Mixer Volume, playing, with an unselected target.
The request retained original `value: 0.123456789`; the observed requested-model
value in `verification.requestedValue` was `0.8777777552604675`. The receipt kept
`nativeInvoked: true` and `execution: completed`, with 15 verification passes,
475 ms of verification and 502 ms total. Accumulated UI work was 284.058 ms,
including 257.652 ms of verification across callbacks. The native setter ran once;
verification only read. Host/sanitizer/ARM cases independently cover delayed/normalized values
and the same bounded unknown outcome.

Locating to zero itself can apply a native pan event. The experiment cleanup
therefore stops and locates before making its single stopped restoration write.
The initial cleanup-order failure is retained in private research artifacts;
the final 28-row populated matrix restores both captured state and timeline data.
This is experiment cleanup, not navigation or repeated writes inside a command.

## Watch and timing

The temporary production Unix service ran with ten tracks, seven steady watch
subscribers and an eighth during retained replay. Twenty-four pan moves cover
stopped/playback, independent readback, 24 replay/reconnect cases, unchanged
retained receipts and two discarded responses. An eight-client burst applied all
eight requests. The shared producer recorded 78 successful lightweight samples,
seven full reads and three cancelled sample attempts with zero recorded callback
work. Every measured pan move reached the steady subscribers; full reads were
periodic rather than a full snapshot per update.

| Measurement | Median ms | p95 ms | Maximum ms |
|---|---:|---:|---:|
| Matrix native callback, 560 invoked requests | 26.522 | 32.307 | 38.051 |
| Matrix receipt | 32 | 43 | 502 |
| Unix watch delivery through SSH, 24 moves | 127.764 | 245.429 | 362.106 |
| Successful watch observation callback | 3.909 | 7.390 | 10.874 |

The eight-request burst's maximum command callback was 38.571 ms. The #52 small
matrix reported callback p95 33.838 ms and receipt p95 43 ms; #53's broader Read
mute/solo/volume matrix reported 51.960 ms and 60 ms. #39's saturated workload
reached a 145.804 ms callback maximum. These differing workloads do not establish
causal overhead or a real-time guarantee. Pan callback p95 still exceeds #52's
25 ms target; observation p95 exceeds its 3 ms target. Maximum-density and
automation-rate delivery remain unqualified.

The UI evidence workload also reached the existing 128-receipt retention limit.
The service rejected further admission without evicting retained results; capture
continued using fresh generation-bound references in a new temporary API
instance. No instrument restart was involved.

## Restoration and installed availability

All captured original eight-track/bus flags, mixer values, native parameter maps,
timeline digests, automation modes, sequence, selection, settings/context and zero
playhead match the initial capture. Both temporary tracks, including the test
pan lane, were removed. Menu over Mixer Volume and its original viewport were
restored. Both temporary labs drained, unloaded and removed their device files.
MPC's PID is unchanged; no project was saved or loaded and MPC was not restarted.

At the end of implementation qualification, installed 0.12.0 resumed ready.
Discovery and all five read projections passed schemas and independent flag/mixer
readback; Read pan remained unavailable in that older host. The subsequent
owner-requested 0.13.0 installation is recorded below.

## Owner-requested installation, 2026-10-28

Package 0.13.0 from clean source `70ff03f` is installed and ready after the
authorized MPC restart. A new `Claude Experiments/US54 Install Backup.xpj` was
saved, independently verified and reopened; no pre-existing project was
overwritten. Package and installed artifact hashes match. Configuration,
persistent identity, preload ordering, privtap and Remote are preserved.

Actual installed Read pan writes pass on the selected plugin and an unselected
audio track, including independent readback, watch changes, retained replay,
retry/get_command, no-ops and expected conflicts. Read mute/solo/volume regressions
pass too. All five read projections match independent flag/mixer observations
and schemas. Active-watch stop/drain, API instance rotation, old-cursor resync,
old-receipt unavailability and fresh watch recovery/unsubscribe pass.

All captured track/bus flags, mixer values, Read modes, settings/context,
selection, sequence, zero playhead and Menu over Mixer Volume are restored. The
backup bytes remain unchanged. Installed host is ready/OK; see the
[sanitized installation evidence](native-pan-installed-20261028.json). Future
installs/restarts still require owner approval.
