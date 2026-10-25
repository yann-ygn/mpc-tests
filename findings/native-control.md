# Native control behind the disabled scripting frontend

2026-10-24, Key 37, MPC 3.9.1.2. **Native mute, solo and record-arm writes now work in a bounded temporary bridge.**
A callback ran on MPC's main UI thread, then the actual notifying property setters changed live state and restored
the tested flags. The independent process-memory reader confirmed each change. The bridge was subsequently stopped
and unloaded, and the initial observed state and MIDI Learn view were restored. No application restart, persistent
addin installation or scripting execution was required. The named script wrapper entry points remain analysis
landmarks: the runtime adapter used their underlying property/filter paths.

## What survives in the binary

`ScriptableTrackPool` registers named methods with `Scripting::ThreadBasedRegistrar<JSRegistrar>`. The registration
function is at RVA `0x02872718`. Its registration records associate method names with C++ member-function pointers:

| Method | Wrapper RVA | Registration site | Native operation after resolving the named Track |
|---|---|---|---|
| `SetMute(trackName, bool)` | `0x0286DB64` | `0x02874C80` | notifying locked bool setter at `0x00E298BC`, receiver `Program + 0x50` |
| `SetSolo(trackName, bool)` | `0x02870A84` | `0x02874D8C` | notifying bool setter at `0x00E4AEE8`, receiver `Program + 0x88` |
| `SetTrackRecordArmed(trackName, bool)` | `0x02870824` | `0x0287487C` | apply the Track's arm filter, then `0x00E4AEE8`, receiver `Track + 0x474` |

These are RVAs for GNU build ID `8371199f945ecc40f79cb19c17ff5de72810a91c`, not reusable absolute addresses. The
observed image load bias is `0x00400000`; heap addresses changed after the user's MPC session restarted. A bridge
must discover the process and objects each time and reject other executable builds.

The wrappers share this shape: obtain TrackPool from `this + 0x60`, convert the supplied C++ string for lookup,
resolve the Track through `0x0257EEC4`, obtain its Program through `Track + 0x648`, then use the property path above.
Unknown track names take a C++ exception path. No scripting-enable condition was found on the successful paths of
these three wrappers. This is direct disassembly evidence, separate from whether the scripting frontend calls them.

The wrapper ABI is approximately `void (ScriptableTrackPool*, std::string const&, bool)`, with ARM hard-float C++
calling conventions and the firmware's GNU string layout. This is not a C interface. The wrappers are not exported
dynamic symbols, and a standalone SSH process cannot call into MPC merely by casting their addresses. A disabled
frontend also does not guarantee that a live ScriptableTrackPool instance exists. A bridge should either establish
that instance and ABI, or adapt the validated model objects to the underlying property calls.

## Why property calls matter

The notifying bool setter compares the requested value to the current value, changes it only when different, and
walks the property's listener list. The mute specialization additionally takes a recursive mutex. Simply changing
the boolean bytes found by the observer skips those listeners.

Record arm has an extra step: the wrapper checks the filter's manager at `Track + 0x48C`, invokes its function at
`Track + 0x490` with a bool reference and filter storage at `Track + 0x484`, then supplies the filtered result to the
notifying setter. The current filter's callable resolves to `0x0257BA7C`, whose captured member call must be preserved;
the wrapper does not just store the incoming bool into the observed arm byte.

Read-only traversal of both normal tracks' actual listener lists found:

- Mute: `Muteable`, `PerSequenceParameterValueUpdater`, audio-thread forwarding connectors and meter listeners.
- Solo: `AudioThreadValue<bool>`, a Program old/new listener and `TrackStateFollowOrMultiBehaviour`. The associated
  audio-side property's listeners include `Muteable` and `MuteGroup`.
- Record arm: `RecordArmable`, `ClipPlayer`, `MpcClipMixer`, `TrackStateFollowOrMultiBehaviour`, meters and filtered
  property connectors, including ones with a UI-thread-only locking policy.

These are live subscriptions, not merely class-name strings. The native-call tests below exercised their basic
behaviour. Per-sequence mute across sequence changes, automation, queued changes and other program families still
need a broader test matrix.

## Requested values and audio-side copies

The scripting getters read different locations from two fields validated in the earlier observer:

| Value | Script getter RVA | Getter field | Earlier observer field |
|---|---|---|---|
| Mute | `0x028688C0` | `Program + 0x78` | same |
| Solo | `0x02867614` | `Program + 0x97` | `Program + 0xC0` |
| Record arm | `0x0286780C` | `Track + 0x483` | `Track + 0x494` |

