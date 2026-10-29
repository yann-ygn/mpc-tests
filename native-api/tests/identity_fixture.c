#include "identity_fixture_backend.h"
#include "mpc_mixer_profile.h"
#include "mpc_family_profile.h"

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
  assert(
    first->tracks[0].known == MPC_NATIVE_ALL_FIELDS && first->tracks[0].soloed &&
    !first->tracks[0].audio_soloed);
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
  fixture.state_mutation = 4;
  first = state_read(&fixture, executor, MPC_NATIVE_OK);
  assert(first->attempts == 2 && first->mute_scope == MPC_MUTE_SEQUENCE);
  assert(first->selected_sequence_generation == first->sequences[0].identity.generation);
  free(first);
  fixture.state_mutation = 5;
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

/* Production full captures must fence assignment while controls remain cheap. */
static unsigned plugin_reads, plugin_mutation;
static void plugin_during_read(struct identity_fixture *f, uintptr_t address) {
  if (address != PROGRAM + PLUGIN_programInstance)
    return;
  plugin_reads++;
  if (plugin_mutation && plugin_reads % 2 == 0) {
    if (plugin_mutation == 4) {
      uint32_t uid;
      memcpy(&uid, f->native.objects + PROGRAM - OBJECTS + PLUGIN_descriptionUid, 4);
      put32(&f->native, PROGRAM + PLUGIN_descriptionUid, uid + 1);
      return;
    }
    bool hype = !strcmp((char *)f->native.objects + 0x90000, "Hype");
    if (plugin_mutation >= 3 && hype)
      vst_assignment(
        &f->native,
        PROGRAM,
        "Midimix Editor",
        "/storage/Synths/Midimix/editor.so",
        0x4d6d4564);
    else
      plugin_assignment(
        &f->native,
        PROGRAM,
        hype ? "TubeSynth" : "Hype",
        hype ? "MPC:TubeSynth" : "MPC:Hype");
    if (plugin_mutation == 1)
      plugin_mutation = 0;
  }
}

