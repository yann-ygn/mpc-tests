# Unix watch qualification (US #33)

MPC Key 37, MPC 3.9.1.2, executable build
`8371199f945ecc40f79cb19c17ff5de72810a91c`. Measurements started on 2026-10-27;
restoration and final verification completed on 2026-10-28. The
[sanitized proof](native-watch-proof-20261028.json) records the results and limits.

Version 0.12.0 enables discovery of the embedded Unix watch implementation when
its authoritative observation backend is ready. Acknowledgements report
`qualified: true`. The public API version and native adapter ABIs are unchanged.
HTTP, MCP and SDK delivery remain separate stories. Installed 0.10.0 was stopped
temporarily for qualification and resumed afterward; no addin was installed and
MPC was not restarted during qualification. The subsequent owner-requested
0.12.0 installation and restart completed on 2026-10-28. Installed watch/replay,
recovery, lifecycle and actual Read mute/solo/volume writes pass. A fresh session
backup was reopened; independent reads matched all pre-install track/bus flags,
mixer values, automation, selection, sequence and zero playhead. See the
[installed-device proof](native-watch-installed-20261028.json).

## Workload and delivery

The owner-loaded project contains six plugin and two audio tracks, 28 buses,
128 allocated sequence objects/slots and five used sequences. The selected
sequence is four bars at 70 BPM. Recording/overdub remain Off and all eight
tracks retain Automation Read. Tests cover stopped and ordinary looping sequence
playback, one and eight subscribers, API changes and controller/UI changes.
There is no acoustic or physical MIDI/CV output claim.

The temporary `tools/watch_lab.so` runs the production host, server, publisher,
adapter and live command policy with private `/tmp` paths. Its optional diagnostic
callback records owned native result timing off the UI thread. The existing
checked ARM loader uses the idle privtap worker; load/unload never targets an
audio or UI thread. Full host drain and UI quiescence precede unload. This entry
does not install a preload hook or modify another addin.

`tools/mpc_watch_qualify.py` runs on the workstation and consumes the C Unix
service through an SSH Unix-socket forward. Timing spans command send to receipt
of a complete frame. It includes SSH/network and client scheduling; it is not
native dispatch latency. Schema checks run outside the timing path. Representative
frames, every frame type and the shared schemas are also validated by host tests.

| Latency, ms | Count | p50 | p95 | p99 / maximum |
|---|---:|---:|---:|---:|
| Stopped updates | 48 | 110.756 | 200.266 | 217.862 |
| Playback updates | 48 | 116.560 | 209.178 | 228.717 |
| Combined updates | 96 | 113.076 | 203.893 | 228.717 |
| Replay/reconnect | 24 | 43.487 | 61.656 | 77.074 |

Twelve replay cases change native mute after an authoritative read and before
subscription, exercising the snapshot/subscription interval. Twelve further
disconnect/replay cases reproduce the identical retained observation. All eight
subscriber models match independent process-memory flags, selection and stable
mixer values. Playback audio volume changes between observations; bracketed reads
identify those changing fields instead of asserting equality between different
sample times. Complete mixer restoration is independently checked after stopping.

The loaded workload qualifies eight subscribers on eight tracks. Nine 16-track
pages would be needed at the 136-track structural bound; maximum-density device
performance and automation-rate delivery remain unqualified. Observations can
miss transitions between samples and are not a lossless native event log.

## Native work and resource observations

| Transport / subscribers | Window, s | Control samples | Full captures | Control callback p95 / max, ms |
|---|---:|---:|---:|---:|
| Stopped / 1 | 8.037 | 74 | 1 | 3.903 / 5.868 |
| Stopped / 8 | 8.043 | 74 | 1 | 4.169 / 7.130 |
| Playback / 1 | 8.039 | 73 | 1 | 7.115 / 8.048 |
| Playback / 8 | 8.102 | 73 | 2 | 6.191 / 7.815 |

There are no capture failures in these windows. Subscriber count does not
multiply native observation jobs. The 100 ms schedule runs after each completed
page; it is not a hard 10 Hz deadline. Full callbacks in these windows take
11.572–14.127 ms, with background metadata audits every five seconds. Explicit
reads and initialization can request additional full captures. Topology
reconciliation has a one-full-capture-per-second background budget.

The controls sampler includes mixer values added by #39. These results exceed
#52's historical **3 ms flags-only p95 target**; no 3 ms claim is made for the
combined flags/mixer path. Its measured maximum remains below the 10 ms sampling
maximum target on this workload. Native callback timing includes bounded result
publication/fanout, excluding final capture destruction and lock release. File
logging and subscriber I/O occur off the UI thread.

