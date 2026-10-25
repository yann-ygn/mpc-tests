# Akai's built-in web object service

Observed on the Key 37 on 2026-10-24, MPC 3.9.1.2. The running service identifies itself as
`AZ0X Web Server/0.5.6+gite579a1d`.

**The web service works as a generic object publisher, but MPC publishes no objects in the session tested.** After
pairing a diagnostic client, the complete object tree was empty. A temporary publisher then exposed a known property,
and its changes appeared over HTTP. This supplies a possible transport for our state service; it does not supply the
missing live track/sequence state by itself.

## Runtime and ownership

- `/usr/bin/az0x-webserver` is a separate process managed by `az0x-webserver.service`, listening on TCP port 80.
- Application publishers connect through the Unix **datagram** socket `/tmp/az0x-webserver`.
- The device ships `/usr/lib/libaz0x-webserver-client.so.0.5.6`, with C++ client, object, property, method, pairing and
  polling interfaces. This library was not mapped in the running MPC process at inspection.
- There were no publisher sockets before the diagnostic. Navigating through Sounds and Preferences did not produce
  an application publisher or a pairing UI. The authenticated empty inventory is stronger evidence than socket names
  alone, but does not rule out another mode or firmware registering objects later.
- GET `/` returns HTTP 404, `No document root set`: a listening server does not imply an installed browser frontend.

Server settings read through native IPC were:

```json
{
  "webAccessEnabled": true,
  "pairingRequestsEnabled": true,
  "unpairedSessionsEnabled": false
}
```

## Pairing

An unpaired API request returns HTTP 401, `{"pairState":"notPaired"}`, and establishes a `sessionID` cookie. Keep the
same cookie jar through the handshake.

POST `/api/auth` with a JSON body `{}` begins pairing. Without an application registered as a pairing agent, this
returns `pairError: backendError`. There is no PIN-entry step available through the stock UI we inspected.

A temporary native client registered with `hasPairAgentRole: true` received `startPairingSession`, containing a
`webSession` (session ID, user agent, client address and last-seen time), a PIN and flags. The POST initially returned
HTTP 401 with `pairState: pending` and a `pairPin`; the backend's later decision changed subsequent requests.

The backend reply uses **string values**, not C++ enum integers:

```json
{
  "message": "endPairingSession",
  "clientID": "<the diagnostic webSession.sessionID>",
  "result": "accepted"
}
```

`declined` was verified to produce `pairError: denied`. An early integer-result experiment caused a diagnostic IP
block; it was removed with `unblockAddress` for that exact address and the blocked list was verified empty. Do not
guess numeric result values: the verified protocol uses `accepted`, `declined`, and `blocked` strings (blocking was
observed during that calibration; the successful probe does not request it).

The retained probe approves only the session whose user agent contains its own freshly generated random identifier.
It never changes `webAccessEnabled`, `pairingRequestsEnabled`, or `unpairedSessionsEnabled`. It unpairs its own HTTP
session before exiting. Cookies and pairing credentials are kept in memory, excluded from the saved report.

## Native publisher protocol

The client library sends CBOR maps over a connected Unix datagram socket, with no additional framing. Its local
socket is `/tmp/az0x-webserver-client.<pid>`. An observed header has:

```json
{
  "message": "requestServerSettings",
  "senderID": 1234,
  "seq": 1,
  "clientName": "MPCWebProbe_example"
}
```

`senderID` matched the process PID. Sequence numbers increase per sender. Replies contain `ref` for the request's
sequence number. A `ping`/`pong` round trip provides an ordering barrier after native updates; it is not a general
acknowledgement that a musical command has executed.

Verified message types:

| Message | Payload / behavior |
|---|---|
| `addClient` | `hasPairAgentRole` boolean; registers the header's client name |
| `removeClient` | removes that client and its published data |
| `requestServerSettings` | replies with `returnServerSettings` and the three access flags |
| `requestBlockedAddresses` | replies with `returnBlockedAddresses`; the probe compares counts without saving addresses |
| `addObject` | `objectPath`, `objectMeta`, `objectValue` |
| `changePropertyValue` | `objectPath`, `propertyName`, `propertyValue`; updates the server's cached object value |
| `removeObject` | `objectPath` |
| `endPairingSession` | `clientID`, string `result`; pairing-agent response |
| `unpairWebSession` | `sessionID`; used only for the diagnostic's own session |

