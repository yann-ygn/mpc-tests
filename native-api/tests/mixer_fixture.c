/* Reuse the command harness so numeric commands exercise the same real FIFO,
 * policy, lifetime registry, retained worker and draining shutdown as flags. */
#define main flag_fixture_main
#include "commands_fixture.c"
#undef main

static struct mpc_mixer_state mixer_sample(struct commands_fixture *f, unsigned index) {
  struct mpc_executor_request request = {.operation = MPC_EXECUTOR_SAMPLE_FLAGS};
  struct mpc_executor_job *job;
  assert(!mpc_executor_submit(f->executor, &request, &job));
  pump(&f->identity);
  struct mpc_executor_result result;
  assert(!mpc_executor_wait(job, 1000, &result));
  assert(result.flags && result.flags->tracks[index].mixer.generation);
  struct mpc_mixer_state copy = result.flags->tracks[index].mixer;
  mpc_executor_release(job);
  return copy;
}
static struct mpc_command_request mixer_request(
  struct commands_fixture *f,
  unsigned op,
  unsigned target_index,
  unsigned control_index,
  double value,
  const char *id) {
  struct mpc_command_request r = request(f, id, op, target_index, value != 0);
  r.mixer.generation = mixer_sample(f, target_index).generation;
  r.mixer.index = control_index;
  if (mpc_mixer_numeric(op)) {
    r.value = false;
    r.mixer.value = value;
  }
  return r;
}
static void controls_only(struct identity_fixture *fixture, uintptr_t address) {
  (void)fixture;
  assert(address != TRACK + NATIVE_TRACK_NAME && address != TRACK2 + NATIVE_TRACK_NAME);
  assert(address != SEQUENCE + NATIVE_SEQUENCE_NAME && address != SEQUENCE + NATIVE_SEQUENCE_BPM);
}

