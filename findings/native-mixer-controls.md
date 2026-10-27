# Native mixer controls (US #39)

US #53 subsequently extends this baseline with [actual native Read mute/solo/volume
changes](native-automation-read-writes.md). The qualification below records the earlier release.

Key 37, MPC 3.9.1.2, build `8371199f945ecc40f79cb19c17ff5de72810a91c`.
The implementation adds desired-state volume, pan, send level, insert-rack enable
and insert-slot enable to the existing authenticated local command contract.
It does not expose effect parameters or bus setters. Initial qualification kept
the installed 0.7.0 addin in place. The subsequent owner-approved 0.10.0 upgrade
is recorded in [the installed-device report](native-mixer-installed-20261027.json).

## Typed contract

| Operation | Value | Address within the track | Endpoints |
|---|---|---|---|
| `set_volume` | finite number, 0–1 | none | native fader bottom/silence to native maximum |
| `set_pan` | finite number, 0–1 | none | left, centre at 0.5, right |
| `set_send_level` | finite number, 0–1 | `sendIndex`, integer 0–3 | no send to native full send |
| `set_insert_rack_enabled` | JSON boolean | none | global rack enable; preserves slot enables |
| `set_insert_enabled` | JSON boolean | `slotIndex`, integer 0–3 | individual occupied slot enable |

Numeric units are normalized native control values. They are neither dB nor
linear audio amplitude. Volume and send conversion methods remain native; the
API does not substitute an inferred gain formula. A request retains its JSON
double and compares/executes its conversion to native binary32. Expected values
use that same conversion and exact equality, without an arbitrary tolerance.
The receipt's `verification.requestedValue` is the actual observed model value;
`value` retains the original request. This makes float rounding visible. Numeric
JSON tokens are parsed with a C numeric locale, independently of the host locale.
Non-finite, malformed, wrong-type and out-of-range inputs fail before acceptance.
Numbers must decode as finite binary64 without a conversion range error; an
exponent that underflows that representation is rejected. Indices use decimal
JSON integer tokens, without fractions or exponents.

Every mixer request requires the track's `mixer.generation` as
`mixerGeneration`. It is an opaque incarnation, not a state revision. It fences
the Program, its scaling layout, rack and ordered slot objects. Track references
remain instance/project/track IDs. Names, collection indices and `uiIndex` are
not wire targets. Canonical retries compare the original binary64 desired/expected values (decimal
spellings that decode equally are identical). Two distinct doubles that happen
to round to the same float still conflict under one request ID. Retries include the operation, complete track,
mixer generation, send/slot index, desired and expected values, and original
start deadline. `waitMs` changes delivery waiting only.

Sends A–D address the four fixed native return destinations, indices 0–3. The
API does not change routing or choose a bus by name. Slots 0–3 correspond to UI
inserts 1–4. An empty slot is observable as `occupied:false, enabled:null` and
rejects enable requests with `EMPTY_INSERT`; it never acquires an effect as a
side effect. Disabling a rack leaves individual slot choices intact.

## Qualification matrix and completion

Each qualified cell below covers independent reads and desired-state writes;
boolean rack and slot controls have separate native call paths.

| Family | Volume | Pan | Sends A–D | Rack enable | Occupied slot enable |
|---|---|---|---|---|---|
| Plugin | model | model | model | model | model |
| Drum | model | model | model | model | model |
| Keygroup | model | model | model | model | model |
| Audio | model | model | model | model | model |
| MIDI | unqualified | unqualified | unqualified | unqualified | unqualified |
| CV | unqualified | unqualified | unqualified | unqualified | unqualified |
| Clip/unknown | unqualified | unqualified | unqualified | unqualified | unqualified |
| Input/output/master/return/submix | unavailable | unavailable | unavailable | unavailable | unavailable |

Bus inventory remains metadata only and has no writable user-track reference.
Existing MIDI/CV flag support does not qualify these mixer controls. Unknown
mixer fields remain null and absent from `mixer.known`; occupied-slot knowledge
is separate from its enable bit.

Qualified command context is Main, the five Channel Mixer pages, or Track Mute;
normal sequence mode, stopped or ordinary playback; recording/overdub Off;
no popup; target Automation Off. Read, Write and unknown automation modes reject
mixer commands, including desired-state no-ops. Record arm's Read exemption does
not apply. Neither an empty lane nor cached event absence grants eligibility.
The service never changes settings, selects a track/sequence or navigates to
obtain eligibility. Mixer properties have global scope; per-sequence mute
settings do not turn them into sequence-scoped controls.

