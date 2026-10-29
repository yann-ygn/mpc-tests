# Assigned instrument identity (US #55)

Package 0.16.0 adds read-only instrument product metadata to every authoritative
user-track projection and watch. Installed availability remains 0.15.0 until an
owner-authorized upgrade. Qualification evidence is in
[native-plugin-identity-proof-20261029.json](native-plugin-identity-proof-20261029.json).

## Public contract

Each track and bus has an `instrument` object. For an assigned, loaded MPC-format
instrument, for example:

```json
{
  "status": "available",
  "reason": null,
  "displayName": "Hype",
  "productId": "MPC:Hype",
  "instanceId": null
}
```

`displayName` is the assigned plugin's native product name. The editable Track
name is independent: `Plugin 001` and `Plugin 002` both used Hype during device
qualification. `productId` is the native `PluginDescription.fileOrIdentifier`
value for the `MPC` format. Its scope is the native format/catalog: it identifies
a product, is shared by separate loaded instances, and survives the tested
instance differences. It is not a Track reference, preset identifier, loaded
instance identity, license identifier or promised cross-firmware/vendor UUID.
No persistent identity is derived from an address or either name. `instanceId`
is explicitly unsupported and always null. Other native formats are unqualified.

| Status | Reason | Name/product fields |
|---|---|---|
| `available` | null | Both qualified strings |
| `not_applicable` | `NOT_PLUGIN_TRACK` | Both null (including buses) |
| `unassigned` | `NO_INSTRUMENT_ASSIGNED` | Both null; no instance and empty descriptor |
| `unavailable` | `PLUGIN_NOT_LOADED` | Both null; assigned descriptor without a loaded UI instance |
| `unsupported` | `UNQUALIFIED_PLUGIN_FORMAT` | Both null |

Loading and failed loads are deliberately grouped under `unavailable` when an
assigned descriptor exists without an instance. No unqualified loading-state
flag is interpreted. Supported-path read failures, invalid UTF-8, unbounded
strings or inconsistent owners fail the whole capture with `READ_FAILED` or
bounded `STATE_UNSTABLE`; they never substitute cached identity. Both strings
have a 256-byte storage bound including NUL, without silent truncation.

`get_state`, `list_tracks`, `current_track` and `recovery_snapshot` share the same
capture/serialization path. Reads do not select a track, open an instrument UI,
load a preset or invoke a setter. Discovery's `instrumentIdentity` names the
supported fields, plugin family, MPC format, product scope and unsupported
instance/preset/parameter capabilities. Strict schemas require the new object
and correlate status, reason and nullable fields.

## Qualified native ownership and capture

The pinned build remains `8371199f945ecc40f79cb19c17ff5de72810a91c`.
The established application/Engine/active Project resolver and destruction-observed
Track generation precede `Track +0x648 -> Program`. PluginProgram's primary
vtable is RVA `0x6932a48`. The Program owns:

| Field | Offset | Interpretation |
|---|---:|---|
| UI AudioPluginPtr value | `+0x1b68` | Loaded instance association |
| UI AudioPluginPtr owner | `+0x1b6c` | Shared ownership control block |
| PluginDescription.name | `+0x1bd0` | JUCE UTF-8 string storage |
| PluginDescription.pluginFormatName | `+0x1bd8` | Qualified only for `MPC` |
| PluginDescription.fileOrIdentifier | `+0x1be8` | Native product identifier |

The assignment path calls `0x8ada04`, whose virtual dispatch at `0x8ada64`
asks the actual plugin instance to fill its description, then copies the strings
and descriptor into the Program at `0x2502384` / `0x25023d4`. The native descriptor
copy at `0x24fcaec` independently establishes the same layout. Read-only
association access is at `0x24f8bfc`. These paths and the assignment boundary are
fingerprinted in the validated firmware profile; no native getter or setter is
invoked by the reader.

The independent process-memory tool [mpc_plugin_identity.py](../tools/mpc_plugin_identity.py)
checks both Program metadata and the instance's own descriptor. On this device,
Hype's descriptor-fill virtual is `0x226ac00`, copying instance fields beginning
at `+0x268`; Electric's is `0x2008aa0`, copying from `+0x390`. Both implementations
are separately fingerprinted. The independent reader rejects other implementations
rather than guessing offsets. The production reader uses the qualified common
Program descriptor path and never dereferences the plugin instance pointer.

