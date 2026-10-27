#include "identity_fixture_backend.h"
#include "mpc_observation.h"
#include "mpc_family_profile.h"

static unsigned reads;
static unsigned mutation;
static void observing_read(struct identity_fixture *f, uintptr_t address) {
  reads++;
  /* Flag observations must never depend on names or sequence metadata. */
  assert(address != TRACK + NATIVE_TRACK_NAME && address != TRACK2 + NATIVE_TRACK_NAME);
  assert(address != SEQUENCE + NATIVE_SEQUENCE_NAME && address != SEQUENCE + NATIVE_SEQUENCE_BPM);
  if (address != PROGRAM + native_bindings[0].requested || !mutation)
    return;
  unsigned change = mutation;
  mutation = 0;
  if (change == 1) {
    put32(&f->native, OBJECTS + NATIVE_SELECTED_TRACK, TRACK2);
    put32(&f->native, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, TRACK2);
    put32(&f->native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 1);
  } else if (change == 2)
    select_project(f, PROJECT2);
  else if (change == 3)
    put32(&f->native, TRACK + NATIVE_PROGRAM_OFFSET, OBJECTS + 0x16000);
  else if (change == 4)
    put32(&f->native, PROGRAM, IMAGE + NATIVE_DRUM_VTABLE);
  else if (change == 5)
    put_bool(&f->native, OBJECTS + 0x24000 + native_command_context.perSequenceMute, true);
}
static struct mpc_executor_job *
sample_job(struct mpc_executor *executor, unsigned offset, unsigned count, uint64_t topology) {
  struct mpc_executor_request request = {
    .operation = MPC_EXECUTOR_SAMPLE_FLAGS,
    .sample_offset = offset,
    .sample_count = count,
    .sample_topology = topology};
  struct mpc_executor_job *job;
  assert(mpc_executor_submit(executor, &request, &job) == MPC_NATIVE_OK);
  return job;
}
static struct mpc_command_observation sample(
  struct identity_fixture *f,
  struct mpc_executor *executor,
  unsigned offset,
  unsigned count,
  uint64_t topology,
  enum mpc_native_error expected) {
  struct mpc_executor_job *job = sample_job(executor, offset, count, topology);
  pump(f);
  struct mpc_executor_result result;
  assert(mpc_executor_wait(job, 1000, &result) == expected);
  struct mpc_command_observation copy = {0};
  if (!expected) {
    assert(result.flags && !result.snapshot);
    copy = *result.flags;
  } else
    assert(!result.flags);
  mpc_executor_release(job);
  return copy;
}
static struct mpc_state_snapshot *full(struct identity_fixture *f, struct mpc_executor *executor) {
  struct mpc_executor_job *job =
    submit(executor, MPC_EXECUTOR_GET_STATE, (struct mpc_executor_identity){0});
  pump(f);
  struct mpc_executor_result result;
  assert(mpc_executor_wait(job, 1000, &result) == MPC_NATIVE_OK);
  struct mpc_state_snapshot *copy = malloc(sizeof *copy);
  assert(copy);
  *copy = *result.snapshot;
  mpc_executor_release(job);
  return copy;
}
static void compare(struct identity_fixture *f, struct mpc_executor *executor) {
  struct mpc_command_observation flags = sample(f, executor, 0, 16, 0, MPC_NATIVE_OK);
  struct mpc_state_snapshot *state = full(f, executor);
  assert(flags.project == state->project && flags.generation == state->project_generation);
  assert(
    flags.selected_track == state->selected_track &&
    flags.selected_sequence == state->selected_sequence);
  assert(flags.selected_sequence_generation == state->selected_sequence_generation);
  assert(flags.mute_scope == state->mute_scope && flags.total_tracks == state->track_count);
  assert(flags.track_count == state->track_count && flags.duration_ns && flags.captured_ms);
  for (unsigned i = 0; i < flags.track_count; i++) {
    const struct mpc_command_track *a = &flags.tracks[i];
    const struct mpc_state_track *b = &state->tracks[i];
    assert(a->id == b->identity.id && a->generation == b->identity.generation);
    assert(a->known == b->known && a->family == mpc_family_by_type(b->type)->family);
    assert(a->muted == b->muted && a->soloed == b->soloed && a->record_armed == b->record_armed);
    assert(a->audio_soloed == b->audio_soloed && a->audio_record_armed == b->audio_record_armed);
    assert(a->silenced == b->silenced);
  }
  free(state);
}
int main(void) {
  (void)validate_target;
  struct identity_fixture f;
  state_init(&f);
  struct mpc_executor *executor = open_executor(&f);
  compare(&f, executor);
  /* Cross-family flags, including requested-only and unknown families. */
  for (unsigned i = 1; i < sizeof mpc_family_matrix / sizeof *mpc_family_matrix; i++) {
    put32(&f.native, PROGRAM, IMAGE + mpc_family_matrix[i].vtable);
    bool audio = mpc_family_matrix[i].family == MPC_NATIVE_AUDIO;
    put32(&f.native, POOL + NATIVE_MIDI_END, VECTOR + (audio ? 0 : 8));
    put32(&f.native, POOL + NATIVE_AUDIO_BEGIN, audio ? VECTOR : 0);
    put32(&f.native, POOL + NATIVE_AUDIO_END, audio ? VECTOR + 8 : 0);
    put32(&f.native, POOL + NATIVE_AUDIO_CAPACITY, audio ? VECTOR + 8 : 0);
    compare(&f, executor);
  }
  put32(&f.native, PROGRAM, IMAGE + NATIVE_PLUGIN_VTABLE);
  put_bool(&f.native, PROGRAM + native_bindings[0].requested, true);
  compare(&f, executor); /* Manual edits are sampled, never cached. */
  f.before_read = observing_read;
  reads = 0;
  struct mpc_command_observation first = sample(&f, executor, 0, 1, 0, MPC_NATIVE_OK);
  assert(first.track_count == 1 && first.total_tracks == 2 && !first.offset);
  unsigned single_reads = reads;
  struct mpc_command_observation second =
    sample(&f, executor, 1, 1, first.topology_epoch, MPC_NATIVE_OK);
  assert(
    second.offset == 1 && second.track_count == 1 && second.tracks[0].id != first.tracks[0].id);
  /* Identical pending observers share one native capture. */
  reads = 0;
  struct mpc_executor_job *a = sample_job(executor, 0, 1, 0);
  struct mpc_executor_job *b = sample_job(executor, 0, 1, 0);
  pump(&f);
  struct mpc_executor_result ra, rb;
  assert(!mpc_executor_poll(a, &ra) && !mpc_executor_poll(b, &rb));
  assert(ra.state == MPC_EXECUTOR_COMPLETED && rb.state == MPC_EXECUTOR_COMPLETED);
  assert(ra.flags && rb.flags && reads == single_reads);
  assert(!ra.flags_shared && rb.flags_shared && !rb.ui_callback_us);
  assert(!memcmp(ra.flags, rb.flags, sizeof *ra.flags));
  mpc_executor_release(a);
  mpc_executor_release(b);
  /* Reorder invalidates paging, while preserving object IDs. */
  put32(&f.native, VECTOR, TRACK2);
  put32(&f.native, VECTOR + 4, TRACK);
  put32(&f.native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 1);
  sample(&f, executor, 1, 1, first.topology_epoch, MPC_NATIVE_STATE_UNSTABLE);
  second = sample(&f, executor, 0, 1, 0, MPC_NATIVE_OK);
  assert(
    second.tracks[0].id != first.tracks[0].id && second.topology_epoch != first.topology_epoch);
  put32(&f.native, VECTOR, TRACK);
  put32(&f.native, VECTOR + 4, TRACK2);
  put32(&f.native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 0);
  for (unsigned i = 1; i <= 5; i++) {
    mutation = i;
    sample(&f, executor, 0, 1, 0, MPC_NATIVE_STATE_UNSTABLE);
    select_project(&f, OBJECTS);
    put32(&f.native, TRACK + NATIVE_PROGRAM_OFFSET, PROGRAM);
    put32(&f.native, PROGRAM, IMAGE + NATIVE_PLUGIN_VTABLE);
    put32(&f.native, OBJECTS + NATIVE_SELECTED_TRACK, TRACK);
    put32(&f.native, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, TRACK);
    put32(&f.native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 0);
    put_bool(&f.native, OBJECTS + 0x24000 + native_command_context.perSequenceMute, false);
  }
  f.before_read = NULL;
  /* Deletion, address reuse and same-file project replacement retire IDs. */
  first = sample(&f, executor, 0, 16, 0, MPC_NATIVE_OK);
  destroy_object(&f, TRACK2);
  second = sample(&f, executor, 0, 16, 0, MPC_NATIVE_OK);
  assert(second.tracks[1].id != first.tracks[1].id);
  f.project_watch->vtable[0](f.project_watch, OBJECTS);
  second = sample(&f, executor, 0, 16, 0, MPC_NATIVE_OK);
  assert(second.project != first.project);
  /* Removing a member changes coverage; old continuation cannot skip it. */
  uint64_t epoch = second.topology_epoch;
  put32(&f.native, POOL + NATIVE_USER_END, VECTOR + 4);
  sample(&f, executor, 1, 1, epoch, MPC_NATIVE_STATE_UNSTABLE);
  second = sample(&f, executor, 0, 16, 0, MPC_NATIVE_OK);
  assert(second.total_tracks == 1 && second.track_count == 1);
  put32(&f.native, POOL + NATIVE_USER_END, VECTOR + 8);
  /* Admission limits apply independently of observer count. */
  struct mpc_executor_job *pressure[8];
  for (unsigned i = 0; i < 8; i++)
    pressure[i] = sample_job(executor, 0, 16, 0);
  struct mpc_executor_request sampling = {.operation = MPC_EXECUTOR_SAMPLE_FLAGS};
  struct mpc_executor_job *overflow;
  assert(mpc_executor_submit(executor, &sampling, &overflow) == MPC_NATIVE_QUEUE_FULL);
  for (unsigned i = 0; i < 8; i++)
    mpc_executor_release(pressure[i]);
  /* Owned slots bound pressure; disconnect/cancel and shutdown drain samples. */
  a = sample_job(executor, 0, 16, 0);
  b = sample_job(executor, 0, 16, 0);
  assert(mpc_executor_cancel(b, &rb) == MPC_NATIVE_CANCELLED && !rb.flags);
  mpc_executor_release(b);
  mpc_executor_release(a);
  close_executor(&f, executor);
  assert(!f.native.setters);
  assert(!pthread_cond_destroy(&f.changed));
  assert(!pthread_mutex_destroy(&f.mutex));
  puts("observation_flags_cases=full_equivalence,families,paging,coalescing,lifecycle,context,"
       "shutdown");
  return 0;
}