Expected values are checked at UI dispatch before desired-state no-op detection.
No-op pan requests skip its native filter. Lifetimes, Program identity, mixer
generation and live policy are rechecked before native calls and after callbacks.
Post-invocation context/topology loss retains uncertainty with actual execution
metadata. Verification performs no additional setter and remains bounded by the
existing 500 ms budget. Native rack work can return before its model changes;
read-only verification waits for the addressed model value. A terminal timeout
receipt stays fixed while the handler retains and drains an outstanding reader.

Completion evidence is **model only** for these controls. Native audio forwarding
and smoothing exist, but independent engine copies have not been qualified as
completion criteria. `audioSideValue:null` and `audioCompletionKnown:false` are
intentional. Neither model completion nor UI agreement proves audibility,
persistence, engine-buffer completion or sample-accurate scheduling.

## Native paths and lifetime evidence

All RVAs below are private profile landmarks, not public addresses or callable
scripting wrappers. Their code prefixes are checked with the exact firmware.

| Control | Wrapper landmark | Actual native receiver / call | Independent requested model |
|---|---|---|---|
| Volume | `0x286a1b4` | `Program +0x3c`, float method `0x236ac88` | `Program +0x5d8`, format at `+0x5dc` must be 3 |
| Pan | `0x286cffc` | filter storage `+0x3fc`, invoker `0x1844614`, then float property `Program +0x3d0`, setter `0xe5af48` | `Program +0x3f8` |
| Sends | **`0x2868cc8`** | `Program +0x3c`, `(unsigned,float)` method `0x236af74` | `Program +0x450 + 56*index`, format 1 |
| Rack | `0x2866fe8` | Inserts pointer at `Program +0x5e0`, bool method `0x235d2e4` | Inserts byte `+0x38` |
| Slot | `0x2866bd8` | Inserts, `(int,bool)` method `0x235d660` | Inserts' bounded bit vector at `+0xb0`; getter `0x235dad8` |

The corrected SetSendLevel candidate mapping is preserved. Registration sites
are not substituted for callable implementations. Volume and sends notify their
ConstProperty and run native conversion/forwarding. Pan uses the profiled native
range filter and notifying property setter. Rack/slot methods retain native
queued forwarding. External `/proc/PID/mem` reads and UI edits independently
check the model representation; no raw model-value write is used by the API.

The lifetime registry attaches destruction observers to the volume property's
subject (`Program +0x5b0`), Inserts (`+0xc`) and occupied InsertEffect (`+0x14`).
The corresponding property/rack/effect destructors unlink before calling observer
slot 2. Dead subjects are never traversed during detach. Generations also fence
Program vtable, scaling formats and ordered slot pointers. Removal/re-addition
retires the old generation even if an address is reused. Full snapshots and
partial samples recheck topology before publishing. Existing project/track and
same-file-load generations continue to apply. Observer removal precedes UI
capture drain and DSO unload.

## Observation and compatibility

Snapshots carry a `mixer` object for every track/bus, with independent known bits,
nulls for unqualified fields and the current mixer generation. Snapshot freshness
covers these values. Shared lightweight samples carry the same owned structure,
explicit partial user-order coverage, capture time and duration. Ordinary
commands, delayed verification and refreshes do not request names, bus metadata
or sequence metadata. Initialization/resync still uses authoritative full state;
collection topology changes still require paging reconciliation. These fields
are available to watch consumers; watch transport remains US #33.

Public JSON API remains 0.1 with additive typed operations. Recompile trusted C
hosts for executor ABI **9**, command ABI **7**, observation ABI **2** and snapshot
ABI **5** / `STATE_V5`. Primitive ABI 2 and flag request semantics are unchanged.
Older snapshot attachment tags/layouts cannot enable reads. The shared request,
receipt and state schemas are the contract for later HTTP, MCP and SDK delivery.

## Device qualification and measured limits

Sanitized evidence is in [the qualification report](native-mixer-controls-proof-20261027.json).
Raw reports, project copies and process-memory diagnostics remain in ignored
`build/us39/`; no device identity, project names, sample contents or raw addresses
are included in the published report.

The disposable four-track fixture contained plugin, drum, keygroup and audio
Programs, with an occupied insert on every family and four on the drum. The full
Main matrix exercised endpoints, interior binary64-to-binary32 conversion, all
four send/slot indices, selected/unselected targets, expected conflicts, no-ops
and retained retries. The final native build passed another **1,232 rows** across
Main, all five Mixer pages and Track Mute, both stopped and playing. Every run
compared all user and bus mixer values and flags before/after, including rack
and slot enables and automation. Empty slots rejected without native invocation.
Automation Read/Write produced 40 expected pre-invocation rejections. Nine
checks of adjacent binary32 values around 0.5 passed independent readback. Manual
controller edits were visible to the sampler and made stale expected values
conflict. The same global volume persisted across two active sequences.

