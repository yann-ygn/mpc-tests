# Investigation review: 2026-10-24

The controller experiments establish usable input injection and useful live feedback. They do not yet establish a
complete live state API. The main missing pieces are reliable track/sequence identity, coverage of unselected tracks,
and recovery when the saved project and live controller state disagree.

This review replayed the existing capture (254,114 parsed MIDI messages), compared the three retained project saves,
and inspected the running Key 37 through read-only SSH. No playback, project edits, settings changes, installs or
restarts were performed. New raw artifacts are under gitignored `build/review-20261024/`; controller captures contain
the hardware identity and must stay out of git.

## What the evidence supports

- Private input injection, button LEDs and pad colours work. The current process has privtap 0.2.0 and Remote 1.0.3
  loaded; `identity=` is empty. MPC owns the Private input and the Public/Private outputs. ALSA reported zero input
  overruns at inspection; this alone does not prove every earlier capture was complete.
- The retained saves reproduce exactly the four documented changes: one program mute, one program solo, one
  sequence clip parameter and the screen mode. They remain a four-track, one-saved-sequence example, not general
  validation of the project schema.
- The tested sequence change has no dedicated sequence identifier in the captured traffic. TAP edge intervals change
  from about 180 to 250 ms, then back: tempo is indirect evidence, not a unique sequence identity.
- Full Q-Link display payloads are 591 bytes: five range/display bytes plus 586 packed bytes. All 22 full-screen
  examples in the retained capture have that length. The earlier finding said 590.
- Settings commands `66`–`6A` occur with family `57` in normal Key 37 sessions as well as family `3A` in the MPC X
  session. The fresh normal-mode capture confirms this. They are not an MPC X benefit.

## First fix the capture assumptions

These are local tooling issues. They do not require more invasive device experiments, but can make a negative result
or reconstructed state misleading:

| Issue | Evidence | Required improvement |
|---|---|---|
| Log replacement can be missed | `mpcx.fetch_log()` compares byte counts only. An isolated reproduction with a replacement of equal size returned the old cached bytes; a larger replacement would append a suffix to the old capture | Track log/session identity, detect replacement, retain rotated segments, and report missing ranges |
| State crosses MPC restarts | `parse()`/`scan()` do not reset streams or `State` at process-start events. A synthetic restart retained the previous MUTE value | Reset state and MIDI/OLED reconstruction at each session boundary; expose unknown fields until refreshed |
| Experiment boundaries are not checked | Missing start/end labels are not rejected by `scan()` | Require both marks in order in the same capture session; reject incomplete experiments |
| Dynamic values can be called blinking | `fold_blinks()` turns any three changes into "blinking", including a rising meter; reproduced with values 1, 3, 7 | Classify meters separately and verify repeated alternating states before assigning blink semantics |

The existing privtap tests cover the C shim, not the Python capture/reconstruction behavior above. Also give the
experiment runner the working SSH configuration (`-F /dev/null` in this environment) before using it here.

## Highest-value device probes

### 1. Inspect the built-in web object model

There is another interface the original plan did not cover. PID 295, `/usr/bin/az0x-webserver`, owns the Unix datagram
socket `/tmp/az0x-webserver` and a TCP listener on port 80. Static strings in its executable include:

```
/api/v1/subtree
/api/v1/object-meta
/api/v1/object-properties
/api/v1/object-method
```

They also name object paths, metadata, properties, methods and notifications. GET requests to `/api/v1/object-meta`
and `/api/v1/subtree` both returned HTTP 401 with `{"pairState":"notPaired"}`. This confirms the service responds;
it does not establish the request schema or whether musical state is exposed.

The next probe is to pair through its normal interface and inspect the object metadata and readable subtree. Determine
whether registered objects include project/track/sequence/transport state or only device settings and file operations.
If musical state and change notifications exist, this could remove much of the need for internal hooks. Do not treat
the presence of generic object routes as proof that such state exists. System D-Bus name enumeration did not reveal
an obvious music-state service; that is a limited observation, not an exhaustive IPC audit.

### 2. Establish live identity and recovery

Use a disposable project with two deliberately similar tracks (same kind, pad colours and mute/solo/arm values) and
two sequences with identical tempos. Compare complete raw output while changing selection through the hardware,
touchscreen and an external controller. Test after starting the observer late, loading a project, and waking the
screen. Do not use input events alone as confirmation: a button or touch may have no effect in a dialog.

For topology, add/rename/reorder/delete a track in that disposable project, then save controlled snapshots. The four
sampled track objects expose no obvious scalar ID field beyond their names, while clip maps use names as keys.
Determine whether an internal ID exists elsewhere and whether sparse sequence keys differ from array positions.

Pass condition: every live update can be assigned to an unambiguous track/sequence, and a fresh observer can establish
a correct snapshot without forcing a save or taking over the screen. If the available interfaces cannot do that,
return `unknown` with a source/timestamp rather than retaining an unverified saved value. This is the strongest reason
to pursue a small internal state bridge if the web object model cannot help.

