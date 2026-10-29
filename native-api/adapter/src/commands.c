#include "command_private.h"
#include "mpc_family_profile.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct record {
  bool used, pending, verifying, frozen;
  unsigned readers;
  uint64_t caller, serial, accepted_ms, start_deadline, verify_deadline;
  struct mpc_executor_job *job;
  struct mpc_command_receipt receipt;
  bool context_known;
  struct mpc_command_context context;
};
struct mpc_commands {
  struct mpc_commands_config config;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t worker;
  struct record *records;
  uint64_t next_serial;
  unsigned pending;
  bool stopping, exited;
  char adapter_id[37];
  uint64_t (*now)(void *);
  void *clock_data;
};

static uint64_t monotonic_ms(void *unused) {
  (void)unused;
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}
static struct timespec deadline(unsigned milliseconds) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  value.tv_sec += milliseconds / 1000;
  value.tv_nsec += (long)(milliseconds % 1000) * 1000000;
  if (value.tv_nsec >= 1000000000) {
    value.tv_sec++;
    value.tv_nsec -= 1000000000;
  }
  return value;
}
static bool identifier(const char *id) {
  size_t n = strnlen(id, MPC_COMMAND_ID_BYTES);
  return n && n < MPC_COMMAND_ID_BYTES &&
    strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == n;
}
static bool instance(const char *id) {
  return strnlen(id, 37) == 36 && !strncmp(id, "run_", 4) &&
    strspn(id + 4, "0123456789abcdef") == 32;
}
static enum mpc_native_error check(struct mpc_commands *commands, unsigned wait_ms) {
  if (!commands || wait_ms > MPC_COMMAND_MAX_WAIT_MS)
    return MPC_NATIVE_INVALID_ARGUMENT;
  return mpc_executor_on_ui(commands->config.executor) ? MPC_NATIVE_WRONG_THREAD : MPC_NATIVE_OK;
}
static bool canonical(const struct mpc_command_request *a, const struct mpc_command_request *b) {
  return a->operation == b->operation && a->target.project == b->target.project &&
    a->target.track == b->target.track && a->mute_scope == b->mute_scope &&
    a->sequence.project == b->sequence.project && a->sequence.sequence == b->sequence.sequence &&
    a->sequence.generation == b->sequence.generation &&
    !memcmp(a->sequence.instance_id, b->sequence.instance_id, 37) &&
    !memcmp(a->target.instance_id, b->target.instance_id, 37) && a->value == b->value &&
    a->has_expected == b->has_expected && (!a->has_expected || a->expected == b->expected) &&
    a->edit.value == b->edit.value && (!a->has_expected || a->edit.expected == b->edit.expected) &&
    a->pulse == b->pulse && a->bpm == b->bpm && a->start_within_ms == b->start_within_ms &&
    a->mixer.generation == b->mixer.generation && a->mixer.index == b->mixer.index &&
    a->mixer.value == b->mixer.value &&
    (!a->has_expected || a->mixer.expected == b->mixer.expected);
}
static void finish(
  struct mpc_commands *commands,
  struct record *record,
  enum mpc_command_status status,
  enum mpc_native_error error) {
  record->receipt.status = status;
  record->receipt.error = error;
  record->receipt.finished_ms = commands->now(commands->clock_data);
  record->pending = false;
  commands->pending--;
  pthread_cond_broadcast(&commands->changed);
}
static const struct mpc_command_track *target_track(const struct mpc_command_receipt *receipt) {
  if (!receipt->after_known || receipt->after.project != receipt->request.target.project)
    return NULL;
  for (unsigned i = 0; i < receipt->after.track_count; i++)
    if (receipt->after.tracks[i].id == receipt->request.target.track)
      return &receipt->after.tracks[i];
  return NULL;
}
static bool verified(const struct mpc_command_receipt *receipt) {
  unsigned property = receipt->request.operation;
  if (mpc_command_track_edit(property)) {
    unsigned field = property - MPC_COMMAND_TRACK_COLOUR;
    return (receipt->setup_after.known & (1u << field)) &&
      receipt->setup_after.values[field] ==
      (field == MPC_TRACK_VELOCITY ? (double)(float)receipt->request.edit.value
                                   : receipt->request.edit.value);
  }
  if (property >= MPC_COMMAND_PLAY) {
    const struct mpc_navigation_state *state = &receipt->navigation_after;
    if (!state->known)
      return false;
    if (property == MPC_COMMAND_SELECT_TRACK)
      return state->selected_track == receipt->request.target.track;
    if (state->selected_sequence != receipt->request.sequence.sequence)
      return false;
    if (property == MPC_COMMAND_SEQUENCE_BPM)
      return state->sequence_bpm == (float)receipt->request.bpm;
    if (property == MPC_COMMAND_LOCATE)
      return state->pulse == receipt->request.pulse;
    if (property == MPC_COMMAND_SELECT_SEQUENCE)
      return true;
    return state->playing == (property != MPC_COMMAND_STOP);
  }
  const struct mpc_command_track *track = target_track(receipt);
  if (!track)
    return false;
  if (property >= MPC_COMMAND_VOLUME) {
    double value;
    return track->mixer.generation == receipt->request.mixer.generation &&
      mpc_mixer_value(&track->mixer, property, receipt->request.mixer.index, &value) &&
      value ==
      (mpc_mixer_numeric(property) ? (double)(float)receipt->request.mixer.value
                                   : receipt->request.value);
  }
  const struct mpc_family_support *support = mpc_family_by_id(track->family);
  unsigned required = support->required[property];
  if (!required || (track->known & required) != required)
    return false;
  if (
    receipt->request.operation == MPC_COMMAND_MUTE &&
    (receipt->after.mute_scope != (unsigned)receipt->request.mute_scope ||
     (receipt->request.mute_scope == MPC_MUTE_SEQUENCE &&
      (receipt->after.selected_sequence != receipt->request.sequence.sequence ||
       receipt->after.selected_sequence_generation != receipt->request.sequence.generation))))
    return false;
  bool value = property == MPC_COMMAND_MUTE ? track->muted
    : property == MPC_COMMAND_SOLO          ? track->soloed
                                            : track->record_armed;
  if (value != receipt->request.value)
    return false;
  unsigned copy = property == MPC_COMMAND_SOLO ? MPC_NATIVE_SOLO_COPY : MPC_NATIVE_ARM_COPY;
  return property == MPC_COMMAND_MUTE || !(required & copy) ||
    (property == MPC_COMMAND_SOLO ? track->audio_soloed : track->audio_record_armed) ==
    receipt->request.value;
}
static void fail_before_execution(
  struct mpc_commands *commands, struct record *record, enum mpc_native_error error) {
  record->receipt.native_known = true;
  record->receipt.execution = MPC_COMMAND_NOT_STARTED;
  finish(
    commands,
    record,
    error == MPC_NATIVE_CANCELLED ? MPC_COMMAND_CANCELLED : MPC_COMMAND_REJECTED,
    error);
}
static void advance(struct mpc_commands *commands, struct record *record) {
  struct mpc_command_receipt *receipt = &record->receipt;
  uint64_t now = commands->now(commands->clock_data);
  /* A terminal verification timeout is immutable even if its read-only UI job
   * is still returning. Keep ownership/retention pressure until that job drains. */
  if (record->frozen) {
    struct mpc_executor_result drained;
    if (mpc_executor_poll(record->job, &drained) || drained.state == MPC_EXECUTOR_RUNNING)
      return;
    if (drained.state == MPC_EXECUTOR_QUEUED) {
      mpc_executor_cancel(record->job, &drained);
      if (drained.state == MPC_EXECUTOR_RUNNING)
        return;
    }
    mpc_executor_release(record->job);
    record->job = NULL;
    record->pending = false;
    commands->pending--;
    pthread_cond_broadcast(&commands->changed);
    return;
  }
  if (!record->job) {
    if (!record->verifying && (commands->stopping || now >= record->start_deadline)) {
      fail_before_execution(commands, record, MPC_NATIVE_CANCELLED);
      return;
    }
    if (record->verifying && (commands->stopping || now >= record->verify_deadline)) {
      finish(
        commands,
        record,
        MPC_COMMAND_UNKNOWN,
        commands->stopping ? MPC_NATIVE_SHUTTING_DOWN : MPC_NATIVE_VERIFICATION_TIMEOUT);
      return;
    }
    struct mpc_executor_request request = {
      .operation = receipt->request.operation >= MPC_COMMAND_PLAY
        ? (record->verifying ? MPC_EXECUTOR_COMMAND_OBSERVE : MPC_EXECUTOR_COMMAND_NAVIGATION)
        : record->verifying ? MPC_EXECUTOR_COMMAND_OBSERVE
        : receipt->request.operation >= MPC_COMMAND_VOLUME
        ? MPC_EXECUTOR_COMMAND_MIXER
        : (enum mpc_executor_operation)(MPC_EXECUTOR_COMMAND_MUTE + receipt->request.operation),
      .identity =
        {.project = receipt->request.target.project,
         .track = receipt->request.target.track,
         .sequence = receipt->request.sequence.sequence,
         .sequence_generation = receipt->request.sequence.generation},
      .mute_scope = receipt->request.mute_scope,
      .desired = receipt->request.value,
      .has_expected = receipt->request.has_expected,
      .expected = receipt->request.expected,
      .mixer = receipt->request.mixer,
      .pulse = receipt->request.pulse,
      .bpm = receipt->request.bpm,
      .edit = receipt->request.edit,
      .queue_timeout_ms =
        record->verifying ? MPC_COMMAND_VERIFICATION_MS : (unsigned)(record->start_deadline - now),
      .command_property = receipt->request.operation,
      .command_deadline_ms = record->verifying ? record->verify_deadline : record->start_deadline};
    memcpy(request.identity.instance_id, commands->adapter_id, 37);
    enum mpc_native_error error = mpc_executor_command_submit(
      commands->config.executor,
      &request,
      record->context_known ? &record->context : NULL,
      &record->job);
    if (error == MPC_NATIVE_QUEUE_FULL)
      return; /* Acceptance order and original deadline remain unchanged. */
    if (error) {
      if (record->verifying)
        finish(commands, record, MPC_COMMAND_UNKNOWN, error);
      else
        fail_before_execution(commands, record, error);
    }
    return;
  }
  struct mpc_executor_result result;
  enum mpc_native_error error = mpc_executor_poll(record->job, &result);
  if (error)
    return; /* The owner is a non-UI thread and keeps the job until drain. */
  if (
    result.state == MPC_EXECUTOR_QUEUED &&
    (commands->stopping ||
     now >= (record->verifying ? record->verify_deadline : record->start_deadline)))
    mpc_executor_cancel(record->job, &result);
  if (result.state == MPC_EXECUTOR_QUEUED)
    return;
  if (result.state == MPC_EXECUTOR_RUNNING) {
    if (record->verifying && now >= record->verify_deadline) {
      record->frozen = true;
      receipt->finished_ms = now;
      receipt->status = MPC_COMMAND_UNKNOWN;
      receipt->error = MPC_NATIVE_VERIFICATION_TIMEOUT;
    }
    if (!record->verifying) {
      receipt->execution = MPC_COMMAND_STARTED;
      if (!record->verify_deadline)
        record->verify_deadline = now + MPC_COMMAND_VERIFICATION_MS;
      if (now >= record->verify_deadline) {
        receipt->status = MPC_COMMAND_UNKNOWN;
        receipt->error = MPC_NATIVE_VERIFICATION_TIMEOUT;
      }
    }
    pthread_cond_broadcast(&commands->changed);
    return;
  }
  if (result.command) {
    if (!record->verifying) {
      receipt->dispatched_ms = result.command->dispatched_ms;
      receipt->dispatch_finished_ms = result.command->finished_ms;
    } else
      receipt->verification_passes++;
    receipt->ui_work_us += result.command->ui_work_us;
    if (record->verifying)
      receipt->verification_ui_us += result.command->ui_work_us;
    if (receipt->max_ui_callback_us < result.command->ui_work_us)
      receipt->max_ui_callback_us = result.command->ui_work_us;
    for (unsigned i = 0; i < MPC_UI_STAGES; i++)
      receipt->stages_us[i] += result.command->profile.ns[i] / 1000;
    if (result.command->context_known) {
      record->context_known = true;
      record->context = result.command->context;
    }
    if (!record->verifying)
      record->verify_deadline = result.command->dispatched_ms + MPC_COMMAND_VERIFICATION_MS;
    if (!record->verifying)
      receipt->navigation_before = result.command->navigation_before;
    receipt->navigation_after = result.command->navigation_after;
    if (!record->verifying)
      receipt->setup_before = result.command->setup_before;
    receipt->setup_after = result.command->setup_after;
    if (result.command->before_known && !record->verifying) {
      receipt->before = result.command->before;
      receipt->before_known = true;
    }
    if (result.command->after_known) {
      receipt->after = result.command->after;
      receipt->after_known = true;
    }
  }
  if (!record->verifying) {
    receipt->native_known = true;
    receipt->native_invoked = result.write.native_invoked;
    receipt->changed_known = result.write.changed_known;
    receipt->changed = result.write.changed;
    receipt->execution = result.write.native_invoked || !result.error ? MPC_COMMAND_COMPLETED
                                                                      : MPC_COMMAND_NOT_STARTED;
  }
  bool readbacks_consistent = !result.command || result.command->readbacks_consistent;
  mpc_executor_release(record->job);
  record->job = NULL;
  if (result.error) {
    if (record->verifying || receipt->native_invoked) {
      finish(
        commands,
        record,
        result.error == MPC_NATIVE_VALUE_REJECTED ? MPC_COMMAND_REJECTED : MPC_COMMAND_UNKNOWN,
        record->verifying && result.error == MPC_NATIVE_CANCELLED ? MPC_NATIVE_VERIFICATION_TIMEOUT
                                                                  : result.error);
    } else
      fail_before_execution(commands, record, result.error);
    return;
  }
  if (
    receipt->request.operation >= MPC_COMMAND_VOLUME &&
    receipt->request.operation < MPC_COMMAND_PLAY && receipt->before_known &&
    receipt->after_known) {
    double before, after;
    const struct mpc_command_track *a = NULL, *b = target_track(receipt);
    for (unsigned i = 0; i < receipt->before.track_count; i++)
      if (receipt->before.tracks[i].id == receipt->request.target.track)
        a = &receipt->before.tracks[i];
    if (
      a && b && a->mixer.generation == b->mixer.generation &&
      mpc_mixer_value(
        &a->mixer,
        receipt->request.operation,
        receipt->request.mixer.index,
        &before) &&
      mpc_mixer_value(
        &b->mixer,
        receipt->request.operation,
        receipt->request.mixer.index,
        &after)) {
      receipt->changed_known = true;
      receipt->changed = before != after;
    }
  }
  if (readbacks_consistent && verified(receipt)) {
    if (receipt->request.operation >= MPC_COMMAND_PLAY) {
      /* Invocation alone does not prove a queued operation changed state.
       * Before/desired comparisons already filtered native no-ops at dispatch. */
      receipt->changed_known = true;
      receipt->changed = receipt->native_invoked;
    }
    receipt->execution = MPC_COMMAND_COMPLETED; /* Includes verified native no-ops. */
    finish(commands, record, MPC_COMMAND_APPLIED, MPC_NATIVE_OK);
    return;
  }
  /* Model completion alone cannot promote a lagging solo/arm audio copy. Never
   * call the setter again: verification is read-only and revalidates identities. */
  record->verifying = true;
}
static void *work(void *data) {
  struct mpc_commands *commands = data;
  pthread_setname_np(pthread_self(), "mpc commands");
  pthread_mutex_lock(&commands->mutex);
  while (!commands->stopping || commands->pending) {
    struct record *first = NULL;
    for (unsigned i = 0; i < commands->config.receipt_capacity; i++) {
      struct record *record = &commands->records[i];
      if (record->pending && (!first || record->serial < first->serial))
        first = record;
    }
    if (first)
      advance(commands, first);
    struct timespec until = deadline(5);
    if (!commands->stopping || commands->pending)
      pthread_cond_timedwait(&commands->changed, &commands->mutex, &until);
  }
  commands->exited = true;
  pthread_cond_broadcast(&commands->changed);
  pthread_mutex_unlock(&commands->mutex);
  return NULL;
}

