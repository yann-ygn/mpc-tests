#include "native_fixture_backend.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

/* An explicitly pumped fake JUCE queue: no native entry points.
 * Captures use the production manager/copy/destruction protocol. */
struct harness {
  struct fixture native;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  struct mpc_native_function pending;
  bool has_pending, reject, reject_after_copy, block_setter, entered, resume;
  unsigned posts, resolves, reject_resolution;
  uint64_t generation;
};
static _Thread_local bool is_ui;
static const struct mpc_executor_identity handle = {17, 3, 29, 5};

static struct timespec test_deadline(void) {
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += 5;
  return until;
}

static void init(struct harness *h) {
  memset(h, 0, sizeof *h);
  reset(&h->native);
  h->generation = handle.project_generation;
  assert(!pthread_mutex_init(&h->mutex, NULL));
  assert(!pthread_cond_init(&h->changed, NULL));
}

static void destroy(struct harness *h) {
  assert(!h->has_pending);
  assert(!pthread_cond_destroy(&h->changed));
  assert(!pthread_mutex_destroy(&h->mutex));
}

static bool on_executor_ui(void *unused) {
  (void)unused;
  return is_ui;
}

static bool post(void *data, uintptr_t address, struct mpc_native_function *function) {
  struct harness *h = data;
  assert(!is_ui && address == IMAGE + NATIVE_CALL_ASYNC);
  pthread_mutex_lock(&h->mutex);
  if (h->reject) {
    pthread_mutex_unlock(&h->mutex);
    return false;
  }
  assert(!h->has_pending);
  h->pending = *function;
  function->manager(&h->pending, function, 2);
  h->has_pending = true;
  h->posts++;
  pthread_cond_broadcast(&h->changed);
  bool accepted = !h->reject_after_copy;
  pthread_mutex_unlock(&h->mutex);
  return accepted;
}

static struct mpc_native_function take(struct harness *h) {
  struct timespec until = test_deadline();
  pthread_mutex_lock(&h->mutex);
  while (!h->has_pending)
    assert(pthread_cond_timedwait(&h->changed, &h->mutex, &until) != ETIMEDOUT);
  struct mpc_native_function function = h->pending;
  h->has_pending = false;
  pthread_mutex_unlock(&h->mutex);
  return function; /* Move, not an uncounted std::function copy. */
}

static void drop(struct mpc_native_function *function) {
  function->manager(function, function, 3);
  memset(function, 0, sizeof *function);
}

static void run_function(struct mpc_native_function *function) {
  is_ui = true;
  function->invoke(function);
  is_ui = false;
  drop(function);
}

static void run(struct harness *h) {
  struct mpc_native_function function = take(h);
  run_function(&function);
}

static enum mpc_native_error resolve(
  void *data, const struct mpc_executor_identity *identity, struct mpc_native_target *output) {
  struct harness *h = data;
  assert(is_ui);
  h->resolves++;
  if (
    memcmp(identity, &handle, sizeof handle) || identity->project_generation != h->generation ||
    h->resolves == h->reject_resolution)
    return MPC_NATIVE_STALE_TARGET;
  *output = target;
  return validate_target(&h->native, output);
}

