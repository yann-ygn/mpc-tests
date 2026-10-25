# Native API: discovery and live state service

The first implementation of the [accepted 0.1 contract](../findings/native-api-contract.md).
`describe` reads device-tree metadata, GNU ELF notes and verified adapter attachment status. With an
authoritative attachment, `get_state`, `list_tracks`, `current_track`, `current_sequence` and
`recovery_snapshot` capture current native state through the UI executor. Public Unix writes, receipt lookup
and streaming remain unavailable. Transport-independent [flag handlers](adapter/COMMANDS.md) are implemented by #19;
#32 still supplies the authenticated wire binding and live mode/settings policy. The service does not load
the [native adapter](adapter/README.md) itself. The [persistent addin](INSTALL.md) embeds the adapter and the same
Unix service in MPC, with health and draining stop/start commands; the standalone binary can also use a temporary lab attachment. See the
[snapshot findings and qualification limits](../findings/native-state.md).

The local transport is a restricted Unix stream socket. Each connection carries **one compact JSON request plus
a newline**, then one JSON response plus a newline. Read through the response newline and close the connection.
HTTP and MCP adapters follow in their own stories; this service has no TCP listener.

## Source layout

- `src/main.c`: command-line parsing and validation.
- `src/server.c`: socket ownership, peer admission, bounded client I/O and shutdown.
- `src/client.c`: the `request` command's delivery and response relay.
- `src/protocol.c`: request parsing and discovery JSON; `src/discovery.c` reads process metadata.
- `src/state.c`: validated snapshot serialization/projections; `src/attachment.c` checks the MPC peer and
  performs bounded metadata/state transfers.
- `src/host.c` and `src/addin.c`: persistent in-process attachment, startup gate, lifecycle and management socket.
  `package.py` builds the checked ARM package with the shared addin installer; [installation guide](INSTALL.md).

`transport.h` is the private CLI/transport boundary; schemas cover discovery and the additive state operations.

## Build and test

```sh
uv sync --locked
uv run --no-sync meson setup builddir/host
uv run --no-sync meson compile -C builddir/host
uv run --no-sync meson test -C builddir/host --suite native-api --print-errorlogs

# ARM, using the pinned Zig compiler:
uv run --no-sync meson setup builddir/arm --cross-file build-support/cross/mpc-armhf.ini
uv run --no-sync meson compile -C builddir/arm
```

Zig's `ZIG_GLOBAL_CACHE_DIR` and `ZIG_LOCAL_CACHE_DIR` can be set to writable directories if its default cache
is unavailable. Builds go to ignored `builddir/{host,arm}/native-api/mpc-api`; they never deploy or restart MPC.
JSON Schema validation is included in the development environment. See the [root build guide](../README.md)
for requirements, all targets and dependency checks.

The host tests use synthetic proc/device-tree fixtures, including ELF32 and ELF64 note sections at unrelated
file offsets. They cover identity changes, firmware/model mismatches, malformed requests, connection bounds,
slow/disconnected clients, socket ownership/permissions and clean shutdown. `--root` is a server startup option
for these fixtures, never a public API argument.

To upload and smoke-test the ARM binary in a fresh private device directory:

```sh
python3 native-api/device_smoke.py --output build/native-api/device-smoke.json
```

This cleans up afterwards. Add `--keep` to leave the temporary service running for further requests.
Add `--state` with an existing authoritative attachment to check all read operations, compare them to the
independent observer and measure repeated native captures.
The report records its directory, socket and service PID. This does not install a startup job or addin.

## Start and query

On the machine running the compiled binary:

```sh
mkdir -m 700 /tmp/mpc-api
./mpc-api serve --socket /tmp/mpc-api/api.sock --device-id dev_studio
```

Run the request from another shell as the same user:

```sh
printf '%s\n' '{"apiVersion":"0.1","op":"describe"}' |
  ./mpc-api request --socket /tmp/mpc-api/api.sock
```

For a service left by the smoke test, use its reported directory in place of `.` and its reported socket path.
Over SSH, pipe the same request into the remote binary's `request` subcommand. Use the SSH options already
established for this device (`-F /dev/null`, batch mode, strict host-key checking).

`request` returns a nonzero exit code for delivery errors. A delivered API error is JSON in stdout with a zero
delivery exit code; inspect its `error.code`. Server shutdown uses SIGTERM or SIGINT and removes its own socket.
It refuses an existing socket/file rather than unlinking another service's path. Remove its temporary directory
after shutdown when no longer needed.

The configured `deviceId` must stay the same when this installation restarts. It is not derived from the hardware
serial. The smoke test stores its generated value in `device-id` beside the temporary executable. The opaque
`instanceId` stays stable between queries, changes on service restart and rotates on observed MPC process or
executable/model identity changes. Observed attachment identity/loss also changes the service instance.
No public object handles are issued yet.

## Response semantics

- `firmware.model`: device-tree model, or `null` when unavailable.
- `firmware.buildId`: GNU build ID read from the live process's executable, or `null` when not reliably readable.
- `firmware.version`: version mapped from that exact known build ID, with `versionSource: build_profile`.
  An unknown build never inherits the research profile's version.