enum mpc_native_error mpc_commands_open_clock(
  const struct mpc_commands_config *config,
  uint64_t (*now)(void *),
  void *clock_data,
  struct mpc_commands **output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  *output = NULL;
  if (!config || config->abi_version != MPC_COMMANDS_ABI || config->struct_bytes != sizeof *config)
    return MPC_NATIVE_ABI_MISMATCH;
  if (
    !config->executor || !instance(config->instance_id) || !config->pending_capacity ||
    config->pending_capacity > MPC_COMMAND_MAX_PENDING || !config->receipt_capacity ||
    config->receipt_capacity > MPC_COMMAND_MAX_RECEIPTS ||
    config->pending_capacity > config->receipt_capacity ||
    config->retention_ms < MPC_COMMAND_MIN_RETENTION_MS || !now)
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (mpc_executor_on_ui(config->executor))
    return MPC_NATIVE_WRONG_THREAD;
  struct mpc_commands *commands = calloc(1, sizeof *commands);
  if (!commands)
    return MPC_NATIVE_NOT_READY;
  commands->records = calloc(config->receipt_capacity, sizeof *commands->records);
  if (!commands->records) {
    free(commands);
    return MPC_NATIVE_NOT_READY;
  }
  commands->config = *config;
  commands->now = now;
  commands->clock_data = clock_data;
  commands->next_serial = 1;
  enum mpc_native_error error = MPC_NATIVE_NOT_READY;
  if (pthread_mutex_init(&commands->mutex, NULL))
    goto free_records;
  pthread_condattr_t attributes;
  if (pthread_condattr_init(&attributes))
    goto free_mutex;
  int rc = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
  if (!rc)
    rc = pthread_cond_init(&commands->changed, &attributes);
  pthread_condattr_destroy(&attributes);
  if (rc)
    goto free_mutex;
  struct mpc_executor_status status;
  error = mpc_executor_get_status(config->executor, &status);
  if (error)
    goto free_condition;
  memcpy(commands->adapter_id, status.adapter.instance_id, 37);
  error = mpc_executor_commands_attach(config->executor, commands, &config->policy);
  if (error)
    goto free_condition;
  if (pthread_create(&commands->worker, NULL, work, commands)) {
    mpc_executor_commands_detach(config->executor, commands);
    error = MPC_NATIVE_NOT_READY;
    goto free_condition;
  }
  *output = commands;
  return MPC_NATIVE_OK;
free_condition:
  pthread_cond_destroy(&commands->changed);
free_mutex:
  pthread_mutex_destroy(&commands->mutex);
free_records:
  free(commands->records);
  free(commands);
  return error;
}

