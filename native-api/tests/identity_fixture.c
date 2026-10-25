#include "native_fixture_backend.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

enum {
  APP = OBJECTS + 0x8000,
  WINDOW = OBJECTS + 0x9000,
  COMPONENT = OBJECTS + 0xa000,
  SEQUENCES = OBJECTS + 0xb000,
  SEQUENCE = OBJECTS + 0xc000,
  SEQUENCE2 = OBJECTS + 0xd000,
  PROJECT2 = OBJECTS + 0xe000,
  TRACK2 = OBJECTS + 0x10000
};

struct identity_fixture {
  struct fixture native;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  struct mpc_native_function pending;
  bool queued;
  bool reject_post, drop_post, retain_sequence;
  uint32_t subjects[260];
  struct mpc_native_lifetime *watches[260];
  struct mpc_native_sequence_watch *sequence_watches[128];
  struct mpc_native_project_watch *project_watch;
  unsigned registered, removed, expired;
};
static _Thread_local bool ui;

static struct timespec deadline(void) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += 5;
  return until;
}

static bool on_identity_ui(void *unused) {
  (void)unused;
  return ui;
}

static bool enqueue(void *data, uintptr_t address, struct mpc_native_function *function) {
  struct identity_fixture *fixture = data;
  assert(!ui && address == IMAGE + NATIVE_CALL_ASYNC);
  pthread_mutex_lock(&fixture->mutex);
  if (fixture->reject_post || fixture->drop_post) {
    bool accepted = fixture->drop_post;
    pthread_mutex_unlock(&fixture->mutex);
    return accepted;
  }
  assert(!fixture->queued);
  fixture->pending = *function;
  function->manager(&fixture->pending, function, 2);
  fixture->queued = true;
  pthread_cond_broadcast(&fixture->changed);
  pthread_mutex_unlock(&fixture->mutex);
  return true;
}

static void pump(struct identity_fixture *fixture) {
  struct timespec until = deadline();
  pthread_mutex_lock(&fixture->mutex);
  while (!fixture->queued)
    assert(pthread_cond_timedwait(&fixture->changed, &fixture->mutex, &until) != ETIMEDOUT);
  struct mpc_native_function function = fixture->pending;
  fixture->queued = false;
  pthread_mutex_unlock(&fixture->mutex);
  ui = true;
  function.invoke(&function);
  ui = false;
  function.manager(&function, &function, 3);
}

static int identity_read(void *data, uintptr_t address, void *output, size_t length) {
  if (address == IMAGE + NATIVE_APPLICATION_SLOT && length == 4) {
    uint32_t app = APP;
    memcpy(output, &app, 4);
    return 0;
  }
  return read_memory(data, address, output, length);
}

static void expired(struct mpc_native_lifetime *lifetime) {
  __atomic_store_n(&lifetime->alive, false, __ATOMIC_RELEASE);
}
static void (*const vtable[])(struct mpc_native_lifetime *) = {expired, expired, expired};

static enum mpc_native_error
watch(void *data, uint32_t subject, struct mpc_native_lifetime *lifetime) {
  struct identity_fixture *fixture = data;
  assert(ui);
  for (unsigned i = 0; i < 260; i++) {
    if (fixture->watches[i])
      continue;
    fixture->subjects[i] = subject;
    fixture->watches[i] = lifetime;
    lifetime->vtable = vtable;
    lifetime->alive = true;
    fixture->registered++;
    return MPC_NATIVE_OK;
  }
  return MPC_NATIVE_NOT_READY;
}

static void unwatch(void *data, struct mpc_native_lifetime *lifetime) {
  struct identity_fixture *fixture = data;
  assert(ui);
  for (unsigned i = 0; i < 260; i++)
    if (fixture->watches[i] == lifetime) {
      fixture->watches[i] = NULL;
      fixture->removed++;
    }
  lifetime->alive = false;
}

static void destroy_object(struct identity_fixture *fixture, uint32_t object) {
  /* Destruction and recreation happen between UI samples; no pointer scan can
   * distinguish them. The native destruction observer must remember the event. */
  for (unsigned i = 0; i < 260; i++)
    if (fixture->watches[i] && fixture->subjects[i] == object + 4) {
      fixture->watches[i]->vtable[2](fixture->watches[i]);
      fixture->expired++;
    }
  for (unsigned i = 0; i < 128; i++)
    if (fixture->sequence_watches[i] && fixture->sequence_watches[i]->sequence == object)
      fixture->sequence_watches[i]->destruction_observers = NULL;
  if (fixture->project_watch && fixture->project_watch->project == object)
    fixture->project_watch->destruction_observers = NULL;
}

