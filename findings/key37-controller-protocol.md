# MPC Key 37: the controller protocol, as observed

MPC Key 37, MPC 3.9.1.2, 2026-10-23. Read with privtap (`privtap/`), which logs every byte MPC reads from and writes
to the controller's rawmidi ports, and driven with `tools/mpcx.py`, which injects presses, taps the screen and diffs
what MPC sends back between two marks. The test project was the HipHop "TheDrop" demo (4 tracks) after the button
mapping. Everything below was seen on the device unless marked *inferred*.

## Ports

The controller is one USB MIDI device with three ports. MPC opens two of them directly through alsa-lib:

| Port | MPC reads | MPC writes |
|---|---|---|
| `hw:0,0,1` MPC Private | buttons, pads, keepalive and identity replies (owns the input; non-blocking) | button LEDs (CC) |
| `hw:0,0,0` MPC Public | nothing | sysex: identity request, keepalive, setup, pad colours |
| `hw:0,0,2` MPC MIDI Port A | through the ALSA sequencer, as an ordinary MIDI port | |

## Hardware to MPC

| Control | Message |
|---|---|
| Button | `90 nn 7F` pressed, `90 nn 00` released (channel 1). Notes: `privtap/profiles/mpc-key-37-buttons.conf`, the MPC Live's numbering |
| Pad | `99 nn vv` (channel 10, velocity sensitive), `89 nn 00` released. No aftertouch seen. The MPC Live's pad notes |
| Data wheel | `B0 64 01` one step right, `B0 64 7F` left (relative CC 100) — *inferred from injecting it*: it stepped the selected field |
| Keepalive reply | `F0 47 7F 57 53 00 05 … F7` every 4 s |

A combo is visible from nesting: `SHIFT down, PLAY down, PLAY up, SHIFT up`.

## MPC to hardware

**Akai sysex** on the Public port: `F0 47 <7F|00> 57 <cmd> <len hi> <len lo> <payload> F7` (0x47 Akai; 0x57 is the
family code in the hardware's identity reply).

| Command | What |
|---|---|
| `65` | pad colour: `<pad> <r> <g> <b>`, 7-bit RGB. Pad 0 is PAD-1 (bottom left), 15 is PAD-16; a 17th entry (`10`) is sent too |
| `52` / `53` | keepalive: MPC asks every 4 s, the hardware answers |
| `62`, `63`, `64`, `41`, `5A`, `5B` | sent once at startup with the identity exchange (`F0 7E 00 06 01 F7`); not decoded |

**LEDs** on the Private port: `B0 <button note> <value>`. Values seen: `01` dim (off), `03` lit, `04` a second colour
(AUTOMATION in read mode, TC while SHIFT is held). MPC also writes LEDs the Key 37 doesn't have (notes 3, 4, 5, 8,
41, 44–48, 113 …), and some of those carry state (below). 74, 76 and 77 are the MPC X's REC ARM, MUTE and SOLO buttons
for the selected track: MPC lights them on a Key 37 too, and obeys their notes when injected. CC 90–101 take `00`/`7F`/`03` and don't look like LEDs; 90/91
swap when the view changes. Not decoded.

When the screen dims, MPC turns the pads off and resends their colours when it wakes: pad state is only readable while
the screen is awake.

## The state map

From the plan in `mpc-standalone-reverse-engineering-notes.md`, step 7. "Live" means MPC sends it on change, with no
polling.

| State | Source | Live | How |
|---|---|---|---|
| Playing / stopped / record armed | LEDs | yes | stopped: STOP `03`; playing: PLAY `03`, STOP `01`; armed from stop: PLAY and REC blink |
| Tempo | TAP LED | yes | it toggles every half beat: BPM = 30000 / interval in ms (250 ms at 120, 180 ms at 167) |
| View (Main, Mix, Grid, Track Mute …) | LEDs | yes | the view's button LED is `03`, the others `01` |
| Pad bank | BANK A–D LEDs | yes | one of the four is `03` |
| Selected track muted | LED 76 (`0x4C`) | yes | `03` muted, `01` not; follows the selected track whichever way it was muted |
| Selected track soloed | LED 77 (`0x4D`) | yes | `03` soloed, `01` not |
| Selected track record-armed | LED 74 (`0x4A`) | yes | `03` armed, `01` not; the Main screen's MULTI REC button and the mixer's arm icon show the same |
| Mute and solo of every track (16 per bank) | pad colours, Track Mute mode | yes, in that mode | steady yellow `7F 7F 00` normal; blinking orange `7F 3B 00` muted; blinking cyan `00 7F 7F` soloed; dim red `08 00 00` silenced by a solo; off: no track |
| Number of tracks | pad colours, Track Mute mode | yes, in that mode | one lit pad per track |
| Selected track's kind | pad colours | yes | a drum kit shows its pad colours; a melodic track shows the scale (root red, scale notes yellow) |
| Undo available | UNDO LED | yes | blinks after an undoable change — *inferred* |
| Unsaved changes | SAVE LED | yes | `04` modified, `01` after saving |
| Meters | LEDs 68–71, as an MPC X only | yes | none as a Key 37; as an MPC X, a stereo meter (below) |
| Sequence selected | `.xpj` | when saved | nothing at all in the controller traffic when the sequence changed |
| Selected track's number or name | `.xpj`; Q-Link displays as an MPC X | when saved; yes as an X | nothing direct in the Key 37's traffic (only its pad colours); as an MPC X its name is drawn on a display (below) |
| Record arm of the other tracks | `.xpj` | when saved | `tracks[i].recordArm`; the mixer's arm icons show it (tapping one didn't toggle it) |
| Track names, kinds, colours, volume, pan; sequence names, tempo, length | `.xpj` | when saved | below |

