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
41, 44–48, 113 …), and some of those carry state (below). CC 90–101 take `00`/`7F`/`03` and don't look like LEDs; 90/91
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
| Mute and solo of every track (16 per bank) | pad colours, Track Mute mode | yes, in that mode | steady yellow `7F 7F 00` normal; blinking orange `7F 3B 00` muted; blinking cyan `00 7F 7F` soloed; dim red `08 00 00` silenced by a solo; off: no track |
| Number of tracks | pad colours, Track Mute mode | yes, in that mode | one lit pad per track |
| Multi Rec on | LED 74 (`0x4A`) | yes | `03` on |
| Selected track's kind | pad colours | yes | a drum kit shows its pad colours; a melodic track shows the scale (root red, scale notes yellow) |
| Undo available | UNDO LED | yes | blinks after an undoable change — *inferred* |
| Meters | — | — | nothing: no continuous traffic during playback (the Key 37 has no meters to drive) |
| Sequence selected | — | — | nothing at all when the sequence changed |
| Selected track's number or name | — | — | nothing direct (only its pad colours) |
| Record arm per track | — | — | not found: the mixer's arm icon didn't toggle by tapping |

## Commands

Everything needed by the notes' API can be pressed or tapped:

| Command | How |
|---|---|
| play, stop, record, overdub | PLAY, STOP, REC, OVERDUB, PLAY-START |
| open mixer / main / grid / browser | MIX, MAIN, GRID, LOAD |
| mute / solo a track | Track Mute mode: the track's pad / SHIFT + pad. Or the Main screen's MUTE / SOLO for the selected track |
| select a track | TRACK − / + on the Main screen, or tap its header in the mixer |
| select a sequence | tap the sequence field on the Main screen, then the data wheel (`dial:N`) |

## Open questions

- Record arm per track, and where MPC shows it.
- The selected sequence and track: from the screen, a 3.x `.xpj` (the demos are MPC 2 XML) or hooks in MPC.
- The startup sysex (`62`–`64`, `41`, `5A`, `5B`) and CC 90–101.
- Whether an MPC X identity (`iamX`) makes MPC send more, e.g. meters or per-track mute/solo/arm LEDs.
