#include "private.h"
#include "mpc_adapter_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct mpc_native_adapter {
  struct mpc_native_io io;
  struct mpc_native_hooks hooks;
  int fault;
  char instance_id[37];
};

static enum mpc_native_error verify(struct mpc_native_adapter *adapter) {
  if (!adapter)
    return MPC_NATIVE_INVALID_ARGUMENT;
  int fault = __atomic_load_n(&adapter->fault, __ATOMIC_ACQUIRE);
  if (fault)
    return (enum mpc_native_error)fault;
  for (size_t i = 0; i < sizeof native_fingerprints / sizeof *native_fingerprints; i++) {
    unsigned char bytes[32];
    uintptr_t address = adapter->io.image + native_fingerprints[i].rva;
    if (
      address < adapter->io.image ||
      adapter->io.read(adapter->io.userdata, address, bytes, sizeof bytes)) {
      __atomic_store_n(&adapter->fault, MPC_NATIVE_READ_FAILED, __ATOMIC_RELEASE);
      return MPC_NATIVE_READ_FAILED;
    }
    if (memcmp(bytes, native_fingerprints[i].bytes, sizeof bytes)) {
      __atomic_store_n(&adapter->fault, MPC_NATIVE_FINGERPRINT_MISMATCH, __ATOMIC_RELEASE);
      return MPC_NATIVE_FINGERPRINT_MISMATCH;
    }
  }
  return MPC_NATIVE_OK;
}

static int new_identity(char output[37]) {
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  unsigned char bytes[16];
  size_t used = 0;
  while (used < sizeof bytes) {
    ssize_t n = read(fd, bytes + used, sizeof bytes - used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      return -1;
    }
    used += (size_t)n;
  }
  close(fd);
  memcpy(output, "adp_", 4);
  for (size_t i = 0; i < sizeof bytes; i++)
    sprintf(output + 4 + 2 * i, "%02x", bytes[i]);
  return 0;
}

/* Ownership of io.userdata transfers only on success. */
enum mpc_native_error mpc_native_open_backend(
  const struct mpc_native_io *io,
  const struct mpc_native_hooks *hooks,
  struct mpc_native_adapter **output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  *output = NULL;
  if (
    !io || !io->identity || !io->read || !io->on_ui_thread || !io->setter || !io->filter ||
    io->image > UINT32_MAX)
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (hooks && (hooks->abi_version != MPC_NATIVE_ABI || hooks->struct_bytes != sizeof *hooks))
    return MPC_NATIVE_ABI_MISMATCH;
  char build[129] = {0}, model[256] = {0};
  enum mpc_native_error error = io->identity(io->userdata, build, model);
  if (error)
    return error;
  if (strcmp(build, NATIVE_PROFILE_BUILD))
    return MPC_NATIVE_BUILD_MISMATCH;
  if (strcmp(model, NATIVE_PROFILE_MODEL))
    return MPC_NATIVE_MODEL_MISMATCH;
  struct mpc_native_adapter *adapter = calloc(1, sizeof *adapter);
  if (!adapter)
    return MPC_NATIVE_NOT_READY;
  adapter->io = *io;
  if (hooks)
    adapter->hooks = *hooks;
  error = verify(adapter);
  if (!error && new_identity(adapter->instance_id))
    error = MPC_NATIVE_NOT_READY;
  if (error) {
    free(adapter);
    return error;
  }
  *output = adapter;
  return MPC_NATIVE_OK;
}

void mpc_native_close(struct mpc_native_adapter *adapter) {
  if (!adapter)
    return;
  if (adapter->io.destroy)
    adapter->io.destroy(adapter->io.userdata);
  free(adapter);
}

bool mpc_native_on_ui_thread(struct mpc_native_adapter *adapter) {
  return adapter->io.on_ui_thread(adapter->io.userdata);
}

enum mpc_native_error
mpc_native_post(struct mpc_native_adapter *adapter, struct mpc_native_function *function) {
  enum mpc_native_error error = verify(adapter);
  if (error)
    return error;
  return adapter->io.post &&
      adapter->io.post(adapter->io.userdata, adapter->io.image + NATIVE_CALL_ASYNC, function)
    ? MPC_NATIVE_OK
    : MPC_NATIVE_DISPATCH_FAILED;
}

enum mpc_native_error
mpc_native_get_status(struct mpc_native_adapter *adapter, struct mpc_native_status *output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  memset(output, 0, sizeof *output);
  enum mpc_native_error error = verify(adapter);
  if (error)
    return error;
  output->abi_version = MPC_NATIVE_ABI;
  output->code_verified = true;
  output->target_validator_present = adapter->hooks.validate_target != NULL;
  strcpy(output->instance_id, adapter->instance_id);
  strcpy(output->profile_id, NATIVE_PROFILE_ID);
  strcpy(output->build_id, NATIVE_PROFILE_BUILD);
  return MPC_NATIVE_OK;
}