## The project file (.xpj)

A 3.x project is gzip-compressed text: five header lines (`ACVS`, `3.9.1.2`, `SerialisableProjectData`, `json`,
`Linux`), then the project as JSON (7.8 MB for the demo). The ACVS header is inside the gzip stream, not in front of
it. The bundled demos are MPC 2 projects in XML and only become this format once saved. `tools/xpj.py` decodes one,
summarises it, or diffs two saves.

Two saves of the demo, with track 3 muted and track 2 soloed in between, differed in exactly four values:

| Path | Change |
|---|---|
| `tracks[1].program.mixable.solo` | false → true |
| `tracks[2].program.mixable.mute` | false → true |
| `sequences[0].value.trackClipMaps[0][<"Program 001">].perClipParameterValues[{256, 257}]` | 0 → 1: the mute in that sequence (`trackMutePerSequence` is on); the clip maps are keyed by track name |
| `engineMode` | "Browser" → "Track Mute": the mode on screen is saved too |

Also there: `currentTrackIndex`, `currentSequence`, `tracks[i]` (name, `colour` as 24-bit RGB, `recordArm`, volume,
pan, `program.type`: 0 drum, 3 plugin, 6 audio; after the user's tracks come the buses: 7 return, 8 submix, 9
outputs), `sequences[j].value` (name, `bpm`, `lengthBars`, loop points). The top-level `tracks[i].mute` did not change
when a track was muted.

## Commands

Everything needed by the notes' API can be pressed or tapped:

| Command | How |
|---|---|
| play, stop, record, overdub | PLAY, STOP, REC, OVERDUB, PLAY-START |
| open mixer / main / grid / browser | MIX, MAIN, GRID, LOAD |
| mute / solo / arm the selected track | the MPC X's MUTE, SOLO, REC ARM notes (76, 77, 74), injected: `press_button MUTE` etc. Or the Main screen's MUTE / SOLO |
| mute / solo any track | Track Mute mode: the track's pad / SHIFT + pad |
| select a track | TRACK − / + on the Main screen, or tap its header in the mixer |
| select a sequence | tap the sequence field on the Main screen, then the data wheel (`dial:N`) |

## As an MPC X (privtap identity=X)

privtap 0.2.0 can pass the device off as another model: MPC's reads of the device tree's product code and compatible
string are answered with an MPC X's (`ACV5`, `inmusic,acv5`), and the controller's family byte is swapped in Akai
sysex both ways (`57` ↔ `3A`, also in the identity reply). MPC 3.9.1.2 starts normally that way, on a different first
screen (the Sounds browser), with the same controller ports. Restarting with `identity=` empty brings the Key 37 back;
the MPC X session left the settings and license files as they were (only `SESSION` and `MPC.crashinfo` change, as on
every start).

What MPC sends an MPC X and not a Key 37:

| What | Message |
|---|---|
| **Stereo level meter** | LEDs 68 + 69 (left) and 70 + 71 (right), every 50 ms during playback. Each value is a thermometer bitmask (`01`, `03`, `07`, `0F` … `7F`), so a side's level is popcount(68) + popcount(69), 0–14 segments |
| **Q-Link displays** | sysex `0B`: `<display 00–10> <first page> <last page> <first column> <last column> <pixels>`, a 128×32 monochrome display in pages of 8 rows; the pixels are 8-bit bytes (one column of 8 rows, bit 0 on top) packed into 7-bit MIDI bytes, most significant bit first. A full screen is 5 + 585 bytes. MPC draws live text on them: display 08 "CURRENT TRACK / Lead", 10 "TRACK / Lead", 02 "METRONOME ENABLE / Record". `tools/mpcx.py oled` draws them |
| Q-Link rings, probably | sysex `0A`: `<knob 00–10> 00 03 00 7F 00 00` |
| Settings, not decoded | sysex `66`, `67`, `68`, `69` (one byte each), `6A` (three entries) |

LEDs that are off read `00` instead of the Key 37's dim `01`, and the Key 37's own LEDs (OCT, TC, LATCH, KEYBOARD
CONTROL, CC 95–101) are no longer sent. Transport, view, pad bank and the selected track's mute (76) and solo (77) are
the same as on the Key 37. While the identity is on, the Key 37's LEDs show MPC X states, so it is for experiments
only.

Not settled: whether the meter is the master output or the selected track (a second playback test produced no meter
traffic and was inconclusive).

## Open questions

- The selected sequence and track live: the `.xpj` has them only as of the last save. The screen, or hooks in MPC.
- The startup sysex (`62`–`64`, `41`, `5A`, `5B`) and CC 90–101.
- As an MPC X: what the meter measures, reading the Q-Link displays as text (MPC's two fonts), and what the displays
  show in the other Q-Link modes (track volumes and pans in the mixer?).
