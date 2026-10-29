#ifndef MPC_TRACK_SETUP_H
#define MPC_TRACK_SETUP_H

#include "mpc_native.h"

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

/* Pointer-free model observations. Unknown fields have no value authority.
 * MIDI channels use 1..16, with 0 meaning follow the incoming channel. Bank
 * bytes and program numbers use MIDI wire numbering, 0..127. */
enum mpc_track_setup_field {
  MPC_TRACK_COLOUR,
  MPC_TRACK_TRANSPOSE,
  MPC_TRACK_VELOCITY,
  MPC_TRACK_MIDI_OUTPUT_CHANNEL,
  MPC_TRACK_MIDI_BANK_ENABLED,
  MPC_TRACK_MIDI_BANK_MSB,
  MPC_TRACK_MIDI_BANK_LSB,
  MPC_TRACK_MIDI_PROGRAM_ENABLED,
  MPC_TRACK_MIDI_PROGRAM,
  MPC_TRACK_SETUP_FIELDS
};

struct mpc_track_setup {
  uint32_t known;
  double values[MPC_TRACK_SETUP_FIELDS];
};
struct mpc_track_edit {
  double value, expected;
};

static inline unsigned mpc_track_setup_fields(unsigned family) {
  if (family == MPC_NATIVE_AUDIO)
    return 1u << MPC_TRACK_COLOUR;
  if (
    family == MPC_NATIVE_PLUGIN || family == MPC_NATIVE_DRUM || family == MPC_NATIVE_KEYGROUP ||
    family == MPC_NATIVE_CV)
    return 7u;
  return family == MPC_NATIVE_MIDI ? (1u << MPC_TRACK_SETUP_FIELDS) - 1 : 0;
}

static inline bool mpc_track_setup_boolean(unsigned field) {
  return field == MPC_TRACK_MIDI_BANK_ENABLED || field == MPC_TRACK_MIDI_PROGRAM_ENABLED;
}
static inline bool mpc_track_setup_scalar_valid(unsigned field, double value) {
  if (!isfinite(value) || field >= MPC_TRACK_SETUP_FIELDS)
    return false;
  if (field == MPC_TRACK_VELOCITY)
    return value >= (double)0.01f && value <= 2;
  if (value < -24 || value > 0xffffff || value != (int32_t)value)
    return false;
  if (field == MPC_TRACK_COLOUR)
    return value >= 0 && value <= 0xffffff;
  if (field == MPC_TRACK_TRANSPOSE)
    return value >= -24 && value <= 24;
  if (field == MPC_TRACK_MIDI_OUTPUT_CHANNEL)
    return value >= 0 && value <= 16;
  return value >= 0 && value <= (mpc_track_setup_boolean(field) ? 1 : 127);
}
static inline bool mpc_track_setup_valid(const struct mpc_track_setup *s) {
  if (s->known >> MPC_TRACK_SETUP_FIELDS)
    return false;
  for (unsigned i = 0; i < MPC_TRACK_SETUP_FIELDS; i++)
    if ((s->known & (1u << i)) && !mpc_track_setup_scalar_valid(i, s->values[i]))
      return false;
  return true;
}

#endif
