#ifndef MPC_IDENTITY_H
#define MPC_IDENTITY_H

#include <stdint.h>

#define MPC_IDENTITY_MAX_TRACKS 128u
#define MPC_IDENTITY_MAX_SEQUENCES 128u

/* No native address crosses this interface. IDs are never reused in an adapter
 * instance. A generation identifies an incarnation, not a name or list index. */
struct mpc_identity_object {
  uint64_t id, generation;
  unsigned index;
};

struct mpc_identity_snapshot {
  char instance_id[37];
  uint64_t project, project_generation;
  uint64_t selected_track, selected_sequence;
  unsigned track_count, sequence_count, sequence_slots;
  struct mpc_identity_object tracks[MPC_IDENTITY_MAX_TRACKS];
  struct mpc_identity_object sequences[MPC_IDENTITY_MAX_SEQUENCES];
};

#endif
