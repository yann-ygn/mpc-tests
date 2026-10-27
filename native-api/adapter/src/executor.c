#include "private.h"
#include "command_private.h"
#include "mpc_family_profile.h"

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
  struct mpc_state_snapshot *snapshot;
  struct mpc_command_native_result *command;
  struct mpc_command_observation *flags;
  struct timespec execution_deadline;
  bool client_owned, has_deadline;
};

struct mpc_executor {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t scheduler;
  struct mpc_native_adapter *adapter;
  struct mpc_executor_config config;
  struct mpc_identity_registry *registry;
  struct mpc_executor_job *jobs, *head, *tail;
  enum mpc_executor_state state;
  enum mpc_native_error failure;
  unsigned live_jobs, queued_jobs, captures;
  uint32_t ui_thread_id;
  bool probe_posted, callback_invoked, exited, active;
  bool identities_detached, cleanup_rejected, cleanup_posted;
  struct mpc_executor_identity identity;
  void *command_owner;
  struct mpc_command_policy command_policy;
  struct mpc_command_native_result *command_context;
  enum mpc_command_operation command_property;
  enum mpc_mute_scope mute_scope;
  bool qualifying;
  struct mpc_mixer_request mixer;
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
  free(job->snapshot);
  job->snapshot = NULL;
  free(job->flags);
  job->flags = NULL;
  free(job->command);
  job->command = NULL;
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
static enum mpc_native_error resolve_target(
  struct mpc_executor *executor,
  const struct mpc_executor_identity *identity,
  struct mpc_native_target *target) {
  if (executor->registry) {
    unsigned stage = mpc_ui_enter(executor->adapter, MPC_UI_IDENTITY);
    enum mpc_native_error error = mpc_identity_resolve(executor->registry, identity, target);
    mpc_ui_leave(executor->adapter, stage);
    return error;
  }
  return executor->config.resolve
    ? executor->config.resolve(executor->config.userdata, identity, target)
    : MPC_NATIVE_NOT_READY;
}

static enum mpc_native_error qualify_command(struct mpc_executor *executor) {
  unsigned stage = mpc_ui_enter(executor->adapter, MPC_UI_POLICY);
  executor->qualifying = true;
  if (executor->command_property >= MPC_COMMAND_VOLUME) {
    enum mpc_native_error error = mpc_identity_mixer(
      executor->registry,
      &executor->identity,
      &executor->command_context->last_mixer);
    if (!error && !executor->command_context->last_mixer.known)
      error = MPC_NATIVE_UNSUPPORTED_TRACK_TYPE;
    if (!error && executor->command_context->last_mixer.generation != executor->mixer.generation)
      error = MPC_NATIVE_STALE_MIXER;
    if (error) {
      executor->qualifying = false;
      mpc_ui_leave(executor->adapter, stage);
      return error;
    }
  }
  enum mpc_native_error error = executor->command_policy.qualify
    ? executor->command_policy.qualify(
        executor->command_policy.userdata,
        executor->command_property)
    : MPC_NATIVE_NOT_READY;
  executor->qualifying = false;
  mpc_ui_leave(executor->adapter, stage);
  return error;
}

static enum mpc_native_error revalidate(void *data, const struct mpc_native_target *target) {
  struct mpc_executor *executor = data;
  if (!executor->active)
    return MPC_NATIVE_NOT_READY;
  struct mpc_native_target current = {0};
  enum mpc_native_error error = resolve_target(executor, &executor->identity, &current);
  if (error)
    return error;
  if (
    current.project != target->project || current.track != target->track ||
    current.program != target->program)
    return MPC_NATIVE_STALE_TARGET;
  return executor->command_context && !executor->qualifying ? qualify_command(executor)
                                                            : MPC_NATIVE_OK;
}

static struct mpc_executor_result command_execute(
  struct mpc_executor *executor,
  const struct mpc_executor_request *input,
  struct mpc_command_native_result *out) {
  struct mpc_executor_result result = {.state = MPC_EXECUTOR_COMPLETED, .command = out};
  out->readbacks_consistent = true;
  struct timespec dispatched;
  clock_gettime(CLOCK_MONOTONIC, &dispatched);
  out->dispatched_ms = (uint64_t)dispatched.tv_sec * 1000 + (uint64_t)dispatched.tv_nsec / 1000000;
  struct mpc_executor_request request = *input;
  unsigned property = (input->operation == MPC_EXECUTOR_COMMAND_OBSERVE ||
                       input->operation == MPC_EXECUTOR_COMMAND_MIXER)
    ? input->command_property
    : (unsigned)(input->operation - MPC_EXECUTOR_COMMAND_MUTE);
  executor->adapter->profile = &out->profile;
  unsigned stage = mpc_ui_enter(executor->adapter, MPC_UI_IDENTITY);
  result.error = mpc_identity_bind(executor->registry, &request.identity);
  mpc_ui_leave(executor->adapter, stage);
  if (result.error)
    goto done;
  executor->identity = request.identity;
  executor->active = true;
  executor->command_context = out;
  executor->command_property = (enum mpc_command_operation)property;
  executor->mute_scope = request.mute_scope;
  executor->mixer = request.mixer;
  result.error = qualify_command(executor);
  executor->active = false;
  if (result.error)
    goto done;
  bool writing = input->operation != MPC_EXECUTOR_COMMAND_OBSERVE;
  if (writing) {
    stage = mpc_ui_enter(executor->adapter, MPC_UI_BEFORE);
    result.error =
      mpc_identity_flags(executor->registry, 0, MPC_IDENTITY_MAX_TRACKS, 0, &out->before);
    mpc_ui_leave(executor->adapter, stage);
    if (result.error)
      goto done;
    out->before_known = true;
  }
  executor->active = true;
  executor->identity = request.identity;
  struct mpc_native_target target;
  result.error = resolve_target(executor, &request.identity, &target);
  if (!result.error) {
    const bool *expected = request.has_expected ? &request.expected : NULL;
    if (writing && property >= MPC_COMMAND_VOLUME)
      result.error = mpc_mixer_set(
        executor->adapter,
        &target,
        property,
        &request.mixer,
        request.desired,
        request.has_expected,
        request.expected,
        &result.write);
    else if (!writing)
      result.error = mpc_native_read_track(executor->adapter, &target, &result.observed);
    else if (property == MPC_COMMAND_MUTE)
      result.error =
        mpc_native_set_mute(executor->adapter, &target, request.desired, expected, &result.write);
    else if (property == MPC_COMMAND_SOLO)
      result.error =
        mpc_native_set_solo(executor->adapter, &target, request.desired, expected, &result.write);
    else
      result.error = mpc_native_set_record_armed(
        executor->adapter,
        &target,
        request.desired,
        expected,
        &result.write);
  }
  executor->active = false;
  if (writing)
    result.observed = result.write.observed;
  executor->active = true;
  enum mpc_native_error captured = qualify_command(executor);
  executor->active = false;
  if (!captured) {
    stage = mpc_ui_enter(executor->adapter, MPC_UI_AFTER);
    captured = mpc_identity_flags(executor->registry, 0, MPC_IDENTITY_MAX_TRACKS, 0, &out->after);
    mpc_ui_leave(executor->adapter, stage);
  }
  executor->active = true;
  if (!captured)
    captured = qualify_command(executor);
  executor->active = false;
  if (!result.error && captured)
    result.error = captured;
  if (
    !captured && out->after.project == request.identity.project &&
    out->after.generation == request.identity.project_generation) {
    out->after_known = true;
    /* The final live policy sample must agree with the completion observation
     * for the addressed operation. A Read automation/model or engine change
     * between these reads needs bounded verification, never another setter. */
    if (property >= MPC_COMMAND_VOLUME) {
      out->readbacks_consistent = false;
      for (unsigned i = 0; i < out->after.track_count; i++) {
        const struct mpc_command_track *sample = &out->after.tracks[i];
        if (sample->id != request.identity.track)
          continue;
        double live, sampled;
        out->readbacks_consistent = sample->mixer.generation == request.mixer.generation &&
          mpc_mixer_value(&out->last_mixer, property, request.mixer.index, &live) &&
          mpc_mixer_value(&sample->mixer, property, request.mixer.index, &sampled) &&
          live == sampled;
        break;
      }
    } else if (out->last_readback_known) {
      const struct mpc_native_track_state *live = &out->last_readback;
      unsigned required = mpc_family_by_id(live->family)->required[property];
      out->readbacks_consistent = false;
      for (unsigned i = 0; i < out->after.track_count; i++) {
        const struct mpc_command_track *sample = &out->after.tracks[i];
        if (sample->id != request.identity.track)
          continue;
        bool model = property == MPC_COMMAND_MUTE ? live->muted
          : property == MPC_COMMAND_SOLO          ? live->soloed
                                                  : live->record_armed;
        bool observed = property == MPC_COMMAND_MUTE ? sample->muted
          : property == MPC_COMMAND_SOLO             ? sample->soloed
                                                     : sample->record_armed;
        out->readbacks_consistent = required && live->family == sample->family &&
          (live->known & required) == required && (sample->known & required) == required &&
          model == observed &&
          (!(required & MPC_NATIVE_SOLO_COPY) || live->audio_soloed == !!sample->audio_soloed) &&
          (!(required & MPC_NATIVE_ARM_COPY) ||
           live->audio_record_armed == !!sample->audio_record_armed);
        break;
      }
    }
  } else if (!result.error)
    result.error = captured ? captured : MPC_NATIVE_STALE_TARGET;
done: {
  mpc_ui_leave(executor->adapter, MPC_UI_OTHER);
  executor->adapter->profile = NULL;
  struct timespec finished;
  clock_gettime(CLOCK_MONOTONIC, &finished);
  out->finished_ms = (uint64_t)finished.tv_sec * 1000 + (uint64_t)finished.tv_nsec / 1000000;
  out->ui_work_us = ((uint64_t)(finished.tv_sec - dispatched.tv_sec) * 1000000000 +
                     finished.tv_nsec - dispatched.tv_nsec) /
    1000;
}
  executor->command_context = NULL;
  executor->active = false;
  return result;
}

static struct mpc_executor_result execute(
  struct mpc_executor *executor,
  const struct mpc_executor_request *request,
  struct mpc_state_snapshot *snapshot,
  struct mpc_command_native_result *command,
  struct mpc_command_observation *flags) {
  if (request->operation >= MPC_EXECUTOR_COMMAND_MUTE)
    return command_execute(executor, request, command);
  struct mpc_executor_result result = {.state = MPC_EXECUTOR_COMPLETED};
  if (request->operation == MPC_EXECUTOR_SAMPLE_FLAGS) {
    result.error = mpc_identity_flags(
      executor->registry,
      request->sample_offset,
      request->sample_count ? request->sample_count : MPC_EXECUTOR_SAMPLE_TRACKS,
      request->sample_topology,
      flags);
    if (!result.error)
      result.flags = flags;
    return result;
  }
  if (request->operation == MPC_EXECUTOR_GET_STATE) {
    result.error = mpc_identity_state(executor->registry, snapshot);
    if (!result.error)
      result.snapshot = snapshot;
    return result;
  }
  if (request->operation == MPC_EXECUTOR_IDENTITIES) {
    result.error = mpc_identity_capture(executor->registry, &result.identities);
    return result;
  }
  if (request->operation == MPC_EXECUTOR_VALIDATE_SEQUENCE) {
    result.error = mpc_identity_sequence(executor->registry, &request->identity);
    return result;
  }
  struct mpc_native_target target = {0};
  executor->active = true;
  executor->identity = request->identity;
  result.error = resolve_target(executor, &request->identity, &target);
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
    case MPC_EXECUTOR_IDENTITIES:
    case MPC_EXECUTOR_VALIDATE_SEQUENCE:
    case MPC_EXECUTOR_GET_STATE:
    case MPC_EXECUTOR_SAMPLE_FLAGS:
    case MPC_EXECUTOR_COMMAND_MUTE:
    case MPC_EXECUTOR_COMMAND_SOLO:
    case MPC_EXECUTOR_COMMAND_ARM:
    case MPC_EXECUTOR_COMMAND_OBSERVE:
    case MPC_EXECUTOR_COMMAND_MIXER:
      break;
    }
    if (request->operation != MPC_EXECUTOR_READ_TRACK)
      result.observed = result.write.observed;
  }
  executor->active = false;
  return result;
}

