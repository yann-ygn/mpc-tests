# Track metadata and routing, US #41

MPC Key 37, MPC 3.9.1.2, GNU build
`8371199f945ecc40f79cb19c17ff5de72810a91c`, 2026-10-29.
Package 0.15.0. Sanitized implementation qualification is recorded in
`native-track-setup-proof-20261029.json`. The subsequent owner-requested 0.15.0
installation is verified in [installed evidence](native-track-setup-installed-20261029.json).

## Supported initial subset

| Command | Value / unit | Qualified families |
|---|---|---|
| `set_track_colour` | Integer RGB24, 0..16777215 | Plugin, drum, keygroup, MIDI, CV, audio |
| `set_track_transpose` | Integer semitones, −24..24 | Plugin, drum, keygroup, MIDI, CV |
| `set_track_velocity_scale` | Multiplier, 0.01..2 (UI 1%..200%) | Plugin, drum, keygroup, MIDI, CV |
| `set_midi_output_channel` | Integer 1..16; 0 follows the incoming channel | MIDI |
| `set_midi_bank_enabled` | Boolean | MIDI |
| `set_midi_bank_msb` / `set_midi_bank_lsb` | Integer 0..127 | MIDI |
| `set_midi_program_enabled` | Boolean | MIDI |
| `set_midi_program` | MIDI wire program number, integer 0..127 | MIDI |

The stopped UI keypad clamps transpose to −24/+24 and velocity to 1%/200%.
Velocity requests, expected values and no-ops compare after binary32 conversion.
The exact minimum readback is `0.009999999776482582`; the schema accepts that
representation too. Integer and boolean fields have exact comparisons. JSON
strings, fractional integers and numeric booleans are rejected.

Every operation requires the full current instance/project/Track reference,
Main, one of the five Channel Mixer pages, or Track Mute; a stable view with
the native engine popup field clear or Menu; stopped ordinary sequence mode; recording and pending
record/overdub Off; local transport source and an idle native transport queue.
Track setup uses the same direct model-property path under Automation Off
and Read. Write/unknown modes are rejected, including no-ops. Automation mode
is fenced through invocation and delayed readback. No automation events are authored.
Audio transpose/velocity, master/bus editing, legacy clip/unknown families and
playing/recording setup changes are unqualified. The service never changes
selection or screen to make a request eligible. MPC numeric keypads and Track
Settings overlays can leave the engine popup enum clear. They are not blocked
by this signal: the keypad probe applied all nine model changes. This is an
engine-context qualification, not an exclusive UI-edit lock; later controller
or editor changes can supersede a completed value.

Full read projections contain `tracks[].setup`, with explicit nulls and
`unsupportedFields` for unqualified fields. Setup metadata travels in full
watch snapshots, including retained replay; it follows the existing metadata
audit interval, not the 100 ms flag/mixer sampler. A fresh public read publishes
an immediate full baseline. Command receipts give independent model readback
and observed setup differences. `applied` is model completion; it does not
promise a physical MIDI receiver, CV voltage, acoustic result or audio timing.

## Exact native paths

All offsets below are firmware-relative and private. Public commands contain
opaque identities, never pointers or indices. The UI executor resolves and
revalidates the incarnation immediately before invocation and readback, fences
the Program pointer/type through delayed verification, and retains uncertain
post-invocation outcomes. A retry returns the retained result and never sends
a second setter or bank/program operation.

| Track property / value offsets | Native RVA | Signature after the receiver |
|---|---|---|
| Colour `+0x5f0` / `+0x618` | `0xe5243c` | `uint32_t` |
| Transpose `+0x4a4` / `+0x4cc` | `0xe4fb24` | `int` |
| Velocity scale `+0x5c4` / `+0x5ec` | `0xe5af48` | `float`, ARM hard-float |
| MIDI output channel `+0x3d0` / `+0x3f8` | `0x1462c7c` | `const int *` |
| Bank enable `+8` / `+0x30` | `0xe298bc` | `bool` |
| Bank MSB `+0x34` / `+0x5c` | `0x1ac2e0c` | `unsigned char` |
| Bank LSB `+0x60` / `+0x88` | `0x1ac2e0c` | `unsigned char` |
| Program enable `+0x8c` / `+0xb4` | `0xe298bc` | `bool` |
| Program number `+0xb8` / `+0xe0` | `0x1ac2e0c` | `unsigned char` |

