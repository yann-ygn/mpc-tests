#include "native_fixture_backend.h"

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
