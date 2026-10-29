# Local watch and reconnect (US #33)

The embedded C host implements qualified `watch` over its private Unix
socket. HTTP, MCP and client SDKs are separate delivery stories. There is no
Python runtime in the service. The standalone discovery/lab attachment has no
incremental sampling backend and rejects watch with `NOT_READY`.

Version 0.12.0 advertises watch when the authoritative embedded observation
backend is ready. Acknowledgements carry `qualified: true`; discovery reports
the configured replay/sampling limits, eight subscribers, eight tested tracks
and `maximumDensityQualified: false`. Device qualification covers stopped and
ordinary sequence playback on six plugin and two audio tracks, with recording
Off and Automation Read. See the [measurements and evidence](../findings/native-watch.md).
Qualification used a temporary host without an addin install, MPC restart, or
project save/load. The owner-requested persistent 0.12.0 installation and restart
completed on 2026-10-28. The saved session was reopened and independently matched
the pre-install capture. Installed watch/replay, stop/start, recovery and actual
Read mute/solo/volume writes pass; all changed values were restored. See
[installed-device evidence](../findings/native-watch-installed-20261028.json).

## Start and consume

```sh
builddir/host/native-api/mpc-api watch --socket /private/path/api.sock
builddir/host/native-api/mpc-api watch --socket /private/path/api.sock \
  --cursor run_0123456789abcdef0123456789abcdef:42
```

The same commands use `builddir/arm/native-api/mpc-api` on the MPC. A consumer
can instead connect directly and send one compact request followed by a newline:

```json
{"apiVersion":"0.1","op":"watch"}
```

Delivery is compact newline-delimited JSON, including nested snapshots. Read
through each newline, preserve any remaining bytes, and accept multiple frames
per socket read. The existing 0600 socket, private parent directory and same-UID
peer admission apply. JSON cannot choose a caller or subscribe to raw native
objects. Each frame is bounded by `maxResponseBytes`.

| Frame | Consumer action |
|---|---|
| `ack` | Setup accepted; `resumed` distinguishes replay from initialization. Do not checkpoint this frame. |
| `snapshot` | Replace state with the nested full authoritative snapshot; checkpoint its cursor. Initial watch and metadata reconciliation use this frame. |
| `change` | Apply the covered track controls and selection, then checkpoint its cursor. |
| `heartbeat` | Connection liveness; no new observation or freshness assertion. Do not advance the applied cursor. |
| `resync_required` | Discard the cursor; fetch `recovery_snapshot` or open watch without a cursor. The connection closes. |
| `terminated` | Explicit unsubscribe, invalid subsequent request, or best-effort service shutdown notice. The connection closes. |

All frames carry the service instance and project/generation context; invalidated
project context is null. Snapshot/change/ack/heartbeat also carry the revision
and opaque cursor. There is no command-completion or causal-event claim in a
watch frame. Command receipts retain their separate verification semantics.

The [request schema](schema/watch-request.schema.json) strictly permits `watch`
with an optional cursor, and `unwatch` with no extra fields. The
[frame schema](schema/watch-response.schema.json) covers every frame type and
reuses the qualified [state model](schema/state-response.schema.json).
[Examples](examples/watch-request.json) include [initial state](examples/watch-snapshot.json),
[controls](examples/watch-change.json) and [recovery](examples/watch-resync-required.json).

## Snapshot continuity and recovery

In the embedded host all five public reads request a fresh capture from the
same publisher that produces watch revisions. A successful read publishes and
retains the complete baseline before returning its revision, even for a read
projection such as `current_track`. A full read's cursor is the literal
concatenation `instanceId + ":" + revision`. Cursor syntax is checked strictly;
clients otherwise treat it as opaque.

Subscribe with that cursor to receive an acknowledgement, every retained
successor in order, and then live observations. This includes changes observed
between the read and subscription, and full snapshots published by other reads.
`previousRevision` connects successive snapshot/change frames. An initial
snapshot or the first snapshot after invalidation has null `previousRevision`.
Starting without a cursor asks for a fresh full capture before setup completes.

Only checkpoint a completely decoded and applied snapshot/change. After a
disconnect, discard any incomplete frame and resume from the last applied
cursor. A replayed frame has the same contents and revision. Resume at the
current retained revision delivers subsequent changes and heartbeats without
duplicating the applied state. A missing, expired, future or other-instance
cursor produces `resync_required`, as does a replay gap during delivery.

Project replacement or generation retirement invalidates existing subscriptions
and retained baselines. Adapter loss or a failed sample invalidates continuity;
recovery requires a fresh baseline. Instance changes invalidate old references
and cursors. Stop/start and service/device restart create a new instance. Same-
project track reorder/add/remove, selection/sequence/scope changes and family
changes reconcile with a full snapshot before further controls are published.
An expensive UI operation can also expire a queued sampling deadline and
explicitly require resync. Temporary-track creation exercised this path on the
device; a fresh snapshot recovered the updated inventory with original IDs
preserved. Consumers must implement resync even for same-project edits.