static void project_changed(struct mpc_native_project_watch *watch, uint32_t object) {
  (void)object;
  watch->changed = true;
}
static void (*const project_vtable[])(struct mpc_native_project_watch *, uint32_t) = {
  project_changed};

static enum mpc_native_error
project_watch(void *data, uint32_t project, struct mpc_native_project_watch *watch) {
  struct identity_fixture *fixture = data;
  assert(ui && !fixture->project_watch);
  fixture->project_watch = watch;
  watch->project = project;
  watch->vtable = project_vtable;
  watch->attached = true;
  watch->destruction_observers = (void *)watch;
  return MPC_NATIVE_OK;
}

static enum mpc_native_error
project_unwatch(void *data, struct mpc_native_project_watch *watch, bool object_alive) {
  struct identity_fixture *fixture = data;
  assert(ui && fixture->project_watch == watch);
  if (!object_alive)
    assert(!watch->destruction_observers);
  fixture->project_watch = NULL;
  watch->destruction_observers = NULL;
  watch->attached = false;
  return MPC_NATIVE_OK;
}

static void sequence_event(struct mpc_native_sequence_watch *watch, uint32_t sequence) {
  assert(watch->sequence == sequence);
  watch->changed = true;
}
static void (*const sequence_vtable[])(struct mpc_native_sequence_watch *, uint32_t) = {
  sequence_event};

static enum mpc_native_error
sequence_watch(void *data, uint32_t sequence, struct mpc_native_sequence_watch *watch) {
  struct identity_fixture *fixture = data;
  assert(ui);
  for (unsigned i = 0; i < 128; i++)
    if (!fixture->sequence_watches[i]) {
      fixture->sequence_watches[i] = watch;
      watch->sequence = sequence;
      watch->vtable = sequence_vtable;
      watch->attached = true;
      watch->destruction_observers = (void *)watch;
      return MPC_NATIVE_OK;
    }
  return MPC_NATIVE_NOT_READY;
}

static enum mpc_native_error
sequence_unwatch(void *data, struct mpc_native_sequence_watch *watch, bool object_alive) {
  struct identity_fixture *fixture = data;
  assert(ui);
  if (fixture->retain_sequence)
    return MPC_NATIVE_NOT_READY;
  if (!object_alive)
    assert(!watch->destruction_observers);
  for (unsigned i = 0; i < 128; i++)
    if (fixture->sequence_watches[i] == watch)
      fixture->sequence_watches[i] = NULL;
  watch->destruction_observers = NULL;
  watch->attached = false;
  return MPC_NATIVE_OK;
}

static void select_project(struct identity_fixture *fixture, uint32_t project) {
  put32(&fixture->native, COMPONENT + NATIVE_COMPONENT_ENGINE + NATIVE_ENGINE_PROJECT, project);
}

static void init(struct identity_fixture *fixture) {
  memset(fixture, 0, sizeof *fixture);
  reset(&fixture->native);
  assert(!pthread_mutex_init(&fixture->mutex, NULL));
  assert(!pthread_cond_init(&fixture->changed, NULL));
  struct fixture *native = &fixture->native;
  put32(native, APP, IMAGE + NATIVE_APPLICATION_VTABLE);
  put32(native, APP + NATIVE_APPLICATION_WINDOW, WINDOW);
  put32(native, WINDOW, IMAGE + NATIVE_WINDOW_VTABLE);
  put32(native, WINDOW + NATIVE_WINDOW_COMPONENT, COMPONENT);
  put32(native, COMPONENT, IMAGE + NATIVE_COMPONENT_VTABLE);
  put32(native, COMPONENT + NATIVE_COMPONENT_ENGINE, IMAGE + NATIVE_ENGINE_VTABLE);
  select_project(fixture, OBJECTS);
  put32(native, OBJECTS + NATIVE_SEQUENCE_ARRAY, SEQUENCES);
  put32(native, OBJECTS + NATIVE_SEQUENCE_CAPACITY, 136);
  put32(native, OBJECTS + NATIVE_SEQUENCE_COUNT, 2);
  put32(native, SEQUENCES, SEQUENCE);
  put32(native, SEQUENCES + 4, SEQUENCE2);
  put32(native, SEQUENCE, IMAGE + NATIVE_SEQUENCE_VTABLE);
  put32(native, SEQUENCE2, IMAGE + NATIVE_SEQUENCE_VTABLE);
  put32(native, OBJECTS + NATIVE_SELECTED_TRACK, TRACK);
  put32(native, OBJECTS + NATIVE_SELECTED_SEQUENCE, SEQUENCE);
  memcpy(native->objects + PROJECT2 - OBJECTS, native->objects, 0x1000);
  memcpy(native->objects + TRACK2 - OBJECTS, native->objects + TRACK - OBJECTS, 0x2000);
  /* The tests use typed reads of TRACK2; setters only target TRACK. */
}

