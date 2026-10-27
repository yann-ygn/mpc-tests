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
  live metadata and verified attachment status. Authoritative attachments enable snapshots; the embedded host also
  provides authenticated flag commands under native live qualification. Watch remains unavailable. See
  `native-api/README.md` and `native-api/adapter/README.md` for tests, builds and lifecycle limits.
- `build/` is gitignored: local copies of the log, project saves and the settings backup.
- `builddir/{host,arm}/` is gitignored: canonical Meson build directories. See `README.md` for all targets.

## The device

- `ssh root@$MPC_HOST` (the default is in `tools/mpcx.py`). BusyBox userland: no `readelf` or `aconnect`, and `head`
  needs `-n N`. The root filesystem is read-only; addins live in `/data/mpc-addins/`.
- The MPC Remote addin (`/data/mpc-addins/remote`, 1.0.3) serves HTTP and MCP on port 6720 (`/tap`, `/button`,
  `/screen.png`). The `mpc` MCP server is the same addin. `privtap/profiles/` keeps a copy of its `buttons.conf`.
- MPC Native API 0.10.0 is installed and ready (owner-approved #39 upgrade, 2026-10-27).
  Earlier story entries below record historical installed versions; see `findings/native-mixer-installed-20261027.json` for the current verification.
- privtap's settings: `/data/mpc-addins/privtap/privtap.conf`. Keep `identity=` empty except during an experiment:
  as another model, the Key 37's LEDs show the wrong states.
- Settings backup taken before identity experiments: `/data/claude-backup-20261023-settings.tar`.
- Embedded scripting was previously tested and is disabled in production firmware. Compiled wrappers and
  `/data/scriptingDefaultEnabled` do not establish a working script API. Native control can reuse the underlying
  property/filter setters without running scripts; see `findings/native-control.md`.

## Rules

- This is someone's instrument. Installing privtap or restarting MPC loses unsaved work: ask first. New Project can also
  restart MPC on this firmware; ask before using it for cleanup. Save experiment projects only to `MPC Documents/Projects/Claude Experiments`; don't touch the user's other projects.
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
  `native-api/` under local-service story #32 (implemented: state, authenticated commands/receipts and native live policy; watch remains #33).
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
  state/settings were restored, with no restart/addin install. Other modes/families stay gated; #32 adds the production public binding.
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
  files were removed. Command binding and persistent supervision are implemented by #32/#21; watch remains #33.
- Flag handlers (#19) implemented: command ABI 1 on executor ABI 4 provides caller-scoped retained receipts,
  desired-state writes, dispatch-time expected values, native/audio verification, observed peer/selection changes,
  and draining shutdown. The handler owns work across waits/disconnects; retries return the retained outcome.
  Missing live host policy rejects writes; the lab policy is explicitly manual/experimental. US #32 now binds the public Unix setters/get_command and supplies production live mode/settings qualification.
  Twelve host entries, seven ASan/UBSan native entries and four isolated ARM fixtures pass. Twelve device rows
  cover all flags on selected/unselected plugin/drum tracks, no-ops/conflicts, retries and complete restoration.
  Both DSOs were unloaded; per-sequence mute was restored enabled and Main restored. See
  findings/native-commands.md and findings/native-commands-proof-20261025.json; C integration is documented in
  native-api/adapter/COMMANDS.md. tools/mpc_command_lab.py is the bounded device runner.
- Local command service (#32), branch `us-32-local-command-service`: strict Unix setters/get_command now bind peer UID to retained handlers.
  Native Main/no-popup/stopped/automation-Off qualification reads verified live owners/settings at dispatch; mute requires global scope.
  Queue/retention pressure, asynchronous waits/disconnects and draining shutdown are covered by 16 host Meson entries, eleven
  ASan/UBSan native entries and isolated ARM wire/handler fixtures. Twelve actual device write rows and sixty no-op policy checks pass;
  flags/settings restored. See `findings/native-service.md` and sanitized proof. During test-project cleanup MPC exited cleanly and
  auto-restarted after New Project; the old installed 0.1.0 read host recovered. Temporary mappings/files are gone; graceful lab dlclose
  was not established in that cleanup. The owner-approved 0.2.0 upgrade is installed and ready: all five reads,
  host stop/start and rejected-target receipt/replay checks pass. Configuration, identity and other addins were preserved.
  See `findings/native-service-installed-20261026.json`. The corrected production policy also passed installed Unix
  mute/solo/arm writes on a selected plugin in an owner-loaded project, independent readback, retries/no-ops/conflicts
  and fifteen policy no-op checks. All captured track/bus flags, selection, sequence state, settings and Mixer Effects
  view were restored without save/restart. See `findings/native-service-live-20261026.json`.
- Complete track inventory (#46), branch `us-46-complete-track-inventory`: identities and snapshots now use
  the real normal/user vector (`TrackPool +0x74`), including audio. The historical `normal*` profile offsets
  (`+0x44`) identify MIDI-triggered tracks. `uiIndex` is canonical Track List order; `index` stays collection-relative,
  with explicit `collection`. Master objects come from `+0x84`; audio from `+0x94`. Bounds are 128 MIDI + 8 audio
  and 128 masters. Snapshot ABI 2 / STATE_V2, executor ABI 5 and command ABI 2 replace older layouts; recompile C
  hosts. Package 0.3.0 includes the change; the installed 0.2.0 remains unchanged. Mixed-family host/ARM fixtures,
  full host checks, ASan/UBSan and live eight-track inventory/audio-selection verification pass. The original
  flags, sequence state, selection and Mixer Effects view were restored; lab mappings/files removed and installed
  host resumed without MPC restart. See `findings/native-track-inventory.md` and sanitized proof. Additional
  family flags/setters remain US #49.
- Screen qualification (#47), branch `us-47-mixer-track-mute`: production flag commands now qualify Main,
  all five Channel Mixer pages and Track Mute, with stopped/automation-Off/global-mute restrictions retained.
  A private context fence survives native filters/setters and delayed verification; transitions reject before
  invocation or retain uncertainty afterward. Discovery names supported screens; public C ABIs remain unchanged.
  All 17 host entries, 12 ASan/UBSan native entries and five isolated ARM fixtures pass. Device evidence covers
  168 writes, 12 Single/Multi peer cases, nine controller edits and nine lost-response retries. Original flags,
  native/saved command settings, inventory, selection and Mixer Effects view were restored; temporary drum track,
  lab mappings and files removed. MPC was not restarted or saved; installed 0.2.0 resumed ready. Package 0.4.0 is
  built but not installed. See `findings/native-command-screens.md` and its sanitized proof.
- Sequence-scoped mute (#48), branch `us-48-sequence-scoped-mute`: optional `muteScope` preserves global defaults;
  sequence scope requires a complete same-instance/project sequence reference and generation. Dispatch and every
  traversal require that generation to remain active, with a matching live per-sequence preference. Queued changes
  reject before invocation; post-setter loss remains unknown with actual execution metadata. Reads and receipt
  observations label mute scope; inactive editing returns `SEQUENCE_NOT_ACTIVE` without selecting anything.
  Executor ABI 6 / command ABI 3 / snapshot ABI 3 and STATE_V3 replace older layouts; primitive ABI 1 is unchanged.
  All 17 host entries, 11 ASan/UBSan entries and five isolated ARM fixtures pass. Twelve device rows cover
  plugin/drum and selected/unselected targets on Main/Mixer Effects/Track Mute across two different sequence
  patterns, with eight controller-edit/discarded-response rows. All five reads match independent state; p95 capture
  is 11.032 ms. Original flags, sequence, selection, inventory, automation/settings and Mixer Effects viewport were
  restored; the temporary drum, lab mappings and files were removed. Installed 0.2.0 resumed ready without restart
  or save/load. Package 0.5.0 is built, not installed. See `findings/native-sequence-mute.md` and its sanitized proof.
- Family qualification (#49), branch `us-49-track-family-flags`: a validated shared matrix now separates
  recognized family, individual readable flags, applicable operations and required completion evidence.
  Plugin/drum/keygroup/audio qualify requested flags, solo/arm engine copies and silenced state; MIDI/CV
  qualify native requested flags only, with explicit nulls and no physical-output claim. Legacy clip/unknown
  flags and bus writes stay unavailable. Dispatch and delayed verification fence Program address/vtable;
  family changes cannot inherit old eligibility. Recompile for primitive ABI 2, executor ABI 7, command ABI 4,
  snapshot ABI 4 / STATE_V4; receipts carry a checked ABI/size prefix. All 18 host entries, 13 ASan/UBSan entries,
  five isolated ARM fixtures and ARM deployment guards pass. Device evidence covers 96 new-family flag rows,
  48 cross-family Single/Multi peer rows, 12 controller edits, 12 discarded-response recoveries and eight
  two-sequence pattern rows, plus 12 final plugin/drum regression rows. All five reads match independent
  observations; final capture p95 is 11.911 ms. Four empty temporary tracks were removed; original flags,
  inventory, selection, sequence, automation/settings and Mixer Effects view restored, lab unloaded and
  installed 0.2.0 resumed ready without save/load or restart. Package 0.6.0 is built, not installed. See
  `findings/native-family-flags.md` and its sanitized proof. MIDI/CV have no connected external endpoint;
  physical output remains unqualified at the owner's request.
- Playback flags (#50), branch `us-50-playback-flags`: ordinary sequence playback now qualifies with
  recording/overdub Off and target automation Off. Recorder pending/live enum, timeline ownership,
  native selector and separate Song source fence every traversal and delayed verification. Command ABI 5
  adds retained queue/dispatch/verification/total durations; other ABIs remain at #49's versions. Each
  executor UI message handles one job. All 18 host entries, 13 ASan/UBSan entries, five isolated ARM fixtures
  and ARM guards pass. Device evidence covers 168 command rows, 36 Single/Multi peer rows, 18 controller
  edits/recoveries, sequence transitions, loops, expiry, shutdown and eight-client pressure. Total command
  p95 is 97 ms; native capture p95 is 14.451 ms. Whole-process CPU/RSS and ALSA state samples have explicit
  attribution/underrun limits. The original inventory, flags, automation, selection, sequence, playhead,
  settings and Mixer Effects viewport were restored; lab/probe removed and installed 0.2.0 resumed ready.
  The owner then authorized discarding unsaved changes, loading another project and installing 0.7.0.
  A second four-track DnB demo at 174 BPM / 32 bars passed 18 additional command rows, eight-client pressure
  and all five reads (capture p95 12.843 ms, command total p95 78 ms), with flags/automation restored and
  lab removed. Evidence is published in `findings/native-playback-flags.md` and its sanitized proof.
  Owner-approved 0.7.0 installation/restart completed from commit `6e6d425`; hashes, configuration, identity
  and preload list match, with privtap/Remote preserved. Installed five reads, host stop/start and actual
  playback mute/solo/arm writes/readbacks/retries pass. The original project was reopened from disk as
  authorized; its fresh baseline flags/automation were restored after testing, with original selection,
  sequence, zero playhead, command preferences and Mixer Effects view. Installed 0.7.0 is ready/OK.
  See `findings/native-playback-installed-20261027.json`. Unsaved changes were discarded with permission;
  no project was saved. Future installs/restarts still require the owner's approval.
- Lightweight observations (#52), branch `us-52-lightweight-observation`: ordinary commands and delayed
  verification use live user flags/selection/scope instead of full metadata captures. Executor ABI 8 / command
  ABI 6 / observation ABI 1 add explicit partial coverage, 16-track shared sampling and bounded coalescing;
  primitive ABI 2 and snapshot ABI 4 / STATE_V4 stay unchanged. Lifetime/context/Program checks, FIFO, native
  filters, peer effects and retained receipts remain intact. All 19 host entries, 14 ASan/UBSan entries, six
  isolated ARM fixtures and ARM guards pass. On the same eight-track project, matrix median UI work falls
  52.8 -> 12.7 ms; longest matrix callback 61.1 -> 33.8 ms. Saturated eight-client bursts still reach 91.4 ms
  (baseline 138.1 ms); the 25 ms command p95 target is missed, with native listener and saturation limits
  reported explicitly. Independent observation callback p95 is 2.427 ms. Full reads remain authoritative;
  watch transport and Automation Read remain #33/#51. Original flags, buses, selection, sequence, automation,
  live context, zero playhead and Mixer Effects are restored; the lab is unloaded and installed 0.7.0 resumed.
  Package 0.8.0 is built locally, not installed. See `findings/native-ui-cost.md` and its sanitized proof.
- Automation Read (#51), branch `us-51-automation-read`: Read now qualifies verified desired-state
  no-ops for all six supported families and both mute scopes, plus native record-arm writes.
  Read mute/solo writes remain `AUTOMATION_READ_NOOP_ONLY`; Write/unknown modes remain
  `AUTOMATION_UNQUALIFIED`. Expected values precede no-op detection; no-op arm skips the native
  filter too. Private invocation boundaries recheck before filter/setter; final policy readbacks must
  agree with the completion snapshot or continue bounded read-only verification. Existing ABIs/layouts
  remain unchanged. All 19 host entries, 14 ASan/UBSan entries, six isolated ARM fixtures and ARM guards
  pass, with additional Read cross-family Single/Multi peer cases. Live plugin/audio matrices cover
  selected/unselected, stopped/playing, three screens, both mute scopes and a populated per-sequence
  mute-source rejection/control case. Flags/buses, selection, sequence, automation, settings, zero
  playhead and Mixer Effects are restored; temporary lab unloaded and installed 0.7.0 resumed.
  Package 0.9.0 is prepared locally. See `findings/native-automation-read.md` and its sanitized proof.
- US #53 extends the historical #51/#39 restrictions with actual native Read mute/solo/volume writes.
  See findings/native-automation-read-writes.md for source ownership, native reversion and remaining limits.
  Installed availability requires a separately approved 0.11.0 upgrade/restart. Streaming stays #33; event editing stays #43.
- Packaging (#21), branch `us-21-native-package`: `native-api/package.py` builds a reproducible ARM addin with
  the shared preload-preserving installer. `mpc_native_host.so` embeds the adapter and service with zero exports
  and NODELETE; no ptrace/privtap worker dependency. `mpc-api host-status|host-stop|host-start` manages it through
  `/tmp/mpc-native-api/control.sock`. Stop joins API callers, detaches UI observers and drains callbacks; a stalled
  UI retains the executor and reports draining. Full code unload/upgrades require MPC restart. See
  `native-api/INSTALL.md` and `findings/native-package.md`. The installed 0.2.0 build adds #32's conditional command capabilities. With owner-approved
  restarts, actual startup, reads on two tracks/28 buses, listener drain, upgrade, uninstall/reinstall and
  configuration recovery pass. The API is installed at `/data/mpc-addins/mpc-native-api` and remains ready;
  privtap and Remote remain functional. Normal service termination can bypass ELF destructors: the uninstall
  wrapper uses `mpc-api host-cleanup` under the lifetime lock after process exit. Nine host lifecycle cases,
  ASan/UBSan, all 14 host entries and ARM deployment guards pass. See sanitized native-package proof.
  Future instrument installs/restarts still need the owner's approval under the device-work rule above.

- Mixer controls (#39), branch `us-39-native-mixer-controls`: normalized volume/pan/four sends and boolean rack/occupied-slot enables
  extend authenticated retained commands, full state and lightweight observations. Mixer generations fence Program/rack/ordered slots
  with native destruction observers. Plugin/drum/keygroup/audio qualify on Main, all five Mixer pages and Track Mute, stopped/playing,
  Automation Off; MIDI/CV mixer fields and bus writes remain unqualified. Completion is model-only. Rebuild C callers for executor 9,
  commands 7, observations 2, snapshot 5 / STATE_V5; primitive 2 is unchanged. All 21 host, 16 native ASan/UBSan, seven isolated ARM
  fixtures and ARM guards pass. Device evidence includes 1,232 screen/playback rows, endpoint/float/conflict/retry tests, 40 Read/Write
  rejections, controller edits, effect deletion/re-add and project reload fencing, and 320 numeric pressure submissions with a concurrent
  observer (188 applied, 132 queue-capacity rejections). Saturated callback maximum was 145.804 ms; automation-rate updates remain
  unqualified. See findings/native-mixer-controls.md and sanitized proof. The owner-approved session backup was reloaded and verified;
  all captured values/flags/settings, selection, sequence, zero playhead and Mixer Effects view restored. Lab drained/unloaded and
  installed 0.7.0 resumed without MPC restart. Package 0.10.0 is built locally, not installed. The backup and disposable playback copy
  remain under Claude Experiments; no existing experiment was overwritten. Future installs/restarts still require owner approval.

- Owner-approved 0.10.0 installation (#39 follow-up, 2026-10-27) completed from `b797853`. A fresh session copy was saved as
  `Claude Experiments/US39 Install Backup.xpj` before the required MPC restart, then reopened. Package/installed hashes match;
  configuration, persistent identity and preload list were preserved, with privtap/Remote loaded. All five installed mixer setters
  passed independent readback, retained replay/get_command, no-op and expected-conflict checks; Automation Read rejected mixer
  commands before invocation. Host stop/start drained and rotated the API instance. All five read projections/schema checks and
  independent flags/mixer observations pass. All captured user/bus values, automation, selection, active sequence, settings, zero
  playhead and Mixer Effects/no-popup context are restored. Backup bytes are unchanged; installed host is ready/OK. See
  findings/native-mixer-installed-20261027.json. Future installs/restarts still require owner approval.
