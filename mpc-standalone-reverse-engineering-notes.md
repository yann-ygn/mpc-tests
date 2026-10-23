# MPC Standalone Reverse-Engineering Notes

## Goal

Explore whether an Akai MPC standalone device—specifically the **MPC Key 37**—can be exposed through a usable control/state API.

The target is something conceptually like:

```json
{
  "sequence": 2,
  "track": 5,
  "playing": true,
  "tracks": [
    {
      "name": "Drums",
      "type": "drum",
      "mute": false,
      "solo": false,
      "armed": false
    }
  ]
}
```

with commands such as:

```text
mute track 4
arm track 7
select sequence 3
play
stop
open mixer
```

The important discovery so far is that the MPC standalone application appears to expose several different surfaces that can be combined rather than requiring a full reverse-engineering of the application from day one.

---

## Relevant Repositories

### akai_standalone_remap

https://github.com/mmiroshnikov/akai_standalone_remap

Purpose:

- Remaps physical MPC controls.
- Uses `LD_PRELOAD`.
- Intercepts ALSA raw MIDI access.
- Targets the internal/private MPC controller MIDI path.
- Can translate one physical action into another.
- Can emit native MPC button/pad actions.
- Can trigger `Menu + Pad` mode shortcuts.
- Supports single press, double press, long press and modifier-style mappings.
- Can also inject touchscreen events through Linux input devices.

Key architectural clue:

```text
physical MPC control
        ↓
internal/private MIDI
        ↓
LD_PRELOAD shim
        ↓
replacement button/pad/touch action
        ↓
MPC application
```

This proves that control injection does not require an internal MPC API. The MPC application can simply be made to believe that another physical control was used.

---

### MPCLiveXplore-libs

https://github.com/TheKikGen/MPCLiveXplore-libs

Related project:

https://github.com/TheKikGen/MPC-LiveXplore

Purpose:

- Reverse-engineers the MPC controller communication layer.
- Uses `LD_PRELOAD`.
- Intercepts MPC raw MIDI/controller traffic.
- Can create virtual ALSA MIDI devices.
- Can transform controller messages.
- Can attach external controllers.
- Can inspect traffic in both directions.
- Contains hardware/model spoofing functionality such as pretending to be an MPC X or Force.

Important tools/concepts include:

- `TKGL_CTRLDUMP`
- `TKGL_ANYCTRL`
- `TKGL_MPCMAPPER`
- MPC/Force hardware identity emulation
- `iamX`
- `iamForce`

The critical insight is that the private controller path is **bidirectional**:

```text
hardware → MPC application
MPC application → hardware
```

The outbound direction is potentially extremely valuable because it must carry whatever state the MPC application needs to display on the physical hardware.

Examples may include:

```text
button LEDs
pad colours
transport state
selected modes
display updates
meter values
channel-strip state
```

The exact encoding still needs to be mapped.

---

### mpc-addin-commander

https://github.com/jacob-sabella/mpc-addin-commander

Purpose:

- Runs as an injected add-in inside the MPC process.
- Uses `LD_PRELOAD`.
- Provides remote/control functionality.
- Demonstrates several independent ways of observing MPC state.

This project is especially relevant because it has been tested on the **MPC Key 37**.

It gives us three useful lessons.

#### 1. Project data can be read from the saved `.xpj`

The project file is effectively:

```text
ACVS header
+
gzip-compressed JSON
```

The parsed project data contains useful information including:

```text
tracks
track names
track types
current track
current sequence

track mute
record arm

mixer mute
solo
volume
pan

sequence names
tempo
bars
loop information
time signature

plugin information
```

Important limitation:

> This is saved project state, not necessarily the current live state.

If a track is muted after the project was last saved, the `.xpj` may still contain the older value.

Therefore the `.xpj` is best viewed as a **state bootstrap / project database**, not a live API.

#### 2. Transport can be observed through MIDI/MMC

Some live transport information can be obtained without touching MPC internals.

Potential examples:

```text
play
stop
record
position
tempo/clock
```

#### 3. In-process hooks are practical

Commander intercepts `dlsym()` and obtains live access to third-party VST instances.

That proves that useful live objects can be observed from code executing inside the MPC process.

