# MPC Key 37 API

Native state/control research and a local service for the Akai MPC Key 37.
The API implements discovery, authoritative live state reads for every user track (including audio) in Track List order,
and guarded Unix mute/solo/arm commands across plugin, drum, keygroup, MIDI, CV and audio tracks,
including explicit active-sequence mute targets.
The [mixer extension](findings/native-mixer-controls.md) adds normalized volume/pan,
four send levels and rack/slot enable for qualified plugin/drum/keygroup/audio
tracks under Automation Off, with generation-bound targets and model-only completion. [Family qualification](findings/native-family-flags.md)
separates native requested flags from engine copies; MIDI/CV output remains unmeasured.
The [managed flag handlers](native-api/adapter/COMMANDS.md) retain verified command results across retries and disconnects;
a native live policy qualifies Main/Mixer/Track Mute, stopped or during normal sequence playback,
with recording/overdub Off. Automation Read supports verified no-ops for every qualified flag
and native record-arm writes; Read mute/solo writes remain unavailable.
See [Automation Read qualification](findings/native-automation-read.md). [Playback qualification](findings/native-playback-flags.md)
reports timing and tested limits. Command observations use a [lightweight flag sampler](findings/native-ui-cost.md);
authoritative full reads retain their coverage. Streaming follows separately. See [the service](native-api/README.md),
[the native adapter](native-api/adapter/README.md)
and [privtap](privtap/README.md) for their interfaces and limitations.
US #21 adds the [persistent addin package](native-api/INSTALL.md), including startup,
health, stop/start and recovery. Build it with `uv run --no-sync python native-api/package.py`;
installation requires saved work and approval for an MPC restart.

## Requirements

Build on Linux with Meson 1.4 or newer, Ninja, a host C compiler, binutils
(`readelf`), Python 3.11 or newer, and uv. Formatting also uses clang-format 19.
The development environment pins Zig 0.14.1, Ruff and JSON Schema validation
dependencies in `uv.lock`.

From the repository root, bootstrap once:

```sh
uv sync --locked
```

## Host build and tests

```sh
uv run --no-sync meson setup builddir/host
uv run --no-sync meson compile -C builddir/host
uv run --no-sync meson test -C builddir/host --print-errorlogs
```

The default build type is `debugoptimized` (`-O2 -g`), with `-Wall -Wextra -Werror`.
Host privtap and test fixtures retain their original `-O1` setting.
The native API uses C11 and the interposition/research tools use GNU C11.
Meson generates `compile_commands.json` in each build directory for editor support.
Use `--buildtype=debug` at setup for unoptimized service and research-tool builds;
the explicit privtap and fixture optimization settings are retained.

The tests include Python API/adapter/experiment cases with JSON Schema validation,
the isolated native, executor and identity lifecycle/concurrency fixtures, privtap protocol tests, both preload cases
(normal and identity X), and eight tests exercising the ELF deployment guards.
Tests consume Meson's binaries and do not invoke compilers.
The [flag-semantics matrix](findings/native-flag-semantics.md) documents device
experiments and the private runner for US #31. The [live-state findings](findings/native-state.md)
cover US #18 and its device evidence. To run one suite:

```sh
uv run --no-sync meson test -C builddir/host --suite native-api --print-errorlogs
uv run --no-sync meson test -C builddir/host --suite privtap --print-errorlogs
```

## ARM build

```sh
uv run --no-sync meson setup builddir/arm --cross-file build-support/cross/mpc-armhf.ini
uv run --no-sync meson compile -C builddir/arm
```

The cross file uses the repository's `.venv` interpreter to invoke the pinned Zig
compiler, targeting little-endian ARM hard-float, Cortex-A7 and glibc 2.31.
An additional ARM compiler or device sysroot is unnecessary for the current targets.
Builds perform no deployment. ARM fixture executables are built but cannot run on
the host; `meson test -C builddir/arm` reports them as skipped without an executable wrapper.
The service's Python integration tests run in the host build only.

| Artifact | Path relative to `builddir/host/` or `builddir/arm/` |
|---|---|
| Discovery/state service | `native-api/mpc-api` |
| Native adapter | `native-api/libmpc_native.so` |
| Persistent host (adapter and service, no exports) | `native-api/mpc_native_host.so` |
| Native fixture | `native-api/tests/native_fixture` |
| Executor fixture | `native-api/tests/executor_fixture` |
| Mixed inventory fixture | `native-api/tests/inventory_fixture` |
| Incremental observation fixture | `native-api/tests/observation_fixture` |
| Identity/lifecycle fixture | `native-api/tests/identity_fixture` |
| Command/receipt fixture | `native-api/tests/commands_fixture` |
| Persistent host lifecycle fixture | `native-api/tests/host_fixture` |
| Interposition addin | `privtap/privtap.so` |
| privtap fixtures | `privtap/tests/{test_protocol,test_privtap,libfakeasound.so}` |
| Memory/web helpers | `tools/{mem_probe,ipc_bridge}` |
| ARM-only lab bundle | `tools/{hotload,native_client,native_bridge.so,libmpc_native.so,test_payload.so}` |

Firmware headers are generated from the validated state/control profile pair.
Changes to either profile, the generator or its validator rebuild their consumers.
Required build checks verify shared-library export allowlists, the adapter SONAME,
the lab bridge's `$ORIGIN` RUNPATH, portable dependencies, ARM ABI and the glibc
version ceiling. Export-map changes also trigger relinking and verification.

The previous `native-api/build.py` and `tools/build_native_control.py` commands are
replaced by this workflow. `privtap/build.sh` retains its convenience, package and
device-fixture commands, delegating compilation to Meson. Packaging still uses the
shared installer from an `mpc-vst-plugins` checkout; set `MPC_VST_PLUGINS` when it is
not at `../torpedo/mpc/mpc-vst-plugins` relative to this repository.

## Formatting

C follows the Spice style: two spaces, attached braces and a 100-column limit.
`.clang-format` defines the policy and preserves include order. Python uses Ruff
with four spaces and the same line-length limit. `.editorconfig` supplies editor
defaults for whitespace and line endings.

After `uv sync --locked`, format or check all project C/Python sources:

```sh
uv run --no-sync meson compile -C builddir/host format
uv run --no-sync meson compile -C builddir/host format-check
```

These commands include source and test files while excluding ignored build
artifacts, research captures and dependencies. Checks leave files untouched and
fail when formatting is needed. The normal `meson test` run includes this check
in the `style` suite. For one file, use `clang-format-19 -i path/to/file.c`
or `uv run --no-sync ruff format path/to/file.py`.

## Local artifacts and device tools

Use `builddir/host` and `builddir/arm` for generated builds. The existing ignored
`build/` directories hold research reports, backups and package staging; they are
not cleaned or relocated by Meson. Device tools default to the ARM paths above.

The compiler's `ZIG_GLOBAL_CACHE_DIR` and `ZIG_LOCAL_CACHE_DIR`, and uv's
`UV_CACHE_DIR`, may be set to writable directories in restricted environments.

Installing privtap or restarting MPC requires saving the instrument's project
and explicit approval. See [CLAUDE.md](CLAUDE.md) for device-work rules.