static enum mpc_native_error
blocking_setter(void *data, uintptr_t function, uintptr_t property, bool value) {
  struct harness *h = data;
  assert(is_ui);
  pthread_mutex_lock(&h->mutex);
  h->entered = true;
  pthread_cond_broadcast(&h->changed);
  struct timespec until = test_deadline();
  while (h->block_setter && !h->resume)
    assert(pthread_cond_timedwait(&h->changed, &h->mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&h->mutex);
  return setter(&h->native, function, property, value);
}

static struct mpc_executor *open_executor(struct harness *h, unsigned capacity, bool resolver) {
  struct mpc_native_io backend = io(&h->native);
  backend.on_ui_thread = on_executor_ui;
  backend.post = post;
  backend.setter = blocking_setter;
  struct mpc_executor_config config =
    {MPC_EXECUTOR_ABI, sizeof config, capacity, h, resolver ? resolve : NULL};
  struct mpc_executor *executor;
  assert(mpc_executor_open_backend(&backend, &config, &executor) == MPC_NATIVE_OK);
  return executor;
}

static struct mpc_executor *ready(struct harness *h, unsigned capacity) {
  struct mpc_executor *executor = open_executor(h, capacity, true);
  run(h);
  assert(mpc_executor_wait_ready(executor, 1000) == MPC_NATIVE_OK);
  return executor;
}

static struct mpc_executor_job *
submit(struct mpc_executor *executor, enum mpc_executor_operation op) {
  struct mpc_executor_request request = {.operation = op, .identity = handle, .desired = true};
  struct mpc_executor_job *job;
  assert(mpc_executor_submit(executor, &request, &job) == MPC_NATIVE_OK);
  return job;
}

static void close_executor(struct mpc_executor *executor, struct harness *h) {
  assert(mpc_executor_shutdown(executor, 1000) == MPC_NATIVE_OK);
  assert(mpc_executor_close(executor) == MPC_NATIVE_OK);
  destroy(h);
}

static void startup(void) {
  struct harness h;
  init(&h);
  struct mpc_native_io backend = io(&h.native);
  struct mpc_executor_config config = {MPC_EXECUTOR_ABI, sizeof config, 0, &h, resolve};
  struct mpc_executor *executor = (void *)1;
  assert(mpc_executor_open_backend(&backend, &config, &executor) == MPC_NATIVE_INVALID_ARGUMENT);
  assert(!executor);
  config.capacity = MPC_EXECUTOR_MAX_JOBS + 1;
  assert(mpc_executor_open_backend(&backend, &config, &executor) == MPC_NATIVE_INVALID_ARGUMENT);
  config.capacity = 1;
  config.abi_version++;
  assert(mpc_executor_open_backend(&backend, &config, &executor) == MPC_NATIVE_ABI_MISMATCH);
  config.abi_version--;
  config.struct_bytes--;
  assert(mpc_executor_open_backend(&backend, &config, &executor) == MPC_NATIVE_ABI_MISMATCH);
  config.struct_bytes++;
  h.native.corrupt = 0;
  assert(
    mpc_executor_open_backend(&backend, &config, &executor) == MPC_NATIVE_FINGERPRINT_MISMATCH);
  assert(!executor);
  destroy(&h);

  init(&h);
  executor = open_executor(&h, 1, true);
  struct mpc_native_function function = take(&h);
  assert(mpc_executor_wait_ready(executor, 0) == MPC_NATIVE_TIMED_OUT);
  struct mpc_executor_request request = {.identity = handle};
  struct mpc_executor_job *job;
  assert(mpc_executor_submit(executor, &request, &job) == MPC_NATIVE_NOT_READY && !job);
  assert(mpc_executor_close(executor) == MPC_NATIVE_NOT_READY);
  assert(mpc_executor_shutdown(executor, 0) == MPC_NATIVE_TIMED_OUT);
  run_function(&function); /* Delayed startup after shutdown cannot become ready. */
  assert(mpc_executor_wait_ready(executor, 0) == MPC_NATIVE_SHUTTING_DOWN);
  close_executor(executor, &h);

  init(&h);
  h.reject = true;
  executor = open_executor(&h, 1, true);
  assert(mpc_executor_wait_ready(executor, 1000) == MPC_NATIVE_DISPATCH_FAILED);
  close_executor(executor, &h);

  init(&h);
  h.reject_after_copy = true;
  executor = open_executor(&h, 1, true);
  function = take(&h);
  assert(mpc_executor_wait_ready(executor, 1000) == MPC_NATIVE_DISPATCH_FAILED);
  assert(mpc_executor_shutdown(executor, 0) == MPC_NATIVE_TIMED_OUT);
  assert(mpc_executor_close(executor) == MPC_NATIVE_NOT_READY);
  drop(&function); /* Even a rejected post cannot free a retained capture. */
  close_executor(executor, &h);

  init(&h);
  executor = open_executor(&h, 1, true);
  function = take(&h);
  function.invoke(&function); /* Deliberately wrong dispatch thread. */
  drop(&function);
  assert(mpc_executor_wait_ready(executor, 1000) == MPC_NATIVE_WRONG_THREAD);
  assert(!h.native.object_reads && !h.native.setters && !h.resolves);
  close_executor(executor, &h);

  init(&h);
  executor = open_executor(&h, 1, true);
  function = take(&h);
  drop(&function); /* Message queue discarded the probe without invocation. */
  assert(mpc_executor_wait_ready(executor, 1000) == MPC_NATIVE_DISPATCH_FAILED);
  close_executor(executor, &h);

  init(&h);
  executor = open_executor(&h, 1, false);
  run(&h);
  assert(mpc_executor_wait_ready(executor, 1000) == MPC_NATIVE_OK);
  assert(mpc_executor_submit(executor, &request, &job) == MPC_NATIVE_NOT_READY);
  struct mpc_executor_status status;
  assert(mpc_executor_get_status(executor, &status) == MPC_NATIVE_OK);
  assert(!status.adapter.target_validator_present);
  close_executor(executor, &h);
}

static void typed_operations(void) {
  struct harness h;
  init(&h);
  h.native.propagate_audio = false;
  struct mpc_executor *executor = ready(&h, 4);
  struct mpc_executor_job *jobs[4];
  for (unsigned i = 0; i < 4; i++)
    jobs[i] = submit(executor, (enum mpc_executor_operation)i);
  struct mpc_executor_request request = {.identity = handle};
  struct mpc_executor_job *overflow;
  assert(mpc_executor_submit(executor, &request, &overflow) == MPC_NATIVE_QUEUE_FULL && !overflow);
  is_ui = true;
  struct mpc_executor_result result;
  assert(mpc_executor_wait(jobs[0], 0, &result) == MPC_NATIVE_WRONG_THREAD);
  assert(mpc_executor_wait_ready(executor, 0) == MPC_NATIVE_WRONG_THREAD);
  assert(mpc_executor_shutdown(executor, 0) == MPC_NATIVE_WRONG_THREAD);
  assert(mpc_executor_close(executor) == MPC_NATIVE_WRONG_THREAD);
  is_ui = false;
  run(&h);
  for (unsigned i = 0; i < 4; i++) {
    assert(mpc_executor_wait(jobs[i], 0, &result) == MPC_NATIVE_OK);
    assert(result.state == MPC_EXECUTOR_COMPLETED);
    if (i) {
      assert(result.write.native_invoked && result.write.changed_known && result.write.changed);
      assert(!result.observed.audio_soloed && !result.observed.audio_record_armed);
    }
  }
  assert(h.native.setters == 3 && h.native.filters == 1);
  assert(mpc_executor_submit(executor, &request, &overflow) == MPC_NATIVE_QUEUE_FULL);
  assert(mpc_executor_shutdown(executor, 1000) == MPC_NATIVE_OK);
  assert(mpc_executor_close(executor) == MPC_NATIVE_NOT_READY); /* Client results still owned. */
  for (unsigned i = 0; i < 4; i++)
    mpc_executor_release(jobs[i]);
  close_executor(executor, &h);

  init(&h);
  executor = ready(&h, 3);
  /* FIFO acceptance order and copied preconditions, even though the producer
   * reuses/mutates the request storage before the delayed callback. */
  request = (struct mpc_executor_request){.operation = MPC_EXECUTOR_SET_MUTE,
                                          .identity = handle,
                                          .has_expected = true};
  for (unsigned i = 0; i < 3; i++) {
    request.desired = i != 1;
    request.expected = !request.desired;
    assert(mpc_executor_submit(executor, &request, &jobs[i]) == MPC_NATIVE_OK);
  }
  memset(&request, 0xff, sizeof request);
  run(&h);
  for (unsigned i = 0; i < 3; i++) {
    assert(mpc_executor_wait(jobs[i], 0, &result) == MPC_NATIVE_OK);
    assert(result.observed.muted == (i != 1));
    mpc_executor_release(jobs[i]);
  }
  assert(h.native.setters == 3);
  close_executor(executor, &h);
}

static void cancellation_and_captures(void) {
  struct harness h;
  init(&h);
  struct mpc_executor *executor = ready(&h, 2);
  struct mpc_executor_job *job = submit(executor, MPC_EXECUTOR_SET_MUTE);
  struct mpc_native_function function = take(&h), copy = function;
  function.manager(&copy, &function, 2); /* JUCE owns a second copy after invocation. */
  struct mpc_executor_result result;
  assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_CANCELLED);
  assert(result.state == MPC_EXECUTOR_CANCELLED && !result.write.native_invoked);
  assert(mpc_executor_cancel(job, &result) == MPC_NATIVE_CANCELLED);
  mpc_executor_release(job);
  /* Repeated queued disconnects reuse fixed slots, not native callback objects. */
  for (unsigned i = 0; i < 100; i++)
    mpc_executor_release(submit(executor, MPC_EXECUTOR_SET_MUTE));
  run_function(&function);
  assert(!h.resolves && !h.native.object_reads && !h.native.setters);
  assert(mpc_executor_shutdown(executor, 0) == MPC_NATIVE_TIMED_OUT);
  assert(mpc_executor_close(executor) == MPC_NATIVE_NOT_READY);
  drop(&copy);
  close_executor(executor, &h);

  init(&h);
  executor = ready(&h, 2);
  job = submit(executor, MPC_EXECUTOR_SET_MUTE);
  function = take(&h);
  assert(mpc_executor_shutdown(executor, 0) == MPC_NATIVE_TIMED_OUT);
  assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_CANCELLED);
  run_function(&function);
  assert(!h.native.setters && !h.resolves);
  mpc_executor_release(job);
  close_executor(executor, &h);

  init(&h);
  executor = ready(&h, 1);
  job = submit(executor, MPC_EXECUTOR_SET_MUTE);
  function = take(&h);
  drop(&function); /* Discarded work is failed, never silently retried. */
  assert(mpc_executor_wait(job, 1000, &result) == MPC_NATIVE_DISPATCH_FAILED);
  mpc_executor_release(job);
  close_executor(executor, &h);

  init(&h);
  executor = ready(&h, 1);
  job = submit(executor, MPC_EXECUTOR_READ_TRACK);
  function = take(&h);
  copy = function;
  function.manager(&copy, &function, 2);
  struct mpc_native_function metadata = function;
  function.manager(&metadata, &function, 0);
  assert(!metadata.data[0]);
  function.manager(&metadata, &function, 1);
  assert(metadata.data[0] == &function);
  run_function(&function);
  assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_OK);
  mpc_executor_release(job);
  job = submit(executor, MPC_EXECUTOR_READ_TRACK);
  struct mpc_executor_status status;
  assert(mpc_executor_get_status(executor, &status) == MPC_NATIVE_OK);
  assert(status.queued_jobs == 1 && status.callback_captures >= 1);
  drop(&copy); /* Last destructor wakes the scheduler for the next bounded batch. */
  run(&h);
  assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_OK);
  mpc_executor_release(job);
  close_executor(executor, &h);
}

