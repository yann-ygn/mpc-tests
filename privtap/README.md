# privtap

An `LD_PRELOAD` addin for MPC OS standalone devices, written for the MPC Key 37. It sits on the controller's own
rawmidi ports inside the MPC process and:

- **logs the controller protocol in both directions**: every byte MPC reads from the hardware (buttons, pads, keys,
  knobs) and every byte it writes back (LEDs and other feedback), with a timestamp and the port's name;
- **injects MIDI as if the hardware sent it**: bytes written to the FIFO `/dev/midi_inject` are merged into what MPC
  reads from the `... Private` port. The [MPC Remote addin](https://github.com/jacob-sabella/mpc-addin-remote) writes
  there when the device has no "Virtual RawMIDI" port, so this is what makes its `press_button` work on a stock device.

> **Status:** 0.2.0 running in MPC on a Key 37 (MPC 3.9.1.2) next to the MPC Remote addin 1.0.3: the traffic log works
> both ways, the remote addin's `press_button` works through `/dev/midi_inject`, and `identity=X` ran MPC as an MPC X
> (see `../findings/`). Offline: 44 checks, and 26 more with `identity=X`, on the host (also under ASan/UBSan) and built
> for ARM on the Key 37 against a fake alsa-lib.

## Why it is needed on a Key 37

- MPC opens the controller's raw MIDI device itself (`hw:0,0,1` "MPC Private") and holds its input exclusively
  (`/proc/asound/card0/midi0` shows MPC as the owner), so nothing outside MPC can read the buttons: the remote addin's
  `learn_button` fails at once.
- The remote addin presses buttons through a sequencer port named "Virtual RawMIDI" (`snd-virmidi`), `/dev/midi_inject`
  or `/data/midi.in`. The Key 37's kernel is built without `snd-virmidi` (`CONFIG_SND_VIRMIDI is not set`) and the
  other two don't exist, so `press_button` has nowhere to send.

## How it works

It interposes alsa-lib's `snd_rawmidi_open`, `_read`, `_write`, `_close` and `_nonblock`, and libc's `poll`.

- On open it asks alsa-lib for the port's name and remembers the handle. The input whose name contains `Private` is the
  one it injects into. The first time that input opens, it creates the FIFO and starts a thread that reads it.
- MPC's `MIDI` thread waits in `poll()` on that input and a wake fd of its own, then calls `snd_rawmidi_read` (seen on
  the device: `poll({39, POLLIN}, {42, POLLIN|POLLERR|POLLNVAL})`, fd 42 being `midiC0D0`). The `poll` hook adds a wake
  pipe to that wait and reports the input readable while injected messages are queued. A blocking read with no poll
  before it waits on both too.
- Hardware and injected bytes are parsed into whole messages and merged at message boundaries only, so an injected
  press can't land inside a hardware message or sysex. Running status is written out in full.
- Symbols are found through libc's own `dlsym`, fetched by version: the commander addin exports a `dlsym`, which breaks
  plain `dlsym(RTLD_NEXT)` for every shim preloaded with it (mpc-vst-plugins NOTES, 2026-10-13).
- With `identity=`, MPC's reads of the device tree's product code and compatible string (`open`, `open64`,
  `__open64_2`, `openat64`, `fopen64` and the plain forms; MPC and libaz0x-info both read them) are answered from
  memory, and the controller's family byte is swapped in Akai sysex both ways, including its identity reply. The log
  keeps each side's bytes as sent. After TheKikGen's tkgl_mpcmapper `--tkgl_iamX`, without its virtual ports.
- The library exports its hooks and nothing else (`src/exports.map`, checked by `build.sh`). Without that, Zig's
  runtime exports `memcpy`, `memset` and the math functions, which would replace libc's for all of MPC.

## Settings

`privtap.conf` next to `privtap.so`, re-read within a second of a change (`inject` only when MPC starts):

| Key | Default | |
|---|---|---|
| `log` | `1` in the shipped file, `0` without one | log the traffic (`0`: only opens, closes and errors) |
| `log_path` | `/tmp/privtap.log` | `/tmp` is in RAM on these devices |
| `log_max_kb` | `4096` | past this the log moves to `<log_path>.1` and a new one starts |
| `inject` | `/dev/midi_inject` | the FIFO; `none` turns injection off |
| `identity` | empty | pass the device off as another model (`X`, `LIVE`, `LIVE2`, `ONE`, `FORCE`; only `X` tried), read when MPC starts. For experiments: the hardware's LEDs then show that model's states |

## The log

What it looks like (the values are made up):

```
812.441 # privtap 0.1.0 in pid 365 (/usr/bin/MPC) at 2026-10-23 15:28:47, settings /data/mpc-addins/privtap/privtap.conf, traffic log on
812.452 # open hw:0,0,1 in: 'MPC Key 37' / 'MPC Private', fd 42, mode 0: the private input
812.460 # inject: reading /dev/midi_inject (created)
815.103 in  MPC Private: 90 52 7F
815.230 in  MPC Private: 90 52 00
815.231 out MPC Private: B0 52 01
```

Seconds are on the monotonic clock (the first line gives the wall clock). `in` is what MPC read, before injected bytes
are merged in. `out` is what MPC wrote. `inj` is what arrived on the FIFO. To annotate an experiment, append to the log:
`echo "# pressing PLAY" >> /tmp/privtap.log`.

## Injecting

```sh
printf '\x90\x52\x7f' > /dev/midi_inject; sleep 0.1; printf '\x90\x52\x00' > /dev/midi_inject   # press and release note 0x52
```

The remote addin's `press_button` writes here once it knows the button. The Key 37 has no built-in profile, and
`learn_button` can't hear the buttons (see above), so build `/data/mpc-addins/remote/buttons.conf` (`NAME=note
[channel]`) from the log: press each button once and read its note. `profiles/mpc-key-37-buttons.conf` is the one read
so far; the notes match the MPC Live's (MENU 123, REC 73, STOP 81, PLAY 82 ...).

## Build and test

```sh
./build.sh                         # build/arm/privtap.so (Zig 0.14.1 from PyPI through uvx; no other toolchain)
./build.sh test                    # host build with gcc, over tests/fake_asound.c, then the tests
./build.sh device-test <address>   # the ARM build and the same tests, run on a device over ssh in /tmp (MPC untouched)
./build.sh package                 # build/privtap-<version>/: the library, settings, manifest and the shared installer
```

`package` takes the shared addin installer (`install.sh`, `uninstall.sh`, `addin-lib.sh`) from an mpc-vst-plugins
checkout (`MPC_VST_PLUGINS`, default `../../torpedo/mpc/mpc-vst-plugins`): the same installer every addin uses, which
appends to MPC's `LD_PRELOAD` without dropping the other addins.

## Install and remove

Save the project first: installing restarts MPC.

```sh
./build.sh package
scp -r build/privtap-0.1.0 root@<device>:/tmp/
ssh root@<device> sh /tmp/privtap-0.1.0/install.sh -y
```

To remove it: `ssh root@<device> sh /data/mpc-addins/privtap/uninstall.sh`. When MPC exits normally privtap deletes the
FIFO. If MPC was killed instead, delete `/dev/midi_inject` by hand (a reboot also clears it): with no reader left, the
remote addin's next press would block on opening it.
