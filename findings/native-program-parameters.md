# Native Program / Instrument parameter map (US #45)

Metadata schema **1.0**, catalog **1.0.0**, MPC Key 37 / MPC **3.9.1.2**,
GNU build ID `8371199f945ecc40f79cb19c17ff5de72810a91c`.
This is a narrow research qualification. **There is no public `set_parameter`
operation.** Discovery advertises it unavailable; command parsing, retained
receipts, snapshots and watch do not implement generic parameter access.

The [versioned catalog](../native-api/parameters/key37-3.9.1.2.json) and
[JSON Schema](../native-api/schema/parameter-metadata.schema.json) distinguish
verified native controls, static candidates, enum-only identifiers, explicitly
unused identifiers, legacy identifiers and unsupported families. A classification
is evidence about the stated receiver/scope, never permission for a public write.
Only six controls on **drum pad zero** have a device-qualified path in this
revision. Keygroups, other pads/layers, automation, playing, plugin parameters
and insert parameters require separate qualification.

## Receivers and callable ABIs

The active Project comes from the existing application/window/engine owner path.
The temporary lab uses the existing destruction-aware identity registry, resolves
the complete instance/project/track generations on **each UI callback**, and
checks Track → Program association. Track `+0x648` owns the Program. The drum
Program vtable is RVA `0x06930c00`; its 128-entry Instrument pointer vector starts
at `+0x1fec`. Index zero addresses pad A01, not Track index zero. The Instrument
vtable is `0x0692f650` (`Instrument`). No native pointer is a request target.

| Path | RVA / ABI | Meaning |
|---|---|---|
| Program getter | `0x0250b754`, `float (Program*, int index, int id)` | Dispatches to family virtual slot `+0xa8` except special Program/Mixable parameters |
| Drum getter | `0x0249fae4` | Resolves Instrument by pad index, then Instrument virtual slot `+0x44` |
| Instrument getter | `0x02424530`, `float (Instrument*, int id, bool current)` | Shared stored/current reader; thunks `0x02426d10` / `0x02426d18` supply false/true |
| Program setter | `0x0250ba74`, `void (Program*, int index, int id, float value)` | Native dispatch via Program's lifetime-aware proxy at `+0x40` |
| Native dispatch | `0x025175cc` → Program virtual slot `+0xb4` | Executes the normalized family write; the lab does not construct native work items |
| Drum normalized setter | `0x024bb68c` | Resolves Instrument then virtual slot `+0x40`, `0x0242e8fc` |
| Parameter classification | `0x0239359c` | Recognizes SynthSection parameters before the Instrument switch fallback |
| Range/default metadata | `0x02392e18` / `0x02392dbc` | Native metadata lookup; not a universal normalized-to-display conversion |

All native calls use the firmware's **32-bit little-endian ARM hard-float ABI**:
`this`, index and ID occupy integer registers; the normalized binary32 value and
return value use VFP. Calling a C++ wrapper's registration address is incorrect.
The lab fingerprints sixteen landmarks plus the adapter's existing firmware,
model, dispatch and lifecycle guards before exposing its private socket.

The native setter returns before its queued update necessarily appears in the
model. Immediate readback can retain the old value. Qualification waits for a
later UI getter **and an independent external read of the model fields**. Setter
return is not completion, and no result establishes audible or sample-accurate
completion. Callback captures drain before observer detach and DSO unload.

## Verified subset and independent observations

| Catalog key | Native ID | Getter / independent field | Native unit |
|---|---:|---|---|
| `kRTCutoff` | `0x202` / 514 | `SynthSection +0x24c` | Normalized binary32 |
| `kRTEnvAttack` | `0x207` / 519 | `SynthSection +0xaa0` | Normalized binary32 |
| `kLfoRate` | `0x441` / 1089 | `SynthSection +0x5cc` | Normalized binary32 |
| `kLayer1Level` | `0x40f` / 1039 | Layer 0 `+0x87c` | Normalized binary32 |
| `kLayer1SamplePan` | `0x410` / 1040 | Layer 0 `+0xe4` | Normalized binary32; centre 0.5 |
| `kLayer1SemiTune` | `0x413` / 1043 | Layer 0 signed int `+0x30` | −36..36 semitones; normalized `0.5 + native/72` |

All six setter inputs use **0..1 inclusive**. The Layer vector is owned at
Instrument `+0x139c`, stride `0x8b8`; the SynthSection pointer is at `+0x13c8`,
vtable `0x068ac840`. The two-stage SynthSection path matters: a local Instrument
switch can appear to fall through, but its earlier classification redirects
these filter/envelope/LFO IDs to real properties. Do not classify an ID using an
isolated switch table or its name alone.

At −36 semitones the traced tuning getter's fused binary32 formula yields
`-3.7252903e-9` rather than exact zero. The private lab canonicalizes endpoint
overshoot within one binary32 epsilon **only for this integer tuning parameter**;
all caller inputs remain strictly inside 0..1. An explicit host/ARM regression
fixture checks this boundary and rejects wider errors, non-finite values and
overshoot on other parameters. The independent reader verifies the exact native
integer, not the canonicalized getter alone.