For an object published as `/probe` by client `MPCWebInventory`, the external object path is
`/MPCWebInventory/probe`. Do not add the client prefix to the native `objectPath`.

## HTTP results

All following observations used an authenticated diagnostic cookie jar.

| Request | Result |
|---|---|
| GET `/api/v1/subtree` before publication | HTTP 404, `{}`: empty object registry |
| GET `/api/v1/subtree` after publication | HTTP 200; only the diagnostic object, with `meta` and `value` |
| GET `/api/v1/object-meta/MPCWebInventory/probe` | HTTP 200; object schema and property metadata |
| GET `/api/v1/object-properties/MPCWebInventory/probe` | HTTP 200, `{"answer":42}` |
| Same GET after native `changePropertyValue` | HTTP 200, `{"answer":43}` |
| GET `/api/v1/subtree` after removing the object | HTTP 404, `{}` again |
| GET `/api/v1/subtree` after unpairing this session | HTTP 401, `pairState: notPaired` |

Object metadata was published with a property schema declaring `readOnly: true`; enforcement of remote property
writes was not tested. `object-meta` and `object-properties` take the object address in the URL path: adding
`?path=...` to the bare endpoint returned `DataModel: Client not found`.

Static evidence also names `/api/v1/object-method` and change notifications (`objectAdded`, `objectRemoved`,
`propertyAdded`, `propertyRemoved`, `propertyValueChanged`, `webAccessChanged`). Neither musical commands nor the
notification subscription protocol were exercised. A WebSocket upgrade on `/api/v1/subtree` returned ordinary HTTP
200; that does not establish the actual WebSocket endpoint. HTTP polling and native-to-HTTP value propagation are
verified.

## Reproduce the bounded probe

`tools/web_ipc_bridge.c` is a small standalone Unix datagram bridge, with no C++ ABI dependency or MPC injection.
`tools/web_probe.py` handles CBOR, cookie-based pairing, inventory, the 42-to-43 publication test, and cleanup.

From the repository root, with the existing Zig/uv toolchain:

```sh
uv sync --locked
uv run --no-sync meson setup builddir/arm --cross-file build-support/cross/mpc-armhf.ini
uv run --no-sync meson compile -C builddir/arm
uv run --no-project --with cbor2 python tools/web_probe.py
```

`MPC_HOST` or `--host` selects the device. SSH uses the known working `-F /dev/null`, batch mode and strict host-key
checking. The bridge runs from a unique `/tmp` path and removes its own socket and executable on normal exit. The
Python client unregisters its object/client and revokes its session, including cleanup after an exception. The native
helper has a 90-second lifetime limit; forced termination or loss of SSH may still require checking its temporary
socket and pairing state.

The verified run found zero existing objects, read metadata and value 42, observed the update to 43, restored the empty
registry, and confirmed HTTP 401 after unpairing. Server access flags and the blocked-address count were unchanged.
The sanitized report is in gitignored `build/web-probe/verified-report.json`. Earlier exploratory transcripts in that
directory include credentials and must not be committed. An initial C++ ABI diagnostic crashed in its own process;
the retained implementation uses the wire protocol and exits cleanly.

## Consequence for the state API

The existing service could provide authentication, schema/value reads, and object hosting for a small state producer.
We have not found a built-in project/track/sequence/transport model to consume. The next useful implementation is a
publisher fed by the established project/controller observations, with explicit validity and freshness, while targeted
probing resolves the missing live fields. Using this HTTP service does not resolve those observability gaps.

MPC and web-server PIDs remained unchanged during the experiments. No projects, transport controls, addin installs,
application restarts or server access flags were changed. UI navigation was restored to the original Sounds view;
pairing bookkeeping was exercised and the diagnostic sessions were revoked.
