# Live state from MPC's objects

2026-10-24, Key 37, MPC 3.9.1.2. **A read-only process-memory probe now retrieves live track and sequence state, and
a real snapshot was published and refreshed through Akai's HTTP object service.** No scripting execution, addin
installation, process attachment, memory writes, application function calls or MPC restart were required.

The user had already tested the embedded scripting route and found it disabled in production firmware. The binary's
script wrappers were useful static-analysis landmarks, not a runtime interface we attempted to enable or invoke.
The subsequent [native-control investigation](native-control.md) traced actual mute/solo/arm setters and their live
listeners. It also distinguishes script-facing solo/arm values from the audio-side copies observed here. A temporary
bridge subsequently verified native writes and restoration for all three flags, without scripting or a restart.

## The working route

```
/proc/<MPC pid>/mem, opened read-only
    -> locate Project by its virtual table and validate secondary virtual tables
    -> follow Project -> TrackPool -> Track -> Program
    -> follow Project -> sequence array and current Sequence
    -> bounded, typed reads -> JSON snapshot
    -> optional temporary native web publisher -> Akai HTTP object properties
```

Root SSH can read the memory file without stopping the application. The first check read a known mapped constant
and matched it to the executable; later probes read object fields. The helper never uses `ptrace`, modifies target
memory or calls a target function.

RTTI and virtual tables recovered from the executable identified one `Project`, one `TrackPool`, 128 `Sequence`
instances and 31 `Track` instances in the main heap. Secondary virtual tables agreed with the expected subobject
offsets. The Project's track-pool vectors, rather than the heap matches, established which tracks belong in the
snapshot: two normal tracks and 28 buses. The additional `Input` Track singleton is outside that all-tracks vector.

The selected track name read `Plugin 001`, matching Main. Selecting the existing `Trap-Kit-Default` track changed
the Project's pointer and zero-based index to the other verified Track, and selecting back restored both. This
establishes a live source for selection, independently of saved `.xpj` data or panel feedback.

## Firmware identity and layout

This layout is pinned to:

- GNU build ID: `8371199f945ecc40f79cb19c17ff5de72810a91c`
- Executable SHA-256 recorded during analysis: `bc054a3f3ba02c2d33ac9a515a4a8638964da779223502286d6a64b517bf1426`
- Runtime profile: [`tools/profiles/key37-mpc3.9.1.2.json`](../tools/profiles/key37-mpc3.9.1.2.json)

The reader checks the running executable's GNU build ID before using any object offsets. It derives the image load
address from `/proc/<pid>/maps`, adds that bias to virtual-table RVAs, and scans for the Project. Runtime heap
addresses are discovered, not fixed in the profile. A deliberately mismatched build ID was rejected before object
offsets were used. The SHA-256 above is an analysis record; the runtime guard uses the GNU build ID.

| Object | Field / offset | Evidence |
|---|---|---|
| Project | selected Track pointer `+0xA0`, mirror `+0xB0` | followed selection in Main, in both directions |
| Project | selected normal-track index `+0x15C` | changed 0 -> 1 -> 0 with those selections |
| Project | current Sequence pointer `+0xE4`, mirror `+0xF4` | points to the named Sequence displayed in Main |
| Project | sequence-array data `+0xA70`, capacity `+0xA74`, size `+0xA78` | 128 verified Sequence pointers; current sequence is at slot 0 |
| Project | TrackPool pointer `+0xA94` | points to the verified TrackPool |
| TrackPool | all-track vector `+0x34/+0x38/+0x3C` | begin/end/capacity; two normal tracks plus 28 buses |
| TrackPool | normal-track vector `+0x44/+0x48/+0x4C` | `Plugin 001`, `Trap-Kit-Default`, in UI order |
| Track | name character pointer `+0x468` | names for user tracks, submixes, returns and outputs |
| Track | record arm `+0x494` | selected plugin track REC-ARM 1 -> 0 -> 1, matching Multi Rec |
| Track | Program pointer `+0x648` | typed PluginProgram, DrumProgram and bus program instances |
| Sequence | name character pointer `+0x6C` | `Sequence 01`, matching Main; unused slots read `(unused)` |
| Sequence | BPM float `+0xC4` | 120.0, matching Main in sequence-tempo mode; tempo-change tests remain |
| Program | requested mute `+0x78` | plugin and drum MUTE 0 -> 1 -> 0 |
| Program | solo `+0xC0` | plugin and drum SOLO 0 -> 1 -> 0 |
| Program | silenced `+0x1AC` | explicit mute, or suppression by a different track's solo |

Program type comes from the dynamic class, not pad colours: `PluginProgram`, `DrumProgram`, `SubmixProgram`,
`SendProgram`, `OutputProgram`, `MasterOutputProgram` and `InputProgram` were present. The profile includes other
Program-class virtual tables found statically; their flags have not had the same controlled validation.

