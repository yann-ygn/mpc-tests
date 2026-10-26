# Native command screen qualification

US #47, branch `us-47-mixer-track-mute`, MPC Key 37 / MPC 3.9.1.2,
GNU build ID `8371199f945ecc40f79cb19c17ff5de72810a91c`.

## Live view mapping

The verified `Engine +0x114` owner is `EngineMode`. Its property at `+0x40`
identifies the visible view; `+0x6c` independently identifies a popup. These are
screen identifiers, not sequence indices or track/Clip program types. The
sequencer timeline's separately verified playing byte remains a mandatory gate.

| Screen | Native view | Popup required |
|---|---|---|
| Main | `0x2080` | `0x3080` (none) |
| Mixer: Volume | `0x2083` | `0x3080` |
| Mixer: Pan & Volume | `0x2083` | `0x3080` |
| Mixer: Sends | `0x2083` | `0x3080` |
| Mixer: Effects | `0x2083` | `0x3080` |
| Mixer: I/O | `0x2083` | `0x3080` |
| Track Mute | `0x208d` | `0x3080` |

All five Mixer pages share the Channel Mixer view identifier. A page is not a
track inventory, target family or command operation. A supported Mixer page
change does not itself change the qualification context; a different view or
popup does. Commands never synthesize a touch, button or navigation sequence.
Selection and peer flag changes come from the native listeners and are reported
in `observedChanges`.

The profile adds exact 32-byte fingerprints at the mode dispatch table
(`0x01577f94`), Channel Mixer name handler (`0x01578a2c`) and Track Mute name
handler (`0x0157895c`). The name handlers resolve the native labels
`Channel Mixer` and `Track Mute`. These complement the existing mode/popup
getter, mode-name dispatcher, application owner and setting landmarks. The exact
build/model, vtables and every profile fingerprint are checked before native use.

Pad Mixer (`0x2082`), Song (`0x2088`), Clip Matrix (`0x209c`), Sounds (`0x20a0`),
other views and modal popups remain unqualified. Nothing in this story qualifies
Song/Clip workflows, additional program families, sequence-scoped mute,
automation-enabled targets or playback. The explicit stopped, automation Off,
recognized Single/Multi settings and global mute restrictions remain separate
conditions. A Clip program is also independently rejected by the family guard.

## Execution and verification

The production helper still checks the target's family and lifetime before
reading its automation layout. It resolves native owners and captures the full
context twice. The first successful sample becomes a private command-local
fence. Later samples must remain qualified and match that fence, including view,
popup, owners, playback, settings, automation and mute scope.

The shared executor invokes the host policy at dispatch, at guarded adapter
traversals, immediately before the setter, after a native arm filter returns,
and around the owned verification snapshot. Policy reads use a recursion guard
while retaining identity validation. Delayed audio verification carries the
original fence into subsequent jobs; a later qualified screen cannot silently
replace the command's original context. Context samples and numeric owner
identities remain private, bounded by the handler/job lifetime, and are never
exposed in receipts or dereferenced without resolving live owners again.

A failure before native invocation rejects with `nativeInvoked:false` and
`execution:not_started`. An unreadable, unsupported or changed context after a
filter/setter yields retained `unknown`, with `READ_FAILED`, `NOT_READY` or
`STATE_UNSTABLE` as appropriate. The handler does not issue an automatic rollback
or retry a setter. No-ops and expected-value conflicts still skip both the native
setter and record-arm filter. Disconnects retain the command and its outcome.

The public C layouts and export allowlists are unchanged. Package version 0.4.0
includes this qualification. `describe` advertises `screens: ["main", "mixer",
"track_mute"]` and distinct conditions for popup, stopped transport, automation,
recognized settings and global mute. The installed 0.2.0 host is upgraded only
through the separate owner-approved installation/restart workflow.

## Repeating the device matrix

Use an idle, owner-approved project with existing plugin/drum tracks, automation
Off and global mute. Stop the persistent host before attaching the temporary lab;
resume it after detaching. Do not install or restart MPC for this runner.

```sh
uv run --no-sync python tools/mpc_native_lab.py --identities --keep \
  --output build/us47/attachment.json
uv run --no-sync python tools/mpc_screen_flags.py \
  --attachment build/us47/attachment.json --screen mixer_effects \
  --targets 0 1 --output build/us47/mixer-effects.json
uv run --no-sync python tools/mpc_screen_flags.py \
  --attachment build/us47/attachment.json --screen track_mute --scenario overlap \
  --targets 0 1 --output build/us47/track-mute-peers.json
uv run --no-sync python tools/mpc_native_lab.py \
  --detach build/us47/attachment.json
```

`--targets` uses MIDI collection indices. The runner resolves their canonical
Track List positions before UI selection and command submission, including when
audio tracks precede a target. Preparation selects a baseline before opening the
screen; every native submission checks the live view before and after execution.
Each matrix row tests a write, requested/audio readback, retained replay, no-op,
expected-value conflict and restoration. Overlap rows test Single/Multi peers
across two native targets. The operator controls settings between runs. A Single
arm baseline containing multiple armed tracks is rejected before writes.

The older independent memory snapshot now validates selected pointers against
the user vector's canonical order and fences the complete pool layout. Its
historical MIDI collection indices remain available to research tools.

## Verification evidence

The production-policy C matrix exercises 72 writes across Main, Mixer and Track
Mute, plugin/drum, selected/unselected and both settings, plus no-ops/conflicts.
Seven injected transition cases cover unsupported queued views, a qualified view
change before the setter, qualified view changes after a setter/filter, delayed
audio verification, a post-setter popup and an unreadable post-setter owner. The
arm-filter transition invokes no property setter and retains uncertainty. The
same fixture passes on host, under ASan/UBSan, and as an isolated ARM executable
on the MPC. Existing lifecycle, manual-edit, disconnect, retry and shutdown
fixtures continue to pass.

Public wire tests cover discovery, Main-to-Mixer/Track Mute changes while queued,
post-setter uncertainty and exact retained replay. Native writes and readback
are tested through the temporary lab's production callback; the installed 0.2.0
host is not presented as having the new screen qualification. See the sanitized
[device and fixture proof](native-command-screens-proof-20261026.json).

The device proof contains **168 passing write rows**: seven screens/pages × two
setting pairs × plugin/drum × selected/unselected × three flags. The setting
pairs are Single solo / Multi arm and Multi solo / Single arm. Twelve additional
overlap rows verify native peer effects on Main, Mixer Effects and Track Mute.
Nine controller-edit rows reject stale expected values before native invocation;
nine discarded-response cases retrieve/replay the retained outcome, including
after restoration. Menu, Sounds, Song and Pad Mixer reject all three no-op
requests before invocation. Clip view rejection is covered by the host
qualification tests. The final five read projections matched the independent
inventory and memory reader; 20 native captures had p95 **10.993 ms**.

All 17 host Meson entries pass, with the added wire/runner cases also checked
separately; all 12 ASan/UBSan native entries and five isolated ARM fixtures pass.
The final ARM command fixture includes all seven injected transition stages.
The package 0.4.0 build and deployment ELF guards pass.

The owner authorized temporary work in the open project, including one empty
drum track. Original requested/audio flags, silencing, sequence, selection,
eight-track inventory, native context, saved command preferences and Mixer
Effects view were restored. The temporary drum track, lab mappings and service
files were removed; the original installed 0.2.0 host resumed ready and its state
read succeeded. MPC's PID remained unchanged; no project save/load, addin
installation or MPC restart was used. Preferences also flushed the already-open
project's deferred recent-file order; this is reported separately from restoration
of the native command preferences and all Track Mute options.
