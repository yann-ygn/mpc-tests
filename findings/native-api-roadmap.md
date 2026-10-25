# Native API roadmap in Taiga

Created 2026-10-24 in Taiga project **MPC Key 37 API** (id 24, slug `mpc-key-37-api`).

18 new user stories were created and linked to their epics; existing stories #18–21 were rewritten to use the native
adapter architecture. Descriptions, acceptance criteria, dependencies, statuses and epic membership were read back
and verified for all 22 stories. The owner accepted the [public contract](native-api-contract.md) on 2026-10-24
(#28 Done). #29 and #30 are Done with the guarded shared adapter, bounded executor and lab validation.
#32 is In progress with discovery and verified adapter attachment reporting; #19 and #20 remain In progress.

## Epic #4 — Native control library and live API

The earlier State service epic now covers the native adapter, shared protocol, live state, transports and SDKs.
Only the bounded native mute/solo/arm proof is already validated; these stories track the reusable implementation.

| US | Work | Status |
|---|---|---|
| #28 | Define the public native-control API and capability contract | Done |
| #29 | Build the native adapter library with validated firmware profiles | Done |
| #30 | Execute native operations on the MPC UI thread with bounded lifecycle | Done |
| #20 | Resolve active projects and generation-bound track/sequence handles | In progress |
| #18 | Expose authoritative live state snapshots from native MPC objects | New |
| #31 | Validate mute, solo and record-arm semantics beyond the bounded proof | New |
| #19 | Provide idempotent native mute, solo and record-arm command handlers | In progress |
| #32 | Expose the native adapter through a local command service | In progress — `describe` implemented |
| #33 | Publish live state changes and recover subscriptions after reconnect | New |
| #34 | Connect paired HTTP state and commands to Akai's web service | New |
| #35 | Expose the native-control API as typed MCP tools | New |
| #36 | Provide a Python client library for live state and track control | New |
| #21 | Package and run the native adapter and local command service on MPC | New |
| #38 | Qualify the native API for release with device and failure tests | New |
| #37 | Provide a JavaScript/TypeScript client using the shared API contract (follow-up client) | New |

## Epic #27 — Native capability expansion and validation

These are later capability batches or research stories. A catalog name or parameter ID does not mean the operation
is callable or supported. Each story requires the specified mapping and device validation before capability exposure.

| US | Work |
|---|---|
| #39 | Validate and add native volume, pan, sends and insert-enable controls |
| #40 | Validate and add transport, tempo and track/sequence selection |
| #41 | Validate and add track metadata, lifecycle and MIDI/CV routing |
| #42 | Probe clip, scene and song bindings and define the supported API subset |
| #43 | Probe note-event and automation editing through native model operations |
| #44 | Probe audio-event, sample-pool and clip-export operations |
| #45 | Map generic program/instrument parameters and separate IDs from controls |

## Delivery order

Initial service slice: [`native-api/`](../native-api/README.md) implements `describe` on a private Unix socket with
request/response schemas, host fixture tests and an ARM device smoke test. It reports live model/build metadata,
stable installation identity and bounded service-instance identity. All state/write capabilities are unavailable;
the adapter/identity/qualification dependencies still apply before enabling them. HTTP and MCP remain #34/#35.

Adapter slice (#29): [`libmpc_native.so`](../native-api/adapter/README.md) provides a small guarded C interface,
versioned private profiles, seven code fingerprints and typed property/filter primitives. Twenty-two host tests
pass; the isolated 28-case fixture also passed on ARM. Device tests verified UI-thread reads against an independent
reader, full unload of both libraries, and a retained adapter visible through `describe`. Public state/write
capabilities remain unavailable. The lab host's ownership hook does not complete #20.

Executor slice (#30, 2026-10-25): the adapter owns a scheduler and bounded typed FIFO jobs carrying opaque
identities/generations. Resolution and revalidation happen on the verified UI thread. Queued cancellation,
unknown running outcomes, disconnected clients, capture ownership and shutdown/close are exercised by host and
isolated ARM fixtures; ASan/UBSan also pass. All eight host Meson entries pass. A real JUCE read-only device test
matched independent flag observations, preserved state/PID and drained/unloaded both libraries. The lab uses the
shared executor, with external worker/UI quiescence still required before DSO unload. See the
[sanitized evidence](native-executor-proof-20261025.json). Public state/write capabilities remain unavailable
pending #20 and the subsequent state/semantics/handler stories.

1. Define the public contract (#28), firmware adapter (#29) and application-thread executor (#30).
2. Establish active-project/handle lifetimes (#20), live snapshots (#18) and the full flag-semantics matrix (#31), then productize the flag handlers (#19).
3. Build the local service (#32); follow with subscriptions (#33), paired HTTP (#34), MCP (#35) and device packaging/lifecycle (#21).
4. Deliver the first Python client (#36) and release qualification (#38). JavaScript/TypeScript (#37) follows the same contract.
5. Validate expansion batches (#39–45) individually and add only the supported subset to the API and SDKs.

The Taiga story descriptions contain the specific dependency references and acceptance criteria. Existing README
story #25 remains the repository documentation item; individual SDK/deployment stories also require their own usage
and package documentation. No estimates, due dates or sprint commitments were invented.

## Retired stories

The following open stories were removed from Taiga on 2026-10-24 with the owner's authorization because the native
adapter no longer needs MPC X identity spoofing or hardware-display inference:

| Former US | Work | Reason |
|---|---|---|
| #15 | Filter MPC X-only SysEx before forwarding to Key 37 | Only needed for the retired identity-spoofing route |
| #22 | Read Q-Link displays as text | Native model reads provide track identity and metadata directly |
| #23 | Determine what the MPC X meter measures | An MPC X feedback experiment outside the native API roadmap |
| #24 | Translate MPC X LEDs back to Key 37 | Only needed to keep the retired identity mode usable |

Completed discovery, privtap and tooling stories remain as project history. The empty MPC X extras epic is outside
the active roadmap. Historical findings and identity-test code are retained in Git; removing a backlog story does
not erase its research results.

## Evidence

- [Native state reader](live-state.md)
- [Native write proof and runtime constraints](native-control.md)
- [Sanitized verified test record](native-control-proof-20261024.json)
- [Static capability catalog](native-control-catalog.md)