enum mpc_native_error
mpc_commands_open(const struct mpc_commands_config *config, struct mpc_commands **output) {
  return mpc_commands_open_clock(config, monotonic_ms, NULL, output);
}

static void await_receipt(
  struct mpc_commands *commands,
  struct record *record,
  unsigned wait_ms,
  struct mpc_command_receipt *output) {
  struct timespec until = deadline(wait_ms);
  record->readers++;
  pthread_cond_broadcast(&commands->changed);
  while (record->pending && record->receipt.status != MPC_COMMAND_UNKNOWN && wait_ms)
    if (pthread_cond_timedwait(&commands->changed, &commands->mutex, &until) == ETIMEDOUT)
      break;
  *output = record->receipt;
  record->readers--;
}

enum mpc_native_error mpc_commands_submit(
  struct mpc_commands *commands,
  uint64_t caller,
  const struct mpc_command_request *input,
  unsigned wait_ms,
  struct mpc_command_receipt *output) {
  enum mpc_native_error error = check(commands, wait_ms);
  if (error || !input || !output)
    return error ? error : MPC_NATIVE_INVALID_ARGUMENT;
  memset(output, 0, sizeof *output);
  if (
    !identifier(input->request_id) || !instance(input->target.instance_id) ||
    !input->target.project || (input->operation < MPC_COMMAND_PLAY && !input->target.track) ||
    (unsigned)input->operation > MPC_COMMAND_MIDI_PROGRAM ||
    input->start_within_ms > MPC_COMMAND_MAX_START_MS)
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (
    (unsigned)input->mute_scope > MPC_MUTE_SEQUENCE ||
    (input->mute_scope == MPC_MUTE_SEQUENCE &&
     (input->operation != MPC_COMMAND_MUTE || !instance(input->sequence.instance_id) ||
      memcmp(input->sequence.instance_id, input->target.instance_id, 37) ||
      input->sequence.project != input->target.project || !input->sequence.sequence ||
      !input->sequence.generation)) ||
    (input->operation < MPC_COMMAND_PLAY && input->mute_scope == MPC_MUTE_GLOBAL &&
     (input->sequence.instance_id[0] || input->sequence.project || input->sequence.sequence ||
      input->sequence.generation)))
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (!mpc_navigation_request_valid(input))
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (!mpc_mixer_request_valid(input->operation, &input->mixer, input->has_expected))
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (input->has_expected_revision)
    return MPC_NATIVE_UNSUPPORTED_PRECONDITION;
  if (memcmp(input->target.instance_id, commands->config.instance_id, 37))
    return MPC_NATIVE_STALE_INSTANCE;
  struct mpc_command_request request = *input;
  if (!request.start_within_ms)
    request.start_within_ms = MPC_COMMAND_DEFAULT_START_MS;
  pthread_mutex_lock(&commands->mutex);
  uint64_t now = commands->now(commands->clock_data);
  struct record *empty = NULL;
  for (unsigned i = 0; i < commands->config.receipt_capacity; i++) {
    struct record *record = &commands->records[i];
    if (
      record->used && !record->pending && !record->readers &&
      now - record->accepted_ms >= commands->config.retention_ms)
      memset(record, 0, sizeof *record);
    if (
      record->used && record->caller == caller &&
      !strcmp(record->receipt.request.request_id, request.request_id)) {
      if (!canonical(&record->receipt.request, &request))
        error = MPC_NATIVE_REQUEST_ID_CONFLICT;
      else
        await_receipt(commands, record, wait_ms, output);
      goto done;
    }
    if (!record->used && !empty)
      empty = record;
  }
  if (commands->stopping || commands->next_serial == UINT64_MAX)
    error = MPC_NATIVE_SHUTTING_DOWN;
  else if (commands->pending == commands->config.pending_capacity)
    error = MPC_NATIVE_QUEUE_FULL;
  else if (!empty)
    error = MPC_NATIVE_RETENTION_CAPACITY;
  else {
    empty->used = empty->pending = true;
    empty->caller = caller;
    empty->serial = commands->next_serial++;
    empty->accepted_ms = now;
    empty->start_deadline = now + request.start_within_ms;
    empty->receipt.abi_version = MPC_COMMANDS_ABI;
    empty->receipt.struct_bytes = sizeof empty->receipt;
    empty->receipt.request = request;
    empty->receipt.accepted_ms = now;
    empty->receipt.status = MPC_COMMAND_ACCEPTED;
    commands->pending++;
    await_receipt(commands, empty, wait_ms, output);
  }
done:
  pthread_mutex_unlock(&commands->mutex);
  return error;
}