A full UI capture copies owned strings and a disposable association fence, then
re-reads the descriptor, instance/owner and string storage association before
publication. Existing Program property destruction observers fence Program
lifetime/address reuse; Track lifetime, current Program, inventory, family and
project/selection generations are checked too. Capture retries discard all output
on instability. Nothing retains a borrowed plugin pointer after the callback.
A surviving Track keeps its existing handle across assignment/replacement or
conversion. Each full capture obtains fresh product metadata, including when an
instance or Program address is reused; there is no persistent plugin cache or
invented instance generation.

Snapshot ABI **8**, executor ABI **12**, and private `STATE_V8` /
`MPC_ADAPTER_STATE_V8` replace the previous transfer layouts. The track row is
992 bytes and the pointer-free snapshot is 298,952 bytes on host and ARM.
Primitive ABI 2, command ABI 9 and observation ABI 2 are unchanged. Rebuild C
callers; older private state attachments are rejected.

## Watch reconciliation and cost

Instrument identity is metadata. The shared producer's existing five-second full
metadata audit publishes assignment/replacement/unload/conversion changes as an
ordered replacement snapshot; an explicit authoritative read can reconcile sooner.
Controls frames preserve the metadata baseline until its next audit and claim
controls coverage only. They do not assert a fresh instrument scan. This gives a
nominal detection bound of one audit interval plus queued UI capture and delivery,
not the 100 ms controls interval. The existing retained cursor, replay expiry and
resync rules apply unchanged. There is no per-command or high-rate plugin scan.
The isolated native fixture explicitly checks that controls sampling performs no
plugin identity reads.

## Verification and limits

Host native cases cover selected/unselected tracks, track names different from
plugin names, separate Programs with one product, multiple products, absent
instances, empty descriptors, unsupported formats, replacement during capture,
continuous instability, Program lifetime/address reuse, conversion, reload,
invalid UTF-8, unterminated and unreadable strings. Existing identity fixtures
retain stale-generation and Track/Project address reuse checks. Real Unix host
integration checks all four projections, metadata audit delivery, retained replay,
unload/empty states and strict schemas. Isolated ARM exercises the same native
and publisher code; ASan/UBSan exercise native lifetimes and transport integration.

Device assignment/replacement is confined to a temporary fifth plugin track in
the owner's untitled default project. Original tracks are read only. Both Hype
and Electric are compared against Main's actual assigned instrument field and
the loaded instance's independent descriptor. No saved-project name, preset label,
screen text or Track name is used as the API's source. Device evidence separates
implementation/qualification from installed availability. Empty/loading/failure
branches and malicious strings are deterministic isolated cases; no instrument
crash or license failure is manufactured on the device.


Device results (2026-10-29): 25 host Meson entries, 20 ASan/UBSan native entries
and all eight isolated ARM fixtures pass. Additional strict-schema and malformed
wire checks pass on host and sanitizer binaries. The Hype/Electric Program and
loaded-instance descriptors agree independently for selected and unselected tracks.

With eight subscribers, Electric replacement arrived through the ordinary metadata
audit in **4,422.822–4,663.651 ms**, including the native load and SSH Unix forwarding.
All eight models agreed, the Track reference survived, and retained replay matched
the replacement snapshot. In the reverse Hype assignment, a native load exceeded
the existing bounded controls read; all streams conservatively reported
`CONTINUITY_LOST` in **385.010–390.272 ms**. The old cursor required resync; recovery
and eight fresh subscriptions agreed on Hype with the same Track reference.
Fresh retained reconnect passed in both cases. Deleting the temporary Track also
required conservative resync; the next authoritative state contained only the
original four tracks. Loading and deletion do not promise uninterrupted replay.

Forty full reads with five tracks had native capture p95 **14.574 ms**; forty
restored four-track reads had p95 **12.928 ms**. Across the shared producer's 189
successful full captures, UI callback p95 was **15.579 ms**, maximum **27.881 ms**.
Its 2,426 successful controls callbacks had p95 **3.861 ms**, maximum **10.951 ms**.
Three bounded controls reads did not complete (one unknown outcome and two
cancellations during loads/deletion); these are explicit continuity losses, not
successful latency observations. These measurements describe the declared small
project and completed callbacks, not marginal per-plugin cost, every callback in
MPC, maximum density or a guarantee during arbitrary UI loading.

The temporary Track was deleted. Independent flags/selection/sequence/inventory
and native mixer/setup/bus comparisons match the baseline; the original plugin
instruments are unchanged, selected Track is restored, playhead is zero and Main
is restored. The temporary host drained and unloaded; its device bundle was
removed without MPC restart or project save/load. The existing installed **0.15.0**
host resumed ready and all six discovery/read operations pass. It still has no
`instrument` fields. The 0.16.0 package is prepared locally, not installed.