This is important as a fallback if the controller bus does not expose enough state.

---

# Current Model of the MPC

The best working architecture is currently:

```text
                         MPC application
                              │
         ┌────────────────────┼────────────────────┐
         │                    │                    │
         ▼                    ▼                    ▼
      .xpj file          Private MIDI          MIDI/MMC
         │                    │                    │
 project metadata       controller state       transport
 initial snapshot        + feedback             state
         │                    │                    │
         └──────────────┬─────┴────────────────────┘
                        │
                  STATE DAEMON
                        │
                ┌───────┴────────┐
                │                │
              API            commands
                │                │
           query state     inject controls
```

A deeper fallback path could later be added:

```text
MPC process
    │
in-process hooks
    │
internal objects / functions
```

The working principle is:

> Use the cheapest available interface for each type of data.

---

# What Each Layer Could Give Us

## `.xpj`

Best for:

```text
track count
track names
track types
sequence information
plugin assignments
initial mute state
initial solo state
initial arm state
volume
pan
project topology
```

Advantages:

- Rich.
- Easy to parse.
- Already demonstrated by `mpc-addin-commander`.

Disadvantage:

- Not guaranteed to reflect unsaved live changes.

---

## Private controller MIDI

Potentially best for:

```text
live mute changes
live solo changes
record-arm changes
selected track
selected mode
pad bank
button illumination
pad colours
current channel-strip state
meters
display updates
```

This is the most important area to investigate next.

The MPC application must send controller feedback to the hardware.

The main question is:

> How much useful MPC state is already leaking through this controller feedback protocol?

---

## MIDI/MMC

Best for:

```text
play
stop
record
transport position
clock / tempo-related information
```

This appears relatively straightforward and is already used by Commander.

---

## Input Injection

Already proven possible through:

- Private MIDI control injection.
- Synthetic MPC button/pad events.
- Menu + Pad shortcuts.
- Touchscreen event injection.

This can provide the command side of the API.

Conceptually:

```text
POST /track/5/mute
        ↓
inject MPC hardware action
        ↓
MPC executes normal native behaviour
```

---

## In-Process Hooks

Use only when needed.

Potential targets:

```text
live track state
live sequence state
internal project objects
Akai plugin state
state unavailable through controller feedback
```

Commander proves that arbitrary code can execute inside the MPC process and can intercept useful application/library interactions.

However, this should be treated as the expensive/deep reverse-engineering path rather than the first approach.

---

# Why the MPC X / X SE Matters

The MPC X/X SE hardware is interesting because its front panel exposes richer state than the Key 37.

Its hardware surface needs information for things such as:

```text
Mute
Solo
Record Arm
Automation state
selected/target channel
display information
level meters
```

Therefore the MPC software necessarily has some mechanism for sending that information to the X hardware.

This creates an important hypothesis:

> If the Key 37 can impersonate an MPC X, the application may emit a richer controller feedback stream.

KikGen's work already contains hardware identity spoofing such as `iamX`.

This does **not** prove that all desired states will become available, but it makes the experiment very worthwhile.

---

# Why Meters Are Particularly Interesting

A mute LED is only a boolean.

A meter requires continuous dynamic feedback.

Conceptually the MPC application must send something resembling:

```text
meter L = value
meter R = value
meter L = value
meter R = value
...
```

Therefore continuous/high-rate traffic while audio is playing may be especially easy to identify when examining MPC→controller traffic.

If meter traffic can be located, it also confirms that the controller protocol is carrying more than simple button illumination.

---

# State Reconstruction Strategy

We may not need the controller protocol to provide a complete state snapshot.

A useful approach is:

```text
saved .xpj
    ↓
initial project/state model
    +
live controller events
    ↓
updated local state
```

Example:

```text
.xpj says:
Track 4 = unmuted

later controller event says:
Track 4 mute toggled

local state becomes:
Track 4 = muted
```

If our own API performs an action, we can also update the local model optimistically:

```text
POST /tracks/4/mute
        ↓
inject action
        ↓
local state predicts mute=true
```

Then controller feedback can confirm or correct that state.

This substantially reduces how much information must be observable continuously.

---

# Recommended Key 37 Investigation

