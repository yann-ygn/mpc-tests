#ifndef MPC_STATE_H
#define MPC_STATE_H

#include "mpc_identity.h"

/* Pointer-free snapshot, owned by its executor job until release. The private
 * attachment protocol checks this version and size before transferring it. */
#define MPC_STATE_ABI 2u
#define MPC_STATE_MAX_BUSES 128u
#define MPC_STATE_NAME_BYTES 256u
#define MPC_STATE_ATTEMPTS 3u

enum mpc_state_known {
  MPC_STATE_REQUESTED = 1,
  MPC_STATE_AUDIO_SOLO = 2,
  MPC_STATE_AUDIO_ARM = 4,
  MPC_STATE_SILENCED = 8
};

enum mpc_state_collection {
  MPC_STATE_MIDI_TRIGGERED = 1,
  MPC_STATE_AUDIO = 2,
  MPC_STATE_MASTER = 3
};

struct mpc_state_track {
  struct mpc_identity_object identity;
  /* identity.index is canonical user order (uiIndex), or master order for buses.
   * collection_index preserves the legacy MIDI-triggered index contract. */
  uint32_t collection, collection_index;
  char name[MPC_STATE_NAME_BYTES];
  char type[24];
  uint32_t known;
  uint32_t muted, soloed, record_armed, audio_soloed, audio_record_armed, silenced;
};

struct mpc_state_sequence {
  struct mpc_identity_object identity;
  char name[MPC_STATE_NAME_BYTES];
  float bpm;
  uint32_t used;
};

struct mpc_state_snapshot {
  uint32_t abi_version, struct_bytes;
  char instance_id[37];
  uint64_t project, project_generation;
  uint64_t selected_track, selected_sequence;
  uint64_t started_ms, finished_ms, duration_ns;
  uint32_t attempts, track_count, bus_count, sequence_count, sequence_slots, used_sequences;
  struct mpc_state_track tracks[MPC_IDENTITY_MAX_TRACKS];
  /* Buses are metadata only. They have no writable user-track references. */
  struct mpc_state_track buses[MPC_STATE_MAX_BUSES];
  struct mpc_state_sequence sequences[MPC_IDENTITY_MAX_SEQUENCES];
};

#endif
