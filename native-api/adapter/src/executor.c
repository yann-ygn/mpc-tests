#include "private.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct mpc_executor_job {
  struct mpc_executor *owner;
  struct mpc_executor_job *next;
  struct mpc_executor_request request;
  struct mpc_executor_result result;
  struct timespec execution_deadline;
  bool client_owned, has_deadline;
};

struct mpc_executor {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t scheduler;
  struct mpc_native_adapter *adapter;
  struct mpc_executor_config config;
  struct mpc_executor_job *jobs, *head, *tail;
  enum mpc_executor_state state;
  enum mpc_native_error failure;
  unsigned live_jobs, queued_jobs, captures;
  uint32_t ui_thread_id;
  bool probe_posted, callback_invoked, exited, active;
  struct mpc_executor_identity identity;
};

static bool time_before(const struct timespec *left, const struct timespec *right) {
  return left->tv_sec < right->tv_sec ||
    (left->tv_sec == right->tv_sec && left->tv_nsec < right->tv_nsec);
}

/* Queue, slots, results and capture ownership share one lock. No native call or
 * condition wait occurs while the UI callback holds it. A slot cannot be reused
 * while running, even after its client disconnects. */
static void remove_queued(struct mpc_executor *executor, struct mpc_executor_job *job) {
  struct mpc_executor_job *previous = NULL;
  for (struct mpc_executor_job *p = executor->head; p; p = p->next) {
    if (p == job) {
      if (previous)
        previous->next = p->next;
      else
        executor->head = p->next;
      if (executor->tail == p)
        executor->tail = previous;
      p->next = NULL;
      executor->queued_jobs--;
      return;
    }
    previous = p;
  }
}

static void free_slot(struct mpc_executor_job *job) {
  job->result.state = 0;
  job->client_owned = false;
  job->owner->live_jobs--;
}

static void cancel_queued(struct mpc_executor *executor, enum mpc_native_error error) {
  while (executor->head) {
    struct mpc_executor_job *job = executor->head;
    remove_queued(executor, job);
    job->result.state =
      error == MPC_NATIVE_CANCELLED ? MPC_EXECUTOR_CANCELLED : MPC_EXECUTOR_COMPLETED;
    job->result.error = error;
  }
  pthread_cond_broadcast(&executor->changed);
}

static void fail(struct mpc_executor *executor, enum mpc_native_error error) {
  executor->failure = error;
  if (executor->state != MPC_EXECUTOR_STOPPING && executor->state != MPC_EXECUTOR_STOPPED)
    executor->state = MPC_EXECUTOR_FAILED;
  cancel_queued(executor, error);
}

/* The resolver is consulted at dispatch AND by each adapter traversal, including
 * post-setter readback. The active request stays copied and alive until return. */
static enum mpc_native_error revalidate(void *data, const struct mpc_native_target *target) {
  struct mpc_executor *executor = data;
  if (!executor->active || !executor->config.resolve)
    return MPC_NATIVE_NOT_READY;
  struct mpc_native_target current = {0};
  enum mpc_native_error error =
    executor->config.resolve(executor->config.userdata, &executor->identity, &current);
  if (error)
    return error;
  return current.project == target->project && current.track == target->track &&
      current.program == target->program
    ? MPC_NATIVE_OK
    : MPC_NATIVE_STALE_TARGET;
}

static struct mpc_executor_result
execute(struct mpc_executor *executor, const struct mpc_executor_request *request) {
  struct mpc_executor_result result = {.state = MPC_EXECUTOR_COMPLETED};
  struct mpc_native_target target = {0};
  executor->active = true;
  executor->identity = request->identity;
  result.error = executor->config.resolve(executor->config.userdata, &request->identity, &target);
  if (!result.error) {
    const bool *expected = request->has_expected ? &request->expected : NULL;
    switch (request->operation) {
    case MPC_EXECUTOR_READ_TRACK:
      result.error = mpc_native_read_track(executor->adapter, &target, &result.observed);
      break;
    case MPC_EXECUTOR_SET_MUTE:
      result.error =
        mpc_native_set_mute(executor->adapter, &target, request->desired, expected, &result.write);
      break;
    case MPC_EXECUTOR_SET_SOLO:
      result.error =
        mpc_native_set_solo(executor->adapter, &target, request->desired, expected, &result.write);
      break;
    case MPC_EXECUTOR_SET_RECORD_ARMED:
      result.error = mpc_native_set_record_armed(
        executor->adapter,
        &target,
        request->desired,
        expected,
        &result.write);
      break;
    }
    if (request->operation != MPC_EXECUTOR_READ_TRACK)
      result.observed = result.write.observed;
  }
  executor->active = false;
  return result;
}