/* Share one fresh UI sample with consecutive waiting observers. Never cross a
 * command in FIFO order or reuse a sample across UI callbacks. Copying and all
 * result ownership remain bounded by FANOUT and the fixed job capacity. */
static void
share_flags(struct mpc_executor *executor, const struct mpc_command_observation *source) {
  for (unsigned n = 0; source && n < MPC_EXECUTOR_SAMPLE_FANOUT; n++) {
    struct mpc_executor_job *job = executor->head;
    if (!job || job->request.operation != MPC_EXECUTOR_SAMPLE_FLAGS)
      break;
    unsigned offset = job->request.sample_offset;
    unsigned limit =
      job->request.sample_count ? job->request.sample_count : MPC_EXECUTOR_SAMPLE_TRACKS;
    unsigned count = offset <= source->total_tracks ? source->total_tracks - offset : 0;
    if (count > limit)
      count = limit;
    if (
      offset < source->offset || offset > source->total_tracks ||
      offset + count > source->offset + source->track_count)
      break;
    remove_queued(executor, job);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    job->result.state = MPC_EXECUTOR_COMPLETED;
    if (job->has_deadline && !time_before(&now, &job->execution_deadline)) {
      job->result.state = MPC_EXECUTOR_CANCELLED;
      job->result.error = MPC_NATIVE_CANCELLED;
    } else if (
      job->request.sample_topology && job->request.sample_topology != source->topology_epoch)
      job->result.error = MPC_NATIVE_STATE_UNSTABLE;
    else {
      memcpy(job->flags, source, offsetof(struct mpc_command_observation, tracks));
      memcpy(
        job->flags->tracks,
        source->tracks + offset - source->offset,
        count * sizeof *source->tracks);
      job->flags->offset = offset;
      job->flags->track_count = count;
      job->result.flags = job->flags;
      job->result.flags_shared = true;
    }
    if (!job->client_owned)
      free_slot(job);
  }
}

