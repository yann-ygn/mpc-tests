# Native API: discovery service

The first implementation of the [accepted 0.1 contract](../findings/native-api-contract.md).
`describe` is a read-only endpoint in a standalone C process on MPC. It reads device-tree metadata and GNU ELF
notes from the running MPC executable. It does not load the lab bridge or read native project objects.
The [native adapter](adapter/README.md) is a separate shared library. A retained experimental host can expose its
verified attachment status to this service; this does not enable state queries or commands.

The local transport is a restricted Unix stream socket. Each connection carries **one compact JSON request plus
a newline**, then one JSON response plus a newline. Read through the response newline and close the connection.
HTTP and MCP adapters follow in their own stories; this service has no TCP listener.

## Source layout

- `src/main.c`: command-line parsing and validation.
- `src/server.c`: socket ownership, peer admission, bounded client I/O and shutdown.
- `src/client.c`: the `request` command's delivery and response relay.
- `src/protocol.c`: request parsing and JSON responses; `src/discovery.c` and `src/attachment.c` provide metadata.

`transport.h` is the private CLI/transport boundary; public request and response schemas are unchanged.

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

This cleans up afterwards. Add `--keep` to leave the temporary discovery service running for further requests.
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
  `nativeAdapter: not_attached` or `attached` and `status: discovery_only`. Verified attachment metadata includes
  its ABI, incarnation ID and `bounded_lab_proof` qualification; clients still consult each operation's availability.
- `capabilities.describe.available`: true even when MPC is absent or its firmware is unsupported. All other
  operations are unavailable, with `NOT_IMPLEMENTED`, `UNSUPPORTED_FIRMWARE` or `NOT_READY` reasons.
- `readableFields`: native state fields exposed by the public API, currently empty.
- `limits`: enforced frame/connection limits. Native queue/receipt capacities are zero and command timing values
  are null while command operations are unavailable. Before enabling commands, receipt retention must meet the
  contract's ten-minute minimum.

Connections require a private parent directory owned by the service user, a socket with mode `0600`, and matching
Unix peer UID. Requests are limited to 1024 bytes excluding the framing newline; there are at most 16 active
connections. A request must finish within 1000 ms of acceptance, even if it trickles in. These are IPC resource
bounds, not native command execution limits. Input permits the two ASCII string identifiers `apiVersion` and
`op`, with ordinary JSON escaping; duplicate/extra fields, invalid types and unsupported API versions produce
structured errors. Other contract operations return `NOT_READY`; unknown operations return `UNSUPPORTED_OPERATION`.

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