static void dispatch_failures(void) {
  for (unsigned variant = 0; variant < 3; variant++) {
    struct harness h;
    init(&h);
    struct mpc_executor *executor = ready(&h, 1);
    if (variant == 0)
      h.reject = true;
    else if (variant == 1)
      h.native.corrupt = 0;
    struct mpc_executor_job *job = submit(executor, MPC_EXECUTOR_SET_MUTE);
    if (variant == 2) {
      struct mpc_native_function function = take(&h);
      function.invoke(&function); /* Wrong-thread delivery after a valid probe. */
      drop(&function);
    }
    struct mpc_executor_result result;
    enum mpc_native_error expected = variant == 0 ? MPC_NATIVE_DISPATCH_FAILED
      : variant == 1                              ? MPC_NATIVE_FINGERPRINT_MISMATCH
                                                  : MPC_NATIVE_WRONG_THREAD;
    assert(mpc_executor_wait(job, 1000, &result) == expected);
    assert(!h.resolves && !h.native.object_reads && !h.native.setters);
    mpc_executor_release(job);
    close_executor(executor, &h);
  }
}

static void stale_targets(void) {
  for (unsigned variant = 0; variant < 4; variant++) {
    struct harness h;
    init(&h);
    struct mpc_executor *executor = ready(&h, 1);
    struct mpc_executor_job *job = submit(executor, MPC_EXECUTOR_SET_MUTE);
    struct mpc_native_function function = take(&h);
    if (variant == 0)
      h.generation++; /* Replaced while queued, before first resolution. */
    else if (variant == 1)
      h.reject_resolution = 2; /* Revalidation inside adapter traversal. */
    else if (variant == 2)
      h.native.invalidate_after_set = true; /* Readback preserves uncertainty. */
    else
      h.native.corrupt = 0; /* Firmware guard rechecked before native traversal. */
    run_function(&function);
    struct mpc_executor_result result;
    assert(
      mpc_executor_wait(job, 0, &result) ==
      (variant == 3 ? MPC_NATIVE_FINGERPRINT_MISMATCH : MPC_NATIVE_STALE_TARGET));
    assert(result.write.native_invoked == (variant == 2));
    assert(!result.write.changed_known);
    assert(h.native.setters == (variant == 2 ? 1u : 0u));
    mpc_executor_release(job);
    close_executor(executor, &h);
  }
}