static int get32(struct mpc_native_adapter *adapter, uintptr_t address, uint32_t *value) {
  return adapter->io.read(adapter->io.userdata, address, value, sizeof *value);
}

static int get_bool(struct mpc_native_adapter *adapter, uintptr_t address, bool *value) {
  unsigned char byte;
  if (adapter->io.read(adapter->io.userdata, address, &byte, 1) || byte > 1)
    return -1;
  *value = byte != 0;
  return 0;
}

static enum mpc_native_error target_check(
  struct mpc_native_adapter *adapter,
  const struct mpc_native_target *target,
  enum mpc_native_family *family) {
  if (
    !target || !target->project || !target->track || !target->program ||
    target->project > UINT32_MAX - 0x10000u || target->track > UINT32_MAX - 0x10000u ||
    target->program > UINT32_MAX - 0x10000u)
    return MPC_NATIVE_INVALID_ARGUMENT;
  enum mpc_native_error error = verify(adapter);
  if (error)
    return error;
  if (!adapter->io.on_ui_thread(adapter->io.userdata))
    return MPC_NATIVE_WRONG_THREAD;
  if (!adapter->hooks.validate_target)
    return MPC_NATIVE_NOT_READY;
  error = adapter->hooks.validate_target(adapter->hooks.userdata, target);
  if (error)
    return error;
  uint32_t value, pool, begin, end, capacity;
  if (get32(adapter, target->project, &value) || value != adapter->io.image + NATIVE_PROJECT_VTABLE)
    return MPC_NATIVE_STALE_TARGET;
  for (size_t i = 0; i < sizeof native_secondary / sizeof *native_secondary; i++)
    if (
      get32(adapter, (uintptr_t)target->project + native_secondary[i].offset, &value) ||
      value != adapter->io.image + native_secondary[i].vtable)
      return MPC_NATIVE_STALE_TARGET;
  if (
    get32(adapter, (uintptr_t)target->project + NATIVE_POOL_OFFSET, &pool) || !pool ||
    pool > UINT32_MAX - 0x10000u || get32(adapter, pool, &value) ||
    value != adapter->io.image + NATIVE_POOL_VTABLE ||
    get32(adapter, (uintptr_t)pool + NATIVE_VECTOR_BEGIN, &begin) ||
    get32(adapter, (uintptr_t)pool + NATIVE_VECTOR_END, &end) ||
    get32(adapter, (uintptr_t)pool + NATIVE_VECTOR_CAPACITY, &capacity) || !begin || end < begin ||
    capacity < end || (end - begin) % 4 || (capacity - begin) % 4 || capacity - begin > 128 * 4)
    return MPC_NATIVE_STALE_TARGET;
  size_t count = (end - begin) / 4;
  uint32_t tracks[128];
  if (!count || adapter->io.read(adapter->io.userdata, begin, tracks, count * 4))
    return MPC_NATIVE_STALE_TARGET;
  unsigned found = 0;
  for (size_t i = 0; i < count; i++) {
    found += tracks[i] == target->track;
    for (size_t j = 0; j < i; j++)
      if (tracks[j] == tracks[i])
        return MPC_NATIVE_STALE_TARGET;
  }
  if (
    found != 1 || get32(adapter, target->track, &value) ||
    value != adapter->io.image + NATIVE_TRACK_VTABLE ||
    get32(adapter, (uintptr_t)target->track + NATIVE_PROGRAM_OFFSET, &value) ||
    value != target->program || get32(adapter, target->program, &value))
    return MPC_NATIVE_STALE_TARGET;
  if (value == adapter->io.image + NATIVE_PLUGIN_VTABLE)
    *family = MPC_NATIVE_PLUGIN;
  else if (value == adapter->io.image + NATIVE_DRUM_VTABLE)
    *family = MPC_NATIVE_DRUM;
  else
    return MPC_NATIVE_UNSUPPORTED_TRACK_TYPE;
  return MPC_NATIVE_OK;
}

