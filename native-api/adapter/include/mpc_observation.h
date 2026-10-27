#ifndef MPC_OBSERVATION_H
#define MPC_OBSERVATION_H

#include "mpc_state.h"

/* Shared command/watch sampling ABI. Historical command type names are retained.
 * No names, bus metadata or sequence metadata. Native values are sampled live;
 * this is not an atomic causal change log. Unknown fields are excluded by known.
 * Absence outside [offset, offset + track_count) conveys no state. Each sample
 * carries its own wall-clock completion time and monotonic duration. A producer
 * must initialize/resync with GET_STATE and restart paging on topology changes.
 * This primitive installs no flag listeners: bounded sampling is the fallback
 * until native change signals have independently been verified. */
#define MPC_OBSERVATION_ABI 1u

/* Pointer-free observations, deliberately narrower than full state snapshots.
 * They establish best-effort peer/selection changes, not an atomic causal log. */
struct mpc_command_track {
  uint64_t id, generation;
  uint32_t known, muted, soloed, record_armed, audio_soloed, audio_record_armed, silenced;
  uint32_t family;
};
struct mpc_command_observation {
  uint64_t project, generation, selected_track, selected_sequence, captured_ms;
  uint64_t selected_sequence_generation;
  uint32_t mute_scope;
  /* Explicit flags-only coverage: rows outside this interval were not read. */
  uint64_t topology_epoch, duration_ns;
  uint32_t offset, total_tracks;
  uint32_t track_count;
  struct mpc_command_track tracks[MPC_IDENTITY_MAX_TRACKS];
};
#endif
