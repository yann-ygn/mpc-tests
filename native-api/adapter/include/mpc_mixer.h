#ifndef MPC_MIXER_H
#define MPC_MIXER_H

#include <stdint.h>

#define MPC_MIXER_SENDS 4u
#define MPC_MIXER_INSERTS 4u

/* Global Program mixer values, never per-sequence mute values. Numeric values
 * are normalized native controls, NOT dB or linear audio amplitude. Volume and
 * sends: [0,1], zero is silence; pan: [0,1], .5 is centre. The wire retains the
 * requested double and reports the accepted IEEE binary32 value separately.
 * Expected values compare after the same binary32 conversion, without epsilon.
 * Rack enable preserves each slot's independent enable bit. Empty slots have
 * no callable enable operation. Indices are zero-based A-D / slots 1-4.
 */
enum mpc_mixer_known {
  MPC_MIXER_VOLUME = 1u,
  MPC_MIXER_PAN = 2u,
  MPC_MIXER_SEND_0 = 4u,
  MPC_MIXER_SENDS_KNOWN = 60u,
  MPC_MIXER_RACK = 64u,
  MPC_MIXER_INSERT_0 = 128u,
  MPC_MIXER_ALL = 2047u
};

struct mpc_mixer_state {
  /* Opaque incarnation of Program/rack/ordered slot topology. Required by
   * mixer writes even when the caller does not use an expected value. */
  uint64_t generation;
  uint32_t known, occupied, rack_enabled, inserts_enabled;
  float volume, pan, sends[MPC_MIXER_SENDS];
};

struct mpc_mixer_request {
  uint64_t generation;
  uint32_t index;         /* send/slot only; zero for volume, pan and rack */
  double value, expected; /* booleans remain in mpc_command_request */
};

#endif