static void invoke(const void *capture) {
  struct timespec callback_start;
  clock_gettime(CLOCK_MONOTONIC, &callback_start);
  struct mpc_executor *executor = ((const struct mpc_native_function *)capture)->data[0];
  pthread_mutex_lock(&executor->mutex);
  executor->callback_invoked = true;
  if (!mpc_native_on_ui_thread(executor->adapter)) {
    executor->cleanup_rejected = true;
    fail(executor, MPC_NATIVE_WRONG_THREAD);
    pthread_mutex_unlock(&executor->mutex);
    return;
  }
  executor->ui_thread_id = (uint32_t)syscall(SYS_gettid);
  if (
    (executor->state == MPC_EXECUTOR_STOPPING || executor->state == MPC_EXECUTOR_FAILED) &&
    executor->registry && !executor->identities_detached) {
    pthread_mutex_unlock(&executor->mutex);
    enum mpc_native_error error = mpc_native_verify_ui(executor->adapter);
    if (!error)
      error = mpc_identity_detach(executor->registry);
    pthread_mutex_lock(&executor->mutex);
    if (error) {
      executor->cleanup_rejected = true;
      fail(executor, error);
    } else
      executor->identities_detached = true;
    pthread_cond_broadcast(&executor->changed);
  } else if (executor->state == MPC_EXECUTOR_STARTING) {
    executor->state = MPC_EXECUTOR_READY;
    pthread_cond_broadcast(&executor->changed);
  } else {
    /* One operation per UI message. A full snapshot is bounded but cannot be
     * preempted; batching capacity snapshots would multiply that UI stall under
     * playback load. Replenishing clients cannot extend this callback. */
    for (unsigned i = 0; i < 1; i++) {
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
      struct mpc_executor_result result =
        execute(executor, &job->request, job->snapshot, job->command, job->flags);
      pthread_mutex_lock(&executor->mutex);
      job->result = result;
      if (!result.error)
        share_flags(
          executor,
          result.flags                                      ? result.flags
            : result.command && result.command->after_known ? &result.command->after
                                                            : NULL);
      {
        struct timespec callback_end;
        clock_gettime(CLOCK_MONOTONIC, &callback_end);
        uint64_t elapsed = ((uint64_t)(callback_end.tv_sec - callback_start.tv_sec) * 1000000000 +
                            callback_end.tv_nsec - callback_start.tv_nsec) /
          1000;
        job->result.ui_callback_us = elapsed;
        if (job->command && elapsed > job->command->ui_work_us) {
          job->command->profile.ns[MPC_UI_OTHER] += (elapsed - job->command->ui_work_us) * 1000;
          job->command->ui_work_us = elapsed;
        }
      }
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
    if (!executor->captures && executor->cleanup_posted && !executor->callback_invoked) {
      executor->cleanup_rejected = true;
      fail(executor, MPC_NATIVE_DISPATCH_FAILED);
    }
    if (
      !executor->captures && executor->probe_posted && !executor->callback_invoked &&
      (executor->state == MPC_EXECUTOR_STARTING || executor->state == MPC_EXECUTOR_READY))
      fail(executor, MPC_NATIVE_DISPATCH_FAILED);
    if (
      (executor->state == MPC_EXECUTOR_STOPPING || executor->state == MPC_EXECUTOR_FAILED) &&
      !executor->captures && executor->identities_detached)
      break;
    bool cleanup =
      (executor->state == MPC_EXECUTOR_STOPPING || executor->state == MPC_EXECUTOR_FAILED) &&
      !executor->identities_detached && !executor->cleanup_rejected;
    bool probe = executor->state == MPC_EXECUTOR_STARTING && !executor->probe_posted;
    if (
      !executor->captures &&
      (probe || cleanup || (executor->state == MPC_EXECUTOR_READY && executor->head))) {
      executor->probe_posted = true;
      executor->cleanup_posted = cleanup;
      executor->callback_invoked = false;
      executor->captures = 1; /* Local function ownership, before posting. */
      struct mpc_native_function function = {{executor, NULL}, manager, invoke};
      pthread_mutex_unlock(&executor->mutex);
      enum mpc_native_error error = mpc_native_post(executor->adapter, &function);
      if (error) {
        pthread_mutex_lock(&executor->mutex);
        if (cleanup)
          executor->cleanup_rejected = true;
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

static enum mpc_native_error open_executor(
  const struct mpc_native_io *io,
  const struct mpc_executor_config *config,
  bool authoritative,
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
  executor->identities_detached = !authoritative;
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
  if (authoritative) {
    error = mpc_identity_create(executor->adapter, &executor->registry);
    if (error) {
      mpc_native_close(executor->adapter);
      goto free_condition;
    }
  }
  if (pthread_create(&executor->scheduler, NULL, schedule, executor)) {
    error = MPC_NATIVE_NOT_READY;
    mpc_identity_destroy(executor->registry);
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
  return open_executor(NULL, config, false, output);
}

enum mpc_native_error mpc_executor_open_backend(
  const struct mpc_native_io *io,
  const struct mpc_executor_config *config,
  struct mpc_executor **output) {
  return open_executor(io, config, false, output);
}

enum mpc_native_error mpc_executor_open_authoritative_backend(
  const struct mpc_native_io *io, unsigned capacity, struct mpc_executor **output) {
  struct mpc_executor_config config = {MPC_EXECUTOR_ABI, sizeof config, capacity, NULL, NULL};
  return open_executor(io, &config, true, output);
}

enum mpc_native_error
mpc_executor_open_authoritative(unsigned capacity, struct mpc_executor **output) {
  return mpc_executor_open_authoritative_backend(NULL, capacity, output);
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
  output->adapter.target_validator_present = executor->config.resolve != NULL || executor->registry;
  pthread_mutex_unlock(&executor->mutex);
  return error;
}

enum mpc_native_error mpc_executor_submit(
  struct mpc_executor *executor,
  const struct mpc_executor_request *request,
  struct mpc_executor_job **output) {
  return mpc_executor_command_submit(executor, request, NULL, output);
}

enum mpc_native_error mpc_executor_command_submit(
  struct mpc_executor *executor,
  const struct mpc_executor_request *request,
  const struct mpc_command_context *context,
  struct mpc_executor_job **output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  *output = NULL;
  if (
    !executor || !request || (unsigned)request->operation > MPC_EXECUTOR_COMMAND_MIXER ||
    request->queue_timeout_ms > MPC_EXECUTOR_MAX_WAIT_MS ||
    (request->operation == MPC_EXECUTOR_SAMPLE_FLAGS &&
     (request->sample_count > MPC_EXECUTOR_SAMPLE_TRACKS ||
      request->sample_offset > MPC_IDENTITY_MAX_TRACKS)) ||
    ((request->operation == MPC_EXECUTOR_COMMAND_OBSERVE ||
      request->operation == MPC_EXECUTOR_COMMAND_MIXER) &&
     request->command_property > MPC_COMMAND_INSERT))
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (request->operation >= MPC_EXECUTOR_COMMAND_MUTE) {
    unsigned property = (request->operation == MPC_EXECUTOR_COMMAND_OBSERVE ||
                         request->operation == MPC_EXECUTOR_COMMAND_MIXER)
      ? request->command_property
      : (unsigned)(request->operation - MPC_EXECUTOR_COMMAND_MUTE);
    if (
      !mpc_mixer_request_valid(property, &request->mixer, request->has_expected) ||
      (unsigned)request->mute_scope > MPC_MUTE_SEQUENCE ||
      (request->mute_scope == MPC_MUTE_SEQUENCE &&
       (property != MPC_COMMAND_MUTE || !request->identity.sequence ||
        !request->identity.sequence_generation)) ||
      (request->mute_scope == MPC_MUTE_GLOBAL &&
       (request->identity.sequence || request->identity.sequence_generation)))
      return MPC_NATIVE_INVALID_ARGUMENT;
  }
  pthread_mutex_lock(&executor->mutex);
  enum mpc_native_error error = MPC_NATIVE_OK;
  if (executor->state != MPC_EXECUTOR_READY)
    error = executor->failure                    ? executor->failure
      : executor->state == MPC_EXECUTOR_STARTING ? MPC_NATIVE_NOT_READY
                                                 : MPC_NATIVE_SHUTTING_DOWN;
  else if (!executor->config.resolve && !executor->registry)
    error = MPC_NATIVE_NOT_READY;
  else if (request->operation >= MPC_EXECUTOR_IDENTITIES && !executor->registry)
    error = MPC_NATIVE_NOT_READY;
  else if (request->operation >= MPC_EXECUTOR_COMMAND_MUTE && !executor->command_owner)
    error = MPC_NATIVE_NOT_READY;
  else if (executor->live_jobs == executor->config.capacity)
    error = MPC_NATIVE_QUEUE_FULL;
  if (!error) {
    for (unsigned i = 0; i < executor->config.capacity; i++) {
      struct mpc_executor_job *job = &executor->jobs[i];
      if (job->result.state)
        continue;
      if (request->operation == MPC_EXECUTOR_GET_STATE) {
        job->snapshot = calloc(1, sizeof *job->snapshot);
        if (!job->snapshot) {
          error = MPC_NATIVE_NOT_READY;
          break;
        }
      }
      if (request->operation == MPC_EXECUTOR_SAMPLE_FLAGS) {
        job->flags = calloc(1, sizeof *job->flags);
        if (!job->flags) {
          error = MPC_NATIVE_NOT_READY;
          break;
        }
      }
      if (request->operation >= MPC_EXECUTOR_COMMAND_MUTE) {
        job->command = calloc(1, sizeof *job->command);
        if (!job->command) {
          free(job->snapshot);
          job->snapshot = NULL;
          error = MPC_NATIVE_NOT_READY;
          break;
        }
      }
      if (context && job->command) {
        job->command->context = *context;
        job->command->context_known = true;
      }
      job->request = *request;
      job->result = (struct mpc_executor_result){.state = MPC_EXECUTOR_QUEUED};
      job->has_deadline = request->queue_timeout_ms != 0;
      if (job->has_deadline)
        job->execution_deadline = deadline(request->queue_timeout_ms);
      if (request->operation >= MPC_EXECUTOR_COMMAND_MUTE && request->command_deadline_ms) {
        job->has_deadline = true;
        job->execution_deadline.tv_sec = (time_t)(request->command_deadline_ms / 1000);
        job->execution_deadline.tv_nsec = (long)(request->command_deadline_ms % 1000) * 1000000;
      }
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
  executor->cleanup_rejected = false;
  executor->cleanup_posted = false;
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
  bool drained = !executor->command_owner && executor->exited && !executor->captures &&
    !executor->live_jobs && executor->identities_detached;
  pthread_mutex_unlock(&executor->mutex);
  if (!drained)
    return MPC_NATIVE_NOT_READY;
  pthread_join(executor->scheduler, NULL);
  mpc_identity_destroy(executor->registry);
  mpc_native_close(executor->adapter);
  pthread_cond_destroy(&executor->changed);
  pthread_mutex_destroy(&executor->mutex);
  free(executor->jobs);
  free(executor);
  return MPC_NATIVE_OK;
}

bool mpc_executor_on_ui(struct mpc_executor *executor) {
  return mpc_native_on_ui_thread(executor->adapter);
}

enum mpc_native_error
mpc_executor_qualify_command(struct mpc_executor *executor, enum mpc_command_operation operation) {
  if (!executor)
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (!mpc_executor_on_ui(executor))
    return MPC_NATIVE_WRONG_THREAD;
  if (!executor->active || !executor->registry)
    return MPC_NATIVE_NOT_READY;
  struct mpc_native_target target;
  enum mpc_native_error error = resolve_target(executor, &executor->identity, &target);
  if (error)
    return error;
  struct mpc_command_native_result *context = executor->command_context;
  if (!context || !executor->qualifying)
    return MPC_NATIVE_NOT_READY;
  error = mpc_native_qualify_command(
    executor->adapter,
    &target,
    operation,
    executor->mute_scope,
    &context->context,
    &context->context_known,
    &context->last_readback);
  context->last_readback_known = !error;
  return error;
}

enum mpc_native_error mpc_executor_commands_attach(
  struct mpc_executor *executor, void *owner, const struct mpc_command_policy *policy) {
  if (!executor || !owner || !policy)
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (mpc_executor_on_ui(executor))
    return MPC_NATIVE_WRONG_THREAD;
  pthread_mutex_lock(&executor->mutex);
  enum mpc_native_error error =
    executor->registry && executor->state == MPC_EXECUTOR_READY && !executor->command_owner
    ? MPC_NATIVE_OK
    : MPC_NATIVE_NOT_READY;
  if (!error) {
    executor->command_owner = owner;
    executor->command_policy = *policy;
  }
  pthread_mutex_unlock(&executor->mutex);
  return error;
}

void mpc_executor_commands_detach(struct mpc_executor *executor, void *owner) {
  pthread_mutex_lock(&executor->mutex);
  if (executor->command_owner == owner) {
    executor->command_owner = NULL;
    memset(&executor->command_policy, 0, sizeof executor->command_policy);
  }
  pthread_mutex_unlock(&executor->mutex);
}

enum mpc_native_error
mpc_executor_poll(struct mpc_executor_job *job, struct mpc_executor_result *output) {
  if (!job || !output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (mpc_executor_on_ui(job->owner))
    return MPC_NATIVE_WRONG_THREAD;
  pthread_mutex_lock(&job->owner->mutex);
  *output = job->result;
  pthread_mutex_unlock(&job->owner->mutex);
  return MPC_NATIVE_OK;
}
