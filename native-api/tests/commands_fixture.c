#include "policy_fixture_backend.h"
#include "command_private.h"

#include <stdatomic.h>
#include <unistd.h>

enum { PROGRAM2 = OBJECTS + 0x16000 };
struct commands_fixture {
  struct identity_fixture identity;
  struct mpc_executor *executor;
  struct mpc_commands *commands;
  struct mpc_identity_snapshot ids;
  enum mpc_native_error context_error;
  bool single_solo, single_arm, block, entered, resume, live_policy;
  unsigned change_at_qualification;
  uint32_t change_after_set, change_after_filter;
  bool popup_after_set, broken_owner_after_set;
  uint32_t family_after_filter, family_after_set;
  uint32_t program_after_set;
  uint32_t playback_change_address, playback_change_value;
  unsigned playback_change_stage;
  bool sequence_values[2][2], switch_sequence_after_set, scope_after_set, reset_after_set;
  unsigned switch_sequence_at_qualification;
  unsigned qualifications;
  atomic_uint_fast64_t time_offset;
};
static const char service[] = "run_0123456789abcdef0123456789abcdef";
static void choose_sequence(struct commands_fixture *f, unsigned index) {
  struct fixture *n = &f->identity.native;
  put32(n, OBJECTS + NATIVE_SELECTED_SEQUENCE, index ? SEQUENCE2 : SEQUENCE);
  put32(n, OBJECTS + NATIVE_SELECTED_SEQUENCE_MIRROR, index ? SEQUENCE2 : SEQUENCE);
  for (unsigned i = 0; i < 2; i++)
    put_bool(
      n,
      (i ? PROGRAM2 : PROGRAM) + native_bindings[0].requested,
      f->sequence_values[index][i]);
}
static uint64_t now(void *data) {
  struct commands_fixture *f = data;
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000 +
    atomic_load(&f->time_offset);
}
static enum mpc_native_error qualify(void *data, enum mpc_command_operation operation) {
  struct commands_fixture *f = data;
  assert(ui && (unsigned)operation <= MPC_COMMAND_RECORD_ARMED);
  f->qualifications++;
  if (f->playback_change_stage == 1 && f->qualifications == 2)
    put32(&f->identity.native, f->playback_change_address, f->playback_change_value);
  if (f->switch_sequence_at_qualification == f->qualifications)
    choose_sequence(f, 1);
  if (f->change_at_qualification == f->qualifications)
    put32(
      &f->identity.native,
      POLICY_MODE + native_command_context.modeValue,
      native_command_context.trackMuteMode);
  if (f->live_policy)
    return mpc_executor_qualify_command(f->executor, operation);
  return f->context_error;
}
static uint32_t get32(struct fixture *native, uint32_t address) {
  uint32_t value;
  assert(!read_memory(native, address, &value, 4));
  return value;
}
static void arm(struct fixture *native, uint32_t track, bool value) {
  put_bool(native, track + native_bindings[2].requested, value);
  if (native->propagate_audio)
    put_bool(native, track + native_bindings[2].observed, value);
}
static void choose(struct commands_fixture *f, uint32_t track) {
  struct fixture *native = &f->identity.native;
  put32(native, OBJECTS + NATIVE_SELECTED_TRACK, track);
  put32(native, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, track);
  put32(native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, track == TRACK ? 0 : 1);
  if (f->single_arm) {
    arm(native, track, true);
    arm(native, track == TRACK ? TRACK2 : TRACK, false);
  }
}
static enum mpc_native_error
command_setter(void *data, uintptr_t function, uintptr_t property, bool value) {
  struct commands_fixture *f = data;
  struct fixture *native = &f->identity.native;
  assert(ui);
  pthread_mutex_lock(&f->identity.mutex);
  f->entered = true;
  pthread_cond_broadcast(&f->identity.changed);
  struct timespec until = deadline();
  while (f->block && !f->resume)
    assert(pthread_cond_timedwait(&f->identity.changed, &f->identity.mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&f->identity.mutex);
  native->setters++;
  for (unsigned i = 0; i < 3; i++) {
    const struct native_binding *binding = &native_bindings[i];
    for (unsigned index = 0; index < 2; index++) {
      uint32_t track = index ? TRACK2 : TRACK;
      uint32_t program = index ? PROGRAM2 : PROGRAM;
      uint32_t owner = binding->owner_is_track ? track : program;
      if (property != owner + binding->property || function != IMAGE + binding->setter)
        continue;
      if (i == MPC_COMMAND_SOLO && f->single_solo && value) {
        uint32_t peer = index ? PROGRAM : PROGRAM2;
        put_bool(native, peer + binding->requested, false);
        if (native->propagate_audio)
          put_bool(native, peer + binding->observed, false);
        choose(f, track);
      }
      if (i == MPC_COMMAND_RECORD_ARMED && f->single_arm && value)
        choose(f, track);
      put_bool(native, owner + binding->requested, value);
      if (native->propagate_audio)
        put_bool(native, owner + binding->observed, value);
      if (i == MPC_COMMAND_MUTE) {
        unsigned sequence = get32(native, OBJECTS + NATIVE_SELECTED_SEQUENCE) == SEQUENCE2;
        f->sequence_values[sequence][index] = value;
        if (f->switch_sequence_after_set)
          choose_sequence(f, 1);
        if (f->scope_after_set)
          put_bool(native, POLICY_MUTE + native_command_context.perSequenceMute, false);
        if (f->reset_after_set)
          for (unsigned w = 0; w < 128; w++)
            if (
              f->identity.sequence_watches[w] &&
              f->identity.sequence_watches[w]->sequence == SEQUENCE)
              f->identity.sequence_watches[w]->vtable[0](f->identity.sequence_watches[w], SEQUENCE);
      }
      if (f->popup_after_set)
        put32(native, POLICY_MODE + native_command_context.popupValue, 0x3081);
      if (f->broken_owner_after_set)
        put32(native, POLICY_MODE, 0);
      if (f->change_after_set)
        put32(native, POLICY_MODE + native_command_context.modeValue, f->change_after_set);
      if (native->invalidate_after_set)
        select_project(&f->identity, PROJECT2);
      if (f->family_after_set)
        put32(native, PROGRAM, IMAGE + f->family_after_set);
      if (f->program_after_set)
        put32(native, TRACK + NATIVE_PROGRAM_OFFSET, f->program_after_set);
      if (f->playback_change_stage == 3)
        put32(native, f->playback_change_address, f->playback_change_value);
      return MPC_NATIVE_OK;
    }
  }
  assert(!"unqualified native setter receiver");
  return MPC_NATIVE_INVALID_ARGUMENT;
}
static enum mpc_native_error
command_filter(void *data, uintptr_t storage, uintptr_t function, bool *value) {
  struct commands_fixture *f = data;
  assert(ui && function == IMAGE + native_bindings[2].filter_rva);
  assert(
    storage == TRACK + native_bindings[2].filter_storage ||
    storage == TRACK2 + native_bindings[2].filter_storage);
  if (f->change_after_filter)
    put32(
      &f->identity.native,
      POLICY_MODE + native_command_context.modeValue,
      f->change_after_filter);
  f->identity.native.filters++;
  if (f->playback_change_stage == 2)
    put32(&f->identity.native, f->playback_change_address, f->playback_change_value);
  if (f->family_after_filter)
    put32(&f->identity.native, PROGRAM, IMAGE + f->family_after_filter);
  *value = *value && f->identity.native.filter_allows;
  return MPC_NATIVE_OK;
}
static void setup(struct commands_fixture *f, unsigned pending, unsigned receipts, bool policy) {
  memset(f, 0, sizeof *f);
  state_init(&f->identity);
  policy_init(&f->identity);
  atomic_init(&f->time_offset, 0);
  struct fixture *native = &f->identity.native;
  put32(native, TRACK2 + NATIVE_PROGRAM_OFFSET, PROGRAM2);
  put32(native, PROGRAM2, IMAGE + NATIVE_DRUM_VTABLE);
  put32(native, TRACK2 + native_bindings[2].filter_manager, IMAGE + 4);
  put32(native, TRACK2 + native_bindings[2].filter_invoker, IMAGE + native_bindings[2].filter_rva);
  put_bool(native, PROGRAM + native_bindings[1].requested, false);
  put_bool(native, PROGRAM + NATIVE_SILENCED_OFFSET, false);
  put_bool(native, TRACK + native_bindings[2].requested, false);
  struct mpc_native_io backend = io(native);
  backend.read = identity_read;
  backend.on_ui_thread = on_identity_ui;
  backend.post = enqueue;
  backend.watch = watch;
  backend.unwatch = unwatch;
  backend.sequence_watch = sequence_watch;
  backend.sequence_unwatch = sequence_unwatch;
  backend.project_watch = project_watch;
  backend.project_unwatch = project_unwatch;
  backend.setter = command_setter;
  backend.filter = command_filter;
  assert(mpc_executor_open_authoritative_backend(&backend, 8, &f->executor) == MPC_NATIVE_OK);
  pump(&f->identity);
  assert(mpc_executor_wait_ready(f->executor, 1000) == MPC_NATIVE_OK);
  f->ids = snapshot(&f->identity, f->executor);
  struct mpc_commands_config config = {
    .abi_version = MPC_COMMANDS_ABI,
    .struct_bytes = sizeof config,
    .executor = f->executor,
    .pending_capacity = pending,
    .receipt_capacity = receipts,
    .retention_ms = MPC_COMMAND_MIN_RETENTION_MS,
    .policy = {f, policy ? qualify : NULL}};
  memcpy(config.instance_id, service, 37);
  assert(mpc_commands_open_clock(&config, now, f, &f->commands) == MPC_NATIVE_OK);
}
static struct mpc_command_request request(
  struct commands_fixture *f,
  const char *id,
  enum mpc_command_operation operation,
  unsigned index,
  bool value) {
  struct mpc_command_request out = {
    .operation = operation,
    .value = value,
    .target = {.project = f->ids.project, .track = f->ids.tracks[index].id}};
  strcpy(out.request_id, id);
  memcpy(out.target.instance_id, service, 37);
  return out;
}
static void tiny_pause(void) {
  struct timespec pause = {0, 1000000};
  nanosleep(&pause, NULL);
}
static bool queued(struct commands_fixture *f) {
  pthread_mutex_lock(&f->identity.mutex);
  bool pending = f->identity.queued;
  pthread_mutex_unlock(&f->identity.mutex);
  return pending;
}
static struct mpc_command_receipt drive(struct commands_fixture *f, const char *id) {
  struct mpc_command_receipt receipt;
  for (unsigned i = 0; i < 5000; i++) {
    assert(mpc_commands_get(f->commands, 7, service, id, 0, &receipt) == MPC_NATIVE_OK);
    if (receipt.status != MPC_COMMAND_ACCEPTED)
      return receipt;
    if (queued(f))
      pump(&f->identity);
    else
      tiny_pause();
  }
  assert(!"handler did not make progress");
  return receipt;
}
static struct mpc_command_receipt
send(struct commands_fixture *f, struct mpc_command_request input) {
  struct mpc_command_receipt receipt;
  assert(mpc_commands_submit(f->commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
  assert(receipt.status == MPC_COMMAND_ACCEPTED);
  return drive(f, input.request_id);
}
static void cleanup(struct commands_fixture *f) {
  enum mpc_native_error error;
  do {
    error = mpc_commands_shutdown(f->commands, 1);
    if (queued(f))
      pump(&f->identity);
  } while (error == MPC_NATIVE_TIMED_OUT);
  assert(error == MPC_NATIVE_OK);
  assert(mpc_commands_close(f->commands) == MPC_NATIVE_OK);
  close_executor(&f->identity, f->executor);
  assert(!pthread_cond_destroy(&f->identity.changed));
  assert(!pthread_mutex_destroy(&f->identity.mutex));
}
static void emit(const struct mpc_command_receipt *receipt) {
  char *json = mpc_commands_receipt_json(receipt);
  assert(json);
  fputs(json, stdout);
  free(json);
}
static void idempotence_and_preconditions(void) {
  struct commands_fixture f;
  setup(&f, 8, 64, true);
  struct mpc_command_receipt receipt;
  struct mpc_command_request invalid = request(&f, "revision", MPC_COMMAND_MUTE, 0, true);
  invalid.has_expected_revision = true;
  assert(
    mpc_commands_submit(f.commands, 7, &invalid, 0, &receipt) ==
    MPC_NATIVE_UNSUPPORTED_PRECONDITION);
  for (unsigned property = 0; property < 3; property++) {
    for (unsigned index = 0; index < 2; index++) {
      char id[80];
      snprintf(id, sizeof id, "set_%u_%u", property, index);
      struct mpc_command_request input = request(&f, id, property, index, true);
      input.has_expected = true;
      unsigned before = f.identity.native.setters;
      receipt = send(&f, input);
      assert(receipt.status == MPC_COMMAND_APPLIED && receipt.changed && receipt.native_invoked);
      assert(f.identity.native.setters == before + 1);
      emit(&receipt);
      /* A retry must return this receipt even after a physical edit away. */
      uint32_t owner = property == 2 ? (index ? TRACK2 : TRACK) : (index ? PROGRAM2 : PROGRAM);
      put_bool(&f.identity.native, owner + native_bindings[property].requested, false);
      assert(mpc_commands_submit(f.commands, 7, &input, 25, &receipt) == MPC_NATIVE_OK);
      assert(receipt.status == MPC_COMMAND_APPLIED && f.identity.native.setters == before + 1);
      put_bool(&f.identity.native, owner + native_bindings[property].requested, true);
      input.value = false;
      assert(
        mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_REQUEST_ID_CONFLICT);
      input.value = true;
      input.has_expected = false;
      snprintf(input.request_id, sizeof input.request_id, "noop_%u_%u", property, index);
      receipt = send(&f, input);
      assert(receipt.status == MPC_COMMAND_APPLIED && !receipt.changed && !receipt.native_invoked);
      assert(f.identity.native.setters == before + 1);
      emit(&receipt);
      input.has_expected = true;
      input.expected = false;
      snprintf(input.request_id, sizeof input.request_id, "conflict_%u_%u", property, index);
      receipt = send(&f, input);
      assert(
        receipt.status == MPC_COMMAND_REJECTED && receipt.error == MPC_NATIVE_PRECONDITION_FAILED);
      assert(!receipt.native_invoked && f.identity.native.setters == before + 1);
      emit(&receipt);
    }
  }
  assert(f.identity.native.filters == 2);
  assert(
    mpc_commands_get(f.commands, 8, service, "set_0_0", 0, &receipt) ==
    MPC_NATIVE_OUTCOME_UNAVAILABLE);
  cleanup(&f);
}
static void races_gates_and_constraints(void) {
  struct commands_fixture f;
  setup(&f, 8, 32, true);
  struct mpc_command_receipt receipt;
  for (unsigned property = 0; property < 3; property++) {
    char id[80];
    snprintf(id, sizeof id, "manual_%u", property);
    struct mpc_command_request input = request(&f, id, property, 0, true);
    input.has_expected = true;
    assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
    uint32_t owner = property == 2 ? TRACK : PROGRAM;
    put_bool(&f.identity.native, owner + native_bindings[property].requested, true);
    receipt = drive(&f, id);
    assert(
      receipt.status == MPC_COMMAND_REJECTED && receipt.error == MPC_NATIVE_PRECONDITION_FAILED);
    emit(&receipt);
  }
  assert(!f.identity.native.setters);
  f.context_error = MPC_NATIVE_NOT_READY;
  receipt = send(&f, request(&f, "mode_gate", MPC_COMMAND_MUTE, 0, true));
  assert(
    receipt.status == MPC_COMMAND_REJECTED && receipt.error == MPC_NATIVE_NOT_READY &&
    !receipt.native_invoked);
  f.context_error = MPC_NATIVE_OK;
  put32(&f.identity.native, PROGRAM2, IMAGE + 4);
  put32(&f.identity.native, POOL + NATIVE_MIDI_END, VECTOR + 4);
  put32(&f.identity.native, POOL + NATIVE_AUDIO_BEGIN, VECTOR + 4);
  put32(&f.identity.native, POOL + NATIVE_AUDIO_END, VECTOR + 8);
  put32(&f.identity.native, POOL + NATIVE_AUDIO_CAPACITY, VECTOR + 8);
  receipt = send(&f, request(&f, "family_gate", MPC_COMMAND_SOLO, 1, true));
  assert(receipt.error == MPC_NATIVE_UNSUPPORTED_TRACK_TYPE && !receipt.native_invoked);
  put32(&f.identity.native, PROGRAM2, IMAGE + NATIVE_DRUM_VTABLE);
  put32(&f.identity.native, POOL + NATIVE_MIDI_END, VECTOR + 8);
  put32(&f.identity.native, POOL + NATIVE_AUDIO_END, VECTOR + 4);
  f.identity.native.filter_allows = false;
  receipt = send(&f, request(&f, "filter_reject", MPC_COMMAND_RECORD_ARMED, 1, true));
  assert(
    receipt.status == MPC_COMMAND_REJECTED && receipt.error == MPC_NATIVE_VALUE_REJECTED &&
    receipt.native_invoked);
  emit(&receipt);
  struct mpc_command_request input = request(&f, "stale_project", MPC_COMMAND_SOLO, 0, true);
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
  select_project(&f.identity, PROJECT2);
  receipt = drive(&f, input.request_id);
  assert(receipt.error == MPC_NATIVE_STALE_PROJECT && !receipt.native_invoked);
  emit(&receipt);
  cleanup(&f);
  setup(&f, 2, 8, false);
  receipt = send(&f, request(&f, "missing_policy", MPC_COMMAND_MUTE, 0, true));
  assert(receipt.error == MPC_NATIVE_NOT_READY && !receipt.native_invoked);
  cleanup(&f);
}
static void effects_and_audio_verification(void) {
  struct commands_fixture f;
  setup(&f, 8, 32, true);
  f.single_solo = f.single_arm = true;
  struct mpc_command_receipt receipt =
    send(&f, request(&f, "solo_first", MPC_COMMAND_SOLO, 0, true));
  assert(receipt.status == MPC_COMMAND_APPLIED);
  receipt = send(&f, request(&f, "solo_second", MPC_COMMAND_SOLO, 1, true));
  assert(
    receipt.status == MPC_COMMAND_APPLIED &&
    receipt.before.selected_track != receipt.after.selected_track);
  assert(!receipt.after.tracks[0].soloed && receipt.after.tracks[1].soloed);
  assert(!receipt.after.tracks[0].record_armed && receipt.after.tracks[1].record_armed);
  emit(&receipt);
  receipt = send(&f, request(&f, "single_arm", MPC_COMMAND_RECORD_ARMED, 0, true));
  assert(
    receipt.status == MPC_COMMAND_APPLIED && receipt.after.tracks[0].record_armed &&
    !receipt.after.tracks[1].record_armed);
  emit(&receipt);
  f.single_arm = f.single_solo = false;
  receipt = send(&f, request(&f, "multi_arm", MPC_COMMAND_RECORD_ARMED, 1, true));
  assert(
    receipt.status == MPC_COMMAND_APPLIED && receipt.after.tracks[0].record_armed &&
    receipt.after.tracks[1].record_armed);
  emit(&receipt);
  f.identity.native.propagate_audio = false;
  struct mpc_command_request input = request(&f, "lagging", MPC_COMMAND_SOLO, 0, false);
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
  for (unsigned i = 0; i < 5000; i++) {
    if (queued(&f))
      pump(&f.identity);
    else
      tiny_pause();
    assert(
      mpc_commands_get(f.commands, 7, service, input.request_id, 0, &receipt) == MPC_NATIVE_OK);
    if (receipt.after_known)
      break;
  }
  /* Force an audio mismatch even for the target's no-op model state. */
  put_bool(&f.identity.native, PROGRAM + native_bindings[1].observed, true);
  struct mpc_command_request wait = request(&f, "wait_audio", MPC_COMMAND_SOLO, 0, false);
  assert(mpc_commands_submit(f.commands, 7, &wait, 0, &receipt) == MPC_NATIVE_OK);
  for (unsigned i = 0; i < 5000; i++) {
    if (queued(&f))
      pump(&f.identity);
    else
      tiny_pause();
    assert(mpc_commands_get(f.commands, 7, service, wait.request_id, 0, &receipt) == MPC_NATIVE_OK);
    if (receipt.after_known)
      break;
  }
  assert(receipt.status == MPC_COMMAND_ACCEPTED);
  put_bool(&f.identity.native, PROGRAM + native_bindings[1].observed, false);
  receipt = drive(&f, wait.request_id);
  assert(receipt.status == MPC_COMMAND_APPLIED && !receipt.native_invoked);
  emit(&receipt);
  put_bool(&f.identity.native, PROGRAM + native_bindings[1].observed, true);
  input = request(&f, "timeout_audio", MPC_COMMAND_SOLO, 0, false);
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
  for (unsigned i = 0; i < 5000; i++) {
    if (queued(&f))
      pump(&f.identity);
    else
      tiny_pause();
    assert(
      mpc_commands_get(f.commands, 7, service, input.request_id, 0, &receipt) == MPC_NATIVE_OK);
    if (receipt.after_known)
      break;
  }
  atomic_fetch_add(&f.time_offset, MPC_COMMAND_VERIFICATION_MS + 1);
  receipt = drive(&f, input.request_id);
  assert(receipt.status == MPC_COMMAND_UNKNOWN && receipt.error == MPC_NATIVE_VERIFICATION_TIMEOUT);
  emit(&receipt);
  cleanup(&f);
}
static void capacity_expiry_and_shutdown(void) {
  struct commands_fixture f;
  setup(&f, 1, 2, true);
  f.live_policy = true;
  put_bool(&f.identity.native, POLICY_TIMELINE + native_command_context.timelinePlaying, true);
  struct mpc_command_receipt receipt;
  struct mpc_command_request first = request(&f, "first", MPC_COMMAND_MUTE, 0, true);
  assert(mpc_commands_submit(f.commands, 7, &first, 0, &receipt) == MPC_NATIVE_OK);
  emit(&receipt);
  struct mpc_command_request second = request(&f, "second", MPC_COMMAND_MUTE, 0, false);
  assert(mpc_commands_submit(f.commands, 7, &second, 0, &receipt) == MPC_NATIVE_QUEUE_FULL);
  atomic_fetch_add(&f.time_offset, MPC_COMMAND_DEFAULT_START_MS + 1);
  for (unsigned i = 0; i < 5000; i++) {
    assert(
      mpc_commands_get(f.commands, 7, service, first.request_id, 0, &receipt) == MPC_NATIVE_OK);
    if (receipt.status == MPC_COMMAND_CANCELLED)
      break;
    tiny_pause();
  }
  assert(!f.identity.native.setters);
  if (queued(&f))
    pump(&f.identity);
  assert(receipt.status == MPC_COMMAND_CANCELLED && !receipt.native_invoked);
  emit(&receipt);
  receipt = send(&f, second);
  assert(receipt.status == MPC_COMMAND_APPLIED);
  struct mpc_command_request third = request(&f, "third", MPC_COMMAND_MUTE, 0, false);
  assert(mpc_commands_submit(f.commands, 7, &third, 0, &receipt) == MPC_NATIVE_RETENTION_CAPACITY);
  atomic_fetch_add(&f.time_offset, MPC_COMMAND_MIN_RETENTION_MS);
  assert(
    mpc_commands_get(f.commands, 7, service, first.request_id, 0, &receipt) ==
    MPC_NATIVE_OUTCOME_UNAVAILABLE);
  receipt = send(&f, third);
  assert(receipt.status == MPC_COMMAND_APPLIED);
  struct mpc_command_request last = request(&f, "last", MPC_COMMAND_SOLO, 0, true);
  assert(mpc_commands_submit(f.commands, 7, &last, 0, &receipt) == MPC_NATIVE_OK);
  assert(mpc_commands_close(f.commands) == MPC_NATIVE_NOT_READY);
  assert(mpc_commands_shutdown(f.commands, 1000) == MPC_NATIVE_OK);
  assert(mpc_commands_get(f.commands, 7, service, last.request_id, 0, &receipt) == MPC_NATIVE_OK);
  assert(receipt.status == MPC_COMMAND_CANCELLED);
  cleanup(&f);
}
static void *pump_thread(void *data) {
  struct commands_fixture *f = data;
  pump(&f->identity);
  return NULL;
}
static void running_unknown_and_drain(void) {
  struct commands_fixture f;
  setup(&f, 2, 8, true);
  f.live_policy = true;
  put_bool(&f.identity.native, POLICY_TIMELINE + native_command_context.timelinePlaying, true);
  f.block = true;
  struct mpc_command_request input = request(&f, "running", MPC_COMMAND_MUTE, 0, true);
  struct mpc_command_receipt receipt;
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, pump_thread, &f));
  struct timespec until = deadline();
  pthread_mutex_lock(&f.identity.mutex);
  while (!f.entered)
    assert(pthread_cond_timedwait(&f.identity.changed, &f.identity.mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&f.identity.mutex);
  for (unsigned i = 0; i < 5000; i++) {
    assert(
      mpc_commands_get(f.commands, 7, service, input.request_id, 0, &receipt) == MPC_NATIVE_OK);
    if (receipt.execution == MPC_COMMAND_STARTED)
      break;
    tiny_pause();
  }
  assert(receipt.execution == MPC_COMMAND_STARTED && !receipt.native_known);
  atomic_fetch_add(&f.time_offset, MPC_COMMAND_VERIFICATION_MS + 1);
  receipt = drive(&f, input.request_id);
  assert(
    receipt.status == MPC_COMMAND_UNKNOWN && receipt.execution == MPC_COMMAND_STARTED &&
    !receipt.native_known);
  emit(&receipt);
  assert(mpc_commands_submit(f.commands, 7, &input, 1, &receipt) == MPC_NATIVE_OK);
  assert(receipt.status == MPC_COMMAND_UNKNOWN);
  struct mpc_command_request following = request(&f, "following", MPC_COMMAND_SOLO, 0, true);
  assert(mpc_commands_submit(f.commands, 7, &following, 0, &receipt) == MPC_NATIVE_OK);
  assert(mpc_commands_shutdown(f.commands, 1) == MPC_NATIVE_TIMED_OUT);
  assert(mpc_commands_close(f.commands) == MPC_NATIVE_NOT_READY);
  assert(
    mpc_commands_get(f.commands, 7, service, following.request_id, 0, &receipt) == MPC_NATIVE_OK);
  assert(receipt.status == MPC_COMMAND_CANCELLED);
  pthread_mutex_lock(&f.identity.mutex);
  f.resume = true;
  pthread_cond_broadcast(&f.identity.changed);
  pthread_mutex_unlock(&f.identity.mutex);
  assert(!pthread_join(thread, NULL));
  for (unsigned i = 0; i < 5000; i++) {
    assert(
      mpc_commands_get(f.commands, 7, service, input.request_id, 0, &receipt) == MPC_NATIVE_OK);
    if (receipt.status == MPC_COMMAND_APPLIED)
      break;
    tiny_pause();
  }
  assert(receipt.status == MPC_COMMAND_APPLIED && receipt.native_invoked);
  emit(&receipt);
  assert(mpc_commands_shutdown(f.commands, 1000) == MPC_NATIVE_OK);
  assert(
    mpc_commands_get(f.commands, 7, service, following.request_id, 0, &receipt) == MPC_NATIVE_OK);
  assert(receipt.status == MPC_COMMAND_CANCELLED && !receipt.native_invoked);
  assert(f.identity.native.setters == 1);
  cleanup(&f);

  setup(&f, 2, 8, true);
  f.identity.native.invalidate_after_set = true;
  receipt = send(&f, request(&f, "lost_after_set", MPC_COMMAND_MUTE, 0, true));
  assert(receipt.status == MPC_COMMAND_UNKNOWN && receipt.native_invoked);
  emit(&receipt);
  cleanup(&f);
}

struct submitting_client {
  struct commands_fixture *fixture;
  struct mpc_command_request request;
  enum mpc_native_error error;
};
static void *submit_client(void *data) {
  struct submitting_client *client = data;
  struct mpc_command_receipt receipt;
  client->error = mpc_commands_submit(client->fixture->commands, 7, &client->request, 0, &receipt);
  return NULL;
}
static void concurrent_retries_and_stale_handles(void) {
  struct commands_fixture f;
  setup(&f, 8, 32, true);
  struct submitting_client clients[8];
  pthread_t threads[8];
  struct mpc_command_request input = request(&f, "concurrent", MPC_COMMAND_MUTE, 1, true);
  for (unsigned i = 0; i < 8; i++) {
    clients[i] = (struct submitting_client){&f, input, MPC_NATIVE_NOT_READY};
    assert(!pthread_create(&threads[i], NULL, submit_client, &clients[i]));
  }
  for (unsigned i = 0; i < 8; i++) {
    assert(!pthread_join(threads[i], NULL));
    assert(clients[i].error == MPC_NATIVE_OK);
  }
  struct mpc_command_receipt receipt = drive(&f, input.request_id);
  assert(receipt.status == MPC_COMMAND_APPLIED && f.identity.native.setters == 1);
  assert(receipt.after.selected_track == f.ids.tracks[0].id);
  emit(&receipt);
  input.start_within_ms = MPC_COMMAND_DEFAULT_START_MS;
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
  input.start_within_ms++;
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_REQUEST_ID_CONFLICT);
  input = request(&f, "reordered", MPC_COMMAND_MUTE, 0, true);
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
  put32(&f.identity.native, VECTOR, TRACK2);
  put32(&f.identity.native, VECTOR + 4, TRACK);
  put32(&f.identity.native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 1);
  receipt = drive(&f, input.request_id);
  assert(receipt.status == MPC_COMMAND_APPLIED);
  assert(receipt.after.tracks[1].id == f.ids.tracks[0].id && receipt.after.tracks[1].muted);
  emit(&receipt);
  input = request(&f, "deleted_recreated", MPC_COMMAND_SOLO, 0, true);
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
  destroy_object(&f.identity, TRACK);
  receipt = drive(&f, input.request_id);
  assert(receipt.status == MPC_COMMAND_REJECTED && receipt.error == MPC_NATIVE_TARGET_NOT_FOUND);
  assert(!receipt.native_invoked && f.identity.native.setters == 2);
  emit(&receipt);
  input = request(&f, "old_instance", MPC_COMMAND_SOLO, 0, true);
  input.target.instance_id[4] = 'f';
  assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_STALE_INSTANCE);
  cleanup(&f);
}

