#include "identity_fixture_backend.h"

static struct mpc_state_snapshot *state_read(
  struct identity_fixture *fixture, struct mpc_executor *executor, enum mpc_native_error expected) {
  struct mpc_executor_job *job =
    submit(executor, MPC_EXECUTOR_GET_STATE, (struct mpc_executor_identity){0});
  pump(fixture);
  struct mpc_executor_result result;
  enum mpc_native_error actual = mpc_executor_wait(job, 1000, &result);
  if (actual != expected)
    fprintf(
      stderr,
      "state capture: expected %s, got %s (mutations %u)\n",
      mpc_native_error_name(expected),
      mpc_native_error_name(actual),
      fixture->mutations);
  assert(actual == expected);
  assert(result.state == MPC_EXECUTOR_COMPLETED);
  struct mpc_state_snapshot *copy = NULL;
  if (!expected) {
    assert(result.snapshot);
    copy = malloc(sizeof *copy);
    assert(copy);
    *copy = *result.snapshot;
  } else
    assert(!result.snapshot);
  mpc_executor_release(job);
  return copy;
}

static void state_tests(void) {
  struct identity_fixture fixture;
  state_init(&fixture);
  struct fixture *f = &fixture.native;
  struct mpc_executor *executor = open_executor(&fixture);
  struct mpc_state_snapshot *first = state_read(&fixture, executor, MPC_NATIVE_OK);
  assert(first->track_count == 2 && first->bus_count == 0 && first->sequence_slots == 2);
  assert(first->used_sequences == 1 && first->sequences[0].used && !first->sequences[1].used);
  assert(first->sequences[0].bpm == 123.5f && first->started_ms && first->finished_ms);
  assert(first->tracks[0].known == 15 && first->tracks[0].soloed && !first->tracks[0].audio_soloed);
  assert(
    first->tracks[0].record_armed && !first->tracks[0].audio_record_armed &&
    first->tracks[0].silenced);
  uint64_t project = first->project, track = first->tracks[0].identity.id;
  free(first);
  fixture.state_mutation = 1;
  first = state_read(&fixture, executor, MPC_NATIVE_OK);
  assert(first->attempts == 2 && first->selected_track == first->tracks[1].identity.id);
  assert(first->project == project && first->tracks[0].identity.id == track);
  free(first);
  fixture.state_mutation = 2;
  fixture.mutations = 0;
  assert(!state_read(&fixture, executor, MPC_NATIVE_STATE_UNSTABLE));
  assert(fixture.mutations == MPC_STATE_ATTEMPTS);
  fixture.state_mutation = 3;
  first = state_read(&fixture, executor, MPC_NATIVE_OK);
  assert(
    first->project != project && first->tracks[0].identity.id != track && first->attempts == 2);
  free(first);
  /* Failed supported fields cannot be replaced with false or an old sample. */
  put_bool(f, PROGRAM + native_bindings[0].requested, 2);
  assert(!state_read(&fixture, executor, MPC_NATIVE_READ_FAILED));
  put_bool(f, PROGRAM + native_bindings[0].requested, 0);
  put32(f, TRACK + NATIVE_TRACK_NAME, 0);
  assert(!state_read(&fixture, executor, MPC_NATIVE_READ_FAILED));
  put32(f, TRACK + NATIVE_TRACK_NAME, OBJECTS + 0x15000);
  select_project(&fixture, 0);
  first = state_read(&fixture, executor, MPC_NATIVE_OK);
  assert(
    !first->project && !first->track_count && !first->selected_track && !first->sequence_slots);
  free(first);
  /* Cancelled/disconnected snapshot jobs still drain and release their buffers. */
  struct mpc_executor_job *job =
    submit(executor, MPC_EXECUTOR_GET_STATE, (struct mpc_executor_identity){0});
  struct mpc_executor_result result;
  assert(mpc_executor_cancel(job, &result) == MPC_NATIVE_CANCELLED && !result.snapshot);
  mpc_executor_release(job);
  close_executor(&fixture, executor);
  assert(!f->setters);
  assert(!pthread_cond_destroy(&fixture.changed));
  assert(!pthread_mutex_destroy(&fixture.mutex));
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--snapshot-wire")) {
    struct identity_fixture fixture;
    state_init(&fixture);
    struct mpc_executor *executor = open_executor(&fixture);
    struct mpc_state_snapshot *state = state_read(&fixture, executor, MPC_NATIVE_OK);
    assert(fwrite(state, sizeof *state, 1, stdout) == 1);
    free(state);
    close_executor(&fixture, executor);
    return 0;
  }
  state_tests();
  (void)validate_target; /* This fixture uses the production identity resolver. */
  struct identity_fixture fixture;
  init(&fixture);
  struct fixture *native = &fixture.native;
  struct mpc_executor *executor = open_executor(&fixture);
  struct mpc_identity_snapshot first = snapshot(&fixture, executor);
  assert(first.track_count == 1 && first.sequence_count == 2 && first.sequence_slots == 2);
  assert(
    first.selected_track == first.tracks[0].id && first.selected_sequence == first.sequences[0].id);
  assert(
    first.project != OBJECTS && first.tracks[0].id != TRACK && first.sequences[0].id != SEQUENCE);
  struct mpc_executor_identity original = handle(&first, 0);
  finish(&fixture, submit(executor, MPC_EXECUTOR_READ_TRACK, original), MPC_NATIVE_OK);

  /* Rename changes native metadata; selection changes outside Main use the
   * project properties. Neither can retarget a retained track handle. */
  put32(native, TRACK + 1128, 123);
  put32(native, VECTOR + 4, TRACK2);
  put32(native, POOL + NATIVE_USER_END, VECTOR + 8);
  put32(native, POOL + NATIVE_USER_CAPACITY, VECTOR + 8);
  put32(native, OBJECTS + NATIVE_SELECTED_TRACK, TRACK2);
  put32(native, OBJECTS + NATIVE_SELECTED_SEQUENCE, SEQUENCE2);
  struct mpc_identity_snapshot two = snapshot(&fixture, executor);
  assert(two.tracks[0].id == original.track && two.selected_track == two.tracks[1].id);
  assert(two.selected_sequence == two.sequences[1].id && two.project == first.project);
  put32(native, VECTOR, TRACK2);
  put32(native, VECTOR + 4, TRACK);
  put32(native, SEQUENCES, SEQUENCE2);
  put32(native, SEQUENCES + 4, SEQUENCE);
  struct mpc_identity_snapshot reordered = snapshot(&fixture, executor);
  assert(reordered.tracks[1].id == original.track && reordered.tracks[1].index == 1);
  assert(reordered.sequences[1].id == first.sequences[0].id);
  finish(&fixture, submit(executor, MPC_EXECUTOR_READ_TRACK, original), MPC_NATIVE_OK);

  destroy_object(&fixture, TRACK);
  struct mpc_executor_result stale =
    finish(&fixture, submit(executor, MPC_EXECUTOR_SET_MUTE, original), MPC_NATIVE_STALE_TARGET);
  assert(!stale.write.native_invoked && !native->setters);
  struct mpc_identity_snapshot recreated = snapshot(&fixture, executor);
  assert(recreated.tracks[1].id != original.track && recreated.project == first.project);
  struct mpc_executor_identity current = handle(&recreated, 1);
  finish(&fixture, submit(executor, MPC_EXECUTOR_SET_MUTE, current), MPC_NATIVE_OK);
  assert(native->setters == 1);

  current.sequence = first.sequences[0].id;
  current.sequence_generation = first.sequences[0].generation;
  finish(&fixture, submit(executor, MPC_EXECUTOR_VALIDATE_SEQUENCE, current), MPC_NATIVE_OK);
  destroy_object(&fixture, SEQUENCE);
  finish(
    &fixture,
    submit(executor, MPC_EXECUTOR_VALIDATE_SEQUENCE, current),
    MPC_NATIVE_STALE_TARGET);
  stale =
    finish(&fixture, submit(executor, MPC_EXECUTOR_SET_SOLO, current), MPC_NATIVE_STALE_TARGET);
  assert(!stale.write.native_invoked && native->setters == 1);
  current.sequence = current.sequence_generation = 0;

  /* Clear/recreate can retain the Sequence object itself. The native unused
   * transition is observed even when both transitions occur between samples. */
  struct mpc_identity_snapshot before_clear = snapshot(&fixture, executor);
  struct mpc_executor_identity clear_ref = handle(&before_clear, 0);
  clear_ref.sequence = before_clear.sequences[0].id;
  clear_ref.sequence_generation = before_clear.sequences[0].generation;
  for (unsigned i = 0; i < 128; i++)
    if (fixture.sequence_watches[i] && fixture.sequence_watches[i]->sequence == SEQUENCE2) {
      fixture.sequence_watches[i]->vtable[0](fixture.sequence_watches[i], SEQUENCE2);
      fixture.sequence_watches[i]->vtable[0](fixture.sequence_watches[i], SEQUENCE2);
    }
  finish(
    &fixture,
    submit(executor, MPC_EXECUTOR_VALIDATE_SEQUENCE, clear_ref),
    MPC_NATIVE_STALE_TARGET);
  finish(&fixture, submit(executor, MPC_EXECUTOR_READ_TRACK, current), MPC_NATIVE_OK);

  /* The former root remains perfectly valid in memory. Only the authoritative
   * owner's pointer tells us it is no longer the active project. */
  struct mpc_executor_job *queued = submit(executor, MPC_EXECUTOR_SET_SOLO, current);
  select_project(&fixture, PROJECT2);
  stale = finish(&fixture, queued, MPC_NATIVE_STALE_TARGET);
  assert(!stale.write.native_invoked && native->setters == 1);
  struct mpc_identity_snapshot replaced = snapshot(&fixture, executor);
  assert(replaced.project != first.project && replaced.tracks[0].id != current.track);
  struct mpc_executor_identity replacement = handle(&replaced, 0);
  destroy_object(&fixture, PROJECT2);
  finish(&fixture, submit(executor, MPC_EXECUTOR_READ_TRACK, replacement), MPC_NATIVE_STALE_TARGET);
  struct mpc_identity_snapshot project_aba = snapshot(&fixture, executor);
  assert(project_aba.project != replaced.project);

  struct mpc_executor_identity before_load = handle(&project_aba, 0);
  fixture.project_watch->loading = true;
  fixture.project_watch->vtable[0](fixture.project_watch, PROJECT2);
  finish(&fixture, submit(executor, MPC_EXECUTOR_SET_SOLO, before_load), MPC_NATIVE_NOT_READY);
  fixture.project_watch->loading = false;
  finish(&fixture, submit(executor, MPC_EXECUTOR_SET_SOLO, before_load), MPC_NATIVE_STALE_TARGET);
  project_aba = snapshot(&fixture, executor);

  /* Malformed topology retires the authority; repairing memory cannot revive
   * references issued before the observation failure. */
  struct mpc_executor_identity before_failure = handle(&project_aba, 0);
  put32(native, VECTOR + 4, TRACK2);
  finish(&fixture, submit(executor, MPC_EXECUTOR_SET_MUTE, before_failure), MPC_NATIVE_READ_FAILED);
  put32(native, VECTOR + 4, TRACK);
  finish(
    &fixture,
    submit(executor, MPC_EXECUTOR_READ_TRACK, before_failure),
    MPC_NATIVE_STALE_TARGET);
  close_executor(&fixture, executor);

  executor = open_executor(&fixture);
  struct mpc_identity_snapshot reattached = snapshot(&fixture, executor);
  assert(strcmp(reattached.instance_id, first.instance_id));
  finish(&fixture, submit(executor, MPC_EXECUTOR_SET_MUTE, original), MPC_NATIVE_STALE_TARGET);
  close_executor(&fixture, executor);
  assert(fixture.expired == 3 && native->setters == 1);
  for (unsigned failure = 0; failure < 3; failure++) {
    executor = open_executor(&fixture);
    snapshot(&fixture, executor);
    /* Refused/dropped dispatch and a still-owned native listener must all keep
     * the authority alive. Retrying shutdown may drain it, then allow close. */
    fixture.reject_post = failure == 0;
    fixture.drop_post = failure == 1;
    fixture.retain_sequence = failure == 2;
    assert(mpc_executor_shutdown(executor, 5) == MPC_NATIVE_TIMED_OUT);
    if (failure == 2)
      pump(&fixture);
    assert(mpc_executor_close(executor) == MPC_NATIVE_NOT_READY);
    fixture.reject_post = fixture.drop_post = fixture.retain_sequence = false;
    close_executor(&fixture, executor);
  }
  assert(!pthread_cond_destroy(&fixture.changed));
  assert(!pthread_mutex_destroy(&fixture.mutex));
  puts("identity_lifecycle_cases=19");
  return 0;
}
