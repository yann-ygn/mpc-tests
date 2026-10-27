#include "private.h"
#include "mpc_commands.h"
#include "mpc_adapter_profile.h"
#include "mpc_mixer_profile.h"

#include <math.h>
#include <string.h>

bool mpc_mixer_numeric(unsigned op) {
  return op >= MPC_COMMAND_VOLUME && op <= MPC_COMMAND_SEND;
}

unsigned mpc_mixer_fields(unsigned family) {
  return family == MPC_NATIVE_PLUGIN || family == MPC_NATIVE_DRUM ||
      family == MPC_NATIVE_KEYGROUP || family == MPC_NATIVE_AUDIO
    ? MPC_MIXER_ALL
    : 0;
}

unsigned mpc_mixer_field(unsigned op, unsigned index) {
  switch (op) {
  case MPC_COMMAND_VOLUME:
    return MPC_MIXER_VOLUME;
  case MPC_COMMAND_PAN:
    return MPC_MIXER_PAN;
  case MPC_COMMAND_SEND:
    return index < 4 ? MPC_MIXER_SEND_0 << index : 0;
  case MPC_COMMAND_RACK:
    return MPC_MIXER_RACK;
  case MPC_COMMAND_INSERT:
    return index < 4 ? MPC_MIXER_INSERT_0 << index : 0;
  default:
    return 0;
  }
}

bool mpc_mixer_request_valid(unsigned op, const struct mpc_mixer_request *r, bool expected) {
  if (op < MPC_COMMAND_VOLUME)
    return !r->generation && !r->index && !r->value && !r->expected;
  if (
    !r->generation || !mpc_mixer_field(op, r->index) ||
    ((op != MPC_COMMAND_SEND && op != MPC_COMMAND_INSERT) && r->index))
    return false;
  if (mpc_mixer_numeric(op))
    return isfinite(r->value) && r->value >= 0 && r->value <= 1 &&
      (!expected || (isfinite(r->expected) && r->expected >= 0 && r->expected <= 1));
  return !r->value && !r->expected;
}

bool mpc_mixer_value(const struct mpc_mixer_state *s, unsigned op, unsigned index, double *out) {
  if (!(s->known & mpc_mixer_field(op, index)))
    return false;
  *out = op == MPC_COMMAND_VOLUME ? s->volume
    : op == MPC_COMMAND_PAN       ? s->pan
    : op == MPC_COMMAND_SEND      ? s->sends[index]
    : op == MPC_COMMAND_RACK      ? s->rack_enabled
                                  : !!(s->inserts_enabled & (1u << index));
  return true;
}

static bool
read_at(struct mpc_native_adapter *a, uint32_t base, uint32_t offset, void *out, size_t n) {
  return base && !(base % 4) && base <= UINT32_MAX - offset && n <= UINT32_MAX - base - offset &&
    !a->io.read(a->io.userdata, base + offset, out, n);
}

static bool number(struct mpc_native_adapter *a, uint32_t base, uint32_t offset, float *out) {
  return read_at(a, base, offset, out, 4) && isfinite(*out) && *out >= 0 && *out <= 1;
}

/* UI-thread sampler. No names, sequence metadata, bus traversal or setter. A
 * complete layout check precedes exposure; unknown fixture/firmware layouts
 * leave all mixer fields explicitly unavailable without affecting flag reads. */
enum mpc_native_error mpc_mixer_read(
  struct mpc_native_adapter *a,
  uint32_t program,
  unsigned family,
  struct mpc_mixer_state *out,
  struct mpc_mixer_topology *topology) {
  memset(out, 0, sizeof *out);
  memset(topology, 0, sizeof *topology);
  if (!mpc_mixer_fields(family))
    return MPC_NATIVE_OK;
  uint32_t format, rack, vtable, begin, end, capacity, bits, bit_begin, bit_end, bit_count;
  if (!read_at(a, program, MIXER_VOLUME_FORMAT, &format, 4) || format != 3)
    return MPC_NATIVE_OK;
  topology->program = program;
  if (!read_at(a, program, 0, &topology->program_vtable, 4))
    return MPC_NATIVE_READ_FAILED;
  if (
    !number(a, program, MIXER_VOLUME_VALUE, &out->volume) ||
    !number(a, program, MIXER_PAN_VALUE, &out->pan))
    return MPC_NATIVE_READ_FAILED;
  out->known = MPC_MIXER_VOLUME | MPC_MIXER_PAN;
  for (unsigned i = 0; i < 4; i++) {
    uint32_t base = MIXER_SEND_BASE + MIXER_SEND_STRIDE * i;
    if (
      !read_at(a, program, base + MIXER_SEND_FORMAT, &format, 4) || format != 1 ||
      !number(a, program, base + MIXER_SEND_VALUE, &out->sends[i]))
      return MPC_NATIVE_READ_FAILED;
    topology->formats[i] = format;
    out->known |= MPC_MIXER_SEND_0 << i;
  }
  if (
    !read_at(a, program, MIXER_RACK_OFFSET, &rack, 4) || !rack ||
    !read_at(a, rack, 0, &vtable, 4) || vtable != a->io.image + MIXER_RACK_VTABLE ||
    !read_at(a, rack, MIXER_RACK_SLOTS, &begin, 4) ||
    !read_at(a, rack, MIXER_RACK_SLOTS + 4, &end, 4) ||
    !read_at(a, rack, MIXER_RACK_SLOTS + 8, &capacity, 4) || begin % 4 || end < begin ||
    end - begin != 16 || capacity < end || capacity - begin > 1024 ||
    !read_at(a, begin, 0, topology->slots, 16) || !read_at(a, rack, MIXER_RACK_BITS, &bits, 4) ||
    !read_at(a, rack, MIXER_RACK_BITS + 4, &bit_begin, 4) || bit_begin ||
    !read_at(a, rack, MIXER_RACK_BITS + 8, &bit_end, 4) || bit_end != bits ||
    !read_at(a, rack, MIXER_RACK_BIT_COUNT, &bit_count, 4) || bit_count != 4 ||
    !read_at(a, bits, 0, &out->inserts_enabled, 4))
    return MPC_NATIVE_READ_FAILED;
  unsigned char enabled;
  if (!read_at(a, rack, MIXER_RACK_ENABLED, &enabled, 1) || enabled > 1)
    return MPC_NATIVE_READ_FAILED;
  topology->rack = rack;
  out->rack_enabled = enabled;
  out->inserts_enabled &= 15u;
  out->known |= MPC_MIXER_RACK;
  for (unsigned i = 0; i < 4; i++) {
    if (!topology->slots[i])
      continue;
    if (!read_at(a, topology->slots[i], 0, &vtable, 4) || vtable != a->io.image + MIXER_SLOT_VTABLE)
      return MPC_NATIVE_READ_FAILED;
    out->occupied |= 1u << i;
    out->known |= MPC_MIXER_INSERT_0 << i;
  }
  return MPC_NATIVE_OK;
}