- `readiness`: distinguishes a matched research profile from unsupported/unknown firmware, and separately reports
  `nativeAdapter: not_attached` or `attached`. Status is `discovery_only`, or `ready` when authoritative
  snapshots are attached. Verified attachment metadata includes its ABI, incarnation ID and `bounded_lab_proof`
  qualification; clients still consult each operation's availability.
- `capabilities.describe.available`: true even when MPC is absent or its firmware is unsupported. Read operations
  become available only through an authoritative snapshot attachment. Other operations remain unavailable, with
  `NOT_IMPLEMENTED`, `UNSUPPORTED_FIRMWARE` or `NOT_READY` reasons.
- `readableFields`: qualified native state fields exposed by the attachment; empty when reads are unavailable.
- `limits`: enforced frame/connection limits. Native queue/receipt capacities are zero and command timing values
  are null while command operations are unavailable. Before enabling commands, receipt retention must meet the
  contract's ten-minute minimum.

Connections require a private parent directory owned by the service user, a socket with mode `0600`, and matching
Unix peer UID. Requests are limited to 1024 bytes excluding the framing newline; there are at most 16 active
connections. A request must finish within 1000 ms of acceptance, even if it trickles in. These are IPC resource
bounds, not native command execution limits. Input permits the two ASCII string identifiers `apiVersion` and
`op`, with ordinary JSON escaping; duplicate/extra fields, invalid types and unsupported API versions produce
structured errors. Unavailable operations return `NOT_READY`; unknown operations return `UNSUPPORTED_OPERATION`.
The response limit is 1 MiB. Snapshot transfer has a 200 ms total deadline and fails closed on malformed,
partial or stale data. Snapshot reads never serve a cached fallback.

The schemas are [request](schema/describe-request.schema.json) and [response/error](schema/describe-response.schema.json).
Requests reject unknown fields; responses permit additive optional fields as the contract requires.
Concrete examples are [request](examples/describe-request.json) and [response](examples/describe-response.json),
using illustrative installation/instance IDs. An [attached response](examples/describe-attached-response.json)
shows the adapter metadata while state/write capabilities remain unavailable.

## Device verification

Verified on 2026-10-24 against the running MPC Key 37 / 3.9.1.2, build ID
`8371199f945ecc40f79cb19c17ff5de72810a91c`. The ARM service read the model/build, preserved its instance ID between
queries, rejected unsupported versions/operations, and kept its socket at `0600`. MPC's PID stayed unchanged.
Process discovery uses the executable path: the actual main thread is named `MPC Main Thread`, so its thread name
cannot identify the process reliably. Host fixtures cover that case.

## Read current state

Enable the temporary authoritative host without installing an addin or restarting MPC:

```sh
python3 tools/mpc_native_lab.py --identities --keep --output build/native-api/state-attachment.json
```

Then send the same two-field request with `op` set to `get_state`, `list_tracks`, `current_track`,
`current_sequence`, or `recovery_snapshot`. `get_state` and `recovery_snapshot` both return a full fresh snapshot.
Each read has its own instance-scoped observation revision and capture interval; this is not an event cursor.
`current_track` and `current_sequence` project a full capture onto its selection; null selections return null.
`list_tracks` returns normal tracks and a separate bus metadata list.

Requested flags and separate audio-side copies are qualified only for plugin/drum normal tracks. Bus flags,
other families' flags, project name, actual audio activity and effective transport tempo remain null.
A sequence's `bpm` is its own property; `used` comes from the native usage flag rather than its name or slot.
Unsupported fields are identified explicitly. A supported field read failure returns `READ_FAILED`; three
unstable topology/identity/selection attempts return `STATE_UNSTABLE`.

Example: [full state response](examples/state-response.json).
Schemas: [state request](schema/state-request.schema.json), [state response](schema/state-response.schema.json).
A successful `describe` advertises each read independently of the still-unavailable write/watch operations.
The public service instance scopes all returned references; the adapter incarnation remains private attachment
metadata. No heap addresses appear in state responses.

Stop and unload the temporary host after testing:

```sh
python3 tools/mpc_native_lab.py --detach build/native-api/state-attachment.json \
  --output build/native-api/state-detached.json
```

On 2026-10-25 the read smoke matched an independent observer on the actual device. Twenty native captures
had p95 9.058 ms for two normal tracks, 28 buses and 128 sequence objects. A Sounds-mode controller mute edit
was visible immediately and restored. See [sanitized evidence](../findings/native-state-proof-20261025.json).

## Managed command handlers

US #19 provides the [typed command API](adapter/include/mpc_commands.h),
[integration guide](adapter/COMMANDS.md), [domain request schema](schema/command-request.schema.json),
and [receipt schema](schema/command-receipt.schema.json). The handler retains caller-scoped outcomes,
deduplicates retries, checks expected values at UI dispatch, verifies required native/audio readbacks,
and reports observed selection/peer changes. It keeps running work alive across client waits/disconnects.

These are library/domain interfaces. Public Unix setters and `get_command` remain unavailable until #32
supplies authenticated wire routing and a validated live mode/settings qualification policy. The bounded
private lab policy is manually qualified and does not enable public commands. See
[device evidence](../findings/native-commands.md).