To unsubscribe, send this on the streaming connection:

```json
{"apiVersion":"0.1","op":"unwatch"}
```

An outstanding frame finishes before the termination frame. Closing the socket
also unsubscribes. EOF, a partial final frame, output timeout or reset may arrive
without a terminal notice; recover using the last applied cursor. Shutdown
never waits for a subscriber to read a termination notice.

## Sampling and freshness

One worker serves all subscribers. It queues at most one owned native job at a
time and performs no subscriber I/O. Ordinary updates use US #52's live controls
sampler, including requested flags, qualified audio-side copies, silenced state,
mixer values, selection, active sequence generation and mute scope. Sampling is
independent of command eligibility and the visible screen. Unsupported fields
remain null, rather than becoming false.

One page of up to 16 tracks is sampled per 100 ms interval after completion.
At 136 tracks, a sweep needs nine pages; page times are distinct and not atomic.
Each change declares its exact interval, total track count, topology epoch,
wall-clock capture time and native capture duration. Tracks outside the interval
are not refreshed. Track names, order and other metadata in controls rows come
from the last full snapshot; apply their controls without claiming a fresh
metadata read. Sequence/scope changes require full reconciliation before another
controls page, avoiding values stitched from different mute contexts.

Initialization, explicit reads and recovery use full captures. Background full
metadata audits run every five seconds; topology/context reconciliation is
limited to one full capture per second. Explicit client reads and fresh watch
requests can request additional full captures. Pending fresh requests coalesce
when the worker has not started their capture. Native queue and wait deadlines
remain bounded; overload can require resync. No serializer or socket work runs
on the UI or audio thread, and no unqualified flag-change hook is installed.

These are observations, not a lossless native event log. Transitions between
samples, or values that change and return before their page is sampled, may be
missed. Metadata changes are normally observed at the next audit. A full snapshot
retains its existing non-atomic identity/topology/selection checks. Allocated
sequence slots never imply used sequences; `usedSequenceCount` comes from native
used flags. Heartbeats do not renew state freshness.

## Bounds and lifecycle

| Resource | Configured bound (not a device performance qualification) |
|---|---|
| Connections / watch subscribers | 16 / 8, preserving admission space for unary requests |
| Replay | 64 frames, 4 MiB total, 30 seconds; earliest limit wins |
| Unsent output | One frame per connection, at most 1 MiB, plus kernel socket buffers |
| Subscriber output deadline | 1000 ms per frame, including partial writes |
| Setup / subsequent input line deadline | 1000 ms; heartbeats cannot extend partial input |
| Heartbeat interval | 1000 ms after the last delivered/generated frame |
| Producer lease without subscribers | 30 seconds after a read/open/last disconnect |
| Native sampling / full waits | 50 / 150 ms, with the executor's cancellation/drain rules |

An expired producer lease retires continuity rather than replaying across an
unobserved interval. With no lease or subscribers the worker sleeps without
polling native state. Replay/output pressure evicts history or closes the affected
connection; it does not block other subscribers, command handlers or the UI.
The host joins the service and publisher before retiring the backend/executor.
The executor continues to own detached running callbacks until its normal drain.

## Verification and qualification

Local verification on 2026-10-27 passes all 23 host Meson entries, all 18 native
API entries under ASan/UBSan, and the ARM build with ELF deployment guards.
All eight isolated ARM fixtures also execute on the device. The 14 new wire
tests use an isolated synthetic UI; final device qualification completed on
2026-10-28, with the loaded session fully restored.

`watch_fixture` exercises ordered snapshot/control publication, unchanged-sample
suppression, ring and time expiry, topology rejection, project-generation
invalidation, and one shared worker for eight consumers. Its C source builds for
host and ARM. `test_watch` uses the actual host, Unix server and isolated native
UI for snapshot/subscription races, manual/API edits, replay/reconnect, schema
examples, rename/reorder, stale/future sessions, partial-input validation,
subscriber pressure, healthy fanout, paused UI, unsubscribe and stop/start.

The [sanitized proof](../findings/native-watch-proof-20261028.json) contains 96
measured updates, 24 replay/reconnect cases, independent readback, seven stalled
subscribers, explicit replay/lease expiry, metadata/controller edits, and API
stop/start. Update p95 is 203.893 ms and reconnect p95 61.656 ms through an SSH
Unix forward. One and eight subscribers execute the same number of native
samples; cold/expired-lease idle windows execute none. ALSA RUNNING samples do
not establish the absence of intervening underruns; no acoustic or reliable
xrun-counter measurement is claimed. Maximum-density and automation-rate
delivery remain unqualified. HTTP and SDK work is not a dependency.


Track setup fields added by US #41 are metadata. They appear in full snapshots
and retained snapshot replay, at the existing metadata reconciliation interval.
A fresh public read publishes them immediately. The lightweight 100 ms controls
sample still covers flags and mixer values; it does not claim setup coverage.