static struct mpc_executor *open_executor(struct identity_fixture *fixture) {
  struct mpc_native_io backend = io(&fixture->native);
  backend.read = identity_read;
  backend.on_ui_thread = on_identity_ui;
  backend.post = enqueue;
  backend.watch = watch;
  backend.unwatch = unwatch;
  backend.sequence_watch = sequence_watch;
  backend.sequence_unwatch = sequence_unwatch;
  backend.project_watch = project_watch;
  backend.project_unwatch = project_unwatch;
  struct mpc_executor *executor;
  assert(mpc_executor_open_authoritative_backend(&backend, 8, &executor) == MPC_NATIVE_OK);
  pump(fixture);
  assert(mpc_executor_wait_ready(executor, 1000) == MPC_NATIVE_OK);
  return executor;
}

static struct mpc_executor_job *submit(
  struct mpc_executor *executor,
  enum mpc_executor_operation operation,
  struct mpc_executor_identity identity) {
  struct mpc_executor_request request = {
    .operation = operation,
    .identity = identity,
    .desired = true};
  struct mpc_executor_job *job;
  assert(mpc_executor_submit(executor, &request, &job) == MPC_NATIVE_OK);
  return job;
}

static struct mpc_executor_result finish(
  struct identity_fixture *fixture, struct mpc_executor_job *job, enum mpc_native_error expected) {
  pump(fixture);
  struct mpc_executor_result result;
  assert(mpc_executor_wait(job, 1000, &result) == expected);
  assert(result.state == MPC_EXECUTOR_COMPLETED);
  mpc_executor_release(job);
  return result;
}

static struct mpc_identity_snapshot
snapshot(struct identity_fixture *fixture, struct mpc_executor *executor) {
  return finish(
           fixture,
           submit(executor, MPC_EXECUTOR_IDENTITIES, (struct mpc_executor_identity){0}),
           MPC_NATIVE_OK)
    .identities;
}

static struct mpc_executor_identity
handle(const struct mpc_identity_snapshot *snapshot, unsigned track) {
  struct mpc_executor_identity identity = {
    .project = snapshot->project,
    .project_generation = snapshot->project_generation,
    .track = snapshot->tracks[track].id,
    .track_generation = snapshot->tracks[track].generation};
  memcpy(identity.instance_id, snapshot->instance_id, 37);
  return identity;
}

static void close_executor(struct identity_fixture *fixture, struct mpc_executor *executor) {
  /* No caller can free the DSO while native objects still hold observers. */
  assert(mpc_executor_shutdown(executor, 0) == MPC_NATIVE_TIMED_OUT);
  assert(mpc_executor_close(executor) == MPC_NATIVE_NOT_READY);
  pump(fixture);
  assert(mpc_executor_shutdown(executor, 1000) == MPC_NATIVE_OK);
  assert(mpc_executor_close(executor) == MPC_NATIVE_OK);
  assert(fixture->registered == fixture->removed);
  assert(!fixture->project_watch);
  for (unsigned i = 0; i < 128; i++)
    assert(!fixture->sequence_watches[i]);
}

int main(void) {
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
  put32(native, POOL + NATIVE_VECTOR_END, VECTOR + 8);
  put32(native, POOL + NATIVE_VECTOR_CAPACITY, VECTOR + 8);
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
