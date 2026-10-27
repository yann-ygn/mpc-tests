# Native manual changes under Automation Read (US #53)

Package 0.11.0 extends the installed 0.10.0 baseline on Key 37 / MPC 3.9.1.2,
build `8371199f945ecc40f79cb19c17ff5de72810a91c`. Installation and implementation
qualification are separate. Installing the NODELETE host requires an approved
MPC restart. See the [sanitized proof](native-automation-read-writes-proof-20261027.json).

## Supported behavior

| Operation | Read behavior | Completion evidence |
|---|---|---|
| Global mute | native desired-state changes with matching global preference | requested model |
| Named active-sequence mute | native desired-state changes with matching sequence preference and generation | requested model |
| Solo | native desired-state changes, retaining native Single/Multi peers and follow selection | family-qualified model and engine copy; MIDI/CV model only |
| Volume | native normalized 0–1 changes with the observed mixer generation | model only |
| Record arm | existing #51 native filter/setter qualification | existing family evidence |
| Pan, sends, rack/slot enable | remains unavailable under Read | `MIXER_AUTOMATION_UNQUALIFIED` |

Mute/solo retain the six qualified user-track families: plugin, drum, keygroup,
MIDI, CV and audio. Volume retains plugin, drum, keygroup and audio. Buses,
legacy clip/unknown families, recording/overdub including pending states,
Write/unknown automation and Clip/Song transport remain gated. Main, all five
Mixer pages and Track Mute qualify, stopped or during ordinary sequence playback.
Single solo requires qualified user peers with automation Off or Read. A Write or
unknown peer returns `AUTOMATION_PEER_UNQUALIFIED` before invocation.

Read changes use the existing notifying native setters once. They do not switch
Off/Write, navigate to gain permission, record/edit/remove timeline events, or
repeat a setter during verification. Sequence mute's native per-Clip parameter
value update is intentional and distinct from editing scheduled timeline events.
Volume retains global scope, binary32 expected/no-op comparisons, original
numeric requests and the mixer generation fence.

`applied` means the required value was observed at completion. It does not reserve
that value against subsequent automation. An applicable scheduled source can
replace a manual value on a later native update. If it replaces the value before
the required completion observation, bounded read-only verification may finish
`unknown` / `VERIFICATION_TIMEOUT` after a single native invocation. The receipt
retains its actual execution stage and observed value. Neither case promises a
permanent override, audibility, persistence or sample-accurate timing.

The owner's normal Read session supplies both useful controls: a plugin without
applicable volume automation and audio tracks with populated Program-volume
sources. Its per-sequence mute parameter maps are populated too. The earlier
qualification harness assumed a manual restoration would remain unchanged during
playback; native volume automation disproved that assumption. Those preliminary
runs are retained in the private evidence, restored successfully, and are not
counted as passing final matrices.

## Native routes and source ownership

The validated control profile pins the following routes and case landmarks in
addition to #49/#39's family, property, filter, forwarding and mixer paths:

- `0x25b7814` binds a Track/Program to the active Project's Clip resolver. Its
  `{Track*, Sequence*, sequence-source}` key uses Project `+0xe4`. Resolver
  `0x253ea48` reaches `0x25b9d88`; the default Sequence Track-to-Clip tree has its
  header at `+0x524`, root `+0x528`, Track key at node `+0x10`, and shared Clip at
  `+0x14/+0x18`. This is the native association, including user and bus entries,
  rather than a UI lane or a cached `HasAutomation` flag. The binder skips its
  subscription when that actual map has no Clip. Global mute/solo/volume use
  the canonical map, including an absent Clip or a new Track whose Clip exists
  before its mute subscription is bound. Sequence-mute changes require
  an existing active Clip/updater and otherwise return `AUTOMATION_SOURCE_UNQUALIFIED`.
  The API does not create a Clip implicitly.