static void invoke(const void *capture) {
  struct mpc_executor *executor = ((const struct mpc_native_function *)capture)->data[0];
  pthread_mutex_lock(&executor->mutex);
  executor->callback_invoked = true;
  if (!mpc_native_on_ui_thread(executor->adapter)) {
    fail(executor, MPC_NATIVE_WRONG_THREAD);
    pthread_mutex_unlock(&executor->mutex);
    return;
  }
  executor->ui_thread_id = (uint32_t)syscall(SYS_gettid);
  if (executor->state == MPC_EXECUTOR_STARTING) {
    executor->state = MPC_EXECUTOR_READY;
    pthread_cond_broadcast(&executor->changed);
  } else {
    /* At most capacity operations per message, even if clients replenish slots.
     * One capture group at a time prevents reentrant execution and bounds JUCE
     * queue ownership independently of client cancellations. */
    for (unsigned i = 0; i < executor->config.capacity; i++) {
      if (executor->state != MPC_EXECUTOR_READY || !executor->head)
        break;
      struct mpc_executor_job *job = executor->head;
      remove_queued(executor, job);
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (job->has_deadline && !time_before(&now, &job->execution_deadline)) {
        job->result.state = MPC_EXECUTOR_CANCELLED;
        job->result.error = MPC_NATIVE_CANCELLED;
        pthread_cond_broadcast(&executor->changed);
        continue;
      }
      job->result.state = MPC_EXECUTOR_RUNNING;
      pthread_mutex_unlock(&executor->mutex);
      struct mpc_executor_result result = execute(executor, &job->request);
      pthread_mutex_lock(&executor->mutex);
      job->result = result;
      if (!job->client_owned)
        free_slot(job);
      pthread_cond_broadcast(&executor->changed);
    }
  }
  pthread_mutex_unlock(&executor->mutex);
}

static int manager(void *output, const void *input, int operation) {
  const struct mpc_native_function *source = input;
  struct mpc_native_function *destination = output;
  struct mpc_executor *executor = source->data[0];
  if (operation == 0)
    destination->data[0] = NULL;
  else if (operation == 1)
    destination->data[0] = (void *)source;
  else if (operation == 2) {
    destination->data[0] = executor;
    destination->data[1] = NULL;
    pthread_mutex_lock(&executor->mutex);
    executor->captures++;
    pthread_mutex_unlock(&executor->mutex);
  } else if (operation == 3) {
    pthread_mutex_lock(&executor->mutex);
    executor->captures--;
    pthread_cond_broadcast(&executor->changed);
    pthread_mutex_unlock(&executor->mutex);
  }
  return 0;
}