static void expiry_at_dispatch(void) {
  struct harness h;
  init(&h);
  struct mpc_executor *executor = ready(&h, 1);
  struct mpc_executor_request request = {
    .operation = MPC_EXECUTOR_SET_MUTE,
    .identity = handle,
    .desired = true,
    .queue_timeout_ms = 1};
  struct mpc_executor_job *job;
  assert(mpc_executor_submit(executor, &request, &job) == MPC_NATIVE_OK);
  struct mpc_native_function function = take(&h);
  /* No waiter is running: expiry must be enforced by the UI callback itself,
   * even if a client thread has not been scheduled to cancel the job. */
  struct timespec pause = {0, 2000000};
  while (nanosleep(&pause, &pause) && errno == EINTR) {
  }
  run_function(&function);
  struct mpc_executor_result result;
  assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_CANCELLED);
  assert(!h.resolves && !h.native.object_reads && !h.native.setters);
  mpc_executor_release(job);
  job = submit(executor, MPC_EXECUTOR_SET_MUTE); /* Reused slot has no stale deadline. */
  run(&h);
  assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_OK);
  assert(h.native.setters == 1);
  mpc_executor_release(job);
  close_executor(executor, &h);
}

/* Model an intervening manual UI edit after acceptance but before dispatch.
 * The queued expected value is checked against current state even when that
 * edit already produced the desired value. A plain desired-state retry is a no-op. */