`Program + 0x88` is the notifying property whose bool is at `+0x97`; its `AudioThreadValue<bool>` listener queues an
update through the app's audio queue. RecordArmable's listener likewise queues a change after the property at
`Track + 0x474` is set. This supports treating the earlier observer locations as audio-side copies rather than the
initial requested values. In this idle session the corresponding values agreed on both tracks. Transient divergence
and exact acknowledgement timing have not been measured. `Track + 0x483` is used by the script getter; it must not be
dismissed as an unrelated mutex byte.

The read-only snapshot format has not been silently changed. A future control response should report request
acceptance separately from propagation to the observed audio state. A timeout after dispatch is not proof that a
command failed to apply. Explicit `set(true/false)` commands also make retries safer than blind toggles.

## Verified dispatch and temporary loading

The script registrations include a thread-based dispatcher; jumping to the final setter would bypass that part.
There are UI-thread-only listeners, so invoking the setters from a socket worker or an arbitrary suspended thread
has not been established as safe.

JUCE's `MessageManager::callAsync` was independently identified at RVA `0x00A12A3C`. Its native callback object is
identified by the RTTI name `*ZN4juce14MessageManager9callAsyncESt8functionIFvvEEE16AsyncCallInvoker`; its callback
function at `0x00A0723C` invokes the captured `std::function<void()>`. The enqueue path at `0x00A0F2D8` posts the object
to JUCE's message queue. The firmware's 32-bit function object is 16 bytes, with manager/invoker at offsets 8/12.
The bridge uses a small C adapter for this exact function-object layout, with capture lifetime management. A real
callback returned thread ID 2723, matching MPC's main/UI thread and process ID. This validates that bounded callback
path on this build; it does not establish compatibility with arbitrary C++ captures or another firmware.

The tested bridge flow is:

```
authenticated command -> narrow in-process adapter
    -> dispatch with the verified app-thread mechanism
    -> resolve the current Project and Track while on that thread
    -> check identity, ownership, type and object lifetime
    -> apply the native property/filter path
    -> observe requested and audio-side values, return acknowledgement
```

The temporary loader used `PTRACE_SEIZE`/`PTRACE_INTERRUPT` on the existing idle `privtap inject` worker, TID 2768.
It saved general and VFP registers, borrowed unused stack space, called libc `dlopen`, then restored the worker. It
did not attach the UI or audio threads. A disposable ARM child first demonstrated library constructor/destructor
execution, load/unload and correct resumption of its original blocked read. The worker inside MPC also resumed its
original read, with the same arguments and PC observed before and after.

The loaded library checks the executable build ID and six code fingerprints before starting its root-only local
Unix socket. It exposes `PING`, bounded `PULSE` operations and `QUIT`; there is no arbitrary native-call or raw-store
command. Calls enter JUCE's message queue before resolving/validating the supplied Project, Track and Program.

After the experiments, `QUIT` checked that callback captures had drained. The loader refused unload until the lab
worker had exited and the UI had returned to its normal `poll()` loop. `dlclose` then removed the library mapping.
The socket and temporary device files were removed. PID 2723 remained unchanged throughout.

The stock web object service is still a separate transport. Publishing a writable-looking property there would
only change its cache unless a bridge routes the command to these native setters.

## More controls

The subsequent [reusable adapter](../native-api/adapter/README.md) extracts these property/filter paths into
`libmpc_native.so` with eight C exports and versioned private profiles. It adds the record-arm invoker as a seventh
code fingerprint. Device tests verified typed UI-thread reads, independent readback, load/unload and discovery of a
retained attachment. The public service still exposes discovery only; reusable execution and authoritative
project/object lifetimes remain separate work. The six-fingerprint description above records the earlier proof.

The broader static [candidate catalog](native-control-catalog.md) groups the other compiled bindings and separates
named methods/properties from parameter identifiers. It was collected without additional device tests.

The same registration function also binds the following native implementations. Their names and member pointers
were recovered; units, ranges, side effects and safe invocation remain untested:

| Method | Wrapper RVA |
|---|---|
| `SetCurrentTrack` | `0x0286FF58` |
| `SetTrackVolume` | `0x0286A1B4` |
| `SetTrackPan` | `0x0286CFFC` |
| `SetSendLevel` | `0x02868CC8` |
| `SetTrackColour` | `0x0287061C` |
| `SetTrackMonitorState` | `0x0286F294` |
| `SetTrackTransposition` | `0x0286FD4C` |

There are additional routing and MIDI-parameter bindings. Their presence does not justify exposing an arbitrary
native-call endpoint; expand a command allowlist only after validating each command's model path and semantics.

## Reproduce the read-only preflight

[`tools/profiles/key37-mpc3.9.1.2-control.json`](../tools/profiles/key37-mpc3.9.1.2-control.json) records the three
bindings, additional candidates and 32-byte fingerprints for the primary wrappers, setters and asynchronous
dispatcher. [`tools/mpc_native_control_audit.py`](../tools/mpc_native_control_audit.py) uses the existing read-only
helper to verify those fingerprints against the live executable, discover the current Project, compare script-facing
and observer values, inspect live listeners, and confirm stable before/after snapshots. This audit remains read-only;
it is separate from the executable lab bridge.