The profiled ScriptableTrackPool bindings call these notifying properties
without an additional filter. Their code and all distinct setters are
fingerprinted before native access. The disabled scripting frontend is not
constructed or enabled. There are no raw model-memory writes or UI injection
in the public handlers. Native listeners own downstream updates.

The scripting output-channel wrapper `0x286f92c` converts its 0..15 channels
to native 1..16, and its 16 sentinel to native 0. The new public API deliberately
uses the native/UI convention, 1..16 plus 0 to follow input. Serialized XPJ
routing uses the scripting convention; copying its number into a public
command without conversion is wrong.

These direct property setters do not register an UndoManager action.
Discovery reports `undo: "not_registered"`. Restore a captured value with a
new request ID, or reload an approved backup. A replay of the original request
returns its original receipt. Native notification can update the display,
colours/pads and live routing; enabling or changing bank/program settings can
cause MIDI messages. The loopback observed channel changes sending CC 123
(All Notes Off) on the old channel, and bank changes sending CC 0, CC 32 and
the enabled program number. Disabling a setting preserves its stored numeric value.
Transpose/velocity are track playback/input modifiers, not note-event edits.

## Traced operations kept unavailable

- Rename: ScriptableClipLanes registration `0x27c55c8` uses checked callback
  `0x27be61c`, rejects restricted characters and calls controller
  `0x13df760(controller, Track *, const JUCEString *)`. That path builds a
  native undo action through `0x1451360` / `0x1477a08`. It is broader than
  replacing Track's name pointer: clip-map/name ownership must remain coherent.
  The controller receiver and complete rename contract are not public API.
- Create: wrapper `0x2864cf4` parses the named Program type with `0x252d88c`,
  then dispatches through controller `0x13d50e0` for MIDI-triggered tracks or
  `0x13d4978` for audio. Program loading, selection and resource/undo ownership
  need a separate production action contract. No raw TrackPool append is used.
- Remove: wrapper `0x2866630` resolves a name with `0x257eec4`, obtains the
  all-track position through `0x257eedc`, then enters a controller path.
  Names and collection positions are discovery inputs, not public identity
  authority. Delete has project-wide content and selection consequences.
- Copy: the ClipLanes `CopyTrack` wrapper `0x27bd14c` dispatches virtual slot
  `+0x68` on its backend, with source/destination indices. A callable name does
  not settle destination replacement, sequence content, undo or resource copies.
- MIDI input/output port setters use Track properties `+0x34c` / `+0x3a4`
  with integer setter `0xe4fb24`; value offsets are `+0x374` / `+0x3cc`.
  Input channel uses notifying reference setter `0x1462c7c` at `+0x378`;
  native value `+0x3a0` is 0=All, 1..16=channel. Port enumeration, native device
  IDs and hotplug fencing are not qualified, so port and input-channel setters
  remain unavailable. A disposable MIDI fixture can route to MPC Remote In
  through the device UI for independent loopback testing.
- CV and gate are available hardware on this model. The device UI exposes Off
  plus CV 1..8. The getters sign-extend Track `+0x548` / `+0x584`, with −1=Off
  and 0..7=logical port. Unlike the supported properties, setters
  `0x28703c0` / `0x2870160` enter native filters at storage `+0x54c` / `+0x588`
  (manager `+0x554` / `+0x590`, invoker `+0x558` / `+0x594`) before notifying
  signed-byte setter `0x1e95b84` on properties `+0x520` / `+0x55c`.
  Filter ownership, port-sharing semantics and external voltage/gate output
  remain unqualified. Discovery says `NOT_QUALIFIED`, not `HARDWARE_UNAVAILABLE`.
  Akai's [Key 37 product FAQ](https://support.akaipro.com/en/support/solutions/articles/69000847255-akai-pro-mpc-key-37-frequently-asked-questions)
  also documents CV/gate availability. Other models are rejected by the exact
  device/build profile; no hardware is inferred from a generic CV Program type.