- The mute property's live `PerSequenceParameterValueUpdater` subscription has
  vtable `0x69347cc`, member callback `0x25b598c`, zero member adjustment and key
  `{parameter: 0x100, instrumentIndex: 0x101}`. Its capture retains the actual
  shared Clip. The updater belongs to the target Project (`+0x30`). Its applying
  guard at `+0x64` must be clear. The notifying setter remains `0xe298bc` on
  Program `+0x50`; the updater changes that Clip's parameter map at `+0x460`,
  notifying through `0x23c5d54`.
- Solo keeps requested Program `+0x88/+0x97`, setter `0xe4aee8`, native engine
  copy, Program suppression `0x2513e14`, parameter notification buffer
  `0x251c2d4` and follow/Single-Multi behavior `0x25849e0`. A Single operation
  checks all possible user peers, including their live Clip associations and
  automation modes. It observes their effects instead of restoring selection
  inside the command.
- Volume keeps Mixable receiver Program `+0x3c`, native setter `0x236ac88`,
  normalized model `+0x5d8` and format 3. Native parameter application at
  `0x242e8fc` reaches the pinned mute/solo/volume cases at `0x2430e04`,
  `0x2430de4` and `0x2430e7c`. The volume case uses the same Mixable setter;
  scheduled volume uses parameter 7 in the Program namespace (`instrumentIndex`
  `0x101`). The solo case reaches the native engine-side value route; it must
  not be confused with a manual requested-solo observation.
- Scheduled source dispatch/iteration at `0x246f2a8` and `0x2470398`, event
  replacement/invalidation at `0x240d54c`/`0x240ce28`, and the native playback
  switches are pinned. The latter live in Sequencer `+0x2fc`, values `+0x54`
  (mute/solo playback) and `+0xd8` (level/pan playback). A source Clip must have
  vtable `0x692f010` and its event-list holder at `+0x388` must have vtable
  `0x6896efc`.

Production qualifies populated and absent applicable events through the same
native routes. It never treats event absence as authorization. On every policy
sample it resolves the current Sequence, compares the subscription's Clip and
shared owner with the Sequence's Track-to-Clip map, and checks the source/list
owners, event-tree structural header and playback preferences. Two samples must
agree. Single solo also checks its peers. Source loss or replacement after the
initial dispatch sample returns `AUTOMATION_SOURCE_CHANGED`; an unrecognized
initial source returns `AUTOMATION_SOURCE_UNQUALIFIED` at the invocation boundary.

The command fence identifies source ownership and structural changes. It is not
a digest or transaction over all automation event values. In-place event/value
changes that preserve qualified owners do not remove manual-write eligibility;
new native values still participate in expected-value and completion observations.
The API makes no lossless event-edit or intervening-ABA detection claim.

Newly added tracks can have an active Clip before MPC binds their mute updater.
The device control rejected sequence writes before invocation in that state.
An ordinary UI switch to another existing sequence and back bound the native
subscriptions for the four empty research tracks, without changing their events
or parameter maps. Each family then passed a direct native mute-parameter update
and restoration check. This is fixture preparation; the API never navigates,
creates a Clip or installs a subscription to obtain permission.

## Delivery and observation

Expected values still precede no-op detection. Matching requests skip both setter
and filter, including when source restrictions prevent an actual write. Actual
Read writes cross the separately qualified private invocation boundary. Final
policy and lightweight completion observations must agree. A lagging copy or
superseding native value triggers bounded observation, never another setter.

Caller UID namespaces, FIFO admission, deduplication/request conflicts, original
start budgets, get_command/retries, disconnect ownership and draining shutdown
are unchanged. Source reads are bounded metadata traversals, with no names, bus
metadata, timeline-event scan or full snapshot per command. The native command
and shared observation paths retain their existing known/coverage fields.
The independent research runner scans timelines only before/after experiments,
outside MPC's UI thread, to verify restoration.