```sh
python3 tools/mpc_native_control_audit.py --output build/internals/native-control-audit.json
```

The helper must already be built as described in [live-state.md](live-state.md). The audit uses only Python's standard
library, uploads a temporary read-only helper, and removes it afterward. It has no setter invocation or write mode.
Listener traversal is not atomic; changing objects may cause it to fail, so run the audit while the instrument is
idle. Matching fingerprints and live listeners are preparation for a write test, not proof of a working writer.

The retained audit at `build/internals/native-control-audit.json` passed against MPC PID 2723: all six fingerprints
matched, both tracks' three script-facing/observer boolean pairs agreed, and the state remained stable across the
audit. Deliberately corrupting one code fingerprint was rejected before object discovery; the temporary helper was
still removed. No controls or UI mode were changed by these checks.


## Native write results

The sanitized record is [native-control-proof-20261024.json](native-control-proof-20261024.json). Full before/during/
after snapshots and native acknowledgements are retained in gitignored `build/native-control/*-proof.json`.

| Test | Independent live readback | Restoration / side effects |
|---|---|---|
| Mute the unselected drum track | drum mute and silenced became true; plugin stayed unmuted/unsilenced | mute restored false; selection stayed on Plugin 001 |
| Solo the unselected drum track | drum solo became true; plugin stayed explicitly unmuted but became silenced | solo restored false; native follow-solo selected the drum track |
| Disarm the now-selected drum track | script-facing arm and observed audio arm became false | both restored true |

The solo selection change is material API behaviour: native calls trigger the normal listeners, including
`TrackStateFollowOrMultiBehaviour`. Restoring a solo flag does not automatically restore the previous selection.
The experiment's original selection was restored through Main's track decrement, then the UI returned to MIDI Learn.
All normal-track/bus flags, selected track, selected sequence and sequence-slot count matched the initial snapshot.
This verifies state propagation; no audio playback or recording test was performed.

Guard checks rejected a wrong expected value, an invalid Project pointer and an unknown property. An already-matching
mute request preserved state. The experimental `PULSE` command restores its original requested flag after a bounded
hold, including if its client disconnects; its expected-value check avoids blindly overwriting a different later
requested value. It does not roll back follow-mode selection changes.

## Build and repeat a selected-track lab test

The retained sources are:

- [`tools/mpc_hotload.c`](../tools/mpc_hotload.c): temporary ARM loader, disposable-child self-test and unload guard.
- [`tools/mpc_native_bridge.c`](../tools/mpc_native_bridge.c): firmware checks, UI dispatch and bounded native pulses.
- [`tools/mpc_native_client.c`](../tools/mpc_native_client.c): local Unix-socket command relay.
- [`tools/meson.build`](../tools/meson.build): builds the tools with generated firmware headers and
  rejects any exported bridge symbols that could interpose on MPC or other addins.
- [`tools/mpc_native_lab.py`](../tools/mpc_native_lab.py): host orchestration with independent readback and cleanup.

```sh
uv sync --locked
uv run --no-sync meson setup builddir/arm --cross-file build-support/cross/mpc-armhf.ini
uv run --no-sync meson compile -C builddir/arm
# Callback only, with automatic loading and unloading:
python3 tools/mpc_native_lab.py --output build/native-control/ping-proof.json
# Briefly invert the selected plugin/drum track's mute, then restore it:
python3 tools/mpc_native_lab.py --pulse mute --output build/native-control/mute-lab-proof.json
```

`--pulse solo` and `--pulse arm` use the same selected-track test. The lab uses an existing privtap worker, a private
`/tmp` directory and the existing read-only helper. It never installs an addin or restarts MPC. The default six-second
hold leaves time for the independent SSH reader. Every run checks the build and code bytes and first self-tests the
loader in a disposable process. The lower-level bridge can target a validated unselected track, as in the initial
mute/solo tests, but the retained host lab deliberately targets the track already selected to limit follow-mode
selection effects.

## Remaining limits

This is an experimental control proof, not a persistent HTTP writer. Project replacement, freed/stale roots,
concurrent user edits, restoring follow-mode effects, multiple simultaneous clients, crash recovery, larger/mixed
projects and other firmware remain outside its validation. In particular, vtables and pool membership alone cannot
prove a retained Project is still the active Project after a load. The bridge accepts only plugin/drum normal tracks
in its bounded snapshots. Volume, pan, routing and transport bindings have not been invoked. The production service
should resolve an authoritative current root on dispatch, define completion/timeout semantics, and expose a narrow
command allowlist.