The selected MIDI fixture routed to MPC Remote In produced native program
changes 0/127 and bank MSB/LSB 37/91 on channel 16, plus program 0 on channel 1.
Retained retries produced no duplicate messages. A direct ALSA injection
control verified the listener. The live note-thru probes produced no captured
notes, including after a stopped sequence round trip; note-thru, DIN output
and analog CV/gate signals remain unqualified. These are internal ALSA endpoint
observations, not an external synth or physical-connector test.

## Lifetimes, tests and restoration

The public initial subset intentionally excludes create/copy/remove/rename.
Disposable UI lifecycle tests exercise the existing authoritative identity
registry with the new commands, including surviving rename/reorder and retiring
deleted/replaced objects. Host fixtures force address reuse so a newly allocated
Track cannot inherit an old handle merely by occupying the same address.
The live renamed/reordered MIDI object retained its ID and generation; UI
Duplication allocated a new identity, and Undo removed that duplicate while
preserving the subsequent native colour edit on the original. Deleting the
original made its old handle return `TARGET_NOT_FOUND` without invocation;
creating another MIDI track allocated a fresh identity. The allocator did not
reuse a removed Track address in these device cases. Forced address reuse is
covered by host identity fixtures. Reloading the approved backup made a retained
pre-load metadata target return `STALE_PROJECT` before invocation.

Host integration covers every operation's value boundaries and strict types,
expected-before-no-op ordering, queued manual edits, queued rename/reorder,
project replacement, unqualified families, concurrent duplicate IDs, retained
unknown outcomes, model divergence/timeouts and watch/replay. ASan/UBSan and
isolated ARM fixtures exercise the same native adapter and lifecycle code.
All 25 host Meson entries, 20 ASan/UBSan entries, eight isolated ARM fixtures
and deployment guards pass. The device matrix has 910 rows: 756 applied
(including verified no-ops), 154 expected precondition rejections and 546 native
invocations. It covers Main, all five Mixer pages and Track Mute across six
families, with independent readback, retained retries and per-value restoration.
The final automation-fenced build adds 18 Read and 18 Off positive rows plus
nine rejections each for Write, playback, pending record and Grid. A Track Edit
popup rejects before invocation; a numeric keypad overlay is not detected by the
engine popup enum, as documented above. Across 1,001 recorded device request/
receipt pairs, the shared JSON schemas pass. Matrix command total p95 is 24 ms;
maximum UI callback is 11.599 ms. This is not a maximum-density performance claim.

`tools/mpc_track_setup.py` supplies repeatable six-family screen matrices with
separate-process readback and per-value restoration. Raw diagnostic reports,
addresses and project/settings files remain ignored under `build/us41`.

The owner approved saving a fresh session backup and using a disposable copy.
Only files under `MPC Documents/Projects/Claude Experiments` were created.
The original two-track backup was reloaded without restarting MPC. Captured
track/bus flags, mixer values, automation, selection, sequence, tempo, zero
playhead and Main context match the baseline. Backup bytes are unchanged.
Persisted settings differ only in recent-project history and two newly discovered
MPC Remote MIDI endpoints; every pre-existing MIDI configuration is unchanged.
The temporary bridge drained and unloaded, its files were removed, and the
installed 0.13.0 host resumed ready with all five reads passing.
No addin upgrade is included in this story.

Command ABI 9, executor ABI 11 and snapshot ABI 7 / `STATE_V7` require C clients
to rebuild. Primitive ABI 2 and observation ABI 2 remain unchanged. Client SDK
implementation remains in the separate SDK stories; the typed C interface and
shared JSON schemas carry this subset now.

## Owner-requested installation

0.15.0 was installed from clean source `8c3e664` on 2026-10-29 with explicit
authorization to discard the loaded project and restart MPC. Configuration,
persistent identity, preload order and other addins were preserved; installed
artifact hashes match the package. All nine commands pass installed MIDI writes,
independent readback, retained retry/lookup, no-op and conflict checks. The three
shared properties also pass on an unselected plugin. All five read projections
and schemas, setup watch/replay, active-stream drain, stop/start identity rotation
and recovery pass. Test values were restored and the temporary MIDI track removed.
The host is ready/OK in a fresh untitled project, Main, plugin selected, stopped
at zero, recording Off, sequence 01 at 120 BPM. No project file was saved or
overwritten. A full device reboot was unnecessary.
