# Native-control candidates on MPC 3.9.1.2

2026-10-24, Key 37; executable build ID `8371199f945ecc40f79cb19c17ff5de72810a91c`.

**This is a static inventory for choosing the next tests.** Mute, solo and record arm are the only underlying
property writes already validated through the temporary native bridge. Everything else below is a compiled binding
candidate, not a successfully invoked command. This inventory used the local executable copy only; no device
commands, state changes or additional native-call tests were performed.

The [machine-readable catalog](native-control-catalog.json) records 37 wrapper classes and 368 named registration
entries, with the registration implementation and name-reference RVAs. These entries mix methods, properties and
some readback names; they are not 368 independent writable endpoints. Short names constructed inline were checked
separately. The scan is an inventory of inspected wrappers, not an exhaustive list of every native function in MPC.

| Area | Candidate operations / registered properties |
|---|---|
| Track mixing | Volume, pan, send levels; global insert-rack enable, individual insert enable; reset channel strip |
| Track selection and setup | Select track; create/remove/copy tracks; rename, colour, transpose, velocity scale, mute-group assignment, independent length, pads-follow-colour, skip-from-row-launch |
| MIDI and CV routing | Track input/output ports and channels; MIDI bank enable/MSB/LSB and program-change enable/number; CV and gate ports; all-notes-off |
| Transport | Play, play from start, stop, record, overdub, record off, toggle playback; locate and step by bar, beat, pulse or event; jump to start/end, pulse or sample |
| Tempo and sequence selection | Set BPM; select current sequence; loop property; read current BPM/playhead/loop boundaries |
| Recording setup | Count-in enable; punch-in enable and punch start/end in bars/beats/pulses |
| Clips | Create, select, edit, copy, delete, trigger and stop; rename; set start/end and loop start/end; flatten, including an Elastique variant |
| Rows / scenes | Select, insert, capture into a new row, copy, clear, delete, launch; row colour and tempo trigger; trigger arrange track |
| Songs | Select song, rename, append/insert/delete steps, change a step's sequence/repeat count, clear song |
| Note events | Create notes with position, length, velocity and probability; move events; change start/end, pitch, velocity, note-on modifier and ratchet; split/delete events |
| Audio events | Create from a sample path; change mute, volume, reverse, fades, warp enable, event tempo and coarse/fine tuning; move/trim/split/delete |
| Automation | Create track/program/instrument automation events and sequence-tempo-change events; set automation values; track/global automation-filter selection |
| Quantizer | Time-division getter/setter; named properties for enable, swing, timing shift, window and strength |
| Arpeggiator | Named properties for enable, action, latch, arp/rhythm/pattern selection, velocity, note length, aftertouch action, accent levels/probabilities, swing, variation, octaves and sustain-pedal latch behaviour |
| Plugin presets | Next/previous preset for the current track |
| Samples | Add/import, remove, remove all, list names and retrieve metadata |
| Projects and imports | New/load/save project; load tracks/sequences/programs; import MPC 2 projects to clips or sequences; save track and track FX rack |
| Undo history | Undo, redo, availability queries, undo to an index, command count and reset |
| UI navigation | Read/select engine mode, inspect current popup and push popup buttons; mouse/touch down/up/drag; grid/aperture navigation and banking |
| Physical / virtual controls | Button press/release/hold/double-press; physical/virtual pad events; data-dial twist; Q-Link touch/twist/value/position; crossfader/touch-strip movement; registered light state properties |
| MIDI output | Send note on/off, CC and SysEx to an output |
| Engine and settings | Product/hardware/version queries; user-setting writes; thread synchronization; engine quit |
| Filesystem | File read, write, delete and size query |

Properties without explicit setters in the names still need access-mode tracing. Hardware-specific wrappers may
require a supported controller/model, and some wrapper objects may not exist in this production session. The
disabled scripting frontend is not being re-enabled: a future adapter would resolve the underlying objects and
dispatch their native methods as in the three validated tests.

## Useful readback candidates

- Track counts, names, types/capabilities, mixer flags, automation presence and independent-length type.
- Playing/recording status, playhead position, loop boundaries, TC period and tempo/tempo-source fields.
- Clip and track playing/recording/stopped state; selected clip coordinates and row/column dimensions.
- Song name, length, steps, sequence names/indices, repeats, bar counts and tempo.
- Event count and audio event at a time; sample names and metadata.
- Current engine mode/popup, available modes, attached controller, pressed pads/buttons and Q-Link state.

Companion state-export code also names future sequence index, recording modes, fixed-length/retrospective recording,
Link/tempo-source state, clip launch quantization and pending clip actions. Those are readback leads; a serialized
field name does not establish a writable property or an independently callable getter.

## Named-method anchors for later probing