enum mpc_native_error mpc_commands_get(
  struct mpc_commands *commands,
  uint64_t caller,
  const char *instance_id,
  const char *request_id,
  unsigned wait_ms,
  struct mpc_command_receipt *output) {
  enum mpc_native_error error = check(commands, wait_ms);
  if (error || !output || !instance_id || !request_id)
    return error ? error : MPC_NATIVE_INVALID_ARGUMENT;
  memset(output, 0, sizeof *output);
  if (!instance(instance_id) || !identifier(request_id))
    return MPC_NATIVE_INVALID_ARGUMENT;
  if (memcmp(instance_id, commands->config.instance_id, 37))
    return MPC_NATIVE_OUTCOME_UNAVAILABLE;
  pthread_mutex_lock(&commands->mutex);
  error = MPC_NATIVE_OUTCOME_UNAVAILABLE;
  uint64_t now = commands->now(commands->clock_data);
  for (unsigned i = 0; i < commands->config.receipt_capacity; i++) {
    struct record *record = &commands->records[i];
    if (
      record->used && record->caller == caller &&
      !strcmp(record->receipt.request.request_id, request_id) &&
      (record->pending || now - record->accepted_ms < commands->config.retention_ms)) {
      await_receipt(commands, record, wait_ms, output);
      error = MPC_NATIVE_OK;
      break;
    }
  }
  pthread_mutex_unlock(&commands->mutex);
  return error;
}