Discovery and per-track flag/mixer operations expose `automation.read`,
`readBehavior`, `readConditions` and `timelineEdits: false`. These are conditional
firmware qualifications; they do not cache a live permission. C layouts and
transfer tags stay at primitive 2, executor 9, commands 7, observations 2 and
snapshot 5 / STATE_V5. The three source/peer errors are additive enum values.

## Reproducing qualification

Host, sanitizer and isolated ARM cases cover actual changes and no-ops, six-family
flags and four-family volume, selected/unselected targets, screens/playback,
expected conflicts, populated structural sources, malformed/replaced sources,
wrong active-sequence associations/shared owners, cross-family Single/Multi peers,
float endpoints/rounding, stale identities/generations, delayed copies and native
value drift. Existing retry/disconnect, FIFO, pressure and shutdown cases remain
part of those suites.

Use an authoritative temporary lab and prepare Read targets, matching mute scope,
recording Off and zero playhead. The runner never changes automation settings:

```sh
python3 tools/mpc_read_writes.py --attachment build/us53/release-attachment.json \
  --targets 0 4 --scope sequence --screens main mixer_volume track_mute \
  --output build/us53/read-matrix.json
```

Raw reports remain under ignored `build/`. Final device coverage, latency/load
measurements, restoration and installed availability are recorded in the linked
proof; implementation qualification does not imply that 0.11.0 is installed.
## Final device qualification and package

The final canonical build passed all 21 host Meson entries, all 16 native
ASan/UBSan entries, seven isolated ARM fixtures and ARM ELF/export/deployment
guards. The final device matrix contains 896 distinct rows: both native mute
preferences, all seven screens, stopped/playback, selected/unselected and every
advertised family/operation. Flag rows make an actual transition and restore it;
volume rows exercise 0, 1 and binary32 rounding. Independent native reads verify
the results. The three-screen global matrix additionally checks no-ops, expected
conflicts and retained replay; earlier sequence regression and host fixtures
cover those delivery rules too. Six direct Clip checks verify intentional mute
parameter changes while scheduled events stay unchanged.

Twenty-four Single/Multi cross-family solo cases and 13 controller/manual and
discarded-response cases passed. A mixed playback burst submitted 64 requests
from eight clients alongside an 8 Hz target observer. All 64 applied, and all
nine observer polls completed without errors.
Successful observation callbacks: p95 9.021 ms, maximum 9.021 ms.
The actual completed observation rate was 2.405 Hz;
8 Hz is the target, without a delivery guarantee under pressure.

Across unpressured invoked matrix commands, callback median/p95/max were
28.507/51.960/71.456 ms;
receipt median/p95/max were 37/60/513 ms.
The saturated burst's maximum callback was 73.341 ms.
The #52 optimized small-matrix baseline was 12.669 ms median, 33.838 ms p95,
with 43 ms receipt p95; #39's saturated maximum was 145.804 ms. Read source and
Single-peer checks add metadata work. These differing workloads do not establish
causal overhead or a real-time budget guarantee. The implementation retains
lightweight observations and performs no timeline scan in a command callback.
The Read matrix exceeds #52's 25 ms callback p95, 40 ms callback maximum and
50 ms receipt p95 targets; the pressure observer exceeds its 3 ms p95 target.
Populated sources can prolong bounded verification; the proof records
7 timeouts among populated-source playing-volume samples and
49 independently observed values superseded after an applied receipt.

All original track/bus flags, mixer values, Read modes, native parameter maps and
event digests, sequence/selection, settings, zero playhead and Mixer Volume view
were restored. The four empty temporary tracks were removed. The lab drained and
unloaded, its files were removed and installed 0.10.0 resumed ready; all five reads
matched independent observations and schemas. MPC was not restarted and no
project was saved or loaded.

Package 0.11.0 was built from clean production sources at `27ccb67`.
Archive SHA-256: `193291019ab6b7d7a79d55e5531047d26ef7a8d39b002ee38e6ead2312d2f2e9`.
It remains a local artifact pending owner-approved install/restart and installed
Read-write verification; US #53 stays In progress until that step is completed.