static void live_screens_and_transitions(void) {
  const uint32_t screens[] = {
    native_command_context.mainMode,
    native_command_context.mixerMode,
    native_command_context.trackMuteMode};
  for (unsigned screen = 0; screen < 3; screen++) {
    for (unsigned single = 0; single < 2; single++) {
      struct commands_fixture f;
      setup(&f, 8, 128, true);
      f.live_policy = true;
      f.single_arm = f.single_solo = single;
      put32(&f.identity.native, POLICY_MODE + native_command_context.modeValue, screens[screen]);
      put32(
        &f.identity.native,
        POLICY_RECORD_PROPERTIES + native_command_context.armModeValue,
        !single);
      put32(&f.identity.native, POLICY_SOLO_PROPERTY + native_command_context.soloValue, !single);
      for (unsigned index = 0; index < 2; index++) {
        for (unsigned selected = 0; selected < 2; selected++) {
          for (unsigned property = 0; property < 3; property++) {
            choose(&f, selected ? (index ? TRACK2 : TRACK) : (index ? TRACK : TRACK2));
            uint32_t owner =
              property == 2 ? (index ? TRACK2 : TRACK) : (index ? PROGRAM2 : PROGRAM);
            bool original =
              get32(&f.identity.native, owner + native_bindings[property].requested) & 1;
            char id[80];
            snprintf(id, sizeof id, "screen_%u_%u_%u_%u", screen, index, selected, property);
            struct mpc_command_request input = request(&f, id, property, index, !original);
            input.has_expected = true;
            input.expected = original;
            struct mpc_command_receipt receipt = send(&f, input);
            assert(receipt.status == MPC_COMMAND_APPLIED && receipt.native_invoked);
            assert(
              get32(&f.identity.native, POLICY_MODE + native_command_context.modeValue) ==
              screens[screen]);
            unsigned setters = f.identity.native.setters, filters = f.identity.native.filters;
            input.expected = !original;
            snprintf(
              input.request_id,
              sizeof input.request_id,
              "noop_%u_%u_%u",
              index,
              selected,
              property);
            receipt = send(&f, input);
            assert(receipt.status == MPC_COMMAND_APPLIED && !receipt.native_invoked);
            input.expected = original;
            snprintf(
              input.request_id,
              sizeof input.request_id,
              "conflict_%u_%u_%u",
              index,
              selected,
              property);
            receipt = send(&f, input);
            assert(receipt.error == MPC_NATIVE_PRECONDITION_FAILED && !receipt.native_invoked);
            assert(f.identity.native.setters == setters && f.identity.native.filters == filters);
          }
        }
      }
      cleanup(&f);
    }
  }
  for (unsigned stage = 0; stage < 7; stage++) {
    struct commands_fixture f;
    setup(&f, 8, 16, true);
    f.live_policy = true;
    struct mpc_command_request input =
      request(&f, "transition", stage == 3 ? MPC_COMMAND_RECORD_ARMED : MPC_COMMAND_SOLO, 0, true);
    struct mpc_command_receipt receipt;
    if (stage == 1)
      f.change_at_qualification = 2; /* After admission, before the setter. */
    if (stage == 2)
      f.change_after_set = native_command_context.mixerMode;
    if (stage == 3)
      f.change_after_filter = native_command_context.trackMuteMode;
    if (stage == 4)
      f.identity.native.propagate_audio = false;
    f.popup_after_set = stage == 5;
    f.broken_owner_after_set = stage == 6;
    assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
    while (!queued(&f))
      tiny_pause();
    if (!stage)
      put32(&f.identity.native, POLICY_MODE + native_command_context.modeValue, 0x20a0);
    pump(&f.identity);
    if (stage == 4)
      put32(
        &f.identity.native,
        POLICY_MODE + native_command_context.modeValue,
        native_command_context.trackMuteMode);
    receipt = drive(&f, input.request_id);
    assert(receipt.status == (stage < 2 ? MPC_COMMAND_REJECTED : MPC_COMMAND_UNKNOWN));
    assert(
      receipt.error ==
      (!stage || stage == 5 ? MPC_NATIVE_NOT_READY
         : stage == 6       ? MPC_NATIVE_READ_FAILED
                            : MPC_NATIVE_STATE_UNSTABLE));
    assert(receipt.native_invoked == (stage >= 2));
    assert(f.identity.native.setters == (stage == 2 || stage >= 4));
    struct mpc_command_receipt replay;
    assert(mpc_commands_submit(f.commands, 7, &input, 0, &replay) == MPC_NATIVE_OK);
    assert(replay.status == receipt.status && replay.native_invoked == receipt.native_invoked);
    cleanup(&f);
  }
}