### 3. Separate mute, solo, arm and bank semantics

The demo enabled `trackMutePerSequence`. We have not separated all of the following:

- Global mixer mute, per-sequence mute, scheduled mute changes, and actual audibility while another track is soloed.
- Track `recordArm` versus global REC/OVERDUB, multi-record mode, and the selected track's arm LED.
- State visible in Track Mute versus updates made while that view is closed, including external MIDI Mix changes.
- Visible-bank occupancy versus total track count. The four-track example cannot establish the behavior of track 17
  or how bank changes map pads to tracks.

Build two sequences with different mute patterns, then repeat with per-sequence mute enabled and disabled. Add at
least 17 tracks, vary colours and include drum/plugin/audio plus available MIDI/keygroup/CV kinds. Cross-check UI,
raw output and saved values one variable at a time. Restore performance settings after each experiment.

Pass condition: distinguish requested mute, sequence mute and suppression by another solo; map every visible pad to a
known track; prove which unselected-track states remain observable outside Track Mute. Pad layout alone is not a
complete track-type classifier.

### 4. Verify transport beyond the LEDs

Current evidence covers selected transport transitions and approximate tempo from TAP. Missing coverage includes
count-in, recording versus armed-from-stop, overdub, pause/resume, locate, loop wrap, queued sequence changes, tempo
changes, master versus sequence tempo, and external sync.

Related work already narrows the search:

- The sibling `mpc-vst-plugins/docs/NOTES.md`, entries dated 2026-10-03, records Key 37 tests where Play emitted MMC
  locate/play instead of MIDI Start; clock required sync output, and receive-MMC/sync configuration did not reliably
  survive restart. MPC's emitted locate was 12 bytes without a subframe byte. Revalidate on this firmware and the
  Remote port rather than assuming that port connection implies transport feedback.
- The sibling `docs/MIDI_TIMING.md` documents VST2 `audioMasterGetTime` tempo, PPQ and loop information on a **Force**.
  It also documents timing jitter and inactive instances reporting stopped/stale position. A tiny observer plugin or
  existing callback bridge is a candidate on this Key 37, not a confirmed global transport source. Check behavior on
  muted/inactive tracks, stopped transport and projects without that plugin before adopting it.

Pass condition: identify an authoritative source for each transport field and explicitly describe the conditions in
which it stops updating. TAP-derived tempo should remain an estimate, with freshness and confidence.

### 5. Prove commands can set a desired state

We can inject presses; that does not yet establish reliable `mute(track, true)` or `arm(track, true)`. Repeating a
toggle reverses it. Track selection and screen coordinates can also change between checking and acting.

Test duplicate requests, a request to an already-muted track, selecting another track during a request, modal
dialogs, and a delayed/missing acknowledgement. Confirm outcomes through independently observed feedback, including
changes made from the physical controls or external MIDI Mix. A successful FIFO write is not an application-level
acknowledgement. Serialize context-dependent actions and report uncertain outcomes instead of guessing from the input.

## Lower-priority decoding

- MPC X meters: distinguish master versus selected-track signal with two known sources. Vary selection, track fader,
  pan, mute/solo and master level independently; repeat with a running silent transport and a stopped sounding note.
  The second historical meter test was inconclusive, so absence there cannot establish meter scope or availability.
- MPC X displays: test Q-Link modes and partial redraws before building text recognition. They are pixels, not track
  IDs, and have view/mode dependencies. Permanent identity spoofing still changes the instrument's panel behavior.
- Unknown CC 90–101 and startup/setup sysex: correlate one setting or view at a time, preserving raw timestamps and
  distinguishing initialization from repeated feedback. Decode these if they resolve an API gap; their existence alone
  does not make them higher priority than identity or recovery.
- Physical data-wheel capture, Q-Link values/touch, pad pressure, sustain, pitch and modulation remain incomplete.
  "No aftertouch seen" does not establish a lack of support. These matter if the API scope expands to performance
  controls.

## When deeper binary work is justified

Read-only ELF inspection found 7,999 defined dynamic-symbol entries and 1,338 undefined entries in `/usr/bin/MPC`;
the static symbol and debug-info sections are absent. Some remaining RTTI/template names mention
`SerialisableProjectData`, `SerialisableTrackData`, project import/export and clip transport. They are navigation clues,
not evidence of callable live-state getters or functions that can be intercepted through `LD_PRELOAD`.

Inspect the existing web metadata first, then establish precisely which live fields the controller and transport
interfaces cannot provide. If track identity/topology or unselected-track state remain inaccessible, target those
objects/functions specifically. Serialization is a useful static-analysis landmark, but intercepting a save only
provides another saved snapshot.

The state service can be prototyped now with field-level source, timestamp and validity. It should not advertise a
complete live track model until identity, recovery and state semantics pass the probes above.