static void plugin_tests(void) {
  struct identity_fixture f;
  state_init(&f);
  struct fixture *n = &f.native;
  plugin_assignment(n, PROGRAM, "Hype", "MPC:Hype");
  struct mpc_executor *e = open_executor(&f);
  struct mpc_state_snapshot *s = state_read(&f, e, MPC_NATIVE_OK);
  uint64_t track = s->tracks[0].identity.id, generation = s->tracks[0].identity.generation;
  assert(s->selected_track == track);
  for (unsigned i = 0; i < 2; i++) {
    assert(strcmp(s->tracks[i].name, "Hype"));
    assert(s->tracks[i].instrument.status == MPC_PLUGIN_AVAILABLE);
    assert(!strcmp(s->tracks[i].instrument.display_name, "Hype"));
    assert(!strcmp(s->tracks[i].instrument.product_id, "MPC:Hype"));
  }
  free(s);
  /* Distinct Programs can carry the same product, and then different products. */
  uint32_t other = OBJECTS + 0x50000;
  memcpy(n->objects + other - OBJECTS, n->objects + PROGRAM - OBJECTS, 0x2000);
  put32(n, TRACK2 + NATIVE_PROGRAM_OFFSET, other);
  plugin_assignment(n, other, "Hype", "MPC:Hype");
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(!strcmp(s->tracks[1].instrument.product_id, s->tracks[0].instrument.product_id));
  free(s);
  plugin_assignment(n, other, "TubeSynth", "MPC:TubeSynth");
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(!strcmp(s->tracks[1].instrument.display_name, "TubeSynth"));
  assert(s->tracks[0].identity.id == track && s->tracks[0].identity.generation == generation);
  free(s);
  /* One wrapper, distinct products; duplicate product, distinct Programs. */
  vst_assignment(n, PROGRAM, "Midimix Editor", "/storage/Synths/Midimix/editor.so", 0x4d6d4564);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(
    !strcmp(s->tracks[0].instrument.product_id, "VST:4d6d4564:/storage/Synths/Midimix/editor.so"));
  assert(!strcmp(s->tracks[1].instrument.product_id, "MPC:TubeSynth"));
  free(s);
  vst_assignment(n, other, "Midimix Editor", "/storage/Synths/Midimix/editor.so", 0x4d6d4564);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(!strcmp(s->tracks[0].instrument.product_id, s->tracks[1].instrument.product_id));
  free(s);
  vst_assignment(n, other, "Torpedo Control", "/storage/Synths/Torpedo/control.so", 0x54704374);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(strcmp(s->tracks[0].instrument.product_id, s->tracks[1].instrument.product_id));
  free(s);
  /* Unloaded/unassigned keep the same meanings in both formats. */
  put32(n, PROGRAM + PLUGIN_programInstance, 0);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(s->tracks[0].instrument.status == MPC_PLUGIN_UNAVAILABLE);
  free(s);
  vst_assignment(n, PROGRAM, "", "", 0);
  put32(n, PROGRAM + PLUGIN_programInstance, 0);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(s->tracks[0].instrument.status == MPC_PLUGIN_UNASSIGNED);
  free(s);
  vst_assignment(n, PROGRAM, "Midimix Editor", "/storage/Synths/Midimix/editor.so", 0x4d6d4564);
  /* UID, exact path and display name are independent inputs. */
  vst_assignment(n, other, "Renamed", "/storage/Synths/Midimix/editor.so", 0x4d6d4564);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(!strcmp(s->tracks[0].instrument.product_id, s->tracks[1].instrument.product_id));
  free(s);
  put32(n, other + PLUGIN_descriptionUid, 0xffffffffu);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(
    !strcmp(s->tracks[1].instrument.product_id, "VST:ffffffff:/storage/Synths/Midimix/editor.so"));
  free(s);
  vst_assignment(n, PROGRAM, "Midimix Editor", "relative.so", 1);
  assert(!state_read(&f, e, MPC_NATIVE_READ_FAILED));
  vst_assignment(n, PROGRAM, "Midimix Editor", "/", 0);
  memset(n->objects + 0x90101, 'a', 241);
  n->objects[0x901f2] = 0;
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(strlen(s->tracks[0].instrument.product_id) == 255);
  free(s);
  n->objects[0x901f2] = 'a';
  n->objects[0x901f3] = 0;
  assert(!state_read(&f, e, MPC_NATIVE_READ_FAILED));
  vst_assignment(n, PROGRAM, "Midimix Editor", "/", 1);
  n->objects[0x90101] = 0x80;
  assert(!state_read(&f, e, MPC_NATIVE_READ_FAILED));
  vst_assignment(n, PROGRAM, "Midimix Editor", "/", 1);
  memset(n->objects + 0x90200, 'a', MPC_PLUGIN_TEXT_BYTES);
  assert(!state_read(&f, e, MPC_NATIVE_READ_FAILED));
  plugin_assignment(n, PROGRAM, "Hype", "MPC:Hype");
  put32(n, PROGRAM + PLUGIN_programInstance, 0);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(s->tracks[0].instrument.status == MPC_PLUGIN_UNAVAILABLE);
  assert(!s->tracks[0].instrument.display_name[0]);
  free(s);
  plugin_assignment(n, PROGRAM, "", "");
  put32(n, PROGRAM + PLUGIN_programInstance, 0);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(s->tracks[0].instrument.status == MPC_PLUGIN_UNASSIGNED);
  free(s);
  plugin_assignment(n, PROGRAM, "Hype", "MPC:Hype");
  strcpy((char *)n->objects + 0x90200, "VST3");
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(s->tracks[0].instrument.status == MPC_PLUGIN_UNSUPPORTED);
  free(s);
  plugin_assignment(n, PROGRAM, "Hype", "MPC:Hype");
  const unsigned char invalid[][5] =
    {{0xc0, 0xaf, 0}, {0xed, 0xa0, 0x80, 0}, {0xf4, 0x90, 0x80, 0x80, 0}, {0xe2, 0}, {0x80, 0}};
  for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
    memcpy(n->objects + 0x90000, invalid[i], sizeof invalid[i]);
    assert(!state_read(&f, e, MPC_NATIVE_READ_FAILED));
  }
  memset(n->objects + 0x90000, 'a', MPC_PLUGIN_TEXT_BYTES);
  assert(!state_read(&f, e, MPC_NATIVE_READ_FAILED));
  plugin_assignment(n, PROGRAM, "Hype", "MPC:Hype");
  put32(n, PROGRAM + PLUGIN_descriptionIdentifier, UINT32_MAX - 5);
  assert(!state_read(&f, e, MPC_NATIVE_READ_FAILED));
  plugin_assignment(n, PROGRAM, "Hype", "MPC:Hype");
  f.before_read = plugin_during_read;
  plugin_reads = 0;
  plugin_mutation = 1;
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(s->attempts == 2);
  assert(!strcmp(s->tracks[0].instrument.display_name, "TubeSynth"));
  free(s);
  plugin_reads = 0;
  plugin_mutation = 2;
  assert(!state_read(&f, e, MPC_NATIVE_STATE_UNSTABLE));
  plugin_mutation = 0;
  plugin_assignment(n, PROGRAM, "Hype", "MPC:Hype");
  plugin_reads = 0;
  plugin_mutation = 3;
  assert(!state_read(&f, e, MPC_NATIVE_STATE_UNSTABLE));
  plugin_mutation = 0;
  vst_assignment(n, PROGRAM, "Midimix Editor", "/storage/Synths/Midimix/editor.so", 0x4d6d4564);
  plugin_reads = 0;
  plugin_mutation = 4;
  assert(!state_read(&f, e, MPC_NATIVE_STATE_UNSTABLE));
  plugin_mutation = 0;
  /* Reusing the Program address cannot retain cached product metadata. */
  ui = true;
  destroy_object(&f, PROGRAM + MIXER_VOLUME_PROPERTY - 4);
  ui = false;
  plugin_assignment(n, PROGRAM, "Hype", "MPC:Hype");
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(
    s->tracks[0].identity.id == track && !strcmp(s->tracks[0].instrument.display_name, "Hype"));
  free(s);
  put32(n, PROGRAM, IMAGE + mpc_family_by_type("drum")->vtable);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(s->tracks[0].identity.id == track);
  assert(s->tracks[0].instrument.status == MPC_PLUGIN_NOT_APPLICABLE);
  assert(!s->tracks[0].instrument.product_id[0]);
  free(s);
  put32(n, PROGRAM, IMAGE + NATIVE_PLUGIN_VTABLE);
  plugin_reads = 0;
  struct mpc_executor_request request = {
    .operation = MPC_EXECUTOR_SAMPLE_FLAGS,
    .sample_count = 16};
  struct mpc_executor_job *job;
  assert(!mpc_executor_submit(e, &request, &job));
  finish(&f, job, MPC_NATIVE_OK);
  assert(!plugin_reads);
  f.before_read = NULL;
  select_project(&f, PROJECT2);
  s = state_read(&f, e, MPC_NATIVE_OK);
  assert(s->tracks[0].identity.id != track);
  free(s);
  close_executor(&f, e);
  assert(!n->setters && !n->filters);
  pthread_cond_destroy(&f.changed);
  pthread_mutex_destroy(&f.mutex);
}

