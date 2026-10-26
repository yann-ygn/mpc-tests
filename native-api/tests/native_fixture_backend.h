#ifndef MPC_NATIVE_FIXTURE_BACKEND_H
#define MPC_NATIVE_FIXTURE_BACKEND_H

#include "private.h"
#include "mpc_adapter_profile.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
  IMAGE = 0x10000000u,
  OBJECTS = 0x20000000u,
  POOL = OBJECTS + 0x2000,
  VECTOR = OBJECTS + 0x3000,
  TRACK = OBJECTS + 0x4000,
  PROGRAM = OBJECTS + 0x6000
};
struct fixture {
  unsigned char objects[0x110000];
  bool ui, authorized, filter_allows, propagate_audio, invalidate_after_set;
  int corrupt, unreadable;
  unsigned code_reads, object_reads, validations, setters, filters, sequence, filter_order,
    setter_order;
  uint32_t family_after_filter, family_after_set;
  char build[129], model[256];
};
static const struct mpc_native_target target = {OBJECTS, TRACK, PROGRAM};
static void put32(struct fixture *f, uint32_t address, uint32_t value) {
  assert(address >= OBJECTS && address - OBJECTS + 4 <= sizeof f->objects);
  memcpy(f->objects + address - OBJECTS, &value, 4);
}
static void put_bool(struct fixture *f, uint32_t address, unsigned value) {
  assert(address >= OBJECTS && address - OBJECTS < sizeof f->objects);
  f->objects[address - OBJECTS] = (unsigned char)value;
}
static void reset(struct fixture *f) {
  memset(f, 0, sizeof *f);
  f->ui = f->authorized = f->filter_allows = f->propagate_audio = true;
  f->corrupt = f->unreadable = -1;
  strcpy(f->build, NATIVE_PROFILE_BUILD);
  strcpy(f->model, NATIVE_PROFILE_MODEL);
  put32(f, OBJECTS, IMAGE + NATIVE_PROJECT_VTABLE);
  for (size_t i = 0; i < sizeof native_secondary / sizeof *native_secondary; i++)
    put32(f, OBJECTS + native_secondary[i].offset, IMAGE + native_secondary[i].vtable);
  put32(f, OBJECTS + NATIVE_POOL_OFFSET, POOL);
  put32(f, POOL, IMAGE + NATIVE_POOL_VTABLE);
  put32(f, POOL + NATIVE_USER_BEGIN, VECTOR);
  put32(f, POOL + NATIVE_USER_END, VECTOR + 4);
  put32(f, POOL + NATIVE_USER_CAPACITY, VECTOR + 4);
  put32(f, POOL + NATIVE_ALL_BEGIN, VECTOR);
  put32(f, POOL + NATIVE_ALL_END, VECTOR + 4);
  put32(f, POOL + NATIVE_ALL_CAPACITY, VECTOR + 4);
  put32(f, POOL + NATIVE_MIDI_BEGIN, VECTOR);
  put32(f, POOL + NATIVE_MIDI_END, VECTOR + 4);
  put32(f, POOL + NATIVE_MIDI_CAPACITY, VECTOR + 4);
  put32(f, VECTOR, TRACK);
  put32(f, TRACK, IMAGE + NATIVE_TRACK_VTABLE);
  put32(f, TRACK + NATIVE_PROGRAM_OFFSET, PROGRAM);
  put32(f, PROGRAM, IMAGE + NATIVE_PLUGIN_VTABLE);
  put32(f, TRACK + native_bindings[2].filter_manager, IMAGE + 4);
  put32(f, TRACK + native_bindings[2].filter_invoker, IMAGE + native_bindings[2].filter_rva);
}
static enum mpc_native_error identity(void *data, char build[129], char model[256]) {
  struct fixture *f = data;
  strcpy(build, f->build);
  strcpy(model, f->model);
  return MPC_NATIVE_OK;
}
static int read_memory(void *data, uintptr_t address, void *output, size_t length) {
  struct fixture *f = data;
  for (size_t i = 0; i < sizeof native_fingerprints / sizeof *native_fingerprints; i++) {
    if (address != IMAGE + native_fingerprints[i].rva || length != 32)
      continue;
    f->code_reads++;
    if ((int)i == f->unreadable)
      return -1;
    memcpy(output, native_fingerprints[i].bytes, length);
    if ((int)i == f->corrupt)
      ((unsigned char *)output)[0] ^= 1;
    return 0;
  }
  f->object_reads++;
  if (
    address < OBJECTS || address - OBJECTS > sizeof f->objects ||
    length > sizeof f->objects - (address - OBJECTS))
    return -1;
  memcpy(output, f->objects + address - OBJECTS, length);
  return 0;
}
static bool on_ui(void *data) {
  return ((struct fixture *)data)->ui;
}
static enum mpc_native_error
validate_target(void *data, const struct mpc_native_target *candidate) {
  struct fixture *f = data;
  f->validations++;
  return f->authorized && !memcmp(candidate, &target, sizeof target) ? MPC_NATIVE_OK
                                                                     : MPC_NATIVE_STALE_TARGET;
}
static enum mpc_native_error
setter(void *data, uintptr_t function, uintptr_t property, bool value) {
  struct fixture *f = data;
  f->setters++;
  f->setter_order = ++f->sequence;
  for (unsigned i = 0; i < 3; i++) {
    const struct native_binding *b = &native_bindings[i];
    uint32_t owner = b->owner_is_track ? TRACK : PROGRAM;
    if (property != owner + b->property || function != IMAGE + b->setter)
      continue;
    put_bool(f, owner + b->requested, value);
    if (f->propagate_audio)
      put_bool(f, owner + b->observed, value);
    if (f->invalidate_after_set)
      f->authorized = false;
    if (f->family_after_set)
      put32(f, PROGRAM, IMAGE + f->family_after_set);
    return MPC_NATIVE_OK;
  }
  assert(!"Wrong setter receiver/function ABI");
  return MPC_NATIVE_INVALID_ARGUMENT;
}
static enum mpc_native_error
filter(void *data, uintptr_t storage, uintptr_t function, bool *value) {
  struct fixture *f = data;
  assert(
    storage == TRACK + native_bindings[2].filter_storage &&
    function == IMAGE + native_bindings[2].filter_rva);
  f->filters++;
  if (f->family_after_filter)
    put32(f, PROGRAM, IMAGE + f->family_after_filter);
  f->filter_order = ++f->sequence;
  *value = *value && f->filter_allows;
  return MPC_NATIVE_OK;
}
static struct mpc_native_io io(struct fixture *f) {
  return (struct mpc_native_io){.userdata = f,
                                .image = IMAGE,
                                .identity = identity,
                                .read = read_memory,
                                .on_ui_thread = on_ui,
                                .setter = setter,
                                .filter = filter};
}
#endif