static void *schedule(void *data) {
  struct mpc_executor *executor = data;
  pthread_setname_np(pthread_self(), "mpc executor");
  pthread_mutex_lock(&executor->mutex);
  for (;;) {
    if (
      !executor->captures && executor->probe_posted && !executor->callback_invoked &&
      (executor->state == MPC_EXECUTOR_STARTING || executor->state == MPC_EXECUTOR_READY))
      fail(executor, MPC_NATIVE_DISPATCH_FAILED);
    if (
      (executor->state == MPC_EXECUTOR_STOPPING || executor->state == MPC_EXECUTOR_FAILED) &&
      !executor->captures)
      break;
    bool probe = executor->state == MPC_EXECUTOR_STARTING && !executor->probe_posted;
    if (
      !executor->captures && (probe || (executor->state == MPC_EXECUTOR_READY && executor->head))) {
      executor->probe_posted = true;
      executor->callback_invoked = false;
      executor->captures = 1; /* Local function ownership, before posting. */
      struct mpc_native_function function = {{executor, NULL}, manager, invoke};
      pthread_mutex_unlock(&executor->mutex);
      enum mpc_native_error error = mpc_native_post(executor->adapter, &function);
      if (error) {
        pthread_mutex_lock(&executor->mutex);
        fail(executor, error);
        pthread_mutex_unlock(&executor->mutex);
      }
      if (function.manager)
        function.manager(&function, &function, 3);
      pthread_mutex_lock(&executor->mutex);
    } else
      pthread_cond_wait(&executor->changed, &executor->mutex);
  }
  executor->exited = true;
  if (executor->state == MPC_EXECUTOR_STOPPING)
    executor->state = MPC_EXECUTOR_STOPPED;
  pthread_cond_broadcast(&executor->changed);
  pthread_mutex_unlock(&executor->mutex);
  return NULL;
}

enum mpc_native_error mpc_executor_open_backend(
  const struct mpc_native_io *io,
  const struct mpc_executor_config *config,
  struct mpc_executor **output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  *output = NULL;
  if (!config)
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (config->abi_version != MPC_EXECUTOR_ABI || config->struct_bytes != sizeof *config)
    return MPC_NATIVE_ABI_MISMATCH;
  if (!config->capacity || config->capacity > MPC_EXECUTOR_MAX_JOBS)
    return MPC_NATIVE_INVALID_ARGUMENT;
  struct mpc_executor *executor = calloc(1, sizeof *executor);
  if (!executor)
    return MPC_NATIVE_NOT_READY;
  executor->config = *config;
  executor->state = MPC_EXECUTOR_STARTING;
  executor->jobs = calloc(config->capacity, sizeof *executor->jobs);
  if (!executor->jobs) {
    free(executor);
    return MPC_NATIVE_NOT_READY;
  }
  for (unsigned i = 0; i < config->capacity; i++)
    executor->jobs[i].owner = executor;
  enum mpc_native_error error = MPC_NATIVE_NOT_READY;
  if (pthread_mutex_init(&executor->mutex, NULL))
    goto free_jobs;
  pthread_condattr_t attributes;
  if (pthread_condattr_init(&attributes))
    goto free_mutex;
  int rc = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
  if (!rc)
    rc = pthread_cond_init(&executor->changed, &attributes);
  pthread_condattr_destroy(&attributes);
  if (rc)
    goto free_mutex;
  struct mpc_native_hooks hooks = {MPC_NATIVE_ABI, sizeof hooks, executor, revalidate};
  error = io ? mpc_native_open_backend(io, &hooks, &executor->adapter)
             : mpc_native_open(&hooks, &executor->adapter);
  if (error)
    goto free_condition;
  if (pthread_create(&executor->scheduler, NULL, schedule, executor)) {
    error = MPC_NATIVE_NOT_READY;
    mpc_native_close(executor->adapter);
    goto free_condition;
  }
  *output = executor;
  return MPC_NATIVE_OK;

free_condition:
  pthread_cond_destroy(&executor->changed);
free_mutex:
  pthread_mutex_destroy(&executor->mutex);
free_jobs:
  free(executor->jobs);
  free(executor);
  return error;
}

enum mpc_native_error
mpc_executor_open(const struct mpc_executor_config *config, struct mpc_executor **output) {
  return mpc_executor_open_backend(NULL, config, output);
}

static struct timespec deadline(unsigned milliseconds) {
  struct timespec until;
  clock_gettime(CLOCK_MONOTONIC, &until);
  until.tv_sec += milliseconds / 1000;
  until.tv_nsec += (long)(milliseconds % 1000) * 1000000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }
  return until;
}

