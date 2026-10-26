#include "identity_fixture_backend.h"

enum {
  USERS = OBJECTS + 0x16000,
  MIDI = USERS + 0x800,
  AUDIO = MIDI + 0x800,
  MASTERS = AUDIO + 0x800,
  ALL = MASTERS + 0x800,
  TRACK_BASE = OBJECTS + 0x20000
};

static void
vector(struct fixture *f, unsigned offset, uint32_t address, unsigned count, unsigned capacity) {
  put32(f, POOL + offset, address);
  put32(f, POOL + offset + 4, address + count * 4);
  put32(f, POOL + offset + 8, address + capacity * 4);
}

static uint32_t type(const char *name) {
  for (unsigned i = 0; i < sizeof native_program_types / sizeof *native_program_types; i++)
    if (!strcmp(name, native_program_types[i].name))
      return IMAGE + native_program_types[i].vtable;
  return IMAGE + 4;
}

static uint32_t track_address(unsigned i) {
  return TRACK_BASE + i * 0x800;
}

/* Independent native collections, deliberately different from all-track order. */
static void
inventory(struct identity_fixture *fixture, unsigned midi, unsigned audio, unsigned buses) {
  struct fixture *f = &fixture->native;
  unsigned users = midi + audio, count = users + buses;
  for (unsigned i = 0; i < count; i++) {
    uint32_t address = track_address(i), program = OBJECTS + 0xb0000 + i * 0x400;
    memcpy(f->objects + address - OBJECTS, f->objects + TRACK - OBJECTS, 0x700);
    put32(f, address + NATIVE_PROGRAM_OFFSET, program);
    put32(f, program, type(i < midi ? "plugin" : i < users ? "audio" : "submix"));
    put32(f, ALL + (count - 1 - i) * 4, address);
    if (i < users)
      put32(f, USERS + i * 4, address);
    if (i < midi)
      put32(f, MIDI + i * 4, address);
    else if (i < users)
      put32(f, AUDIO + (i - midi) * 4, address);
    else
      put32(f, MASTERS + (i - users) * 4, address);
  }
  vector(f, NATIVE_USER_BEGIN, USERS, users, users > 128 ? 256 : users);
  vector(f, NATIVE_MIDI_BEGIN, MIDI, midi, midi);
  vector(f, NATIVE_AUDIO_BEGIN, AUDIO, audio, audio);
  vector(f, NATIVE_MASTER_BEGIN, MASTERS, buses, buses);
  vector(f, NATIVE_ALL_BEGIN, ALL, count, count > 256 ? 512 : count);
  put32(f, OBJECTS + NATIVE_SELECTED_TRACK, users ? track_address(0) : 0);
  put32(f, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, users ? track_address(0) : 0);
  put32(f, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 0);
}

static struct mpc_state_snapshot *
capture(struct identity_fixture *f, struct mpc_executor *executor, enum mpc_native_error expected) {
  struct mpc_executor_job *job =
    submit(executor, MPC_EXECUTOR_GET_STATE, (struct mpc_executor_identity){0});
  pump(f);
  struct mpc_executor_result result;
  enum mpc_native_error error = mpc_executor_wait(job, 1000, &result);
  if (error != expected)
    fprintf(
      stderr,
      "inventory: expected %s, got %s\n",
      mpc_native_error_name(expected),
      mpc_native_error_name(error));
  assert(error == expected);
  struct mpc_state_snapshot *copy = NULL;
  if (!error) {
    copy = malloc(sizeof *copy);
    assert(copy && result.snapshot);
    *copy = *result.snapshot;
  } else
    assert(!result.snapshot);
  mpc_executor_release(job);
  return copy;
}

static void selected(struct fixture *f, unsigned index) {
  put32(f, OBJECTS + NATIVE_SELECTED_TRACK, track_address(index));
  put32(f, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, track_address(index));
  put32(f, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, index);
}

static void mutate(struct identity_fixture *f, uintptr_t address) {
  if (address != track_address(0) + NATIVE_TRACK_NAME || !f->state_mutation)
    return;
  unsigned mutation = f->state_mutation;
  if (mutation % 2)
    f->state_mutation = 0;
  f->mutations++;
  uint32_t a, b;
  if (mutation <= 4) {
    memcpy(&a, f->native.objects + USERS - OBJECTS, 4);
    memcpy(&b, f->native.objects + USERS + 4 - OBJECTS, 4);
    put32(&f->native, USERS, b);
    put32(&f->native, USERS + 4, a);
    /* Keep the selected pointer fixed, updating its canonical position. */
    put32(&f->native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, a == track_address(0) ? 1 : 0);
  } else {
    uint32_t field = track_address(0) + NATIVE_PROGRAM_OFFSET;
    memcpy(&a, f->native.objects + field - OBJECTS, 4);
    put32(&f->native, field, a == OBJECTS + 0xb0000 ? OBJECTS + 0xa8000 : OBJECTS + 0xb0000);
  }
}