static void manual_edit_conflicts(void) {
  for (unsigned property = 0; property < 3; property++) {
    struct harness h;
    init(&h);
    struct mpc_executor *executor = ready(&h, 1);
    struct mpc_executor_request request = {
      .operation = MPC_EXECUTOR_SET_MUTE + property,
      .identity = handle,
      .desired = true,
      .has_expected = true,
      .expected = false};
    struct mpc_executor_job *job;
    assert(mpc_executor_submit(executor, &request, &job) == MPC_NATIVE_OK);
    struct mpc_native_function function = take(&h);
    const struct native_binding *binding = &native_bindings[property];
    unsigned owner = binding->owner_is_track ? TRACK : PROGRAM;
    put_bool(&h.native, owner + binding->requested, true);
    put_bool(&h.native, owner + binding->observed, true);
    run_function(&function);
    struct mpc_executor_result result;
    assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_PRECONDITION_FAILED);
    assert(result.state == MPC_EXECUTOR_COMPLETED && !result.write.native_invoked);
    assert(!h.native.setters && !h.native.filters);
    mpc_executor_release(job);
    request.has_expected = false;
    assert(mpc_executor_submit(executor, &request, &job) == MPC_NATIVE_OK);
    run(&h);
    assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_OK);
    assert(result.write.changed_known && !result.write.changed && !result.write.native_invoked);
    assert(!h.native.setters && !h.native.filters);
    mpc_executor_release(job);
    close_executor(executor, &h);
  }
}