Avoid deep Ghidra work initially.

Start with controller traffic.

## 1. Enumerate devices

Inspect:

```bash
amidi -l
aconnect -l
```

Identify:

- Key 37 private MIDI input.
- Key 37 private MIDI output.
- Public MIDI ports.
- Relevant ALSA raw-MIDI devices.

---

## 2. Capture hardware → MPC

Press one control at a time and build a Key 37 control map.

Suggested controls:

```text
MAIN
MENU
TRACK MIX
SEQ
ARP
PAD PERFORM
SHIFT
+/-
PLAY
STOP
REC
OVERDUB
pad banks
Q-Link controls
```

Goal:

```text
physical control → raw message
```

---

## 3. Capture MPC → hardware

Perform one controlled state change at a time:

```text
play
stop
record
overdub

select track
mute track
solo track
record-arm track

change sequence
change pad bank
change mode
```

Diff the resulting traffic.

Goal:

```text
MPC state change → outbound controller message
```

---

## 4. Look for meter traffic

Start playback with audio present.

Look for messages that:

- repeat continuously;
- vary with signal level;
- stop or change when playback stops;
- correlate with channel level.

This traffic may be especially obvious.

---

## 5. Test MPC X identity spoofing

Repeat the same captures in:

```text
normal Key 37 mode
```

and:

```text
iamX mode
```

Compare:

```text
normal        iamX
------        -----
baseline      baseline
mute          mute
solo          solo
arm           arm
track change  track change
play audio    play audio
```

The important question is:

> Does `iamX` make the MPC application emit additional feedback that the Key 37 normally does not require?

---

## 6. Correlate with `.xpj`

Parse the active/saved project and record known state:

```text
track names
mute
solo
arm
volume
pan
selected sequence
selected track
```

Then compare those values with captured controller messages.

Known project state provides a useful Rosetta Stone when decoding unknown outbound messages.

---

## 7. Build a state map

Maintain a table such as:

```text
Feature             Source             Live?
------------------------------------------------
Track count         .xpj               No
Track name          .xpj               No
Track type          .xpj               No
Sequence list       .xpj               No

Play                MIDI/MMC           Yes
Stop                MIDI/MMC           Yes
Record              MIDI/MMC           Yes

Selected track      Private MIDI       ?
Mute                Private MIDI       ?
Solo                Private MIDI       ?
Record arm          Private MIDI       ?
Pad bank            Private MIDI       ?
Meters              Private MIDI       ?
Track display       Private MIDI       ?
```

This will tell us exactly which gaps remain.

---

# Only Then: Deep MPC Internals

If important state is still unavailable after:

```text
.xpj
+
Private MIDI feedback
+
MIDI/MMC
```

then investigate MPC internals.

Possible approaches:

```text
LD_PRELOAD interposition
symbol inspection
shared-library inspection
Ghidra / static analysis
runtime tracing
hooking state-changing functions
identifying project/track objects
```

Commander already proves that a resident injected library can operate successfully inside the MPC process.

The goal should be to reverse-engineer only the missing pieces rather than the entire application.

---

# Desired End State

A small daemon could eventually expose something like:

```http
GET /state
GET /tracks
GET /sequences

POST /transport/play
POST /transport/stop

POST /tracks/4/mute
POST /tracks/4/solo
POST /tracks/4/arm

POST /sequence/3/select
POST /screen/mixer
```

Internally:

```text
Queries
   ↓
local state model
   ↑
.xpj + controller feedback + MIDI/MMC

Commands
   ↓
button/pad/touch injection
   ↓
MPC
```

An MCP server could then be layered on top of that API if useful.

---

# Current Working Hypothesis

The most promising architecture is:

> **Use `.xpj` as the project database, MPC private MIDI as the live state/event bus, MIDI/MMC for transport, and injected hardware/touch actions for commands. Only reverse-engineer MPC's internal objects for whatever those mechanisms cannot expose.**

The immediate next step is therefore not full binary reverse-engineering.

It is:

> **Capture and decode the Key 37 private controller protocol in both directions, then repeat the experiment while impersonating an MPC X.**

That experiment should tell us very quickly how close the existing control-surface protocol gets us to a complete MPC state/control API.