int main(int argc, char **argv) {
  (void)validate_target;
  struct identity_fixture *f = calloc(1, sizeof *f);
  assert(f);
  state_init(f);
  inventory(f, 7, 1, 5);
  const char *families[] = {
    "plugin",
    "drum",
    "keygroup",
    "clip",
    "midi",
    "cv",
    "unknown",
    "audio",
    "input",
    "output",
    "master-output",
    "return",
    "submix"};
  for (unsigned i = 0; i < 13; i++)
    put32(&f->native, OBJECTS + 0xb0000 + i * 0x400, type(families[i]));
  selected(&f->native, 7);
  struct mpc_executor *executor = open_executor(f);
  struct mpc_state_snapshot *first = capture(f, executor, MPC_NATIVE_OK);
  assert(first->track_count == 8 && first->bus_count == 5);
  assert(first->selected_track == first->tracks[7].identity.id);
  for (unsigned i = 0; i < 8; i++) {
    assert(first->tracks[i].identity.index == i && !strcmp(first->tracks[i].type, families[i]));
    assert(first->tracks[i].known == (i < 2 ? 15u : 0u));
  }
  assert(first->tracks[7].collection == MPC_STATE_AUDIO && !first->tracks[7].collection_index);
  for (unsigned i = 0; i < 5; i++)
    assert(
      !first->buses[i].identity.id && first->buses[i].collection == MPC_STATE_MASTER &&
      !strcmp(first->buses[i].type, families[i + 8]));
  if (argc == 2 && !strcmp(argv[1], "--snapshot-wire")) {
    assert(fwrite(first, sizeof *first, 1, stdout) == 1);
    goto done;
  }
  struct mpc_identity_snapshot ids = snapshot(f, executor);
  struct mpc_executor_identity audio_ref = handle(&ids, 7);
  finish(f, submit(executor, MPC_EXECUTOR_SET_MUTE, audio_ref), MPC_NATIVE_UNSUPPORTED_TRACK_TYPE);
  assert(!f->native.setters);
  /* Interleave audio before MIDI; legacy collection indices stay unchanged. */
  put32(&f->native, USERS, track_address(7));
  for (unsigned i = 0; i < 7; i++)
    put32(&f->native, USERS + (i + 1) * 4, track_address(i));
  put32(&f->native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 0);
  struct mpc_state_snapshot *next = capture(f, executor, MPC_NATIVE_OK);
  assert(next->tracks[0].identity.id == audio_ref.track && !next->tracks[0].identity.index);
  assert(
    next->tracks[1].identity.id == first->tracks[0].identity.id &&
    !next->tracks[1].collection_index);
  free(next);
  /* Rename and Program replacement preserve the Track incarnation. */
  put32(&f->native, track_address(7) + NATIVE_TRACK_NAME, OBJECTS + 0x15001);
  put32(&f->native, track_address(7) + NATIVE_PROGRAM_OFFSET, OBJECTS + 0xb8000);
  put32(&f->native, OBJECTS + 0xb8000, type("audio"));
  next = capture(f, executor, MPC_NATIVE_OK);
  assert(next->tracks[0].identity.id == audio_ref.track);
  free(next);
  /* Address reuse is observed even when both lifetime events precede capture. */
  destroy_object(f, track_address(7));
  finish(f, submit(executor, MPC_EXECUTOR_SET_SOLO, audio_ref), MPC_NATIVE_STALE_TARGET);
  next = capture(f, executor, MPC_NATIVE_OK);
  assert(next->tracks[0].identity.id != audio_ref.track);
  free(next);
  /* Absence from MIDI never converts a user track into a bus. */
  vector(&f->native, NATIVE_AUDIO_BEGIN, AUDIO, 0, 1);
  assert(!capture(f, executor, MPC_NATIVE_READ_FAILED));
  vector(&f->native, NATIVE_AUDIO_BEGIN, AUDIO, 1, 1);
  put32(&f->native, MASTERS, track_address(7));
  assert(!capture(f, executor, MPC_NATIVE_READ_FAILED));
  put32(&f->native, MASTERS, track_address(8));
  /* Deletion and later recreation retire a reference even without observed ABA. */
  ids = snapshot(f, executor);
  audio_ref = handle(&ids, 0);
  inventory(f, 7, 0, 5);
  next = capture(f, executor, MPC_NATIVE_OK);
  assert(next->track_count == 7);
  free(next);
  finish(f, submit(executor, MPC_EXECUTOR_SET_SOLO, audio_ref), MPC_NATIVE_STALE_TARGET);
  inventory(f, 7, 1, 5);
  next = capture(f, executor, MPC_NATIVE_OK);
  assert(next->tracks[7].identity.id != audio_ref.track);
  free(next);
  /* In-place project reload retires audio references with the rest of the registry. */
  ids = snapshot(f, executor);
  audio_ref = handle(&ids, 7);
  f->project_watch->changed = true;
  finish(f, submit(executor, MPC_EXECUTOR_SET_SOLO, audio_ref), MPC_NATIVE_STALE_TARGET);
  next = capture(f, executor, MPC_NATIVE_OK);
  assert(next->project != ids.project && next->tracks[7].identity.id != audio_ref.track);
  free(next);
  /* Order and Program association changes are fenced, including repeated races. */
  f->before_read = mutate;
  put32(&f->native, OBJECTS + 0xa8000, type("plugin"));
  for (unsigned mutation = 3; mutation <= 6; mutation++) {
    inventory(f, 7, 1, 5);
    selected(&f->native, 0);
    f->state_mutation = mutation;
    f->mutations = 0;
    next = capture(f, executor, mutation % 2 ? MPC_NATIVE_OK : MPC_NATIVE_STATE_UNSTABLE);
    if (mutation % 2) {
      assert(next && next->attempts == 2);
      free(next);
    } else
      assert(!next && f->mutations == MPC_STATE_ATTEMPTS);
    f->state_mutation = 0;
  }
  f->before_read = NULL;
  /* All supported capacities, including entries beyond the old 128-slot loop. */
  inventory(f, 128, 8, 128);
  /* Exercise all 265 simultaneous lifetime watches, not only track storage. */
  uint32_t sequence_base = OBJECTS + 0xf4000;
  for (unsigned i = 0; i < MPC_IDENTITY_MAX_SEQUENCES; i++) {
    uint32_t address = sequence_base + i * 0x300;
    memcpy(f->native.objects + address - OBJECTS, f->native.objects + SEQUENCE - OBJECTS, 0x300);
    put32(&f->native, SEQUENCES + i * 4, address);
  }
  put32(&f->native, OBJECTS + NATIVE_SEQUENCE_COUNT, MPC_IDENTITY_MAX_SEQUENCES);
  put32(&f->native, OBJECTS + NATIVE_SELECTED_SEQUENCE, sequence_base);
  put32(&f->native, OBJECTS + NATIVE_SELECTED_SEQUENCE_MIRROR, sequence_base);
  selected(&f->native, 135);
  next = capture(f, executor, MPC_NATIVE_OK);
  assert(
    next->track_count == 136 && next->bus_count == 128 && next->sequence_count == 128 &&
    next->selected_track == next->tracks[135].identity.id);
  free(next);
  vector(&f->native, NATIVE_USER_BEGIN, USERS, 137, 256);
  assert(!capture(f, executor, MPC_NATIVE_READ_FAILED));
  inventory(f, 128, 8, 128);
  vector(&f->native, NATIVE_AUDIO_BEGIN, AUDIO, 9, 9);
  assert(!capture(f, executor, MPC_NATIVE_READ_FAILED));
  inventory(f, 128, 8, 128);
  vector(&f->native, NATIVE_MIDI_BEGIN, MIDI, 129, 129);
  assert(!capture(f, executor, MPC_NATIVE_READ_FAILED));
  inventory(f, 128, 8, 128);
  vector(&f->native, NATIVE_MASTER_BEGIN, MASTERS, 129, 129);
  assert(!capture(f, executor, MPC_NATIVE_READ_FAILED));
  inventory(f, 0, 0, 0);
  next = capture(f, executor, MPC_NATIVE_OK);
  assert(!next->track_count && !next->bus_count && !next->selected_track);
  free(next);
  puts("mixed_inventory=passed");
done:
  free(first);
  close_executor(f, executor);
  assert(!f->native.setters);
  pthread_cond_destroy(&f->changed);
  pthread_mutex_destroy(&f->mutex);
  free(f);
  return 0;
}