int main(int argc, char **argv) {
  /* Exercise production callback classification, independent of the fixture's
   * own generic lifecycle event injector. BPM is metadata; reset signals stay
   * destructive even when the used flag never changes. */
  struct mpc_native_sequence_watch metadata = {.sequence = SEQUENCE};
  mpc_sequence_callbacks[4](&metadata, SEQUENCE);
  assert(!metadata.changed);
  mpc_sequence_callbacks[2](&metadata, SEQUENCE2);
  assert(!metadata.changed);
  mpc_sequence_callbacks[2](&metadata, SEQUENCE);
  assert(metadata.changed);
  metadata.changed = false;
  mpc_sequence_callbacks[3](&metadata, SEQUENCE);
  assert(metadata.changed);

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
  plugin_tests();
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
  put32(native, OBJECTS + NATIVE_SELECTED_SEQUENCE_MIRROR, SEQUENCE2);
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
  finish(
    &fixture,
    submit(executor, MPC_EXECUTOR_VALIDATE_SEQUENCE, current),
    MPC_NATIVE_SEQUENCE_NOT_ACTIVE);
  put32(native, OBJECTS + NATIVE_SELECTED_SEQUENCE, SEQUENCE);
  put32(native, OBJECTS + NATIVE_SELECTED_SEQUENCE_MIRROR, SEQUENCE);
  finish(&fixture, submit(executor, MPC_EXECUTOR_VALIDATE_SEQUENCE, current), MPC_NATIVE_OK);
  destroy_object(&fixture, SEQUENCE);
  finish(
    &fixture,
    submit(executor, MPC_EXECUTOR_VALIDATE_SEQUENCE, current),
    MPC_NATIVE_STALE_SEQUENCE);
  stale =
    finish(&fixture, submit(executor, MPC_EXECUTOR_SET_SOLO, current), MPC_NATIVE_STALE_SEQUENCE);
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
    MPC_NATIVE_STALE_SEQUENCE);
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