enum mpc_native_error mpc_commands_shutdown(struct mpc_commands *commands, unsigned milliseconds) {
  enum mpc_native_error error = check(commands, milliseconds);
  if (error)
    return error;
  struct timespec until = deadline(milliseconds);
  pthread_mutex_lock(&commands->mutex);
  commands->stopping = true;
  for (unsigned i = 0; i < commands->config.receipt_capacity; i++) {
    struct record *record = &commands->records[i];
    if (record->pending && !record->job && !record->verifying)
      fail_before_execution(commands, record, MPC_NATIVE_CANCELLED);
  }
  pthread_cond_broadcast(&commands->changed);
  while (!commands->exited)
    if (pthread_cond_timedwait(&commands->changed, &commands->mutex, &until) == ETIMEDOUT)
      break;
  error = commands->exited ? MPC_NATIVE_OK : MPC_NATIVE_TIMED_OUT;
  pthread_mutex_unlock(&commands->mutex);
  return error;
}

enum mpc_native_error mpc_commands_close(struct mpc_commands *commands) {
  enum mpc_native_error error = check(commands, 0);
  if (error)
    return error;
  pthread_mutex_lock(&commands->mutex);
  bool drained = commands->exited;
  for (unsigned i = 0; i < commands->config.receipt_capacity; i++)
    if (commands->records[i].readers)
      drained = false;
  pthread_mutex_unlock(&commands->mutex);
  if (!drained)
    return MPC_NATIVE_NOT_READY;
  pthread_join(commands->worker, NULL);
  mpc_executor_commands_detach(commands->config.executor, commands);
  pthread_cond_destroy(&commands->changed);
  pthread_mutex_destroy(&commands->mutex);
  free(commands->records);
  free(commands);
  return MPC_NATIVE_OK;
}