static struct mpc_command_request sequence_request(
  struct commands_fixture *f, const char *id, unsigned index, bool value, unsigned sequence) {
  struct mpc_command_request out = request(f, id, MPC_COMMAND_MUTE, index, value);
  out.mute_scope = MPC_MUTE_SEQUENCE;
  memcpy(out.sequence.instance_id, service, 37);
  out.sequence.project = f->ids.project;
  out.sequence.sequence = f->ids.sequences[sequence].id;
  out.sequence.generation = f->ids.sequences[sequence].generation;
  return out;
}
static void sequence_scope_and_races(void) {
  {
    struct commands_fixture f;
    setup(&f, 8, 16, true);
    struct mpc_command_request input = sequence_request(&f, "invalid_sequence", 0, true, 0);
    struct mpc_command_receipt receipt;
    for (unsigned field = 0; field < 7; field++) {
      struct mpc_command_request invalid = input;
      if (!field)
        invalid.sequence.sequence = 0;
      if (field == 1)
        invalid.sequence.generation = 0;
      if (field == 2)
        invalid.sequence.project++;
      if (field == 3)
        invalid.sequence.instance_id[4] = 'f';
      if (field == 4)
        invalid.operation = MPC_COMMAND_SOLO;
      if (field == 5)
        invalid.mute_scope = MPC_MUTE_GLOBAL;
      if (field == 6)
        invalid.mute_scope = MPC_MUTE_UNKNOWN;
      assert(
        mpc_commands_submit(f.commands, 7, &invalid, 0, &receipt) == MPC_NATIVE_INVALID_ARGUMENT);
    }
    struct mpc_executor_request job_request = {
      .operation = MPC_EXECUTOR_COMMAND_MUTE,
      .mute_scope = MPC_MUTE_SEQUENCE};
    struct mpc_executor_job *job;
    assert(
      mpc_executor_submit(f.executor, &job_request, &job) == MPC_NATIVE_INVALID_ARGUMENT && !job);
    job_request.identity.sequence = input.sequence.sequence;
    job_request.identity.sequence_generation = input.sequence.generation;
    job_request.operation = MPC_EXECUTOR_COMMAND_SOLO;
    assert(
      mpc_executor_submit(f.executor, &job_request, &job) == MPC_NATIVE_INVALID_ARGUMENT && !job);
    assert(!f.identity.native.setters);
    cleanup(&f);
  }
  const uint32_t screens[] = {
    native_command_context.mainMode,
    native_command_context.mixerMode,
    native_command_context.trackMuteMode};
  for (unsigned screen = 0; screen < 3; screen++) {
    struct commands_fixture f;
    setup(&f, 8, 64, true);
    f.live_policy = true;
    put_bool(&f.identity.native, POLICY_TIMELINE + native_command_context.timelinePlaying, true);
    put32(&f.identity.native, POLICY_MODE + native_command_context.modeValue, screens[screen]);
    put_bool(&f.identity.native, POLICY_MUTE + native_command_context.perSequenceMute, true);
    for (unsigned target = 0; target < 2; target++) {
      for (unsigned selected = 0; selected < 2; selected++) {
        choose(&f, selected ? (target ? TRACK2 : TRACK) : (target ? TRACK : TRACK2));
        choose_sequence(&f, 0);
        char id[80];
        snprintf(id, sizeof id, "sequence_%u_%u_%u", screen, target, selected);
        struct mpc_command_request input = sequence_request(&f, id, target, true, 0);
        input.has_expected = true;
        struct mpc_command_receipt receipt = send(&f, input);
        assert(receipt.status == MPC_COMMAND_APPLIED && receipt.native_invoked);
        assert(receipt.after.mute_scope == MPC_MUTE_SEQUENCE);
        assert(receipt.after.selected_sequence == input.sequence.sequence);
        emit(&receipt);
        unsigned setters = f.identity.native.setters;
        choose_sequence(&f, 1);
        assert(!(
          get32(&f.identity.native, (target ? PROGRAM2 : PROGRAM) + native_bindings[0].requested) &
          1));
        /* Retained results survive selection changes, including disconnect/retry. */
        struct mpc_command_receipt replay;
        assert(mpc_commands_submit(f.commands, 7, &input, 0, &replay) == MPC_NATIVE_OK);
        assert(replay.status == MPC_COMMAND_APPLIED && f.identity.native.setters == setters);
        struct mpc_command_request conflict = input;
        conflict.sequence = sequence_request(&f, id, target, true, 1).sequence;
        assert(
          mpc_commands_submit(f.commands, 7, &conflict, 0, &replay) ==
          MPC_NATIVE_REQUEST_ID_CONFLICT);
        conflict = input;
        conflict.sequence.generation++;
        assert(
          mpc_commands_submit(f.commands, 7, &conflict, 0, &replay) ==
          MPC_NATIVE_REQUEST_ID_CONFLICT);
        conflict = input;
        conflict.mute_scope = MPC_MUTE_GLOBAL;
        memset(&conflict.sequence, 0, sizeof conflict.sequence);
        assert(
          mpc_commands_submit(f.commands, 7, &conflict, 0, &replay) ==
          MPC_NATIVE_REQUEST_ID_CONFLICT);
        choose_sequence(&f, 0);
        input.expected = true;
        snprintf(
          input.request_id,
          sizeof input.request_id,
          "sequence_noop_%u_%u",
          target,
          selected);
        receipt = send(&f, input);
        assert(
          receipt.status == MPC_COMMAND_APPLIED && !receipt.native_invoked && !receipt.changed);
        input.expected = false;
        snprintf(
          input.request_id,
          sizeof input.request_id,
          "sequence_expected_%u_%u",
          target,
          selected);
        receipt = send(&f, input);
        assert(receipt.error == MPC_NATIVE_PRECONDITION_FAILED && !receipt.native_invoked);
        input.value = false;
        input.expected = true;
        snprintf(
          input.request_id,
          sizeof input.request_id,
          "sequence_restore_%u_%u",
          target,
          selected);
        receipt = send(&f, input);
        assert(receipt.status == MPC_COMMAND_APPLIED);
        assert(!f.sequence_values[0][target] && !f.sequence_values[1][target]);
      }
    }
    cleanup(&f);
  }
  for (unsigned stage = 0; stage < 10; stage++) {
    struct commands_fixture f;
    setup(&f, 8, 16, true);
    f.live_policy = true;
    put_bool(&f.identity.native, POLICY_TIMELINE + native_command_context.timelinePlaying, true);
    put_bool(&f.identity.native, POLICY_MUTE + native_command_context.perSequenceMute, true);
    struct mpc_command_request input = sequence_request(&f, "sequence_race", 0, true, 0);
    struct mpc_command_receipt receipt;
    f.switch_sequence_after_set = stage == 5;
    f.scope_after_set = stage == 6;
    f.reset_after_set = stage == 7;
    f.switch_sequence_at_qualification = stage == 8 ? 2 : 0;
    assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
    while (!queued(&f))
      tiny_pause();
    if (!stage)
      choose_sequence(&f, 1);
    if (stage == 1)
      put_bool(&f.identity.native, POLICY_MUTE + native_command_context.perSequenceMute, false);
    if (stage == 2)
      destroy_object(&f.identity, SEQUENCE);
    if (stage == 3)
      select_project(&f.identity, PROJECT2);
    if (stage == 4) {
      for (unsigned w = 0; w < 128; w++)
        if (f.identity.sequence_watches[w] && f.identity.sequence_watches[w]->sequence == SEQUENCE)
          f.identity.sequence_watches[w]->vtable[0](f.identity.sequence_watches[w], SEQUENCE);
    }
    if (stage == 9) {
      f.identity.project_watch->vtable[0](f.identity.project_watch, OBJECTS);
    }
    receipt = drive(&f, input.request_id);
    bool invoked = stage >= 5 && stage <= 7;
    assert(receipt.status == (invoked ? MPC_COMMAND_UNKNOWN : MPC_COMMAND_REJECTED));
    assert(receipt.native_invoked == invoked && f.identity.native.setters == invoked);
    assert(receipt.execution == (invoked ? MPC_COMMAND_COMPLETED : MPC_COMMAND_NOT_STARTED));
    assert(!f.sequence_values[1][0]);
    emit(&receipt);
    struct mpc_command_receipt replay;
    assert(mpc_commands_submit(f.commands, 7, &input, 0, &replay) == MPC_NATIVE_OK);
    assert(replay.status == receipt.status && replay.error == receipt.error);
    cleanup(&f);
  }
}