Cutoff reaches the notifying float property at SynthSection `+0x224`, amp attack
at `+0xa78`, through `0x00e5af48`; the native path also forwards updates to matching
live synth voices. Layer level/pan use filtered/notifying paths
`0x0243adbc` / `0x0243b014`. Tuning uses `0x023798cc` and integer conversion.
The lab never writes the observed bytes directly. Ranges in Hz, milliseconds,
dB, tempo-synced LFO divisions, continuous tuning rounding and perceptual display
curves are **not qualified** by the normalized endpoint tests.

Only stopped ordinary sequence mode, recording/pending recording Off, local
transport, Automation Off and native Main/Mixer/Track Mute with no popup qualify
the lab. The installed API's existing Off-only mixer policy and transport/source
checks are additional conservative gates, not evidence that mixer qualification
alone permits arbitrary parameter writes. Expected values precede no-op checks;
no-op/conflict paths never invoke the setter. Read/Write modes are not qualified.

These expected checks occur at the lab's UI enqueue boundary. The native write
may execute later; this research does not establish an atomic precondition at
that final dispatch. Program replacement, pad/layer/SynthSection replacement,
reloads during an outstanding native write, and concurrent operator edits are
outside this private qualification. A future public command needs those narrower
incarnation fences and retained completion semantics in addition to Track refs.

This path executes without an UndoManager transaction supplied by the caller.
The lab creates no explicit undo action. User-facing undo/redo behavior and
transaction grouping remain unqualified; a future public handler must define
those semantics rather than inheriting the scripting frontend's assumptions.

## IDs that are not enabled controls

`kAmpEnvParam1_Unused` / `0x42d` and
`kFilterEnvParam1_Unused` / `0x431` are explicitly unused identifiers, with no
lab/public access. `kRTCutoffGlobal` / `0x22b` is a separate global/keygroup lead;
its presence does not qualify the drum-pad route. The CV-program identifier is
outside this qualified family; this is not a claim that Key 37 lacks CV hardware.
Other identifiers remain static leads in the historical
[catalog](native-control-catalog.md). No identifier is labeled legacy or
unsupported on hardware solely because its spelling looks old or model-specific.

The metadata schema requires explicit classification, qualification, family,
index/layer scope, value units, independent readback and evidence for every row.
Unknown units can stay unknown; unsupported entries carry null value/readback
metadata and false public access. The validator binds metadata to the existing
firmware profile and rejects duplicate IDs/keys, missing native landmarks,
unsupported families in the lab allowlist and evidence/qualification mismatch.
Header generation includes only the six deliberately selected research targets,
never unused/enum-only entries. A static candidate is available only for private
qualification, not a verified control or public capability.

## Plugin and insert paths are separate

A plugin Program uses **namespace/index `0x100`**, with parameter IDs
`0x1000..0x1fff`; `0x024f9098` subtracts the base and reads a loaded parameter bank
at Program `+0x1a4c`. Its normalized setter is `0x0250647c`, not the drum setter.
Metadata, names, ranges, loading and parameter topology depend on the plugin.
The [assigned product identity](native-plugin-identity.md) is not a parameter
instance identity or preset API. No plugin parameter was written by this story.

The Instrument setter recognizes **IDs `0x2000..0x5fff`** as insert leads, resolves
an effect through its effect rack (`Instrument +0x5a4`, lookup `0x0235d13c`), checks
the effect's live availability and calls virtual slot `+0xa4` with the **low 12
bits** as parameter index. The upper ID bits choose the encoded rack namespace.
This is separate from rack/slot *enable* commands already qualified by US #39.
Slot numbering, occupancy, insert identity/generation, plugin format, ranges,
parameter names and Undo/automation behavior remain unqualified. No insert
parameter was written; arbitrary plugin/insert control is not promised.

## Reproduction and evidence

```sh
uv run --no-sync python tools/mpc_program_parameters.py validate
uv run --no-sync meson compile -C builddir/arm
# Existing disposable drum Track, zero-based index; idle on a qualified view:
uv run --no-sync python tools/mpc_program_parameters_qualify.py \
  --track 2 --output build/us45/device.json
```

The runner first captures state and independent fields, self-tests the loader
in a disposable ARM child, pauses the installed native API host, and loads the
zero-export lab. It applies five normalized values per control, checks native
and independent readbacks, no-ops and expected conflicts, restores exact native
fields, detaches observers, drains/unloads, removes its files and resumes the
installed host. Failed native work is never freed/unloaded merely on timeout.
The report preserves private recovery coordinates before any writes.

The [sanitized qualification proof](native-program-parameters-proof-20261029.json)
records **35 complete value/readback cases**, 35 no-ops, 35 expected conflicts,
twelve native Automation Read/Write rejections, malformed/unknown-ID and stale
generation rejection, and a plugin-family rejection. Five controls qualified on
the disposable empty drum; the corrected tuning run supplies its five complete
cases, and another five LFO cases pass on the unselected loaded drum kit. The
initial incomplete tuning endpoint row is excluded from the totals; the original
integer was restored through MPC's native keypad before the corrected run.

All **27 host Meson entries**, **22 native-api ASan/UBSan entries**, the isolated
ARM endpoint fixture and ARM ELF/export/glibc guards pass. Exact parameter fields
on both drums, original flags/buses, Track/Sequence selection, Automation Read,
zero playhead and Main are restored. Lab mappings/files are gone; installed
**0.15.0** resumes ready. No project save/load, addin installation or MPC restart
occurred. No public C layout or snapshot/watch ABI changes are needed.