| Wrapper | Representative exact names |
|---|---|
| `ScriptableTrackPool` | `SetCurrentTrack`, `SetTrackVolume`, `SetTrackPan`, `SetSendLevel`, `SetTrackMonitorState`, `SetTrackTransposition`, `SetTrackInputPort`, `SetTrackOutputChannel`, `SetInsertEnabled`, `ResetTrackChannelStrip` |
| `ScriptableCurrentSequenceWrapper` | `SetBPM`, `SetCurrentSequence` |
| `Scripting::ScriptableSequencerWrapper` | `Play`, `PlayStart`, `Stop`, `Record`, `Overdub`, `RecordOff`, `Locate`, `JumpToPulse`, `JumpToSample`, `JumpByBar`, `JumpByBeat`, `IsPlaying`, `IsRecording` |
| `ScriptableSequenceRecorderProperties` | `EnablePunchIn`, `SetPunchInStartBBP`, `SetPunchInEndBBP` |
| `ScriptableClipLanesWrapper` | `CreateClip`, `TriggerClip`, `StopAllClips`, `SetClipStart`, `SetClipEnd`, `SetClipLoopStart`, `SetClipLoopEnd`, `SetTrackName`, `TriggerRow`, `SetRowTempoTrigger` |
| `ScriptableCurrentSongWrapper` | `SetCurrentSong`, `SetName`, `PushStep`, `InsertStep`, `DeleteStep`, `ModifySequence`, `ModifyRepeats`, `Clear` |
| `ScriptableEventListWrapper` | `CreateNoteEvent`, `CreateAudioEvent`, `CreateTrackAutomationEvent`, `CreateProgramAutomationEvent`, `CreateInstrumentAutomationEvent`, `CreateSequenceTempoChangeEvent`, `SetNoteVelocity`, `SetNoteRatchet`, `SetAudioWarpEnabled`, `SetAutomationValue` |
| `Scripting::ScriptableQuantiser` | `SetTimeDivision`, `GetTimeDivision`; properties `Enabled`, `Swing`, `Shift`, `Window`, `Strength` |
| `ScriptableCurrentTrack` | `NextPluginPreset`, `PrevPluginPreset` |
| `ScriptableSamplePool` | `AddSample`, `RemoveSample`, `RemoveAll`, `GetSampleNames`, `GetSampleMetadata` |
| `ScriptableProject` | `New`, `Load`, `LoadTrack`, `LoadSequence`, `Save`, `SaveTrack`, `SaveTrackFXRack`, `ImportMpc2ProjectToClips`, `ImportMpc2ProjectToSequences` |
| `Scripting::ScriptableCommandManager` | `Undo`, `Redo`, `CanUndo`, `CanRedo`, `UndoToIndex`, `NumCommands`, `Reset` |
| `Scripting::ScriptableEngineMode` | `GetCurrentMode`, `GetCurrentPopup`, `SetCurrentMode`, `GetAvailableModes` |
| `ScriptableQLinkWrapper` | `Touch`, `Twist`, `SetPosition`, `GetPosition`, `SetValue`, `GetValue` |
| `Scripting::ScriptableMidiDeviceControlMapper::MIDIOutput` | `SendMIDINoteOn`, `SendMIDINoteOff`, `SendMIDICC`, `SendMIDISysex` |
| `Scripting::ScriptableMixdown` | `ExportClip` with track name, clip index and export-settings arguments |
| `Scripting::ScriptableEngineWrapper` | `Version`, `GetProduct`, `GetHardwareProduct`, `SetUserSetting`, `SyncThreads`, `Quit` |
| `Scripting::ScriptableFileHandler` | `Read`, `Write`, `Delete`, `FileSizeMB` |

The project wrapper's `LoadSequence` registration also supplies `loadProgram` and `replaceProject` argument labels.
This is evidence of a program-loading option in that path, not an independently established `LoadProgram` method.
Likewise, `ExportClip` establishes one mixdown/export candidate, not a complete project-export API.

## Program and instrument parameter leads

The inspected code additionally names parameter identifiers for:

- Pad level/pan/mute/solo, sends, inserts, output routing and mute groups.
- Filter 1/2 cutoff, resonance, types, routing and envelope amounts.
- Amp/filter/pitch/aux envelopes, curves, delays, looping and tempo sync.
- LFO 1/2, synchronization, reset, modulation amounts and randomization.
- Eight sample layers: sample selection, root note, levels, pan, tuning, ranges, slice/loop boundaries and direction.
- Warp, pad tempo, time stretch, slice crossfades and tails.
- Keygroup voice mode, portamento, unison, harmonizer and pitch-bend ranges.
- Modulation-matrix links and CV-program modulation/step/envelope parameters.

**These are parameter-name/enum leads only.** They are deliberately excluded from the confirmed command list. A
generic parameter getter/setter, its owning object, parameter indexing and valid ranges must still be traced. Some
identifiers are explicitly marked unused or belong to another model/mode. Arbitrary plugin-specific parameters and
insert-effect parameters have not been established by this inventory.

## Evidence and small correction

The named entries were recovered from wrapper RTTI/vtables and references in their registration implementations,
including short inline strings. No inferred wrapper-function addresses are assigned in the catalog: the evidence
RVA locates the registration/name use, not necessarily the eventual callable implementation.

The earlier additional-binding entry for `SetSendLevel` was corrected from `0x02868EE4` (the preceding transposition
getter) to `0x02868CC8`. Its registration constructs a callable after the method name; assuming the most recent
function pointer before a name associated the wrong function. The three tested property paths are unaffected.

Initial extraction/disassembly records remain in gitignored `build/internals/binding-identifiers.json` and
`build/internals/register-*.asm`. The retained catalog and this document contain the curated next-test list.