static void family_pair(struct commands_fixture *f, uint32_t left, uint32_t right) {
  struct fixture *n = &f->identity.native;
  put32(n, PROGRAM, IMAGE + left);
  put32(n, PROGRAM2, IMAGE + right);
  unsigned midi = 0, audio = 0;
  for (unsigned i = 0; i < 2; i++) {
    bool is_audio = (i ? right : left) == NATIVE_AUDIO_VTABLE;
    put32(
      n,
      OBJECTS + (is_audio ? 0x18100 : 0x18000) + 4 * (is_audio ? audio++ : midi++),
      i ? TRACK2 : TRACK);
  }
  put32(n, POOL + NATIVE_MIDI_BEGIN, OBJECTS + 0x18000);
  put32(n, POOL + NATIVE_MIDI_END, OBJECTS + 0x18000 + 4 * midi);
  put32(n, POOL + NATIVE_MIDI_CAPACITY, OBJECTS + 0x18000 + 4 * midi);
  put32(n, POOL + NATIVE_AUDIO_BEGIN, OBJECTS + 0x18100);
  put32(n, POOL + NATIVE_AUDIO_END, OBJECTS + 0x18100 + 4 * audio);
  put32(n, POOL + NATIVE_AUDIO_CAPACITY, OBJECTS + 0x18100 + 4 * audio);
}