enum mpc_native_error mpc_mixer_set(
  struct mpc_native_adapter *a,
  const struct mpc_native_target *target,
  unsigned op,
  const struct mpc_mixer_request *r,
  bool value,
  bool has_expected,
  bool expected,
  struct mpc_native_write_result *out) {
  memset(out, 0, sizeof *out);
  enum mpc_native_family family;
  enum mpc_native_error error = mpc_native_target_check(a, target, &family);
  if (error)
    return error;
  struct mpc_mixer_state before, after;
  struct mpc_mixer_topology topology, verified;
  error = mpc_mixer_read(a, target->program, family, &before, &topology);
  if (error)
    return error;
  double current;
  if (!mpc_mixer_value(&before, op, r->index, &current))
    return op == MPC_COMMAND_INSERT && before.known ? MPC_NATIVE_EMPTY_INSERT
                                                    : MPC_NATIVE_UNSUPPORTED_TRACK_TYPE;
  double desired = mpc_mixer_numeric(op) ? (double)(float)r->value : value;
  double wanted = mpc_mixer_numeric(op) ? (double)(float)r->expected : expected;
  out->changed_known = true;
  if (has_expected && current != wanted)
    return MPC_NATIVE_PRECONDITION_FAILED;
  if (current == desired)
    return MPC_NATIVE_OK;
  if (!a->io.mixer_set)
    return MPC_NATIVE_NOT_READY;
  a->invoking_flag = true;
  error = mpc_native_target_check(a, target, &family);
  a->invoking_flag = false;
  if (error)
    return error;
  uint32_t receiver = op == MPC_COMMAND_PAN ? target->program + MIXER_PAN_PROPERTY
    : op >= MPC_COMMAND_RACK                ? topology.rack
                                            : target->program + MIXER_RECEIVER;
  uintptr_t function = a->io.image +
    (op == MPC_COMMAND_VOLUME   ? MIXER_VOLUME_SETTER
       : op == MPC_COMMAND_PAN  ? MIXER_FLOAT_SETTER
       : op == MPC_COMMAND_SEND ? MIXER_SEND_SETTER
       : op == MPC_COMMAND_RACK ? MIXER_RACK_SETTER
                                : MIXER_SLOT_SETTER);
  if (op == MPC_COMMAND_PAN) {
    uint32_t manager, invoker;
    if (
      !a->io.float_filter || !read_at(a, target->program, MIXER_PAN_MANAGER, &manager, 4) ||
      !manager || !read_at(a, target->program, MIXER_PAN_INVOKER, &invoker, 4) ||
      invoker != a->io.image + MIXER_PAN_FILTER)
      return MPC_NATIVE_FILTER_MISSING;
    float filtered = (float)desired;
    out->native_invoked = true;
    out->changed_known = false;
    unsigned filter_stage = mpc_ui_enter(a, MPC_UI_FILTER_SETTER);
    error =
      a->io.float_filter(a->io.userdata, target->program + MIXER_PAN_STORAGE, invoker, &filtered);
    mpc_ui_leave(a, filter_stage);
    if (error)
      return error;
    if (!isfinite(filtered) || filtered < 0 || filtered > 1)
      return MPC_NATIVE_VALUE_REJECTED;
    desired = filtered;
  }
  a->invoking_flag = true;
  error = mpc_native_target_check(a, target, &family);
  a->invoking_flag = false;
  if (error)
    return error;
  error = mpc_mixer_read(a, target->program, family, &after, &verified);
  if (error)
    return error;
  if (memcmp(&topology, &verified, sizeof topology))
    return MPC_NATIVE_STALE_MIXER;
  out->native_invoked = true;
  out->changed_known = false;
  unsigned stage = mpc_ui_enter(a, MPC_UI_FILTER_SETTER);
  error = a->io.mixer_set(a->io.userdata, op, function, receiver, r->index, desired);
  mpc_ui_leave(a, stage);
  if (error)
    return error;
  /* The managed executor owns independent readback and bounded verification.
   * Rack setters enqueue native audio-thread work; model completion alone is
   * never promoted to an engine or audio-buffer completion claim. */
  return MPC_NATIVE_OK;
}
