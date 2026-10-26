#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "mpc_executor.h"
#include "mpc_commands.h"

/* Bounded lab bridge. No arbitrary calls, raw writes, saves, or transport commands.
 * Commands are local/root-only. PULSE restores its original requested value;
 * SET is an explicit desired-state lab write with no automatic restoration. */
_Static_assert(sizeof(void *) == 4, "32-bit ARM only");
struct flags {
  uint32_t track, program;
  unsigned mute, solo, arm, audio_solo, audio_arm, silenced;
};
struct job {
  int ok;
  pid_t tid;
  unsigned before, count;
  bool completed;
  struct mpc_native_write_result write;
  char error[128];
  struct flags tracks[1];
};
static struct mpc_executor *executor;
static struct mpc_commands *commands;
static bool authoritative;
static struct {
  pthread_mutex_t mutex;
  struct mpc_native_target target;
  uint64_t generation;
} lab = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static char socket_path[108];

/* This private lab registry is deliberately weaker than #20's authoritative
 * project/generation registry. Raw command addresses stay here; executor jobs
 * carry only this registry's opaque identity and generation. */
static enum mpc_native_error lab_resolve(
  void *unused, const struct mpc_executor_identity *identity, struct mpc_native_target *target) {
  (void)unused;
  pthread_mutex_lock(&lab.mutex);
  bool current = identity->project == 1 && identity->track == 1 &&
    identity->track_generation == 1 && identity->project_generation == lab.generation;
  if (current)
    *target = lab.target;
  pthread_mutex_unlock(&lab.mutex);
  return current ? MPC_NATIVE_OK : MPC_NATIVE_STALE_TARGET;
}

static void release(struct job *job) {
  free(job);
}

static struct job *dispatch(
  int op,
  int property,
  int value,
  int expected,
  uint32_t project,
  uint32_t track,
  uint32_t program) {
  struct job *job = calloc(1, sizeof *job);
  if (!job)
    return NULL;
  enum mpc_native_error error = mpc_executor_wait_ready(executor, 3000);
  struct mpc_executor_status status;
  if (!error)
    error = mpc_executor_get_status(executor, &status);
  if (!error) {
    job->tid = (pid_t)status.ui_thread_id;
    if (op) {
      pthread_mutex_lock(&lab.mutex);
      lab.target = (struct mpc_native_target){project, track, program};
      uint64_t generation = ++lab.generation;
      pthread_mutex_unlock(&lab.mutex);
      struct mpc_executor_request request = {
        .operation = op == 1 ? MPC_EXECUTOR_READ_TRACK
          : property == 0    ? MPC_EXECUTOR_SET_MUTE
          : property == 1    ? MPC_EXECUTOR_SET_SOLO
                             : MPC_EXECUTOR_SET_RECORD_ARMED,
        .identity =
          {.project = 1, .project_generation = generation, .track = 1, .track_generation = 1},
        .desired = value != 0,
        .has_expected = op == 2,
        .expected = expected != 0};
      struct mpc_executor_job *native_job;
      error = mpc_executor_submit(executor, &request, &native_job);
      if (!error) {
        struct mpc_executor_result result;
        error = mpc_executor_wait(native_job, 3000, &result);
        job->completed = result.state == MPC_EXECUTOR_COMPLETED;
        if (job->completed) {
          job->write = result.write;
        }
        if (job->completed && (!error || result.write.changed_known)) {
          const struct mpc_native_track_state *state = &result.observed;
          job->count = 1;
          job->before = expected;
          job->tracks[0] = (struct flags){track,
                                          program,
                                          state->muted,
                                          state->soloed,
                                          state->record_armed,
                                          state->audio_soloed,
                                          state->audio_record_armed,
                                          state->silenced};
        }
        mpc_executor_release(native_job);
      }
    }
  }
  job->ok = !error;
  if (error)
    snprintf(job->error, sizeof job->error, "%s", mpc_native_error_name(error));
  return job;
}
static void report(FILE *f, const char *phase, struct job *j) {
  if (!j) {
    fprintf(f, "{\"phase\":\"%s\",\"ok\":false,\"error\":\"allocation failed\"}\n", phase);
    fflush(f);
    return;
  }
  fprintf(
    f,
    "{\"phase\":\"%s\",\"ok\":%s,\"tid\":%d,\"pid\":%d,\"error\":\"%s\","
    "\"completed\":%s,\"nativeInvoked\":%s,\"changedKnown\":%s,\"changed\":%s,\"tracks\":[",
    phase,
    j->ok ? "true" : "false",
    j->tid,
    getpid(),
    j->error,
    j->completed ? "true" : "false",
    !j->completed               ? "null"
      : j->write.native_invoked ? "true"
                                : "false",
    j->write.changed_known ? "true" : "false",
    j->write.changed ? "true" : "false");
  for (unsigned k = 0; k < j->count; k++) {
    struct flags *s = &j->tracks[k];
    fprintf(
      f,
      "%s{\"track\":\"%08x\",\"program\":\"%08x\",\"mute\":%u,\"solo\":%u,\"arm\":%u,\"audioSolo\":"
      "%u,\"audioArm\":%u,\"silenced\":%u}",
      k ? "," : "",
      s->track,
      s->program,
      s->mute,
      s->solo,
      s->arm,
      s->audio_solo,
      s->audio_arm,
      s->silenced);
  }
  fprintf(f, "]}\n");
  fflush(f);
}
static void pause_ms(unsigned ms) {
  struct timespec d = {ms / 1000, (long)(ms % 1000) * 1000000};
  while (nanosleep(&d, &d) && errno == EINTR) {
  }
}
static void stop_executor(void) {
  if (commands) {
    if (mpc_commands_shutdown(commands, 3000) || mpc_commands_close(commands))
      return;
    commands = NULL;
  }
  if (
    executor && mpc_executor_shutdown(executor, 3000) == MPC_NATIVE_OK &&
    mpc_executor_close(executor) == MPC_NATIVE_OK)
    executor = NULL;
}
/* Explicitly experimental host policy: the operator owns the stopped-mode and
 * settings qualification for this bounded lab session, as in #31. This does NOT
 * supply production live mode/settings validation. The Unix service must keep
 * writes disabled until #32 supplies that policy and authenticated routing. */