## Mute versus silencing

The two boolean locations distinguish a real semantic gap in the earlier controller reconstruction:

| Experiment | Plugin mute | Plugin silenced | Drum solo |
|---|---|---|---|
| Baseline | false | false | false |
| Solo the drum track | false | true | true |
| Restore solo | false | false | false |

Muting the drum itself instead sets its requested mute and silenced fields, while the plugin remains unmuted and
unsilenced. Thus `silenced` must not be presented as an explicit mute, and `!silenced` does not mean audio is actually
sounding. Per-sequence mute, automation and queued changes still need a fuller semantic test matrix.

## Reader and reproduction

[`tools/mpc_mem_probe.c`](../tools/mpc_mem_probe.c) is a standalone ARM helper for read-only memory scanning and
bounded binary reads. [`tools/mpc_live_state.py`](../tools/mpc_live_state.py) checks the firmware, discovers the
Project, validates pointers/vectors/types, and emits JSON. It needs only Python's standard library on the host.

```sh
mkdir -p build/internals
uv run --no-project --with ziglang==0.14.1 python -m ziglang cc \
  -target arm-linux-gnueabihf.2.31 -mcpu=cortex_a7 -O2 -Wall -Wextra -Werror \
  tools/mpc_mem_probe.c -o build/internals/mem_probe
python3 tools/mpc_live_state.py --output build/internals/live-state.json
```

`MPC_HOST` or `--host` selects the device. The Python reader uploads the helper to a unique `/tmp` path and removes
it afterward. No files in the MPC project or settings directories are written by the reader. Do not commit raw
memory, copied firmware, or exploratory captures under `build/`.

An actual snapshot included:

```json
{
  "selectedTrack": {
    "index": 0,
    "name": "Plugin 001",
    "type": "plugin",
    "recordArmed": true,
    "muted": false,
    "soloed": false,
    "silenced": false
  },
  "selectedSequence": {"index": 0, "name": "Sequence 01", "bpm": 120.0},
  "sequenceSlots": 128
}
```

Indices are zero-based; the UI displayed track 1 and sequence 1. The complete JSON also contains both normal tracks
and the 28 buses. `sequenceSlots` is allocated slot count, not the number of used sequences.

The observer also worked after returning to Sounds, so the data source is not tied to the Main screen. Typical
snapshot reads in this host prototype took about 2.5 seconds, with initial discovery adding roughly another two
seconds. Most operations are separate SSH calls; this is not yet a low-latency device daemon.

## HTTP proof

With a temporary publisher and a diagnostic-only paired session, a real memory snapshot was published under
`/<temporary client>/state` as its `snapshot` property. Then a second newly read snapshot replaced that property.

- Both GETs from `/api/v1/object-properties/<temporary client>/state` returned HTTP 200 and exactly matched the
  corresponding memory snapshot.
- Capture timestamps advanced; selected track/sequence and all track/bus values came from the actual MPC process.
- The temporary object, client and session were removed afterward; the HTTP client returned to 401/notPaired.
- Server access flags and blocked-address count were unchanged.

The sanitized verification record is in `build/internals/live-http-proof.json`; the live snapshot is in
`build/internals/live-state-sounds.json`. The experimental orchestration script is retained under
`build/internals/live_http_proof.py`; the reusable reader and native web client are in `tools/`.

## Limits and next validation

- **This is a firmware-specific observer.** A new build needs new layout validation; virtual tables and members are
  not a supported public ABI. Only this running Key 37 session has been tested.
- Reads are not atomic across all objects. The tool rechecks selection, topology and selected-sequence fields and
  retries on changes; it explicitly reports `atomic: false`. Simultaneous mute changes can still span samples.
- Project loading/replacement, renaming, reordering, large projects, heap-arena changes and multiple Project objects
  remain untested. The current scanner requires one validated Project in the main heap and rejects ambiguity.
- Mute/solo were controlled on plugin and drum programs, and record arm on the plugin track. Other families/buses
  were identified and read but need controlled semantic checks. Requested mute versus per-sequence mute remains open.
- Sequence name/index and 120 BPM matched Main. Master tempo, external sync, tempo changes, transport position,
  playing/recording state, volume and pan are not established by this reader. Do not treat sequence BPM as effective
  transport tempo without checking its source.
- The retained reader exposes snapshots; the HTTP test was a temporary publication proof, not an installed service.

The remaining engineering step is a bounded device-side observer/publisher, reusing the native web protocol and the
validated object roots. Keeping memory reads local would remove the current SSH round trips. Further probes should
validate the missing fields and object lifetimes before deploying a persistent service.

MPC stayed at PID 365 and the web service at PID 295. Track selection, mute, solo and record-arm values were restored;
the UI was returned to its original Sounds view. No projects were saved or loaded and no application was restarted.
