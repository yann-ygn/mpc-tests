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
  if (
    executor && mpc_executor_shutdown(executor, 3000) == MPC_NATIVE_OK &&
    mpc_executor_close(executor) == MPC_NATIVE_OK)
    executor = NULL;
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
      if (!strcmp(line, "IDENTITIES\n") || !strncmp(line, "HANDLE_", 7)) {
        identity_command(f, line);
      } else if (!strcmp(line, "ADAPTER_STATUS\n")) {
        struct mpc_executor_status status;
        enum mpc_native_error error = mpc_executor_wait_ready(executor, 0);
        if (!error)
          error = mpc_executor_get_status(executor, &status);
        if (error == MPC_NATIVE_OK)
          fprintf(
            f,
            "MPC_ADAPTER_V1 %s %s %s VERIFIED\n",
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
        enum mpc_native_error error = mpc_executor_shutdown(executor, 3000);
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
