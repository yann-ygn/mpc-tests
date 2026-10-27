#ifndef MPC_MIXER_FIXTURE_BACKEND_H
#define MPC_MIXER_FIXTURE_BACKEND_H

#include "policy_fixture_backend.h"
#include "mpc_mixer_profile.h"
#include "mpc_commands.h"
#include <math.h>

static inline void mixer_float(struct fixture *f, uint32_t address, float value) {
  uint32_t bits;
  memcpy(&bits, &value, 4);
  put32(f, address, bits);
}

static inline void mixer_init(struct fixture *f, uint32_t program, uint32_t rack) {
  mixer_float(f, program + MIXER_VOLUME_VALUE, .75f);
  mixer_float(f, program + MIXER_PAN_VALUE, .5f);
  put32(f, program + MIXER_VOLUME_FORMAT, 3);
  put32(f, program + MIXER_PAN_MANAGER, IMAGE + 4);
  put32(f, program + MIXER_PAN_INVOKER, IMAGE + MIXER_PAN_FILTER);
  for (unsigned i = 0; i < 4; i++) {
    mixer_float(f, program + MIXER_SEND_BASE + 56 * i + MIXER_SEND_VALUE, 0);
    put32(f, program + MIXER_SEND_BASE + 56 * i + MIXER_SEND_FORMAT, 1);
  }
  put32(f, program + MIXER_RACK_OFFSET, rack);
  put32(f, rack, IMAGE + MIXER_RACK_VTABLE);
  put32(f, rack + MIXER_RACK_SLOTS, rack + 0x200);
  put32(f, rack + MIXER_RACK_SLOTS + 4, rack + 0x210);
  put32(f, rack + MIXER_RACK_SLOTS + 8, rack + 0x210);
  put_bool(f, rack + MIXER_RACK_ENABLED, 1);
  put32(f, rack + MIXER_RACK_BITS, rack + 0x300);
  put32(f, rack + MIXER_RACK_BITS + 8, rack + 0x300);
  put32(f, rack + MIXER_RACK_BIT_COUNT, 4);
  put32(f, rack + 0x300, 15);
  for (unsigned i = 0; i < 4; i++) {
    uint32_t slot = i == 0 || i == 3 ? rack + 0x400 + i * 0x100 : 0;
    put32(f, rack + 0x200 + i * 4, slot);
    if (slot)
      put32(f, slot, IMAGE + MIXER_SLOT_VTABLE);
  }
}

static inline enum mpc_native_error
fixture_float_filter(void *data, uintptr_t receiver, uintptr_t function, float *value) {
  struct fixture *f = data;
  assert(ui && function == IMAGE + MIXER_PAN_FILTER);
  assert(
    receiver == PROGRAM + MIXER_PAN_STORAGE || receiver == OBJECTS + 0x16000 + MIXER_PAN_STORAGE);
  f->filters++;
  if (f->family_after_filter)
    put32(f, PROGRAM, IMAGE + f->family_after_filter);
  if (*value < 0)
    *value = 0;
  if (*value > 1)
    *value = 1;
  return MPC_NATIVE_OK;
}

static inline enum mpc_native_error fixture_mixer_set(
  void *data, unsigned op, uintptr_t function, uintptr_t receiver, unsigned index, double value) {
  struct fixture *f = data;
  assert(ui && index < 4 && isfinite(value) && value >= 0 && value <= 1);
  f->setters++;
  uint32_t offset;
  if (op == MPC_COMMAND_VOLUME) {
    assert(function == IMAGE + MIXER_VOLUME_SETTER);
    offset = MIXER_VOLUME_VALUE - MIXER_RECEIVER;
  } else if (op == MPC_COMMAND_PAN) {
    assert(function == IMAGE + MIXER_FLOAT_SETTER);
    offset = 0x28;
  } else if (op == MPC_COMMAND_SEND) {
    assert(function == IMAGE + MIXER_SEND_SETTER);
    offset = MIXER_SEND_BASE + MIXER_SEND_STRIDE * index + MIXER_SEND_VALUE - MIXER_RECEIVER;
  } else if (op == MPC_COMMAND_RACK) {
    assert(function == IMAGE + MIXER_RACK_SETTER);
    if (f->propagate_audio)
      put_bool(f, receiver + MIXER_RACK_ENABLED, value != 0);
    return MPC_NATIVE_OK;
  } else {
    assert(op == MPC_COMMAND_INSERT && function == IMAGE + MIXER_SLOT_SETTER);
    uint32_t bits, flags;
    assert(!read_memory(data, receiver + MIXER_RACK_BITS, &bits, 4));
    assert(!read_memory(data, bits, &flags, 4));
    if (value)
      flags |= 1u << index;
    else
      flags &= ~(1u << index);
    if (f->propagate_audio)
      put32(f, bits, flags);
    return MPC_NATIVE_OK;
  }
  if (f->propagate_audio)
    mixer_float(f, receiver + offset, (float)value);
  if (f->family_after_set)
    put32(f, PROGRAM, IMAGE + f->family_after_set);
  return MPC_NATIVE_OK;
}
#endif
