# Firmware adapter, C ABI 1

Taiga #29. `libmpc_native.so` contains firmware-specific state and property primitives extracted from the temporary
control lab. It has no constructor, worker, socket, script frontend or public command endpoint. Addresses in the
trusted C interface never become SDK handles.

## Guards and interface

The runtime backend checks 32-bit ARM hard-float/little-endian ABI, the device-tree model and the executable's
GNU build ID. It verifies seven 32-byte code prefixes before opening an adapter and again before target access
or native calls. A code/read failure latches it unavailable until closed and reopened. The seventh prefix covers
the actual record-arm filter invoker, in addition to the original six lab landmarks.

Private profiles in `tools/profiles/` use schema version 1. The generator rejects mismatched build/version/ABI,
unqualified families, invalid offsets, duplicate functions and bindings without fingerprints. They describe the
Key 37 / 3.9.1.2 research profile, including layouts and setter/filter paths. This is bounded lab qualification;
matching the profile is not release qualification for every MPC mode.

The [header](include/mpc_native.h) and [export allowlist](exports.map) define eight functions:

- `mpc_native_open`, `mpc_native_close`, `mpc_native_get_status`
- `mpc_native_read_track`
- `mpc_native_set_mute`, `mpc_native_set_solo`, `mpc_native_set_record_armed`
- `mpc_native_error_name`

ABI 1 layouts/signatures are fixed; an incompatible future ABI needs separate entry points or a new library
SONAME. The build rejects other exported symbols, including the private fixture backend. The deployment SONAME
is `libmpc_native.so`; the lab host resolves it beside itself using `$ORIGIN`, never a workstation path.

Opening without a target validator permits status checks only. Every target operation must run on the MPC
main/UI thread and call a trusted `validate_target` hook immediately before traversal. The production hook must
check the active project and object generations from #20. The adapter then checks vtables, normal-track ownership,
vector bounds/uniqueness, Program association and family. It does not discover an authoritative active root or
create public handles. A missing hook or stale target fails before any property call.
`mpc_native_target` is a resolved view for one call: queue public handle IDs, resolve them at dispatch, and keep
the command's expected generations in the executor context consulted by the validator. Never queue raw targets.

The retained lab provides an ownership-only hook, explicitly weaker than the required production hook. It is
not used to enable public commands. Executor ownership, queue bounds, cancellation and draining remain #30.
Callers must drain every user before closing an adapter.

## Primitive results

Reads distinguish requested flags, separate audio-side solo/arm copies and effective silencing. There is no
separate qualified audio-side mute copy. Plugin and drum are the only accepted target families.

Setters compare the optional expected value, skip already-desired values and invoke MPC's notifying property
setters. Record arm first calls the validated native filter; its manager must exist and its invoker must match
the profile. The adapter never stores directly into MPC's flag bytes; its memory descriptor is read-only.

An internal `OK` means model readback matched the desired value. It is not a public `applied` receipt: audio-side
propagation can still be pending. Results include observations and whether a native call began; post-call
failures preserve uncertainty. The command layer (#19/#32) must implement completion, timeout and retry rules.
There is no rollback, auto-restoration or undo guarantee.

## Build and verification

```sh
uv sync --locked
uv run --no-sync meson setup builddir/host
uv run --no-sync meson test -C builddir/host --suite native-api --print-errorlogs
uv run --no-sync meson setup builddir/arm --cross-file build-support/cross/mpc-armhf.ini
uv run --no-sync meson compile -C builddir/arm
```

Builds produce `builddir/{host,arm}/native-api/libmpc_native.so`, the service and `tests/native_fixture`.
See the [root build guide](../../README.md) for requirements and all targets.
The isolated fixture uses synthetic addresses and private I/O callbacks. No MPC entry point is called by those
tests. It exercises 28 cases, including each fingerprint mismatch, wrong thread, missing validator, stale
ownership, unknown family, expected-value rejection, filtered arm rejection and pending audio propagation.
Python tests also reject 14 malformed profile variants and check exports and the service attachment boundary.

Verified on 2026-10-24: 22 host tests with JSON Schema validation; the 28-case isolated fixture also passed on
ARM. Rebuilding the adapter with the same compiler, profiles and inputs produced identical bytes. Build paths
are normalized in debug information. These checks do not establish behavior across untested firmware, models,
modes or concurrent project replacement.

## Temporary device host

The existing lab now consumes the shared library. It still owns its experimental JUCE callback dispatcher and
bounded loader; it no longer implements property calls independently.

```sh
uv run --no-sync meson compile -C builddir/arm
python3 tools/mpc_native_lab.py --output build/native-api/adapter-device-unload.json
```

The default test checks the loader on a disposable child, loads the adapter, verifies status, reads flags on
the UI thread, compares them to the independent reader, then drains/stops/unloads both libraries. It performs
no flag write, verifies that observed flags/selection remain unchanged and removes temporary files.

To retain the tested host for discovery, and later remove exactly that attachment:

```sh
python3 tools/mpc_native_lab.py --keep --output build/native-api/adapter-device-retained.json
python3 tools/mpc_native_lab.py --detach build/native-api/adapter-device-retained.json \
  --output build/native-api/adapter-device-detached.json
```

`--keep` cannot be combined with `--pulse`. Detach verifies the recorded adapter instance, drains callbacks and
uses the loader's worker/UI quiescence checks before unloading. It refuses a different attachment. Diagnostic
reports contain private lab details and stay under ignored `build/`. No addin installation or MPC restart occurs.

The service's private status probe requires the peer PID/UID to match the current MPC, the expected ABI tag,
profile/build, a valid adapter incarnation ID and successful code verification. It has a 50 ms total deadline.
Attachment identity/loss changes invalidate the service instance. Public `describe` reports `readiness.adapter`
with `qualification: bounded_lab_proof`; all state/write/watch capabilities remain unavailable. This handshake
is diagnostic, not the future command transport. Device tests verified loading, typed readback, full unload,
and a retained attachment visible through `describe`, with MPC PID and observed state unchanged.
