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
- `tools/mpc_live_state.py` + `tools/mpc_mem_probe.c`: experimental read-only live object snapshots, pinned by GNU
  build ID in `tools/profiles/`. See `findings/live-state.md` for validated fields and limitations.
- `tools/mpc_native_control_audit.py`: read-only verification of internal control code and live property listeners.
  `findings/native-control.md` maps the setters. `tools/mpc_native_lab.py` now verifies temporary UI-thread dispatch
  and bounded native mute/solo/arm pulses; it loads/unloads a lab bridge without restart. Native solo can select its
  target through follow-solo behaviour; restore selection after unselected-track experiments.
- `native-api/`: standalone discovery-only C service and JSON schemas for the accepted 0.1 contract. It serves
  `describe` on a private Unix socket, reads live executable/device metadata, and exposes no native state or writes
  yet. See `native-api/README.md` for host tests, ARM builds and temporary device smoke tests.
- `build/` is gitignored: local copies of the log, project saves and the settings backup.

## The device

- `ssh root@$MPC_HOST` (the default is in `tools/mpcx.py`). BusyBox userland: no `readelf` or `aconnect`, and `head`
  needs `-n N`. The root filesystem is read-only; addins live in `/data/mpc-addins/`.
- The MPC Remote addin (`/data/mpc-addins/remote`, 1.0.3) serves HTTP and MCP on port 6720 (`/tap`, `/button`,
  `/screen.png`). The `mpc` MCP server is the same addin. `privtap/profiles/` keeps a copy of its `buttons.conf`.
- privtap's settings: `/data/mpc-addins/privtap/privtap.conf`. Keep `identity=` empty except during an experiment:
  as another model, the Key 37's LEDs show the wrong states.
- Settings backup taken before identity experiments: `/data/claude-backup-20261023-settings.tar`.
- Embedded scripting was previously tested and is disabled in production firmware. Compiled wrappers and
  `/data/scriptingDefaultEnabled` do not establish a working script API. Native control can reuse the underlying
  property/filter setters without running scripts; see `findings/native-control.md`.

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
- Native API roadmap: `findings/native-api-roadmap.md` (Taiga epic #4, capability expansion epic #27).
- Accepted public API contract: `findings/native-api-contract.md` (#28). Initial `describe` implementation is in
  `native-api/` under local-service story #32 (In progress; remaining operations unavailable).
- Next: firmware adapter (#29) and UI-thread executor (#30), then lifetime/state/flag handlers before enabling
  further service capabilities.