static enum mpc_native_error wait_check(struct mpc_executor *executor, unsigned milliseconds) {
  if (!executor || milliseconds > MPC_EXECUTOR_MAX_WAIT_MS)
    return MPC_NATIVE_INVALID_ARGUMENT;
  return mpc_native_on_ui_thread(executor->adapter) ? MPC_NATIVE_WRONG_THREAD : MPC_NATIVE_OK;
}

enum mpc_native_error
mpc_executor_wait_ready(struct mpc_executor *executor, unsigned milliseconds) {
  enum mpc_native_error error = wait_check(executor, milliseconds);
  if (error)
    return error;
  struct timespec until = deadline(milliseconds);
  pthread_mutex_lock(&executor->mutex);
  while (executor->state == MPC_EXECUTOR_STARTING)
    if (pthread_cond_timedwait(&executor->changed, &executor->mutex, &until) == ETIMEDOUT)
      break;
  error = executor->state == MPC_EXECUTOR_READY ? MPC_NATIVE_OK
    : executor->state == MPC_EXECUTOR_STARTING  ? MPC_NATIVE_TIMED_OUT
    : executor->failure                         ? executor->failure
                                                : MPC_NATIVE_SHUTTING_DOWN;
  pthread_mutex_unlock(&executor->mutex);
  return error;
}