Deleting and re-adding an effect retired the old mixer generation; an old request
returned `STALE_MIXER` before invocation. Loading a different experiment project
retired the old project reference and returned `STALE_PROJECT`. Deterministic
fixtures additionally cover queued topology changes, address reuse/destruction,
post-filter context changes, engine/model lag, disconnect ownership, expiry,
retention and immutable timeout receipts while outstanding readers drain. The
five authoritative read projections matched independent process-memory mixer
observations on all four qualified families. No metadata capture was used by
ordinary mixer command/verification jobs in the instrumented fixture.

| Workload | Applied samples | Total latency p50 / p95 / max | UI work p50 / p95 / max | Longest callback |
|---|---:|---|---|---:|
| Four-family, one request at a time; seven screens, stopped/playing | 980 | 23 / 33 / 47 ms | 19.330 / 22.336 / 35.388 ms | 35.388 ms |
| Eight-track real playback; eight clients, 32 submissions per burst, ten bursts, concurrent observer | 188 | 510 / 796 / 856 ms | 36.098 / 99.209 / 145.804 ms | 145.804 ms |

The second workload used a disposable copy of the saved session: six plugin and
two audio tracks, 28 buses, 70 BPM, four bars, Mixer Effects, recording Off and
automation Off. Of 320 submissions, 188 applied and 132 hit the lab handler's
16-pending capacity before acceptance. Accepted work retained its outcome and
required verification. The concurrent observer requested a sample after each
100 ms pause, including service wait: 78 samples, zero errors, native capture
p50 **3.439 ms**, p95 **15.078 ms**, maximum **17.500 ms**. This is not a guaranteed
10 Hz delivery rate under saturation. The report includes per-burst whole-process
CPU/RSS samples; those include MPC audio/UI work and cannot attribute CPU to the API.

For comparison, US #52's lightweight flag workload in the same eight-track
70 BPM session measured matrix UI-work median 12.669 ms; saturated callback
maximum 91.367 ms and total-latency p95 498 ms. Its flag-only observer capture
p95 was 2.382 ms. These are different operations, sample counts and burst runs;
the comparison does not isolate a causal regression. Mixer sampling and lifetime
checks add work. Saturated numeric requests can hold a UI callback for roughly
146 ms in this measured workload. Clients should coalesce gestures, observe
capacity errors, and use desired-state updates with retained request IDs;
continuous automation-rate control and maximum project density remain unqualified.

ALSA was sampled as `RUNNING` before/after each burst. No reliable cumulative
xrun counter or acoustic measurement was available. These observations cannot
exclude missed deadlines, audible glitches or intervening underruns.

All 21 host Meson entries, 16 native API ASan/UBSan entries, seven isolated ARM
fixtures and ARM deployment guards passed. The separate privtap preload harness
cannot run in this sanitizer configuration because its preload precedes the ASan
runtime; its two normal host tests passed. Schemas/examples and old-layout
rejection are included in the host suite.

## Restoration and release artifact

The owner approved saving the current session as an experiment backup, using the
existing disposable experiment, and reloading the backup afterward. A separate
copy of that backup supplied the real playback workload. Test-only track/effect
additions were discarded by loading the next experiment; no experiment edits were
saved over an existing project.

The original eight-track session was reloaded from `US39 Session Backup.xpj`.
Independent comparisons matched all captured user/bus mixer values, occupied
slot/rack enables, automation and requested/audio flags. Selection, active
sequence, command settings, zero playhead and Mixer Effects view matched. The
saved backup's bytes were unchanged. Loading the saved session restores its
routing, effect parameter state and sequence contents as well; these were not
edited by the API runner. The temporary lab drained/unloaded, its files were
removed, and the installed 0.7.0 host resumed. MPC's PID was unchanged throughout.

Package **0.10.0** was subsequently installed with the owner’s approval. A fresh
experiment session backup was saved before the MPC restart and reopened afterward.
Installed artifact hashes, all five mixer setters/readbacks/retries, five read
projections and host stop/start passed. Configuration, device identity and the
preload list were preserved. Captured values/settings, selection, active sequence,
zero playhead and Mixer Effects/no-popup view were restored; the installed host
is ready/OK. See [installation evidence](native-mixer-installed-20261027.json).
Future installations/restarts still require owner approval.