static enum mpc_native_error
lab_command_policy(void *unused, enum mpc_command_operation operation) {
  (void)unused;
  return (unsigned)operation <= MPC_COMMAND_RECORD_ARMED ? MPC_NATIVE_OK
                                                         : MPC_NATIVE_INVALID_ARGUMENT;
}

static enum mpc_native_error live_command_policy(void *data, enum mpc_command_operation operation) {
  return mpc_executor_qualify_command(data, operation);
}

static void command_request(FILE *stream, const char *line) {
  enum mpc_native_error error = MPC_NATIVE_INVALID_ARGUMENT;
  char instance[37], id[MPC_COMMAND_ID_BYTES], property[16], expected[2], extra;
  unsigned value, start_ms, wait_ms;
  struct mpc_command_receipt *receipt = calloc(1, sizeof *receipt);
  if (!receipt)
    return;
  if (!strchr(line, '\n')) {
    fprintf(stream, "{\"ok\":false,\"error\":\"INVALID_ARGUMENT\"}\n");
    free(receipt);
    return;
  }
  if (
    (sscanf(line, "LAB_COMMAND_OPEN %36s %c", instance, &extra) == 1 &&
     !strncmp(line, "LAB_COMMAND_OPEN ", 17)) ||
    (sscanf(line, "LIVE_COMMAND_OPEN %36s %c", instance, &extra) == 1 &&
     !strncmp(line, "LIVE_COMMAND_OPEN ", 18))) {
    if (authoritative && !commands) {
      struct mpc_commands_config config = {
        .abi_version = MPC_COMMANDS_ABI,
        .struct_bytes = sizeof config,
        .executor = executor,
        .pending_capacity = 16,
        .receipt_capacity = 128,
        .retention_ms = MPC_COMMAND_MIN_RETENTION_MS,
        .policy = {NULL, lab_command_policy}};
      memcpy(config.instance_id, instance, 37);
      if (!strncmp(line, "LIVE_COMMAND_OPEN ", 18))
        config.policy = (struct mpc_command_policy){executor, live_command_policy};
      error = mpc_commands_open(&config, &commands);
    } else
      error = MPC_NATIVE_NOT_READY;
  } else if (!strcmp(line, "LAB_COMMAND_CLOSE\n")) {
    error = commands ? mpc_commands_shutdown(commands, 3000) : MPC_NATIVE_NOT_READY;
    if (!error)
      error = mpc_commands_close(commands);
    if (!error)
      commands = NULL;
  } else if (commands && !strncmp(line, "LAB_COMMAND_GET ", 16)) {
    if (sscanf(line, "LAB_COMMAND_GET %36s %80s %u %c", instance, id, &wait_ms, &extra) == 3)
      error = mpc_commands_get(commands, 0, instance, id, wait_ms, receipt);
  } else if (commands && !strncmp(line, "LAB_COMMAND_SEQUENCE_SET ", 25)) {
    struct mpc_command_request request = {
      .operation = MPC_COMMAND_MUTE,
      .mute_scope = MPC_MUTE_SEQUENCE};
    int fields = sscanf(
      line,
      "LAB_COMMAND_SEQUENCE_SET %u %1s %36s %" SCNu64 " %" SCNu64 " %80s %u %u %" SCNu64 " %" SCNu64
      " %c",
      &value,
      expected,
      request.target.instance_id,
      &request.target.project,
      &request.target.track,
      request.request_id,
      &start_ms,
      &wait_ms,
      &request.sequence.sequence,
      &request.sequence.generation,
      &extra);
    if (fields == 10 && value <= 1 && strchr("-01", expected[0])) {
      memcpy(request.sequence.instance_id, request.target.instance_id, 37);
      request.sequence.project = request.target.project;
      request.value = value != 0;
      request.has_expected = expected[0] != '-';
      request.expected = expected[0] == '1';
      request.start_within_ms = start_ms;
      error = mpc_commands_submit(commands, 0, &request, wait_ms, receipt);
    }
  } else if (commands && !strncmp(line, "LAB_COMMAND_SET ", 16)) {
    struct mpc_command_request request = {0};
    int fields = sscanf(
      line,
      "LAB_COMMAND_SET %15s %u %1s %36s %" SCNu64 " %" SCNu64 " %80s %u %u %c",
      property,
      &value,
      expected,
      request.target.instance_id,
      &request.target.project,
      &request.target.track,
      request.request_id,
      &start_ms,
      &wait_ms,
      &extra);
    if (
      fields == 9 && value <= 1 && strchr("-01", expected[0]) &&
      (!strcmp(property, "mute") || !strcmp(property, "solo") || !strcmp(property, "arm"))) {
      request.operation = !strcmp(property, "mute") ? MPC_COMMAND_MUTE
        : !strcmp(property, "solo")                 ? MPC_COMMAND_SOLO
                                                    : MPC_COMMAND_RECORD_ARMED;
      request.value = value != 0;
      request.has_expected = expected[0] != '-';
      request.expected = expected[0] == '1';
      request.start_within_ms = start_ms;
      error = mpc_commands_submit(commands, 0, &request, wait_ms, receipt);
    }
  }
  if (!error && receipt->request.request_id[0]) {
    char *json = mpc_commands_receipt_json(receipt);
    if (json) {
      fputs(json, stream);
      free(json);
    }
  } else
    fprintf(
      stream,
      "{\"ok\":%s,\"error\":\"%s\"}\n",
      error ? "false" : "true",
      mpc_native_error_name(error));
  free(receipt);
}

