# MPC Key 37 API

A live state and control API for an Akai MPC Key 37 (standalone, MPC 3.9.1.2). The plan is in
`mpc-standalone-reverse-engineering-notes.md`. What is known is in `findings/key37-controller-protocol.md`: read it
before running experiments.

## Layout

- `privtap/`: LD_PRELOAD addin that runs inside MPC (C). It logs the controller's rawmidi traffic both ways
  (`/tmp/privtap.log` on the device), injects MIDI written to `/dev/midi_inject`, and with `identity=` passes the
  device off as another model. See `privtap/README.md`; `profiles/` holds the button map.
- `tools/mpcx.py`: experiments. `exp LABEL ACTION...` marks the log, acts, and diffs the decoded LEDs and pads. Also
  `state`, `events`, `oled`. `tools/xpj.py` reads and diffs 3.x project files.
- `build/` is gitignored: local copies of the log, project saves and the settings backup.

## The device

- `ssh root@$MPC_HOST` (the default is in `tools/mpcx.py`). BusyBox userland: no `readelf` or `aconnect`, and `head`
  needs `-n N`. The root filesystem is read-only; addins live in `/data/mpc-addins/`.
- The MPC Remote addin (`/data/mpc-addins/remote`, 1.0.3) serves HTTP and MCP on port 6720 (`/tap`, `/button`,
  `/screen.png`). The `mpc` MCP server is the same addin. `privtap/profiles/` keeps a copy of its `buttons.conf`.
- privtap's settings: `/data/mpc-addins/privtap/privtap.conf`. Keep `identity=` empty except during an experiment:
  as another model, the Key 37's LEDs show the wrong states.
- Settings backup taken before identity experiments: `/data/claude-backup-20261023-settings.tar`.

## Rules

- This is someone's instrument. Installing privtap or restarting MPC loses unsaved work: ask first. Save experiment
  projects only to `MPC Documents/Projects/Claude Experiments`; don't touch the user's other projects.
- Never commit privtap logs: the hardware's identity reply contains the device's serial number.
- After an experiment, put back what it changed (mute, solo, arm, mode), pressing twice where a press toggles.

## Build and test (`privtap/`)

- `./build.sh`: the ARM library, via Zig from PyPI (`uvx`); there is no other cross toolchain here.
- `./build.sh test`: host tests, run twice (plain and `PRIVTAP_TEST_IDENTITY=X`).
  `./build.sh device-test $MPC_HOST`: the same tests on the device, in `/tmp`, without touching MPC.
- `./build.sh package`, then on the device `sh /tmp/privtap-<version>/install.sh -y` (the shared addin installer;
  it restarts MPC).
- `src/exports.map` must list every hook. `build.sh` fails if anything else is exported (Zig's runtime would
  otherwise replace libc's `memcpy` and the math functions in all of MPC).

## Tracking

- Git: github.com/yann-ygn/mpc-tests, `main` (default) and `privtap`.
- Taiga: project "MPC Key 37 API" (id 24). Move a story's status when its work starts or ends.
- Next step: the state service prototype (Taiga #18).