enum mpc_native_error
mpc_executor_get_status(struct mpc_executor *executor, struct mpc_executor_status *output) {
  if (!executor || !output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  memset(output, 0, sizeof *output);
  enum mpc_native_error error = mpc_native_get_status(executor->adapter, &output->adapter);
  pthread_mutex_lock(&executor->mutex);
  if (error)
    fail(executor, error);
  output->state = executor->state;
  output->failure = executor->failure;
  output->live_jobs = executor->live_jobs;
  output->queued_jobs = executor->queued_jobs;
  output->callback_captures = executor->captures;
  output->ui_thread_id = executor->ui_thread_id;
  /* The internal wrapper exists even in discovery-only mode. */
  output->adapter.target_validator_present = executor->config.resolve != NULL;
  pthread_mutex_unlock(&executor->mutex);
  return error;
}

enum mpc_native_error mpc_executor_submit(
  struct mpc_executor *executor,
  const struct mpc_executor_request *request,
  struct mpc_executor_job **output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  *output = NULL;
  if (
    !executor || !request || (unsigned)request->operation > MPC_EXECUTOR_SET_RECORD_ARMED ||
    request->queue_timeout_ms > MPC_EXECUTOR_MAX_WAIT_MS)
    return MPC_NATIVE_INVALID_ARGUMENT;
  pthread_mutex_lock(&executor->mutex);
  enum mpc_native_error error = MPC_NATIVE_OK;
  if (executor->state != MPC_EXECUTOR_READY)
    error = executor->failure                    ? executor->failure
      : executor->state == MPC_EXECUTOR_STARTING ? MPC_NATIVE_NOT_READY
                                                 : MPC_NATIVE_SHUTTING_DOWN;
  else if (!executor->config.resolve)
    error = MPC_NATIVE_NOT_READY;
  else if (executor->live_jobs == executor->config.capacity)
    error = MPC_NATIVE_QUEUE_FULL;
  if (!error) {
    for (unsigned i = 0; i < executor->config.capacity; i++) {
      struct mpc_executor_job *job = &executor->jobs[i];
      if (job->result.state)
        continue;
      job->request = *request;
      job->result = (struct mpc_executor_result){.state = MPC_EXECUTOR_QUEUED};
      job->has_deadline = request->queue_timeout_ms != 0;
      if (job->has_deadline)
        job->execution_deadline = deadline(request->queue_timeout_ms);
      job->client_owned = true;
      job->next = NULL;
      if (executor->tail)
        executor->tail->next = job;
      else
        executor->head = job;
      executor->tail = job;
      executor->live_jobs++;
      executor->queued_jobs++;
      *output = job;
      pthread_cond_broadcast(&executor->changed);
      break;
    }
  }
  pthread_mutex_unlock(&executor->mutex);
  return error;
}

static enum mpc_native_error
cancel(struct mpc_executor_job *job, struct mpc_executor_result *output) {
  if (job->result.state == MPC_EXECUTOR_QUEUED) {
    remove_queued(job->owner, job);
    job->result.state = MPC_EXECUTOR_CANCELLED;
    job->result.error = MPC_NATIVE_CANCELLED;
    pthread_cond_broadcast(&job->owner->changed);
  }
  *output = job->result;
  if (output->state == MPC_EXECUTOR_RUNNING)
    output->error = MPC_NATIVE_OUTCOME_UNKNOWN;
  return output->error;
}

enum mpc_native_error
mpc_executor_cancel(struct mpc_executor_job *job, struct mpc_executor_result *output) {
  if (!job || !output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  pthread_mutex_lock(&job->owner->mutex);
  enum mpc_native_error error = cancel(job, output);
  pthread_mutex_unlock(&job->owner->mutex);
  return error;
}

enum mpc_native_error mpc_executor_wait(
  struct mpc_executor_job *job, unsigned milliseconds, struct mpc_executor_result *output) {
  if (!job || !output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  struct mpc_executor *executor = job->owner;
  enum mpc_native_error error = wait_check(executor, milliseconds);
  if (error)
    return error;
  struct timespec until = deadline(milliseconds);
  pthread_mutex_lock(&executor->mutex);
  if (job->result.state == MPC_EXECUTOR_QUEUED) {
    if (!job->has_deadline || time_before(&until, &job->execution_deadline)) {
      job->execution_deadline = until;
      job->has_deadline = true;
    } else
      until = job->execution_deadline;
  }
  while (job->result.state == MPC_EXECUTOR_QUEUED || job->result.state == MPC_EXECUTOR_RUNNING)
    if (pthread_cond_timedwait(&executor->changed, &executor->mutex, &until) == ETIMEDOUT)
      break;
  error = cancel(job, output);
  pthread_mutex_unlock(&executor->mutex);
  return error;
}

void mpc_executor_release(struct mpc_executor_job *job) {
  if (!job)
    return;
  struct mpc_executor *executor = job->owner;
  pthread_mutex_lock(&executor->mutex);
  if (job->result.state == MPC_EXECUTOR_QUEUED)
    remove_queued(executor, job);
  job->client_owned = false;
  if (job->result.state != MPC_EXECUTOR_RUNNING)
    free_slot(job);
  pthread_cond_broadcast(&executor->changed);
  pthread_mutex_unlock(&executor->mutex);
}

enum mpc_native_error mpc_executor_shutdown(struct mpc_executor *executor, unsigned milliseconds) {
  enum mpc_native_error error = wait_check(executor, milliseconds);
  if (error)
    return error;
  struct timespec until = deadline(milliseconds);
  pthread_mutex_lock(&executor->mutex);
  executor->state = executor->exited ? MPC_EXECUTOR_STOPPED : MPC_EXECUTOR_STOPPING;
  cancel_queued(executor, MPC_NATIVE_CANCELLED);
  while (!executor->exited)
    if (pthread_cond_timedwait(&executor->changed, &executor->mutex, &until) == ETIMEDOUT)
      break;
  error = executor->exited ? MPC_NATIVE_OK : MPC_NATIVE_TIMED_OUT;
  pthread_mutex_unlock(&executor->mutex);
  return error;
}

enum mpc_native_error mpc_executor_close(struct mpc_executor *executor) {
  enum mpc_native_error error = wait_check(executor, 0);
  if (error)
    return error;
  pthread_mutex_lock(&executor->mutex);
  bool drained = executor->exited && !executor->captures && !executor->live_jobs;
  pthread_mutex_unlock(&executor->mutex);
  if (!drained)
    return MPC_NATIVE_NOT_READY;
  pthread_join(executor->scheduler, NULL);
  mpc_native_close(executor->adapter);
  pthread_cond_destroy(&executor->changed);
  pthread_mutex_destroy(&executor->mutex);
  free(executor->jobs);
  free(executor);
  return MPC_NATIVE_OK;
}