static void identity_command(FILE *stream, const char *line) {
  struct mpc_executor_request request = {0};
  struct mpc_executor_identity *identity = &request.identity;
  enum mpc_native_error error = MPC_NATIVE_OK;
  if (!strcmp(line, "IDENTITIES\n")) {
    if (!authoritative) {
      error = mpc_executor_shutdown(executor, 3000);
      if (!error)
        error = mpc_executor_close(executor);
      if (!error) {
        executor = NULL;
        error = mpc_executor_open_authoritative(16, &executor);
        authoritative = !error;
      }
    }
    request.operation = MPC_EXECUTOR_IDENTITIES;
  } else if (!authoritative)
    error = MPC_NATIVE_NOT_READY;
  else {
    char extra, property[16];
    unsigned desired, expected;
    int fields;
    if (!strncmp(line, "HANDLE_READ ", 12)) {
      fields = sscanf(
        line,
        "HANDLE_READ %36s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %c",
        identity->instance_id,
        &identity->project,
        &identity->project_generation,
        &identity->track,
        &identity->track_generation,
        &extra);
      request.operation = MPC_EXECUTOR_READ_TRACK;
      if (fields != 5)
        error = MPC_NATIVE_INVALID_ARGUMENT;
    } else if (!strncmp(line, "HANDLE_SEQUENCE ", 16)) {
      fields = sscanf(
        line,
        "HANDLE_SEQUENCE %36s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %c",
        identity->instance_id,
        &identity->project,
        &identity->project_generation,
        &identity->sequence,
        &identity->sequence_generation,
        &extra);
      request.operation = MPC_EXECUTOR_VALIDATE_SEQUENCE;
      if (fields != 5)
        error = MPC_NATIVE_INVALID_ARGUMENT;
    } else {
      fields = sscanf(
        line,
        "HANDLE_SET %15s %u %u %36s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %c",
        property,
        &desired,
        &expected,
        identity->instance_id,
        &identity->project,
        &identity->project_generation,
        &identity->track,
        &identity->track_generation,
        &extra);
      if (fields != 8 || desired > 1 || expected > 1)
        error = MPC_NATIVE_INVALID_ARGUMENT;
      else {
        request.operation = !strcmp(property, "mute") ? MPC_EXECUTOR_SET_MUTE
          : !strcmp(property, "solo")                 ? MPC_EXECUTOR_SET_SOLO
                                                      : MPC_EXECUTOR_SET_RECORD_ARMED;
        if (strcmp(property, "mute") && strcmp(property, "solo") && strcmp(property, "arm"))
          error = MPC_NATIVE_INVALID_ARGUMENT;
        request.desired = desired != 0;
        request.has_expected = true;
        request.expected = expected != 0;
      }
    }
  }
  struct mpc_executor_result result = {0};
  if (!error)
    error = mpc_executor_wait_ready(executor, 3000);
  struct mpc_executor_job *job;
  if (!error) {
    error = mpc_executor_submit(executor, &request, &job);
    if (!error) {
      error = mpc_executor_wait(job, 3000, &result);
      mpc_executor_release(job);
    }
  }
  fprintf(
    stream,
    "{\"ok\":%s,\"error\":\"%s\",\"nativeInvoked\":%s",
    error ? "false" : "true",
    mpc_native_error_name(error),
    result.write.native_invoked ? "true" : "false");
  if (!error && request.operation == MPC_EXECUTOR_IDENTITIES) {
    const struct mpc_identity_snapshot *snapshot = &result.identities;
    fprintf(
      stream,
      ",\"instanceId\":\"%s\",\"projectId\":\"%" PRIu64 "\",\"projectGeneration\":\"%" PRIu64
      "\",\"selectedTrackId\":\"%" PRIu64 "\",\"selectedSequenceId\":\"%" PRIu64
      "\",\"sequenceSlots\":%u",
      snapshot->instance_id,
      snapshot->project,
      snapshot->project_generation,
      snapshot->selected_track,
      snapshot->selected_sequence,
      snapshot->sequence_slots);
    for (unsigned kind = 0; kind < 2; kind++) {
      unsigned count = kind ? snapshot->sequence_count : snapshot->track_count;
      const struct mpc_identity_object *objects = kind ? snapshot->sequences : snapshot->tracks;
      fprintf(stream, ",\"%s\":[", kind ? "sequences" : "tracks");
      for (unsigned i = 0; i < count; i++)
        fprintf(
          stream,
          "%s{\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\",\"index\":%u}",
          i ? "," : "",
          objects[i].id,
          objects[i].generation,
          objects[i].index);
      fprintf(stream, "]");
    }
  } else if (!error && request.operation == MPC_EXECUTOR_READ_TRACK)
    fprintf(
      stream,
      ",\"mute\":%s,\"solo\":%s,\"arm\":%s",
      result.observed.muted ? "true" : "false",
      result.observed.soloed ? "true" : "false",
      result.observed.record_armed ? "true" : "false");
  fprintf(stream, "}\n");
}
static void state_command(FILE *stream, const char *line) {
  char instance[37], extra;
  enum mpc_native_error error = MPC_NATIVE_NOT_READY;
  struct mpc_executor_status status;
  struct mpc_executor_job *job = NULL;
  struct mpc_executor_result result = {0};
  if (sscanf(line, "STATE_V4 %36s %c", instance, &extra) != 1)
    error = MPC_NATIVE_INVALID_ARGUMENT;
  else if (authoritative) {
    error = mpc_executor_get_status(executor, &status);
    if (!error && strcmp(instance, status.adapter.instance_id))
      error = MPC_NATIVE_STALE_TARGET;
    if (!error) {
      struct mpc_executor_request request = {
        .operation = MPC_EXECUTOR_GET_STATE,
        .queue_timeout_ms = 150};
      error = mpc_executor_submit(executor, &request, &job);
    }
    if (!error)
      error = mpc_executor_wait(job, 150, &result);
  }
  uint32_t header[] = {MPC_STATE_ABI, sizeof(struct mpc_state_snapshot), error};
  fwrite(header, sizeof header, 1, stream);
  if (!error)
    fwrite(result.snapshot, sizeof *result.snapshot, 1, stream);
  fflush(stream);
  if (job)
    mpc_executor_release(job);
}
static void *worker(void *unused) {
  (void)unused;
  pthread_setname_np(pthread_self(), "mpc native lab");
  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (sock < 0)
    goto stopped;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strcpy(addr.sun_path, socket_path);
  if (bind(sock, (struct sockaddr *)&addr, sizeof addr)) {
    close(sock);
    goto stopped;
  }
  if (chmod(socket_path, 0600) || listen(sock, 2)) {
    close(sock);
    unlink(socket_path);
    goto stopped;
  }
  int quit = 0;
  while (!quit) {
    int fd = accept4(sock, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != 0) {
      close(fd);
      continue;
    }
    struct timeval timeout = {15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    FILE *f = fdopen(fd, "r+");
    if (!f) {
      close(fd);
      continue;
    }
    char line[256], verb[16] = {0}, prop[16] = {0}, extra;
    if (fgets(line, sizeof line, f)) {
      if (!strncmp(line, "LAB_COMMAND_", 12) || !strncmp(line, "LIVE_COMMAND_", 13)) {
        command_request(f, line);
      } else if (!strncmp(line, "STATE_V4 ", 9)) {
        state_command(f, line);
      } else if (!strcmp(line, "IDENTITIES\n") || !strncmp(line, "HANDLE_", 7)) {
        identity_command(f, line);
      } else if (!strcmp(line, "ADAPTER_STATUS\n")) {
        struct mpc_executor_status status;
        enum mpc_native_error error = mpc_executor_wait_ready(executor, 0);
        if (!error)
          error = mpc_executor_get_status(executor, &status);
        if (error == MPC_NATIVE_OK)
          fprintf(
            f,
            "%s %s %s %s VERIFIED\n",
            authoritative ? "MPC_ADAPTER_STATE_V4" : "MPC_ADAPTER_V2",
            status.adapter.instance_id,
            status.adapter.profile_id,
            status.adapter.build_id);
        else
          fprintf(f, "MPC_ADAPTER_ERROR %s\n", mpc_native_error_name(error));
      } else if (!strncmp(line, "READ ", 5)) {
        uint32_t project, track, program;
        if (sscanf(line, "READ %x %x %x %c", &project, &track, &program, &extra) == 3) {
          struct job *j = dispatch(1, 0, 0, 0, project, track, program);
          report(f, "read", j);
          if (j)
            release(j);
        } else
          fprintf(f, "{\"ok\":false,\"error\":\"invalid READ target\"}\n");
      } else if (!strncmp(line, "SET ", 4)) {
        unsigned value, expected;
        uint32_t project, track, program;
        int n = sscanf(
          line,
          "SET %15s %u %u %x %x %x %c",
          prop,
          &value,
          &expected,
          &project,
          &track,
          &program,
          &extra);
        int property = !strcmp(prop, "mute") ? 0
          : !strcmp(prop, "solo")            ? 1
          : !strcmp(prop, "arm")             ? 2
                                             : -1;
        if (n == 6 && property >= 0 && value <= 1 && expected <= 1) {
          struct job *j = dispatch(2, property, value, expected, project, track, program);
          report(f, "set", j);
          release(j);
        } else
          fprintf(f, "{\"ok\":false,\"error\":\"invalid SET request\"}\n");
      } else if (!strcmp(line, "PING\n")) {
        struct job *j = dispatch(0, 0, 0, 0, 0, 0, 0);
        report(f, "ping", j);
        if (j)
          release(j);
      } else if (!strcmp(line, "QUIT\n")) {
        enum mpc_native_error error =
          commands ? mpc_commands_shutdown(commands, 3000) : MPC_NATIVE_OK;
        if (!error && commands) {
          error = mpc_commands_close(commands);
          if (!error)
            commands = NULL;
        }
        if (!error)
          error = mpc_executor_shutdown(executor, 3000);
        if (!error)
          error = mpc_executor_close(executor);
        if (!error) {
          executor = NULL;
          quit = 1;
          fprintf(f, "{\"ok\":true,\"phase\":\"stopping\"}\n");
        } else
          fprintf(f, "{\"ok\":false,\"error\":\"callbacks still pending; retry QUIT\"}\n");
      } else {
        unsigned value, expected, hold;
        uint32_t project, track, program;
        int n = sscanf(
          line,
          "%15s %15s %u %u %u %x %x %x %c",
          verb,
          prop,
          &value,
          &expected,
          &hold,
          &project,
          &track,
          &program,
          &extra);
        int property = !strcmp(prop, "mute") ? 0
          : !strcmp(prop, "solo")            ? 1
          : !strcmp(prop, "arm")             ? 2
                                             : -1;
        if (
          n == 8 && !strcmp(verb, "PULSE") && property >= 0 && value <= 1 && expected <= 1 &&
          hold >= 100 && hold <= 10000) {
          struct job *j = dispatch(2, property, value, expected, project, track, program);
          int applied = j && j->ok;
          unsigned original = j ? j->before : 0;
          report(f, "accepted", j);
          if (j)
            release(j);
          if (applied) {
            pause_ms(150);
            j = dispatch(1, property, 0, 0, project, track, program);
            report(f, "applied", j);
            if (j)
              release(j);
            pause_ms(hold);
            j = dispatch(2, property, original, value, project, track, program);
            report(f, "restoreAccepted", j);
            if (j)
              release(j);
            pause_ms(150);
            j = dispatch(1, property, 0, 0, project, track, program);
            report(f, "restored", j);
            if (j)
              release(j);
          }
        } else {
          fprintf(f, "{\"ok\":false,\"error\":\"expected PING, QUIT, or bounded PULSE\"}\n");
        }
      }
    }
    fclose(f);
  }
  close(sock);
  unlink(socket_path);
stopped:
  stop_executor();
  return NULL;
}
__attribute__((constructor)) static void start(void) {
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  struct mpc_executor_config config = {MPC_EXECUTOR_ABI, sizeof config, 16, NULL, lab_resolve};
  if (mpc_executor_open(&config, &executor) != MPC_NATIVE_OK) {
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return;
  }
  snprintf(socket_path, sizeof socket_path, "/tmp/mpc-native-lab-%d.sock", getpid());
  pthread_t thread;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 256 * 1024);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&thread, &attributes, worker, NULL);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  pthread_attr_destroy(&attributes);
  if (rc)
    stop_executor();
}
__attribute__((destructor)) static void finish(void) {
  /* QUIT must drain/close the executor, and the host must observe both workers
   * exiting and the UI returning to poll before dlclose. Never free captures
   * merely because a shutdown deadline expired. */
  if (executor && mpc_executor_close(executor) == MPC_NATIVE_OK)
    executor = NULL;
}
