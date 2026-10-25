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
  unsigned char objects[0x8000];
  bool ui, authorized, filter_allows, propagate_audio, invalidate_after_set;
  int corrupt, unreadable;
  unsigned code_reads, object_reads, validations, setters, filters, sequence, filter_order,
    setter_order;
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
  put32(f, POOL + NATIVE_VECTOR_BEGIN, VECTOR);
  put32(f, POOL + NATIVE_VECTOR_END, VECTOR + 4);
  put32(f, POOL + NATIVE_VECTOR_CAPACITY, VECTOR + 4);
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
static struct mpc_native_hooks hooks(struct fixture *f) {
  return (struct mpc_native_hooks){.abi_version = MPC_NATIVE_ABI,
                                   .struct_bytes = sizeof(struct mpc_native_hooks),
                                   .userdata = f,
                                   .validate_target = validate_target};
}
static struct mpc_native_adapter *open_fixture(struct fixture *f, bool validator) {
  struct mpc_native_adapter *adapter;
  struct mpc_native_io backend = io(f);
  struct mpc_native_hooks host = hooks(f);
  assert(mpc_native_open_backend(&backend, validator ? &host : NULL, &adapter) == MPC_NATIVE_OK);
  return adapter;
}
int main(void) {
  struct fixture f;
  struct mpc_native_adapter *a;
  struct mpc_native_track_state state;
  struct mpc_native_write_result result;
  struct mpc_native_status status;
  struct mpc_native_io backend;
  bool expected;
  unsigned cases = 0;
  reset(&f);
  backend = io(&f);
  strcpy(f.build, "1122");
  assert(mpc_native_open_backend(&backend, NULL, &a) == MPC_NATIVE_BUILD_MISMATCH);
  assert(!a && !f.code_reads && !f.object_reads && !f.setters);
  cases++;
  reset(&f);
  backend = io(&f);
  strcpy(f.model, "other MPC");
  assert(mpc_native_open_backend(&backend, NULL, &a) == MPC_NATIVE_MODEL_MISMATCH);
  assert(!a && !f.code_reads && !f.setters);
  cases++;
  reset(&f);
  backend = io(&f);
  struct mpc_native_hooks wrong = hooks(&f);
  wrong.abi_version++;
  assert(mpc_native_open_backend(&backend, &wrong, &a) == MPC_NATIVE_ABI_MISMATCH);
  assert(!a && !f.code_reads);
  cases++;
  for (size_t i = 0; i < sizeof native_fingerprints / sizeof *native_fingerprints; i++) {
    reset(&f);
    f.corrupt = (int)i;
    backend = io(&f);
    assert(mpc_native_open_backend(&backend, NULL, &a) == MPC_NATIVE_FINGERPRINT_MISMATCH);
    assert(!a && !f.object_reads && !f.validations && !f.setters);
    cases++;
  }
  reset(&f);
  f.unreadable = 0;
  backend = io(&f);
  assert(mpc_native_open_backend(&backend, NULL, &a) == MPC_NATIVE_READ_FAILED);
  assert(!a && !f.setters);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  assert(mpc_native_get_status(a, &status) == MPC_NATIVE_OK && status.code_verified);
  char epoch[37];
  strcpy(epoch, status.instance_id);
  assert(mpc_native_get_status(a, &status) == MPC_NATIVE_OK && !strcmp(epoch, status.instance_id));
  assert(mpc_native_read_track(a, &target, &state) == MPC_NATIVE_OK);
  assert(
    state.family == MPC_NATIVE_PLUGIN && !state.muted &&
    state.audio_known == (MPC_NATIVE_AUDIO_SOLO | MPC_NATIVE_AUDIO_ARM));
  put32(&f, PROGRAM, IMAGE + NATIVE_DRUM_VTABLE);
  assert(
    mpc_native_read_track(a, &target, &state) == MPC_NATIVE_OK && state.family == MPC_NATIVE_DRUM);
  mpc_native_close(a);
  cases++;
  reset(&f);
  f.ui = false;
  a = open_fixture(&f, true);
  assert(mpc_native_set_mute(a, &target, true, NULL, &result) == MPC_NATIVE_WRONG_THREAD);
  assert(!f.validations && !f.object_reads && !f.setters);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, false);
  assert(mpc_native_set_mute(a, &target, true, NULL, &result) == MPC_NATIVE_NOT_READY);
  assert(!f.object_reads && !f.setters);
  mpc_native_close(a);
  cases++;
  reset(&f);
  f.authorized = false;
  a = open_fixture(&f, true);
  assert(mpc_native_set_mute(a, &target, true, NULL, &result) == MPC_NATIVE_STALE_TARGET);
  assert(!f.object_reads && !f.setters);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  put32(&f, VECTOR, TRACK + 4);
  assert(mpc_native_set_mute(a, &target, true, NULL, &result) == MPC_NATIVE_STALE_TARGET);
  assert(!f.setters);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  put32(&f, VECTOR + 4, TRACK);
  put32(&f, POOL + NATIVE_VECTOR_END, VECTOR + 8);
  put32(&f, POOL + NATIVE_VECTOR_CAPACITY, VECTOR + 8);
  assert(mpc_native_read_track(a, &target, &state) == MPC_NATIVE_STALE_TARGET);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  put32(&f, PROGRAM, IMAGE + 8);
  assert(mpc_native_set_solo(a, &target, true, NULL, &result) == MPC_NATIVE_UNSUPPORTED_TRACK_TYPE);
  assert(!f.setters);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  put_bool(&f, PROGRAM + native_bindings[0].requested, 2);
  assert(mpc_native_read_track(a, &target, &state) == MPC_NATIVE_READ_FAILED);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  expected = false;
  assert(mpc_native_set_mute(a, &target, true, &expected, &result) == MPC_NATIVE_OK);
  assert(
    result.native_invoked && result.changed_known && result.changed && result.observed.muted &&
    f.setters == 1);
  assert(mpc_native_set_mute(a, &target, true, NULL, &result) == MPC_NATIVE_OK);
  assert(!result.native_invoked && result.changed_known && !result.changed && f.setters == 1);
  mpc_native_close(a);
  cases++;
  reset(&f);
  f.propagate_audio = false;
  a = open_fixture(&f, true);
  assert(mpc_native_set_solo(a, &target, true, NULL, &result) == MPC_NATIVE_OK);
  assert(result.observed.soloed && !result.observed.audio_soloed && f.setters == 1);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  assert(mpc_native_set_record_armed(a, &target, true, NULL, &result) == MPC_NATIVE_OK);
  assert(
    f.filters == 1 && f.setters == 1 && f.filter_order < f.setter_order &&
    result.observed.record_armed);
  mpc_native_close(a);
  cases++;
  reset(&f);
  f.filter_allows = false;
  a = open_fixture(&f, true);
  assert(mpc_native_set_record_armed(a, &target, true, NULL, &result) == MPC_NATIVE_VALUE_REJECTED);
  assert(
    result.native_invoked && result.changed_known && !result.changed &&
    !result.observed.record_armed);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  put32(&f, TRACK + native_bindings[2].filter_invoker, IMAGE + 4);
  assert(mpc_native_set_record_armed(a, &target, true, NULL, &result) == MPC_NATIVE_FILTER_MISSING);
  assert(!f.filters && !f.setters);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  expected = true;
  assert(
    mpc_native_set_mute(a, &target, true, &expected, &result) == MPC_NATIVE_PRECONDITION_FAILED);
  assert(!result.native_invoked && !f.setters);
  mpc_native_close(a);
  cases++;
  reset(&f);
  f.invalidate_after_set = true;
  a = open_fixture(&f, true);
  assert(mpc_native_set_mute(a, &target, true, NULL, &result) == MPC_NATIVE_STALE_TARGET);
  assert(result.native_invoked && !result.changed_known);
  mpc_native_close(a);
  cases++;
  reset(&f);
  a = open_fixture(&f, true);
  f.corrupt = 0;
  assert(mpc_native_get_status(a, &status) == MPC_NATIVE_FINGERPRINT_MISMATCH);
  f.corrupt = -1;
  assert(mpc_native_set_mute(a, &target, true, NULL, &result) == MPC_NATIVE_FINGERPRINT_MISMATCH);
  assert(!f.setters);
  mpc_native_close(a);
  cases++;
  a = (void *)1;
  assert(mpc_native_open(NULL, &a) != MPC_NATIVE_OK && !a);
  cases++;
  printf("native_fixture_cases=%u\n", cases);
  return 0;
}
