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
- `native-api/`: discovery-only C service, JSON schemas and a guarded shared native adapter. `describe` reports
  live executable/device metadata and verified lab attachment status. Public state/write operations remain
  unavailable. See `native-api/README.md` and `native-api/adapter/README.md` for tests, builds and lifecycle limits.
- `build/` is gitignored: local copies of the log, project saves and the settings backup.
- `builddir/{host,arm}/` is gitignored: canonical Meson build directories. See `README.md` for all targets.

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

## Build and test

- Bootstrap: `uv sync --locked` (pinned Zig 0.14.1 and JSON Schema test dependencies).
- Host: `uv run --no-sync meson setup builddir/host`, then
  `uv run --no-sync meson test -C builddir/host --print-errorlogs` (builds before testing).
- ARM: `uv run --no-sync meson setup builddir/arm --cross-file build-support/cross/mpc-armhf.ini`, then
  `uv run --no-sync meson compile -C builddir/arm`. No deployment is performed.
- Firmware headers, ELF export checks, SONAME/RUNPATH checks and ARM/glibc compatibility checks are Meson
  dependencies. Python tests consume built binaries; they must not rebuild them.
- In `privtap/`, `./build.sh test` delegates the two host fixture cases to Meson.
  `./build.sh device-test $MPC_HOST` runs the ARM fixtures in `/tmp`, without touching MPC.
- In `privtap/`, `./build.sh package`, then on the device `sh /tmp/privtap-<version>/install.sh -y` (the shared addin installer;
  it restarts MPC).
- `privtap/src/exports.map` must list every hook. The build fails if anything else is exported (Zig's runtime would
  otherwise replace libc's `memcpy` and the math functions in all of MPC).

## Code style

- `.editorconfig` sets whitespace policy: two spaces generally, four for Python.
- `.clang-format` follows the Spice style: two spaces, attached braces and a
  100-column limit. Preserve include order, including generated/private headers.
- Python uses the pinned Ruff formatter, configured in `pyproject.toml`.
- Apply formatting with `uv run --no-sync meson compile -C builddir/host format`;
  check it with the `format-check` target or the normal test run's `style` suite.
  Keep mechanical formatting separate from naming, function extraction and behavior changes.

## Tracking

- Git: github.com/yann-ygn/mpc-tests, `main` (default) and `privtap`.
- Taiga: project "MPC Key 37 API" (id 24). Move a story's status when its work starts or ends.
- Native API roadmap: `findings/native-api-roadmap.md` (Taiga epic #4, capability expansion epic #27).
- Accepted public API contract: `findings/native-api-contract.md` (#28). Initial `describe` implementation is in
  `native-api/` under local-service story #32 (In progress; remaining operations unavailable).
- Firmware adapter (#29) implemented: eight C exports, profile schema v1, exact build/model/ABI plus seven code
  fingerprints, typed read/setter/filter paths and isolated host/ARM fixtures. Native target operations require a
  UI thread and an external lifetime validator; the lab hook is experimental and does not satisfy #20.
- Temporary service details: ignored `build/native-api/device-smoke.json`. Retained lab attachment/unload details:
  `build/native-api/adapter-device-retained.json`; stop via `tools/mpc_native_lab.py --detach REPORT`.
- Next: reusable UI-thread executor (#30), then active-project generations (#20), live state (#18) and flag
  qualification/handlers (#31/#19) before enabling further service capabilities.
