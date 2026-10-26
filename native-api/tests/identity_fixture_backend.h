#ifndef MPC_IDENTITY_FIXTURE_BACKEND_H
#define MPC_IDENTITY_FIXTURE_BACKEND_H

#include "native_fixture_backend.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

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
  uint32_t subjects[MPC_IDENTITY_MAX_TRACKS + MPC_IDENTITY_MAX_SEQUENCES + 1];
  struct mpc_native_lifetime *watches[MPC_IDENTITY_MAX_TRACKS + MPC_IDENTITY_MAX_SEQUENCES + 1];
  struct mpc_native_sequence_watch *sequence_watches[128];
  struct mpc_native_project_watch *project_watch;
  unsigned registered, removed, expired;
  unsigned state_mutation, mutations;
  void (*before_read)(struct identity_fixture *, uintptr_t);
};
static _Thread_local bool ui;

static inline struct timespec deadline(void) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += 5;
  return until;
}

static inline bool on_identity_ui(void *unused) {
  (void)unused;
  return ui;
}

static inline bool enqueue(void *data, uintptr_t address, struct mpc_native_function *function) {
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

static inline void pump(struct identity_fixture *fixture) {
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

static inline int identity_read(void *data, uintptr_t address, void *output, size_t length) {
  struct identity_fixture *fixture = data;
  if (fixture->before_read)
    fixture->before_read(fixture, address);
  if (address == TRACK + NATIVE_TRACK_NAME && fixture->state_mutation) {
    unsigned mutation = fixture->state_mutation;
    if (mutation != 2)
      fixture->state_mutation = 0;
    fixture->mutations++;
    if (mutation == 3)
      put32(
        &fixture->native,
        COMPONENT + NATIVE_COMPONENT_ENGINE + NATIVE_ENGINE_PROJECT,
        PROJECT2);
    else {
      uint32_t current;
      memcpy(&current, fixture->native.objects + NATIVE_SELECTED_TRACK, 4);
      uint32_t selected = current == TRACK ? TRACK2 : TRACK;
      put32(&fixture->native, OBJECTS + NATIVE_SELECTED_TRACK, selected);
      put32(&fixture->native, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, selected);
      put32(&fixture->native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, selected == TRACK ? 0 : 1);
    }
  }
  if (address == IMAGE + NATIVE_APPLICATION_SLOT && length == 4) {
    uint32_t app = APP;
    memcpy(output, &app, 4);
    return 0;
  }
  return read_memory(data, address, output, length);
}

static inline void expired(struct mpc_native_lifetime *lifetime) {
  __atomic_store_n(&lifetime->alive, false, __ATOMIC_RELEASE);
}
static void (*const vtable[])(struct mpc_native_lifetime *) = {expired, expired, expired};

static inline enum mpc_native_error
watch(void *data, uint32_t subject, struct mpc_native_lifetime *lifetime) {
  struct identity_fixture *fixture = data;
  assert(ui);
  for (unsigned i = 0; i < MPC_IDENTITY_MAX_TRACKS + MPC_IDENTITY_MAX_SEQUENCES + 1; i++) {
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

static inline void unwatch(void *data, struct mpc_native_lifetime *lifetime) {
  struct identity_fixture *fixture = data;
  assert(ui);
  for (unsigned i = 0; i < MPC_IDENTITY_MAX_TRACKS + MPC_IDENTITY_MAX_SEQUENCES + 1; i++)
    if (fixture->watches[i] == lifetime) {
      fixture->watches[i] = NULL;
      fixture->removed++;
    }
  lifetime->alive = false;
}

static inline void destroy_object(struct identity_fixture *fixture, uint32_t object) {
  /* Destruction and recreation happen between UI samples; no pointer scan can
   * distinguish them. The native destruction observer must remember the event. */
  for (unsigned i = 0; i < MPC_IDENTITY_MAX_TRACKS + MPC_IDENTITY_MAX_SEQUENCES + 1; i++)
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

static inline void project_changed(struct mpc_native_project_watch *watch, uint32_t object) {
  (void)object;
  watch->changed = true;
}
static void (*const project_vtable[])(struct mpc_native_project_watch *, uint32_t) = {
  project_changed};

static inline enum mpc_native_error
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

static inline enum mpc_native_error
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

static inline void sequence_event(struct mpc_native_sequence_watch *watch, uint32_t sequence) {
  assert(watch->sequence == sequence);
  watch->changed = true;
}
static void (*const sequence_vtable[])(struct mpc_native_sequence_watch *, uint32_t) = {
  sequence_event};

static inline enum mpc_native_error
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

static inline enum mpc_native_error
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

static inline void select_project(struct identity_fixture *fixture, uint32_t project) {
  put32(&fixture->native, COMPONENT + NATIVE_COMPONENT_ENGINE + NATIVE_ENGINE_PROJECT, project);
}

static inline void init(struct identity_fixture *fixture) {
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

static inline struct mpc_executor *open_executor(struct identity_fixture *fixture) {
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

static inline struct mpc_executor_job *submit(
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

static inline struct mpc_executor_result finish(
  struct identity_fixture *fixture, struct mpc_executor_job *job, enum mpc_native_error expected) {
  pump(fixture);
  struct mpc_executor_result result;
  assert(mpc_executor_wait(job, 1000, &result) == expected);
  assert(result.state == MPC_EXECUTOR_COMPLETED);
  mpc_executor_release(job);
  return result;
}

static inline struct mpc_identity_snapshot
snapshot(struct identity_fixture *fixture, struct mpc_executor *executor) {
  return finish(
           fixture,
           submit(executor, MPC_EXECUTOR_IDENTITIES, (struct mpc_executor_identity){0}),
           MPC_NATIVE_OK)
    .identities;
}

static inline struct mpc_executor_identity
handle(const struct mpc_identity_snapshot *snapshot, unsigned track) {
  struct mpc_executor_identity identity = {
    .project = snapshot->project,
    .project_generation = snapshot->project_generation,
    .track = snapshot->tracks[track].id,
    .track_generation = snapshot->tracks[track].generation};
  memcpy(identity.instance_id, snapshot->instance_id, 37);
  return identity;
}

static inline void close_executor(struct identity_fixture *fixture, struct mpc_executor *executor) {
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

static inline void state_init(struct identity_fixture *fixture) {
  init(fixture);
  struct fixture *f = &fixture->native;
  put32(f, POOL + NATIVE_ALL_BEGIN, VECTOR);
  put32(f, POOL + NATIVE_ALL_END, VECTOR + 8);
  put32(f, POOL + NATIVE_ALL_CAPACITY, VECTOR + 8);
  put32(f, POOL + NATIVE_USER_END, VECTOR + 8);
  put32(f, POOL + NATIVE_USER_CAPACITY, VECTOR + 8);
  put32(f, POOL + NATIVE_MIDI_END, VECTOR + 8);
  put32(f, POOL + NATIVE_MIDI_CAPACITY, VECTOR + 8);
  put32(f, VECTOR + 4, TRACK2);
  put32(f, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, TRACK);
  put32(f, OBJECTS + NATIVE_SELECTED_SEQUENCE_MIRROR, SEQUENCE);
  put32(f, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 0);
  uint32_t name = OBJECTS + 0x15000;
  memcpy(f->objects + name - OBJECTS, "Live \"é\"\n", 11);
  put32(f, TRACK + NATIVE_TRACK_NAME, name);
  put32(f, TRACK2 + NATIVE_TRACK_NAME, name);
  put32(f, SEQUENCE + NATIVE_SEQUENCE_NAME, name);
  put32(f, SEQUENCE2 + NATIVE_SEQUENCE_NAME, name);
  float bpm = 123.5f;
  memcpy(f->objects + SEQUENCE + NATIVE_SEQUENCE_BPM - OBJECTS, &bpm, 4);
  memcpy(f->objects + SEQUENCE2 + NATIVE_SEQUENCE_BPM - OBJECTS, &bpm, 4);
  put_bool(f, SEQUENCE + NATIVE_SEQUENCE_USED, 1);
  put_bool(f, TRACK + native_bindings[2].requested, 1);
  /* Requested and audio copies deliberately disagree. */
  put_bool(f, PROGRAM + native_bindings[1].requested, 1);
  put_bool(f, PROGRAM + NATIVE_SILENCED_OFFSET, 1);
  memcpy(f->objects + PROJECT2 - OBJECTS, f->objects, 0x1000);
}

#endif
