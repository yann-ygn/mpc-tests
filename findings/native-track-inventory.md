# Complete user-track inventory (US #46)

Implemented for Key 37 / MPC 3.9.1.2, build
`8371199f945ecc40f79cb19c17ff5de72810a91c`. All user tracks now have
generation-bound references in `get_state`, `list_tracks`, `recovery_snapshot`
and `current_track`, including selected audio tracks. This adds inventory and
ordering metadata; flag qualification remains plugin/drum only (US #49 owns
additional families).

## Native collections and canonical order

Project `+0xa94` owns the TrackPool. The profile's historical `normalBegin: 68`
was misnamed: it is the MIDI-triggered collection, excluding audio. The actual
native normal/user collection is at `+0x74` and includes audio in Track List
order. Collection headers contain begin/end/capacity pointers four bytes apart.

| TrackPool begin offset | Collection | Contents |
|---|---|---|
| `0x34` | All | Every owned track object, including master objects |
| `0x44` | MIDI-triggered (historical `normal*` profile keys) | Drum, keygroup, clip, plugin, MIDI and CV |
| `0x54` | Non-MIDI | Audio and master objects; this is not a bus collection |
| `0x74` | Normal/user (`user*` keys) | Every user track in canonical Track List order |
| `0x84` | Master | Explicit non-user input/output, return and submix objects |
| `0x94` | Audio | Audio tracks, with collection-relative indices |
| `0xa4`, `0xb4`, `0xc4`, `0xd4`, `0xe4`, `0xf4` | Family views | Clip, CV, drum, keygroup, MIDI, plugin respectively |
| `0x104`, `0x114`, `0x124` | Master family views | Output, return, submix respectively |

The static getter group at RVA `0x257e748`, normal getter at `0x257e768`,
master getter at `0x257e770`, type collection selector at `0x257e7c0`,
partition routine at `0x257f7b8` and Track type getter at `0x264add0` are
fingerprinted in the control profile. The latter reads Track `+0x64c`.
Program vtables supply the separately validated public family names; input,
output and master-output remain distinguishable types. The adapter checks
explicit normal/master and MIDI/audio membership, rather than labelling every
object absent from the MIDI vector a bus.

The loaded device project supplied six plugin tracks and two audio tracks.
The Track List displayed all eight in the same order as `+0x74`, with the two
audio tracks before the final two plugins. Selecting each audio track and the
following plugin confirmed the selected Project pointer/index, the independent
memory reader and all five service projections. The Mixer Effects view showed
the same user order in this session. Clients must use `uiIndex` for canonical
Track List order: a Mixer page, bank, family filter or bus page is not a source
of canonical indices.

## Public indices, references and bounds

`tracks` is emitted in `uiIndex` order, starting at zero. Existing `index`
values remain relative to the MIDI-triggered collection. New audio rows have
an audio-relative `index`; `collection` explicitly names `midi_triggered` or
`audio`. Thus a plugin after two audio tracks can have `index: 4, uiIndex: 6`.
Neither number is a writable target. Names and either order may change without
changing a Track reference. Deletion, replacement, project reload, destruction
and address reuse retain the existing incarnation rules.

The `buses` projection is the explicit master collection, including typed I/O
objects. Its `collection` is `master`, `index` is master-relative, and `uiIndex`,
reference and generation are null. Unknown Program types remain null. Families
without qualified flags expose nulls with `unsupportedFields`; their inclusion
cannot authorize a native setter.

The API bounds current elements at 128 MIDI-triggered plus 8 audio tracks
(136 combined), 128 master objects, and 128 sequence slots. These are service
bounds, not claims about every firmware/model's creation limits. Spare native
vector storage is checked separately (256 slots for the user registry, 512 for
inventory vectors), so 136 elements in a 256-slot allocation are accepted.
Overflow, missing membership, duplicate objects and overlapping partitions
fail closed; no truncated inventory is published as complete.

Snapshot ABI **2** carries collection metadata and 136 Track rows; its fixed
pointer-free transfer is **127808 bytes** on both host and ARM. The private
request/tag are `STATE_V2` / `MPC_ADAPTER_STATE_V2`. Old snapshot attachments
cannot enable state reads. Executor ABI **5** expands identities; command ABI
**2** expands receipt observations. Old configurations are rejected and trusted
C clients must recompile. Primitive adapter ABI 1 is unchanged. Discovery,
schemas, examples and package version **0.3.0** accompany these changes.

Capture fences user, MIDI, audio, master and all-track membership/order,
selection, Program pointer association and Program vtables. Valid changes
retry up to three times and then report `STATE_UNSTABLE`. Invalid reads or
partitions fail without a snapshot. Capture is an interval observation, not an
atomic native transaction.

## Validation and restoration

The mixed-inventory fixture runs the real executor and identity implementation
on host and isolated ARM. It covers all seven known user families, unknown
Program metadata, all five master types, audio selection, independent vector
orders, rename/reorder, Program replacement, audio deletion/recreation, ABA,
in-place project reload, stale writes without native invocation, capture races,
empty projects and exact/overflow capacity boundaries. The service fixtures
exercise every projection, invalid transfer metadata, older attachments and
rejection of index/`uiIndex` write targets. Existing command, lifecycle and
protocol coverage continues to pass, including ASan/UBSan.

Device validation changed selection and view only. It did not create/delete
tracks, load/save projects, invoke a flag setter, install an addin or restart
MPC. Reorder, forced ABA and large inventories were tested synthetically on
host and isolated ARM; they are not claimed as live edits to the user's project.
The temporary service/adapter were removed, the installed 0.2.0 host resumed,
and the observed flags, selection, sequence state, inventory and original
Mixer Effects view were restored. Installing 0.3.0 remains a separate upgrade
requiring the owner's restart approval under the repository device rules.

See the [sanitized evidence](native-track-inventory-proof-20261026.json).
The independent reader is `tools/mpc_track_inventory.py`; unlike the earlier
`mpc_live_state.py` diagnostic snapshot, it includes all user tracks and fences
the collection contents as well as their vector headers. Private raw captures
and attachment details remain under ignored `build/us46/`.