struct invocation {
  struct mpc_native_function function;
};
static void *invoke_thread(void *data) {
  run_function(&((struct invocation *)data)->function);
  return NULL;
}

static void running_deadline(bool disconnect) {
  struct harness h;
  init(&h);
  h.block_setter = true;
  struct mpc_executor *executor = ready(&h, 1);
  struct mpc_executor_job *job = submit(executor, MPC_EXECUTOR_SET_MUTE);
  struct invocation invocation = {take(&h)};
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, invoke_thread, &invocation));
  pthread_mutex_lock(&h.mutex);
  struct timespec until = test_deadline();
  while (!h.entered)
    assert(pthread_cond_timedwait(&h.changed, &h.mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&h.mutex);
  struct mpc_executor_result result;
  assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_OUTCOME_UNKNOWN);
  assert(result.state == MPC_EXECUTOR_RUNNING);
  assert(mpc_executor_cancel(job, &result) == MPC_NATIVE_OUTCOME_UNKNOWN);
  assert(mpc_executor_shutdown(executor, 0) == MPC_NATIVE_TIMED_OUT);
  struct mpc_executor_request request = {.identity = handle};
  struct mpc_executor_job *rejected;
  assert(
    mpc_executor_submit(executor, &request, &rejected) == MPC_NATIVE_SHUTTING_DOWN && !rejected);
  assert(mpc_executor_close(executor) == MPC_NATIVE_NOT_READY);
  if (disconnect)
    mpc_executor_release(job);
  pthread_mutex_lock(&h.mutex);
  h.resume = true;
  pthread_cond_broadcast(&h.changed);
  pthread_mutex_unlock(&h.mutex);
  assert(!pthread_join(thread, NULL));
  if (!disconnect) {
    assert(mpc_executor_wait(job, 0, &result) == MPC_NATIVE_OK);
    assert(result.write.native_invoked && result.observed.muted);
    mpc_executor_release(job);
  }
  assert(h.native.setters == 1);
  close_executor(executor, &h);
}

struct producer {
  struct mpc_executor *executor;
  struct mpc_executor_job *job;
  pthread_barrier_t *barrier;
};
static void *produce(void *data) {
  struct producer *p = data;
  pthread_barrier_wait(p->barrier);
  struct mpc_executor_request request = {
    .operation = MPC_EXECUTOR_SET_MUTE,
    .identity = handle,
    .desired = true,
    .has_expected = true,
    .expected = false};
  assert(mpc_executor_submit(p->executor, &request, &p->job) == MPC_NATIVE_OK);
  return NULL;
}

static void concurrent_requests(void) {
  struct harness h;
  init(&h);
  struct mpc_executor *executor = ready(&h, 8);
  pthread_barrier_t barrier;
  assert(!pthread_barrier_init(&barrier, NULL, 8));
  struct producer producers[8];
  pthread_t threads[8];
  for (unsigned i = 0; i < 8; i++) {
    producers[i] = (struct producer){executor, NULL, &barrier};
    assert(!pthread_create(&threads[i], NULL, produce, &producers[i]));
  }
  for (unsigned i = 0; i < 8; i++)
    assert(!pthread_join(threads[i], NULL));
  run(&h);
  unsigned accepted = 0, rejected = 0;
  for (unsigned i = 0; i < 8; i++) {
    struct mpc_executor_result result;
    enum mpc_native_error error = mpc_executor_wait(producers[i].job, 0, &result);
    accepted += error == MPC_NATIVE_OK;
    rejected += error == MPC_NATIVE_PRECONDITION_FAILED;
    mpc_executor_release(producers[i].job);
  }
  assert(accepted == 1 && rejected == 7 && h.native.setters == 1);
  assert(!pthread_barrier_destroy(&barrier));
  close_executor(executor, &h);
}

int main(void) {
  manual_edit_conflicts();
  startup();
  typed_operations();
  cancellation_and_captures();
  dispatch_failures();
  stale_targets();
  expiry_at_dispatch();
  running_deadline(false);
  running_deadline(true);
  concurrent_requests();
  puts("executor lifecycle, typed operations and concurrency passed");
  return 0;
}