Cold idle and expired-lease windows execute zero native captures. The producer
sleeps after the 30-second replay lease expires; the service/control loops retain
their bounded polling. Stopped whole-process CPU observations are 116.586% idle,
122.313% with one subscriber, 122.343% with eight and 116.438% after lease expiry,
where 100% denotes one core. Playback windows observe 214.448% and 207.479% for one
and eight subscribers. These are descriptive MPC process observations, without
API-only attribution or a causal CPU-savings claim. RSS endpoint samples span
1,034,336–1,053,204 KiB and are not peak-memory measurements. Replay and output
allocation bounds are separately exercised under pressure.

A paired follow-up with the API stopped/ready/ready/stopped observes whole-
process CPU of 79.996%, 81.736%, 81.018% and 80.234% over approximately five
seconds per phase, with zero native captures in every phase. Endpoint RSS spans
1,032,816–1,043,192 KiB. This is an idle comparison on the same process and view;
process scheduling and renderer activity still prevent API-only attribution.

Every sampled ALSA playback status is RUNNING. This cannot exclude intervening
underruns; no reliable xrun counter or acoustic measurement was available. The
results qualify the declared observation workload, not hard real-time audio
behavior under arbitrary load.

## Recovery, manual edits and cleanup

Seven intentionally unread subscribers each stall after approximately 2.3 MB of
forwarded data, including SSH/socket buffering. The service closes all seven;
one healthy subscriber continues receiving ordered snapshots. A native command
during the pressure run completes in 95.634 ms; a subsequent command takes
53.947 ms. The ninth watch is rejected at the eight-subscriber limit. Ninety
explicit full reads evict the old cursor, which returns `resync_required`.
Natural cursor/lease expiry also requires resync. Host stop completes in
57.766 ms, and stop/start rotates the instance and rejects the old cursor.

Controller mute/solo/arm edits, including arm outside command-qualified screens,
match independent requested/audio-side/silenced observations. A single empty
temporary drum track exercises addition, rename, reorder and removal. Original
track IDs/generations survive each operation. Sequence round trips exercise
selection, active sequence generation and sequence-mute context without changing
used-sequence interpretation. The temporary track is removed afterward.

An expensive track-creation UI operation invalidates the current observation
continuity, producing explicit `resync_required`; a fresh snapshot recovers the
new inventory. Clients must implement this recovery even for same-project edits.
This is a declared sampling/deadline limit, not silent snapshot stitching.

All 23 host Meson entries, 18 ASan/UBSan native entries, eight isolated ARM
fixtures and ARM ELF guards pass. Fixtures cover project replacement, stale
identities, byte/count/time replay limits, 17-track mixed-page coverage and
shutdown. The instrument's project is not replaced or reloaded for qualification.

Final independent comparison restores original inventory/order/names, requested
and engine flags, mixer values including buses, selection, sequence, zero
playhead, command settings and Automation Read modes. Mixer Volume and its
viewport are restored. The temporary host drains, its exact mappings unload,
owned temporary files are removed and installed 0.10.0 resumes ready. MPC's PID
is unchanged. No project is saved or loaded and no addin is installed. These
checks establish observed restoration, not identical undo history or heap bytes.

## Reproduction

Build host and ARM targets using the repository Meson workflow. Stop the installed
API host and use the existing checked ARM hotloader workflow with a private 0700
`/tmp` bundle containing `hotload`, `test_payload.so`, `libmpc_native.so`,
`watch_lab.so` and `mpc-api`. The bundle's `mpc-native-api.conf` contains
`enabled=1`. Run the disposable loader self-test before loading `watch_lab.so`
through the verified idle privtap worker. Its management/API sockets appear in
the bundle's `run/` directory. Retain the exact PID, worker, loader handle and
bundle path for cleanup.

Forward the private Unix API and management sockets over SSH. The workstation
attachment JSON needs `host`, `pid`, `remoteDirectory`, `localSocket` and
`localControl`. Capture a fresh independent baseline before changing the loaded
session, then run:

```sh
uv run --no-sync python tools/mpc_watch_qualify.py \
  --attachment build/us33/attachment.json --scenario stopped \
  --output build/us33/stopped.json
```

Stop/start only the temporary API before another workload to retire old receipts;
the 128-entry retained store deliberately refuses additional commands when full.
Start ordinary sequence playback without recording and run the same command with
`--scenario playing` and another output path. The diagnostic targets canonical
indices 0 and 4 on the declared eight-track plugin/audio project. It preserves
automation and attempts flag restoration on every exit; inspect `restoreError`
if capacity or authority prevents restoration. The operator owns playback,
selection/playhead/screen restoration and the manual/topology cases.

For cleanup, stop the temporary API, create its private `exit` marker and wait for
`finished` to contain zero. Confirm lab/executor workers have exited and apply
the loader's UI-quiescence unload guard to the exact retained handle. Verify both
temporary DSOs are unmapped before removing owned files. Resume installed 0.10.0
and compare all independent state with the baseline. Never install/restart MPC or
discard unsaved work as an implicit part of this workflow.