static enum mpc_native_error state_read(
  struct mpc_native_adapter *adapter,
  const struct mpc_native_target *target,
  struct mpc_native_track_state *output) {
  enum mpc_native_family family;
  enum mpc_native_error error = target_check(adapter, target, &family);
  if (error)
    return error;
  struct mpc_native_track_state state = {
    .family = family,
    .audio_known = MPC_NATIVE_AUDIO_SOLO | MPC_NATIVE_AUDIO_ARM};
  bool *requested[] = {&state.muted, &state.soloed, &state.record_armed};
  bool *observed[] = {NULL, &state.audio_soloed, &state.audio_record_armed};
  for (size_t i = 0; i < 3; i++) {
    const struct native_binding *binding = &native_bindings[i];
    uintptr_t owner = binding->owner_is_track ? target->track : target->program;
    if (
      get_bool(adapter, owner + binding->requested, requested[i]) ||
      (observed[i] && get_bool(adapter, owner + binding->observed, observed[i])))
      return MPC_NATIVE_READ_FAILED;
  }
  if (get_bool(adapter, (uintptr_t)target->program + NATIVE_SILENCED_OFFSET, &state.silenced))
    return MPC_NATIVE_READ_FAILED;
  *output = state;
  return MPC_NATIVE_OK;
}

enum mpc_native_error mpc_native_read_track(
  struct mpc_native_adapter *adapter,
  const struct mpc_native_target *target,
  struct mpc_native_track_state *output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  memset(output, 0, sizeof *output);
  return state_read(adapter, target, output);
}

static enum mpc_native_error set_flag(
  struct mpc_native_adapter *adapter,
  const struct mpc_native_target *target,
  unsigned property,
  bool desired,
  const bool *expected,
  struct mpc_native_write_result *output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  memset(output, 0, sizeof *output);
  struct mpc_native_track_state before;
  enum mpc_native_error error = state_read(adapter, target, &before);
  if (error)
    return error;
  bool current = property == 0 ? before.muted : property == 1 ? before.soloed : before.record_armed;
  output->observed = before;
  output->changed_known = true;
  if (expected && current != *expected)
    return MPC_NATIVE_PRECONDITION_FAILED;
  if (current == desired)
    return MPC_NATIVE_OK;
  const struct native_binding *binding = &native_bindings[property];
  uintptr_t owner = binding->owner_is_track ? target->track : target->program;
  bool filtered = desired;
  if (property == 2) {
    uint32_t manager, invoker;
    if (
      get32(adapter, owner + binding->filter_manager, &manager) || !manager ||
      get32(adapter, owner + binding->filter_invoker, &invoker) ||
      invoker != adapter->io.image + binding->filter_rva)
      return MPC_NATIVE_FILTER_MISSING;
    output->native_invoked = true;
    output->changed_known = false;
    error =
      adapter->io.filter(adapter->io.userdata, owner + binding->filter_storage, invoker, &filtered);
    if (error)
      return error;
  }
  output->native_invoked = true;
  output->changed_known = false;
  error = adapter->io.setter(
    adapter->io.userdata,
    adapter->io.image + binding->setter,
    owner + binding->property,
    filtered);
  if (error)
    return error;
  error = state_read(adapter, target, &output->observed);
  if (error)
    return error;
  bool after = property == 0 ? output->observed.muted
    : property == 1          ? output->observed.soloed
                             : output->observed.record_armed;
  output->changed_known = true;
  output->changed = after != current;
  return after == desired ? MPC_NATIVE_OK : MPC_NATIVE_VALUE_REJECTED;
}

enum mpc_native_error mpc_native_set_mute(
  struct mpc_native_adapter *adapter,
  const struct mpc_native_target *target,
  bool value,
  const bool *expected,
  struct mpc_native_write_result *output) {
  return set_flag(adapter, target, 0, value, expected, output);
}

enum mpc_native_error mpc_native_set_solo(
  struct mpc_native_adapter *adapter,
  const struct mpc_native_target *target,
  bool value,
  const bool *expected,
  struct mpc_native_write_result *output) {
  return set_flag(adapter, target, 1, value, expected, output);
}

enum mpc_native_error mpc_native_set_record_armed(
  struct mpc_native_adapter *adapter,
  const struct mpc_native_target *target,
  bool value,
  const bool *expected,
  struct mpc_native_write_result *output) {
  return set_flag(adapter, target, 2, value, expected, output);
}

const char *mpc_native_error_name(enum mpc_native_error error) {
  static const char *names[] = {
    "OK",
    "INVALID_ARGUMENT",
    "ABI_MISMATCH",
    "BUILD_MISMATCH",
    "MODEL_MISMATCH",
    "FINGERPRINT_MISMATCH",
    "READ_FAILED",
    "NOT_READY",
    "WRONG_THREAD",
    "STALE_TARGET",
    "UNSUPPORTED_TRACK_TYPE",
    "PRECONDITION_FAILED",
    "FILTER_MISSING",
    "VALUE_REJECTED",
    "QUEUE_FULL",
    "CANCELLED",
    "OUTCOME_UNKNOWN",
    "TIMED_OUT",
    "SHUTTING_DOWN",
    "DISPATCH_FAILED"};
  return (unsigned)error < sizeof names / sizeof *names ? names[error] : "INVALID_ERROR";
}
