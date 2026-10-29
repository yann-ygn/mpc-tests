#include "private.h"
#include "mpc_commands.h"
#include "mpc_adapter_profile.h"

#include <string.h>

_Static_assert(
  MPC_COMMAND_MIDI_PROGRAM - MPC_COMMAND_TRACK_COLOUR + 1 == MPC_TRACK_SETUP_FIELDS,
  "track setup operation/field ordering");

/* Direct notifying property paths recovered from ScriptableTrackPool, without
 * constructing or invoking its disabled scripting frontend. Native setters
 * own listener notification and any audio-side work. */
struct setup_binding {
  uint32_t property, value, setter;
  unsigned kind;
};
static const struct setup_binding bindings[MPC_TRACK_SETUP_FIELDS] = {
  {0x5f0, 0x618, 0xe5243c, MPC_EDIT_UINT},
  {0x4a4, 0x4cc, 0xe4fb24, MPC_EDIT_INT},
  {0x5c4, 0x5ec, 0xe5af48, MPC_EDIT_FLOAT},
  {0x3d0, 0x3f8, 0x1462c7c, MPC_EDIT_INT_REFERENCE},
  {0x8, 0x30, 0xe298bc, MPC_EDIT_BOOL},
  {0x34, 0x5c, 0x1ac2e0c, MPC_EDIT_BYTE},
  {0x60, 0x88, 0x1ac2e0c, MPC_EDIT_BYTE},
  {0x8c, 0xb4, 0xe298bc, MPC_EDIT_BOOL},
  {0xb8, 0xe0, 0x1ac2e0c, MPC_EDIT_BYTE}};

enum mpc_native_error mpc_track_setup_read(
  struct mpc_native_adapter *a, uint32_t track, unsigned family, struct mpc_track_setup *out) {
  memset(out, 0, sizeof *out);
  uint32_t known = mpc_track_setup_fields(family);
  for (unsigned i = 0; i < MPC_TRACK_SETUP_FIELDS; i++) {
    if (!(known & (1u << i)))
      continue;
    const struct setup_binding *b = &bindings[i];
    uint32_t raw = 0;
    size_t bytes = b->kind == MPC_EDIT_BOOL || b->kind == MPC_EDIT_BYTE ? 1 : 4;
    if (a->io.read(a->io.userdata, track + b->value, &raw, bytes))
      return MPC_NATIVE_READ_FAILED;
    double value = raw;
    if (b->kind == MPC_EDIT_INT)
      value = (int32_t)raw;
    if (b->kind == MPC_EDIT_FLOAT) {
      float number;
      memcpy(&number, &raw, 4);
      value = number;
    }
    if (!mpc_track_setup_scalar_valid(i, value))
      return MPC_NATIVE_READ_FAILED;
    out->values[i] = value;
    out->known |= 1u << i;
  }
  return MPC_NATIVE_OK;
}

enum mpc_native_error mpc_track_setup_invoke(
  struct mpc_native_adapter *a, uint32_t track, unsigned field, double value, bool *invoked) {
  if (field >= MPC_TRACK_SETUP_FIELDS || !mpc_track_setup_scalar_valid(field, value))
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (!a->io.track_edit)
    return MPC_NATIVE_NOT_READY;
  const struct setup_binding *b = &bindings[field];
  *invoked = true;
  return a->io
    .track_edit(a->io.userdata, b->kind, a->io.image + b->setter, track + b->property, value);
}