static bool verification_entered, verification_release;
static void blocking_verification(struct identity_fixture *f, uintptr_t address) {
  if (address != PROGRAM + MIXER_VOLUME_VALUE)
    return;
  pthread_mutex_lock(&f->mutex);
  verification_entered = true;
  pthread_cond_broadcast(&f->changed);
  struct timespec until = deadline();
  while (!verification_release)
    assert(pthread_cond_timedwait(&f->changed, &f->mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&f->mutex);
}
static void *verification_pump(void *data) {
  pump(data);
  return NULL;
}

int main(void) {
  struct commands_fixture f;
  setup(&f, 8, 128, true);
  f.live_policy = true;
  struct fixture *n = &f.identity.native;
  mixer_init(n, PROGRAM, OBJECTS + 0x30000);
  mixer_init(n, PROGRAM2, OBJECTS + 0x32000);
  f.identity.before_read = controls_only;
  unsigned serial = 0;
  for (unsigned target_index = 0; target_index < 2; target_index++) {
    for (unsigned op = MPC_COMMAND_VOLUME; op <= MPC_COMMAND_INSERT; op++) {
      unsigned count = op == MPC_COMMAND_SEND || op == MPC_COMMAND_INSERT ? 4 : 1;
      for (unsigned index = 0; index < count; index++) {
        for (unsigned endpoint = 0; endpoint < 2; endpoint++) {
          char id[80];
          snprintf(id, sizeof id, "mixer_%u", serial++);
          struct mpc_command_request r = mixer_request(&f, op, target_index, index, endpoint, id);
          struct mpc_command_receipt result = send(&f, r);
          if (op == MPC_COMMAND_INSERT && index != 0 && index != 3) {
            assert(
              result.status == MPC_COMMAND_REJECTED && result.error == MPC_NATIVE_EMPTY_INSERT);
            assert(!result.native_invoked);
            continue;
          }
          assert(result.status == MPC_COMMAND_APPLIED && result.after_known);
          emit(&result);
          unsigned setters = n->setters, filters = n->filters;
          struct mpc_command_receipt replay;
          assert(!mpc_commands_submit(f.commands, 7, &r, 0, &replay));
          assert(replay.status == MPC_COMMAND_APPLIED && n->setters == setters);
          r.mixer.index = (r.mixer.index + 1) % 4;
          assert(mpc_commands_submit(f.commands, 7, &r, 0, &replay) != MPC_NATIVE_OK);
          r = mixer_request(&f, op, target_index, index, endpoint, "noop");
          snprintf(r.request_id, sizeof r.request_id, "noop_%u", serial++);
          result = send(&f, r);
          assert(result.status == MPC_COMMAND_APPLIED && !result.native_invoked && !result.changed);
          assert(n->setters == setters && n->filters == filters);
          r.has_expected = true;
          r.expected = !endpoint;
          r.mixer.expected = !endpoint;
          if (!mpc_mixer_numeric(op))
            r.mixer.expected = 0;
          snprintf(r.request_id, sizeof r.request_id, "conflict_%u", serial++);
          result = send(&f, r);
          assert(
            result.status == MPC_COMMAND_REJECTED &&
            result.error == MPC_NATIVE_PRECONDITION_FAILED);
          assert(n->setters == setters && n->filters == filters);
          emit(&result);
        }
      }
    }
  }
  /* Preserve the requested double, compare native float steps exactly, and
   * report quantized values instead of requiring unrepresentable equality. */
  struct mpc_command_request r =
    mixer_request(&f, MPC_COMMAND_VOLUME, 0, 0, .123456789, "quantized");
  struct mpc_command_receipt result = send(&f, r);
  assert(result.status == MPC_COMMAND_APPLIED);
  assert(result.after.tracks[0].mixer.volume == (float).123456789);
  emit(&result);
  r = mixer_request(&f, MPC_COMMAND_VOLUME, 0, 0, .123456789, "float_noop");
  r.has_expected = true;
  r.mixer.expected = .123456789;
  result = send(&f, r);
  assert(result.status == MPC_COMMAND_APPLIED && !result.native_invoked);
  /* The current mixer generation is not adopted by a request already queued. */
  r = mixer_request(&f, MPC_COMMAND_VOLUME, 0, 0, .5, "stale_program");
  assert(!mpc_commands_submit(f.commands, 7, &r, 0, &result));
  destroy_object(&f.identity, PROGRAM + MIXER_VOLUME_PROPERTY - NATIVE_DESTRUCTION_OFFSET);
  result = drive(&f, r.request_id);
  assert(result.status == MPC_COMMAND_REJECTED && result.error == MPC_NATIVE_STALE_MIXER);
  assert(!result.native_invoked);
  emit(&result);
  r = mixer_request(&f, MPC_COMMAND_INSERT, 0, 0, 0, "slot_reordered");
  assert(!mpc_commands_submit(f.commands, 7, &r, 0, &result));
  put32(n, OBJECTS + 0x30200, OBJECTS + 0x30700);
  put32(n, OBJECTS + 0x3020c, OBJECTS + 0x30400);
  result = drive(&f, r.request_id);
  assert(result.status == MPC_COMMAND_REJECTED && result.error == MPC_NATIVE_STALE_MIXER);
  emit(&result);
  /* Automation restrictions cover no-ops as well as notifying writes. */
  for (unsigned mode = 1; mode <= 2; mode++) {
    r = mixer_request(&f, MPC_COMMAND_PAN, 0, 0, 1, mode == 1 ? "read" : "write");
    put32(n, PROGRAM + native_command_context.programAutomation, mode);
    result = send(&f, r);
    assert(
      result.status == MPC_COMMAND_REJECTED &&
      result.error == MPC_NATIVE_MIXER_AUTOMATION_UNQUALIFIED);
    assert(!result.native_invoked);
    emit(&result);
  }
  put32(n, PROGRAM + native_command_context.programAutomation, 0);
  /* Lagging native model work remains bounded read-only verification. */
  n->propagate_audio = false;
  r = mixer_request(&f, MPC_COMMAND_RACK, 0, 0, 0, "lag");
  assert(!mpc_commands_submit(f.commands, 7, &r, 0, &result));
  while (!queued(&f))
    tiny_pause();
  pump(&f.identity);
  while (!queued(&f))
    tiny_pause();
  f.identity.before_read = blocking_verification;
  pthread_t verification;
  assert(!pthread_create(&verification, NULL, verification_pump, &f.identity));
  pthread_mutex_lock(&f.identity.mutex);
  struct timespec until = deadline();
  while (!verification_entered)
    assert(pthread_cond_timedwait(&f.identity.changed, &f.identity.mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&f.identity.mutex);
  atomic_fetch_add(&f.time_offset, MPC_COMMAND_VERIFICATION_MS + 1);
  result = drive(&f, r.request_id);
  assert(result.status == MPC_COMMAND_UNKNOWN && result.native_invoked);
  char *frozen = mpc_commands_receipt_json(&result);
  assert(frozen);
  assert(!mpc_commands_submit(f.commands, 7, &r, 0, &result));
  char *replay = mpc_commands_receipt_json(&result);
  assert(replay && !strcmp(frozen, replay));
  free(replay);
  pthread_mutex_lock(&f.identity.mutex);
  verification_release = true;
  pthread_cond_broadcast(&f.identity.changed);
  pthread_mutex_unlock(&f.identity.mutex);
  assert(!pthread_join(verification, NULL));
  f.identity.before_read = NULL;
  while (mpc_commands_shutdown(f.commands, 1) == MPC_NATIVE_TIMED_OUT)
    tiny_pause();
  assert(!mpc_commands_get(f.commands, 7, service, r.request_id, 0, &result));
  replay = mpc_commands_receipt_json(&result);
  assert(replay && !strcmp(frozen, replay));
  free(replay);
  free(frozen);
  emit(&result);
  cleanup(&f);
  return 0;
}