static void qualified_family_matrix(void) {
  const uint32_t families[] = {
    NATIVE_PLUGIN_VTABLE,
    NATIVE_DRUM_VTABLE,
    NATIVE_KEYGROUP_VTABLE,
    NATIVE_MIDI_VTABLE,
    NATIVE_CV_VTABLE,
    NATIVE_AUDIO_VTABLE};
  const uint32_t screens[] = {
    native_command_context.mainMode,
    native_command_context.mixerMode,
    native_command_context.trackMuteMode};
  for (unsigned family = 0; family < 6; family++)
    for (unsigned screen = 0; screen < 3; screen++)
      for (unsigned single = 0; single < 2; single++) {
        struct commands_fixture f;
        setup(&f, 8, 64, true);
        f.live_policy = true;
        /* The same family/screen/peer matrix also exercises ordinary playback.
         * Stopped behavior has independent coverage above and below. */
        put_bool(
          &f.identity.native,
          POLICY_TIMELINE + native_command_context.timelinePlaying,
          true);
        f.single_arm = f.single_solo = single;
        family_pair(&f, families[family], families[(family + 1) % 6]);
        put32(&f.identity.native, POLICY_MODE + native_command_context.modeValue, screens[screen]);
        put32(
          &f.identity.native,
          POLICY_RECORD_PROPERTIES + native_command_context.armModeValue,
          !single);
        put32(&f.identity.native, POLICY_SOLO_PROPERTY + native_command_context.soloValue, !single);
        for (unsigned target = 0; target < 2; target++)
          for (unsigned selected = 0; selected < 2; selected++)
            for (unsigned property = 0; property < 3; property++) {
              choose(&f, (target == selected) ? TRACK2 : TRACK);
              uint32_t owner =
                property == 2 ? (target ? TRACK2 : TRACK) : (target ? PROGRAM2 : PROGRAM);
              bool original =
                get32(&f.identity.native, owner + native_bindings[property].requested) & 1;
              char id[80];
              snprintf(
                id,
                sizeof id,
                "family_%u_%u_%u_%u_%u_%u",
                family,
                screen,
                single,
                target,
                selected,
                property);
              struct mpc_command_request input = request(&f, id, property, target, !original);
              input.has_expected = true;
              input.expected = original;
              struct mpc_command_receipt receipt = send(&f, input);
              assert(receipt.status == MPC_COMMAND_APPLIED && receipt.native_invoked);
              unsigned setters = f.identity.native.setters;
              struct mpc_command_receipt replay;
              assert(mpc_commands_submit(f.commands, 7, &input, 0, &replay) == MPC_NATIVE_OK);
              assert(replay.status == MPC_COMMAND_APPLIED && f.identity.native.setters == setters);
              if (family == 3 || family == 4)
                emit(&receipt);
            }
        cleanup(&f);
      }
  /* Requalify a queued target; an old family's support cannot grant permission. */
  for (unsigned stage = 0; stage < 6; stage++) {
    struct commands_fixture f;
    setup(&f, 8, 16, true);
    f.live_policy = true;
    struct mpc_command_request input =
      request(&f, "family_race", MPC_COMMAND_RECORD_ARMED, 0, true);
    if (stage == 2)
      f.family_after_filter = NATIVE_MIDI_VTABLE;
    if (stage == 3)
      f.family_after_set = NATIVE_MIDI_VTABLE;
    if (stage >= 4) {
      memcpy(
        f.identity.native.objects + 0x17000,
        f.identity.native.objects + PROGRAM - OBJECTS,
        0x400);
      if (stage == 4)
        f.program_after_set = OBJECTS + 0x17000;
      else
        f.identity.native.propagate_audio = false;
    }
    struct mpc_command_receipt receipt;
    assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
    if (stage < 2)
      family_pair(
        &f,
        stage ? NATIVE_MIDI_VTABLE : native_program_types[1].vtable,
        NATIVE_DRUM_VTABLE);
    if (stage == 5) {
      while (!queued(&f))
        tiny_pause();
      pump(&f.identity);
      while (!queued(&f))
        tiny_pause();
      put32(&f.identity.native, TRACK + NATIVE_PROGRAM_OFFSET, OBJECTS + 0x17000);
    }
    receipt = drive(&f, input.request_id);
    assert(
      receipt.status ==
      (stage == 0     ? MPC_COMMAND_REJECTED
         : stage == 1 ? MPC_COMMAND_APPLIED
                      : MPC_COMMAND_UNKNOWN));
    assert(receipt.native_invoked == (stage != 0));
    assert(f.identity.native.setters == (stage == 1 || stage >= 3));
    emit(&receipt);
    cleanup(&f);
  }
}

