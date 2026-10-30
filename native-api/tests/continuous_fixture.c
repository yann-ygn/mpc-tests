/* Exercise admission, retention and lifecycle through the real managed worker. */
#define main flag_fixture_main
#include "commands_fixture.c"
#undef main

static void continuous_setup(struct commands_fixture *f, unsigned pending, unsigned receipts) {
  setup(f, pending, receipts, true);
  f->live_policy = true;
  mixer_init(&f->identity.native, PROGRAM, OBJECTS + 0x30000);
  mixer_init(&f->identity.native, PROGRAM2, OBJECTS + 0x32000);
}
static struct mpc_command_request
control_request(struct commands_fixture *f, unsigned track, unsigned property, const char *id) {
  struct mpc_executor_request sample = {.operation = MPC_EXECUTOR_SAMPLE_FLAGS};
  struct mpc_executor_job *job;
  struct mpc_executor_result result;
  assert(!mpc_executor_submit(f->executor, &sample, &job));
  pump(&f->identity);
  assert(!mpc_executor_wait(job, 1000, &result));
  struct mpc_command_request r = request(
    f,
    id,
    property == 0     ? MPC_COMMAND_VOLUME
      : property == 1 ? MPC_COMMAND_PAN
                      : MPC_COMMAND_SEND,
    track,
    false);
  r.continuous = true;
  r.mixer.generation = result.flags->tracks[track].mixer.generation;
  r.mixer.index = property < 2 ? 0 : property - 2;
  r.mixer.value = .25;
  mpc_executor_release(job);
  return r;
}
static void continuous_contract(void) {
  struct commands_fixture f;
  continuous_setup(&f, 2, 4);
  struct mpc_command_request controls[12];
  for (unsigned i = 0; i < 12; i++) {
    char id[80];
    snprintf(id, sizeof id, "pending_%u", i);
    controls[i] = control_request(&f, i / 6, i % 6, id);
  }
  struct mpc_command_receipt out, copy;
  for (unsigned i = 0; i < 8; i++)
    assert(!mpc_commands_submit(f.commands, 7, &controls[i], 0, &out));
  assert(mpc_commands_submit(f.commands, 7, &controls[8], 0, &out) == MPC_NATIVE_QUEUE_FULL);
  assert(!mpc_commands_submit(f.commands, 7, &controls[0], 0, &out));
  struct mpc_command_request next = controls[0];
  strcpy(next.request_id, "next");
  assert(mpc_commands_submit(f.commands, 7, &next, 0, &out) == MPC_NATIVE_QUEUE_FULL);
  next = controls[0];
  next.mixer.value = .5;
  assert(mpc_commands_submit(f.commands, 7, &next, 0, &out) == MPC_NATIVE_REQUEST_ID_CONFLICT);
  next = controls[0];
  next.continuous = false;
  assert(mpc_commands_submit(f.commands, 7, &next, 0, &out) == MPC_NATIVE_REQUEST_ID_CONFLICT);
  /* Continuous pressure cannot consume either legacy pending slot. */
  struct mpc_command_request a = request(&f, "flag_a", MPC_COMMAND_MUTE, 0, true);
  struct mpc_command_request b = request(&f, "flag_b", MPC_COMMAND_SOLO, 1, true);
  assert(!mpc_commands_submit(f.commands, 7, &a, 0, &out));
  assert(!mpc_commands_submit(f.commands, 7, &b, 0, &out));
  a.request_id[0] = 'x';
  assert(mpc_commands_submit(f.commands, 7, &a, 0, &out) == MPC_NATIVE_QUEUE_FULL);
  out = drive(&f, "flag_b");
  assert(out.status == MPC_COMMAND_APPLIED);
  for (unsigned i = 0; i < 8; i++) {
    assert(!mpc_commands_get(f.commands, 7, service, controls[i].request_id, 0, &out));
    assert(out.status == MPC_COMMAND_APPLIED);
  }
  assert(!mpc_commands_submit(f.commands, 7, &controls[0], 0, &out));
  assert(!mpc_commands_get(f.commands, 7, service, controls[0].request_id, 0, &copy));
  assert(!memcmp(&out, &copy, sizeof out));
  next = controls[0];
  strcpy(next.request_id, "next");
  next.has_expected = true;
  next.mixer.expected = .25;
  next.mixer.value = .5;
  out = send(&f, next);
  assert(out.status == MPC_COMMAND_APPLIED && out.native_invoked);
  assert(
    mpc_commands_get(f.commands, 7, service, controls[0].request_id, 0, &copy) ==
    MPC_NATIVE_OUTCOME_UNAVAILABLE);
  emit(&out);
  /* Other caller has a separate control key and retry namespace. */
  assert(!mpc_commands_submit(f.commands, 8, &controls[0], 0, &copy));
  out = drive(&f, "next");
  while (mpc_commands_get(f.commands, 8, service, controls[0].request_id, 0, &copy) ==
           MPC_NATIVE_OK &&
         copy.status == MPC_COMMAND_ACCEPTED) {
    if (queued(&f))
      pump(&f.identity);
    else
      tiny_pause();
  }
  assert(copy.status == MPC_COMMAND_APPLIED);
  /* Generation remains canonical; reusing a key cannot rebind queued work. */
  next = controls[0];
  strcpy(next.request_id, "stale_generation");
  assert(!mpc_commands_submit(f.commands, 7, &next, 0, &out));
  destroy_object(&f.identity, PROGRAM + MIXER_VOLUME_PROPERTY - NATIVE_DESTRUCTION_OFFSET);
  out = drive(&f, next.request_id);
  assert(out.status == MPC_COMMAND_REJECTED && out.error == MPC_NATIVE_STALE_MIXER);
  assert(!out.native_invoked);
  emit(&out);
  next = controls[1];
  strcpy(next.request_id, "expired_start");
  next.start_within_ms = 1;
  assert(!mpc_commands_submit(f.commands, 7, &next, 0, &out));
  atomic_fetch_add(&f.time_offset, 10);
  out = drive(&f, next.request_id);
  assert(out.status == MPC_COMMAND_CANCELLED && !out.native_invoked);
  emit(&out);
  /* Shutdown owns pending work even after a delivery wait/disconnect. */
  next = controls[2];
  strcpy(next.request_id, "shutdown_pending");
  assert(!mpc_commands_submit(f.commands, 7, &next, 0, &out));
  while (mpc_commands_shutdown(f.commands, 1) == MPC_NATIVE_TIMED_OUT)
    if (queued(&f))
      pump(&f.identity);
  assert(!mpc_commands_get(f.commands, 7, service, next.request_id, 0, &out));
  assert(out.status == MPC_COMMAND_CANCELLED && !out.native_invoked);
  emit(&out);
  cleanup(&f);
}
static void continuous_project_and_capacity(void) {
  struct commands_fixture f;
  continuous_setup(&f, 2, 4);
  struct mpc_command_request c = control_request(&f, 0, 0, "project_queued");
  struct mpc_command_receipt out;
  assert(!mpc_commands_submit(f.commands, 7, &c, 0, &out));
  select_project(&f.identity, PROJECT2);
  out = drive(&f, c.request_id);
  assert(out.status == MPC_COMMAND_REJECTED && !out.native_invoked);
  emit(&out);
  cleanup(&f);
  continuous_setup(&f, 2, 4);
  c = control_request(&f, 0, 0, "caller");
  /* Fill the bounded control pool with independent callers. Only completed
   * continuous records are replaceable; discrete records cannot be victims. */
  for (unsigned i = 0; i < MPC_COMMAND_CONTINUOUS_RECEIPTS + 2; i++) {
    uint64_t caller = i == 0 ? 7 : 100 + i;
    assert(!mpc_commands_submit(f.commands, caller, &c, 0, &out));
    for (;;) {
      assert(!mpc_commands_get(f.commands, caller, service, c.request_id, 0, &out));
      if (out.status != MPC_COMMAND_ACCEPTED)
        break;
      if (queued(&f))
        pump(&f.identity);
      else
        tiny_pause();
    }
    assert(out.status == MPC_COMMAND_APPLIED);
  }
  assert(
    mpc_commands_get(f.commands, 7, service, c.request_id, 0, &out) ==
    MPC_NATIVE_OUTCOME_UNAVAILABLE);
  cleanup(&f);
}
static void block_control_read(struct identity_fixture *identity, uintptr_t address) {
  if (address != PROGRAM + MIXER_VOLUME_VALUE)
    return;
  struct commands_fixture *f = (struct commands_fixture *)identity;
  pthread_mutex_lock(&identity->mutex);
  f->entered = true;
  pthread_cond_broadcast(&identity->changed);
  struct timespec until = deadline();
  while (!f->resume)
    assert(pthread_cond_timedwait(&identity->changed, &identity->mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&identity->mutex);
}
static void continuous_running_drain(void) {
  struct commands_fixture f;
  continuous_setup(&f, 2, 4);
  struct mpc_command_request c = control_request(&f, 0, 0, "running_continuous");
  struct mpc_command_receipt out;
  f.identity.before_read = block_control_read;
  assert(!mpc_commands_submit(f.commands, 7, &c, 0, &out));
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, pump_thread, &f));
  pthread_mutex_lock(&f.identity.mutex);
  struct timespec until = deadline();
  while (!f.entered)
    assert(pthread_cond_timedwait(&f.identity.changed, &f.identity.mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&f.identity.mutex);
  do {
    assert(!mpc_commands_get(f.commands, 7, service, c.request_id, 0, &out));
    tiny_pause();
  } while (out.execution != MPC_COMMAND_STARTED);
  atomic_fetch_add(&f.time_offset, MPC_COMMAND_MIN_RETENTION_MS + 1);
  out = drive(&f, c.request_id);
  assert(out.status == MPC_COMMAND_UNKNOWN && !out.native_known);
  assert(!mpc_commands_submit(f.commands, 7, &c, 0, &out));
  strcpy(c.request_id, "cannot_supersede_running");
  assert(mpc_commands_submit(f.commands, 7, &c, 0, &out) == MPC_NATIVE_QUEUE_FULL);
  assert(mpc_commands_shutdown(f.commands, 1) == MPC_NATIVE_TIMED_OUT);
  assert(mpc_commands_close(f.commands) == MPC_NATIVE_NOT_READY);
  pthread_mutex_lock(&f.identity.mutex);
  f.resume = true;
  pthread_cond_broadcast(&f.identity.changed);
  pthread_mutex_unlock(&f.identity.mutex);
  assert(!pthread_join(thread, NULL));
  cleanup(&f);
}
static void continuous_stream(void) {
  for (unsigned tracks = 1; tracks <= 2; tracks++) {
    struct commands_fixture f;
    continuous_setup(&f, 2, 16);
    struct mpc_command_request controls[12];
    for (unsigned i = 0; i < 6 * tracks; i++)
      controls[i] = control_request(&f, i / 6, i % 6, "stream");
    /* 601 seconds at virtual 10 Hz, beyond the ten-minute retention horizon.
     * Actual callbacks/worker scheduling run normally; only time advances. */
    for (unsigned i = 0; i < 6010; i++) {
      struct mpc_command_request c = controls[i % (6 * tracks)];
      snprintf(c.request_id, sizeof c.request_id, "stream_%u", i);
      c.mixer.value = (i % 11) / 10.;
      struct mpc_command_receipt out = send(&f, c), copy;
      assert(out.status == MPC_COMMAND_APPLIED);
      assert(!mpc_commands_submit(f.commands, 7, &c, 0, &copy));
      assert(!memcmp(&out, &copy, sizeof out));
      if (!(i % 1000)) {
        /* Continuous requests never occupy the legacy receipt slots. */
        struct mpc_command_request flag = request(&f, "flag", MPC_COMMAND_MUTE, 0, i % 2000 == 0);
        flag.request_id[0] = (char)('a' + i / 1000);
        out = send(&f, flag);
        assert(out.status == MPC_COMMAND_APPLIED);
      }
      atomic_fetch_add(&f.time_offset, 100);
    }
    cleanup(&f);
  }
  puts("{\"virtual10HzRequests\":12020,\"streamDurationMs\":601000,\"trackCounts\":[1,2],"
       "\"passed\":true}");
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "stream"))
    continuous_stream();
  else {
    continuous_contract();
    continuous_project_and_capacity();
    continuous_running_drain();
  }
  return 0;
}
