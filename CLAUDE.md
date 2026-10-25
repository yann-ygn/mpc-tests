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
- `native-api/`: C discovery/live-state service, JSON schemas and a guarded shared native adapter. `describe` reports
  live metadata and verified attachment status. Authoritative attachments enable snapshots; public writes/watch
  remain unavailable. See `native-api/README.md` and `native-api/adapter/README.md` for tests, builds and lifecycle limits.
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
  `native-api/` under local-service story #32 (In progress; reads now implemented by #18; writes/watch unavailable).
- Firmware adapter (#29) implemented: eight C exports, profile schema v1, exact build/model/ABI plus seven code
  fingerprints, typed read/setter/filter paths and isolated host/ARM fixtures. Native target operations require a
  UI thread and an external lifetime validator; the lab hook is experimental and does not satisfy #20.
- Temporary service details: ignored `build/native-api/device-smoke.json`. Retained lab attachment/unload details:
  `build/native-api/adapter-device-retained.json`; stop via `tools/mpc_native_lab.py --detach REPORT`.
- UI-thread executor (#30) implemented: nine additive exports, fixed slots, typed FIFO jobs, dispatch-time identity
  resolution/revalidation, cancellation vs unknown running outcomes, capture draining and guarded close. The lab
  now uses it; full host tests, ASan/UBSan, ARM fixtures and read-only device dispatch/unload pass. See
  `native-api/adapter/README.md` and `findings/native-executor-proof-20261025.json`. DSO unload still requires
  external UI quiescence after capture/worker drain; the lab loader checks both workers and UI poll.
- Flag semantics (#31) implemented: `tools/mpc_flag_semantics.py` and the lab's private `SET` diagnostic
  qualify stopped plugin/drum behavior across selected/unselected targets, no-ops/conflicts, sequence mute,
  Single/Multi peer/selection effects, controller edits and disconnects. See `findings/native-flag-semantics.md`
  and `findings/native-flag-semantics-proof-20261025.json` (42 passing device cases, nine contexts; nine host Meson
  entries and isolated ARM fixtures pass). A Multi-to-Single arm transition retained two arm flags until a native
  arm call enforced Single behavior; the runner rejects that inconsistent baseline before writes. Original observed
  state/settings were restored, with no restart/addin install. Public writes and other modes/families stay gated.
- Active-project identity (#20) implemented: executor ABI 2 owns the application-root resolver, native destruction
  observers and Project/Sequence lifecycle notifications. Same-file loads can retain the Project object; sequence
  Clear All can retain both object and used flag. Both retire old generations. Native flag requests revalidate at
  dispatch and every traversal; stale writes fail before setters. Track rename/reorder preserves IDs. Sequence
  destructive edits invalidate references conservatively. See `findings/native-identity.md` and its sanitized
  evidence. UI observer removal precedes executor drain and DSO unload; the native listener weak-reference
  protocol owns deferred registration cleanup.
- Live state (#18) implemented: executor ABI 3 adds bounded owned snapshot jobs; the Unix service exposes
  get_state/list_tracks/current_track/current_sequence/recovery_snapshot through an authoritative attachment.
  Snapshots fence identity/topology/selection, distinguish requested/audio flags, buses and sequence usage/BPM,
  and report capture/revision/freshness with explicit nulls for unsupported fields. Ten host entries, five
  ASan/UBSan native entries and isolated ARM fixtures pass. Device captures matched the independent reader;
  Sounds-mode controller mute edits were visible/restored; small-project native capture p95 was 9.058 ms.
  See findings/native-state.md and findings/native-state-proof-20261025.json. Both DSOs and temporary service
  files were removed. Public writes/watch stay gated; persistent attachment supervision remains #21/#32.
- Flag handlers (#19) implemented: command ABI 1 on executor ABI 4 provides caller-scoped retained receipts,
  desired-state writes, dispatch-time expected values, native/audio verification, observed peer/selection changes,
  and draining shutdown. The handler owns work across waits/disconnects; retries return the retained outcome.
  Missing live host policy rejects writes; the lab policy is explicitly manual/experimental. Public Unix setters
  and get_command remain gated pending #32's authenticated binding and production live mode/settings policy.
  Twelve host entries, seven ASan/UBSan native entries and four isolated ARM fixtures pass. Twelve device rows
  cover all flags on selected/unselected plugin/drum tracks, no-ops/conflicts, retries and complete restoration.
  Both DSOs were unloaded; per-sequence mute was restored enabled and Main restored. See
  findings/native-commands.md and findings/native-commands-proof-20261025.json; C integration is documented in
  native-api/adapter/COMMANDS.md. tools/mpc_command_lab.py is the bounded device runner.
- Next: local command service binding and validated live qualification policy (#32), then streaming (#33).
  Per-sequence mute needs an explicit sequence target/precondition before public exposure.
- Packaging (#21), branch `us-21-native-package`: `native-api/package.py` builds a reproducible ARM addin with
  the shared preload-preserving installer. `mpc_native_host.so` embeds the adapter and service with zero exports
  and NODELETE; no ptrace/privtap worker dependency. `mpc-api host-status|host-stop|host-start` manages it through
  `/tmp/mpc-native-api/control.sock`. Stop joins API callers, detaches UI observers and drains callbacks; a stalled
  UI retains the executor and reports draining. Full code unload/upgrades require MPC restart. See
  `native-api/INSTALL.md` and `findings/native-package.md`. Public writes remain #32. Installed lifecycle tests
  still need the owner's saved-work/restart approval; do not infer it from read-only/isolated fixture tests.