static void playback_transitions(void) {
  const struct native_command_context_layout *p = &native_command_context;
  const struct {
    uint32_t address, value;
    enum mpc_native_error error;
  } changes[] = {
    {POLICY_TIMELINE + p->timelinePlaying, 0, MPC_NATIVE_STATE_UNSTABLE},
    {POLICY_TIMELINE + p->timelinePlaying, 1, MPC_NATIVE_STATE_UNSTABLE},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, 1, MPC_NATIVE_RECORDING_UNQUALIFIED},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, 2, MPC_NATIVE_RECORDING_UNQUALIFIED},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, 3, MPC_NATIVE_RECORDING_UNQUALIFIED},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, 4, MPC_NATIVE_RECORDING_UNQUALIFIED},
    {PROGRAM + p->programAutomation, 1, MPC_NATIVE_AUTOMATION_UNQUALIFIED},
    {PROGRAM + p->programAutomation, 2, MPC_NATIVE_AUTOMATION_UNQUALIFIED},
    {POLICY_SEQUENCER + p->sequencerMode, 1, MPC_NATIVE_PLAYBACK_CONTEXT_UNQUALIFIED},
    {POLICY_SEQUENCER + p->sequencerMode, 3, MPC_NATIVE_PLAYBACK_CONTEXT_UNQUALIFIED},
    {POLICY_SEQUENCER + p->sequencerSongSource, 4, MPC_NATIVE_PLAYBACK_CONTEXT_UNQUALIFIED},
  };
  for (unsigned change = 0; change < sizeof changes / sizeof *changes; change++)
    for (unsigned stage = 0; stage < 5; stage++) {
      struct commands_fixture f;
      setup(&f, 8, 16, true);
      f.live_policy = true;
      put_bool(
        &f.identity.native,
        POLICY_TIMELINE + p->timelinePlaying,
        !(changes[change].address == POLICY_TIMELINE + p->timelinePlaying &&
          changes[change].value));
      f.playback_change_address = changes[change].address;
      f.playback_change_value = changes[change].value;
      f.playback_change_stage = stage;
      /* Stage 2 transitions inside the native arm filter: invoked=true but the
       * setter must not run. Stage 4 loses context during delayed readback. */
      f.identity.native.propagate_audio = stage != 4;
      struct mpc_command_request input =
        request(&f, "playback_transition", MPC_COMMAND_RECORD_ARMED, 0, true);
      struct mpc_command_receipt receipt;
      assert(mpc_commands_submit(f.commands, 7, &input, 0, &receipt) == MPC_NATIVE_OK);
      while (!queued(&f))
        tiny_pause();
      if (!stage)
        put32(&f.identity.native, changes[change].address, changes[change].value);
      pump(&f.identity);
      if (stage == 4)
        put32(&f.identity.native, changes[change].address, changes[change].value);
      receipt = drive(&f, input.request_id);
      /* An accepted command samples its first baseline at dispatch. A stable
       * start/stop before dispatch is allowed; subsequent transitions fence. */
      if (changes[change].address == POLICY_TIMELINE + p->timelinePlaying && !stage) {
        assert(receipt.status == MPC_COMMAND_APPLIED);
      } else {
        assert(receipt.status == (stage < 2 ? MPC_COMMAND_REJECTED : MPC_COMMAND_UNKNOWN));
        assert(receipt.error == changes[change].error);
        assert(receipt.native_invoked == (stage >= 2));
        assert(f.identity.native.setters == (stage >= 3));
      }
      struct mpc_command_receipt replay;
      assert(mpc_commands_submit(f.commands, 7, &input, 0, &replay) == MPC_NATIVE_OK);
      assert(!memcmp(&replay, &receipt, sizeof receipt));
      emit(&receipt);
      cleanup(&f);
    }
  for (unsigned target = 0; target < 2; target++) {
    struct commands_fixture f;
    setup(&f, 8, 16, true);
    f.live_policy = true;
    put_bool(&f.identity.native, POLICY_TIMELINE + p->timelinePlaying, true);
    f.identity.native.filter_allows = false;
    struct mpc_command_receipt receipt =
      send(&f, request(&f, "playback_filter", MPC_COMMAND_RECORD_ARMED, target, true));
    assert(receipt.status == MPC_COMMAND_REJECTED && receipt.error == MPC_NATIVE_VALUE_REJECTED);
    assert(receipt.native_invoked && f.identity.native.filters == 1);
    emit(&receipt);
    cleanup(&f);
  }
}

int main(void) {
  (void)validate_target;
  (void)get32;
  struct mpc_command_receipt incompatible = {
    .abi_version = MPC_COMMANDS_ABI - 1,
    .struct_bytes = sizeof incompatible};
  assert(!mpc_commands_receipt_json(&incompatible));
  qualified_family_matrix();
  playback_transitions();
  sequence_scope_and_races();
  live_screens_and_transitions();
  idempotence_and_preconditions();
  races_gates_and_constraints();
  effects_and_audio_verification();
  capacity_expiry_and_shutdown();
  running_unknown_and_drain();
  concurrent_retries_and_stale_handles();
  return 0;
}
