#include "private.h"
#include "mpc_commands.h"
#include "mpc_adapter_profile.h"
#include "mpc_parameter_profile.h"
#include "mpc_program_parameter_rules.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* Research only. Six fixed parameters on drum pad zero. The native Program
 * setter owns its asynchronous dispatch; this DSO never supplies native queue
 * captures. No raw-address request, arbitrary call, save, or public API binding.
 * Each callback resolves authoritative project/track generations afresh. */
_Static_assert(sizeof(void *) == 4, "ARM32 lab only");
static struct mpc_native_adapter *adapter;
static struct mpc_identity_registry *registry;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned captures;
static struct mpc_executor_identity active_identity;

struct job {
  unsigned operation, parameter;
  struct mpc_executor_identity identity;
  float value, expected, before, after;
  bool invoked, done;
  enum mpc_native_error error;
  pid_t tid;
  struct mpc_identity_snapshot identities;
};

static bool read_at(uint32_t owner, unsigned offset, void *out, size_t bytes) {
  return owner && owner <= UINT32_MAX - offset &&
    !adapter->io.read(adapter->io.userdata, owner + offset, out, bytes);
}

static enum mpc_native_error validate_target(void *unused, const struct mpc_native_target *target) {
  (void)unused;
  struct mpc_native_target current;
  enum mpc_native_error error = mpc_identity_resolve(registry, &active_identity, &current);
  return error                                 ? error
    : memcmp(&current, target, sizeof current) ? MPC_NATIVE_STALE_TARGET
                                               : MPC_NATIVE_OK;
}

static enum mpc_native_error target_for(struct job *job, struct mpc_native_target *target) {
  active_identity = job->identity;
  enum mpc_native_error error = mpc_identity_resolve(registry, &job->identity, target);
  if (error)
    return error;
  uint32_t table, vector[3], instrument, instrument_table;
  if (!read_at(target->program, 0, &table, 4) || table != adapter->io.image + PARAM_drumVtable)
    return MPC_NATIVE_UNSUPPORTED_TRACK_TYPE;
  if (
    !read_at(target->program, PARAM_instruments, vector, sizeof vector) || !vector[0] ||
    vector[1] < vector[0] || vector[2] < vector[1] || vector[1] - vector[0] != 128 * 4 ||
    vector[2] - vector[0] != 128 * 4 || !read_at(vector[0], 0, &instrument, 4) ||
    !read_at(instrument, 0, &instrument_table, 4) ||
    instrument_table != adapter->io.image + PARAM_instrumentVtable)
    return MPC_NATIVE_READ_FAILED;
  return MPC_NATIVE_OK;
}

static void execute(struct job *job) {
  job->tid = (pid_t)syscall(SYS_gettid);
  job->error = mpc_native_verify_ui(adapter);
  if (job->error)
    return;
  if (!registry)
    job->error = mpc_identity_create(adapter, &registry);
  if (job->error)
    return;
  if (job->operation == 0) {
    job->error = mpc_identity_capture(registry, &job->identities);
    return;
  }
  if (job->operation == 3) {
    job->error = mpc_identity_detach(registry);
    return;
  }
  struct mpc_native_target target;
  job->error = target_for(job, &target);
  if (job->error)
    return;
  typedef float (*getter)(void *, int, int);
  getter get = (getter)(adapter->io.image + PARAM_programGet);
  float observed = get((void *)target.program, 0, (int)job->parameter);
  if (!mpc_parameter_canonical_value(job->parameter, observed, &job->before)) {
    job->error = MPC_NATIVE_READ_FAILED;
    return;
  }
  job->after = job->before;
  if (job->operation != 2)
    return;
  /* Reuse the Off-only native mixer policy as an additional conservative gate.
   * Parameter eligibility itself comes solely from this lab's fixed allowlist. */
  struct mpc_command_context context = {0};
  struct mpc_native_track_state flags;
  bool known = false;
  job->error = mpc_native_qualify_command(
    adapter,
    &target,
    MPC_COMMAND_SEND,
    MPC_MUTE_GLOBAL,
    &context,
    &known,
    &flags);
  if (!job->error && context.playing)
    job->error = MPC_NATIVE_PLAYBACK_CONTEXT_UNQUALIFIED;
  struct mpc_navigation_context navigation;
  struct mpc_navigation_state state;
  if (!job->error)
    job->error = mpc_navigation_capture(adapter, target.project, &navigation, &state);
  if (!job->error)
    job->error = mpc_navigation_qualify(&navigation, MPC_COMMAND_LOCATE);
  if (job->error)
    return;
  if (job->before != job->expected) {
    job->error = MPC_NATIVE_PRECONDITION_FAILED;
    return;
  }
  if (job->before == job->value)
    return;
  job->error = target_for(job, &target);
  if (!job->error)
    job->error = mpc_native_qualify_command(
      adapter,
      &target,
      MPC_COMMAND_SEND,
      MPC_MUTE_GLOBAL,
      &context,
      &known,
      &flags);
  if (job->error)
    return;
  job->invoked = true;
  ((void (*)(void *, int, int, float))(
    adapter->io.image +
    PARAM_programSet))((void *)target.program, 0, (int)job->parameter, job->value);
  /* A queued native write may not be visible until a later READ callback. */
  observed = get((void *)target.program, 0, (int)job->parameter);
  if (!mpc_parameter_canonical_value(job->parameter, observed, &job->after))
    job->error = MPC_NATIVE_READ_FAILED;
}

static void invoke(const void *capture) {
  struct job *job = ((const struct mpc_native_function *)capture)->data[0];
  execute(job);
  pthread_mutex_lock(&mutex);
  job->done = true;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&mutex);
}

static int manager(void *out, const void *in, int operation) {
  const struct mpc_native_function *source = in;
  struct mpc_native_function *destination = out;
  if (operation == 0)
    destination->data[0] = NULL;
  else if (operation == 1)
    destination->data[0] = (void *)source;
  else if (operation == 2) {
    destination->data[0] = source->data[0];
    destination->data[1] = NULL;
    pthread_mutex_lock(&mutex);
    captures++;
    pthread_mutex_unlock(&mutex);
  } else if (operation == 3) {
    pthread_mutex_lock(&mutex);
    captures--;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
  }
  return 0;
}

static void dispatch(struct job *job) {
  struct mpc_native_function function = {{job, NULL}, manager, invoke};
  pthread_mutex_lock(&mutex);
  captures++;
  pthread_mutex_unlock(&mutex);
  enum mpc_native_error error = mpc_native_post(adapter, &function);
  if (error)
    job->error = error;
  if (function.manager)
    function.manager(&function, &function, 3);
  pthread_mutex_lock(&mutex);
  /* Never free a running native capture on timeout or client disconnect.
   * A stalled UI keeps this worker and DSO alive; hotload refuses unload. */
  while (captures)
    pthread_cond_wait(&changed, &mutex);
  pthread_mutex_unlock(&mutex);
}

static bool allowed(unsigned id) {
  for (unsigned i = 0; i < sizeof param_ids / sizeof *param_ids; i++)
    if (param_ids[i] == id)
      return true;
  return false;
}

static void respond(FILE *f, struct job *job) {
  fprintf(
    f,
    "{\"ok\":%s,\"error\":\"%s\",\"tid\":%d,\"nativeInvoked\":%s",
    !job->error && job->done ? "true" : "false",
    mpc_native_error_name(job->error),
    job->tid,
    job->invoked ? "true" : "false");
  if (job->operation == 0 && !job->error) {
    const struct mpc_identity_snapshot *s = &job->identities;
    fprintf(
      f,
      ",\"instanceId\":\"%s\",\"project\":\"%" PRIu64 "\",\"projectGeneration\":\"%" PRIu64
      "\",\"tracks\":[",
      s->instance_id,
      s->project,
      s->project_generation);
    for (unsigned i = 0; i < s->track_count; i++) {
      fprintf(
        f,
        "%s{\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\",\"index\":%u}",
        i ? "," : "",
        s->tracks[i].id,
        s->tracks[i].generation,
        s->tracks[i].index);
    }
    fputs("]", f);
  } else if (!job->error && job->operation != 3)
    fprintf(f, ",\"before\":%.9g,\"after\":%.9g", job->before, job->after);
  fputs("}\n", f);
}

static void *worker(void *unused) {
  (void)unused;
  pthread_setname_np(pthread_self(), "mpc native lab");
  char path[108];
  snprintf(path, sizeof path, "/tmp/mpc-native-lab-%d.sock", getpid());
  int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
  if (sock < 0)
    return NULL;
  if (bind(sock, (struct sockaddr *)&address, sizeof address)) {
    close(sock);
    return NULL;
  }
  if (chmod(path, 0600) || listen(sock, 2)) {
    close(sock);
    unlink(path);
    return NULL;
  }
  bool quit = false;
  while (!quit) {
    int fd = accept4(sock, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    struct ucred peer;
    socklen_t size = sizeof peer;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != 0) {
      close(fd);
      continue;
    }
    struct timeval timeout = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    FILE *f = fdopen(fd, "r+");
    if (!f) {
      close(fd);
      continue;
    }
    char line[512], verb[16], extra;
    struct job job = {0};
    if (fgets(line, sizeof line, f) && strchr(line, '\n')) {
      int n = sscanf(
        line,
        "%15s %36s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %u %f %f %c",
        verb,
        job.identity.instance_id,
        &job.identity.project,
        &job.identity.project_generation,
        &job.identity.track,
        &job.identity.track_generation,
        &job.parameter,
        &job.value,
        &job.expected,
        &extra);
      if (!strcmp(line, "LIST\n"))
        dispatch(&job);
      else if (!strcmp(line, "QUIT\n")) {
        job.operation = 3;
        dispatch(&job);
        quit = !job.error && job.done;
      } else if (
        (n == 7 && !strcmp(verb, "READ") &&
         sscanf(
           line,
           "%15s %36s %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %u %c",
           verb,
           job.identity.instance_id,
           &job.identity.project,
           &job.identity.project_generation,
           &job.identity.track,
           &job.identity.track_generation,
           &job.parameter,
           &extra) == 7) ||
        (n == 9 && !strcmp(verb, "SET") && isfinite(job.value) && isfinite(job.expected) &&
         job.value >= 0 && job.value <= 1 && job.expected >= 0 && job.expected <= 1)) {
        job.operation = n == 7 ? 1 : 2;
        if (allowed(job.parameter))
          dispatch(&job);
        else
          job.error = MPC_NATIVE_INVALID_ARGUMENT;
      } else
        job.error = MPC_NATIVE_INVALID_ARGUMENT;
      respond(f, &job);
    }
    fclose(f);
  }
  close(sock);
  unlink(path);
  return NULL;
}

__attribute__((constructor)) static void start(void) {
  const struct mpc_native_hooks hooks = {MPC_NATIVE_ABI, sizeof hooks, NULL, validate_target};
  if (mpc_native_open(&hooks, &adapter))
    return;
  for (unsigned i = 0; i < sizeof param_code / sizeof *param_code; i++) {
    unsigned char code[32];
    if (
      adapter->io
        .read(adapter->io.userdata, adapter->io.image + param_code[i].rva, code, sizeof code) ||
      memcmp(code, param_code[i].bytes, sizeof code)) {
      mpc_native_close(adapter);
      adapter = NULL;
      return;
    }
  }
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  pthread_t thread;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 512 * 1024);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&thread, &attributes, worker, NULL);
  pthread_attr_destroy(&attributes);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (rc) {
    mpc_native_close(adapter);
    adapter = NULL;
  }
}

__attribute__((destructor)) static void finish(void) {
  /* The loader must observe the worker exit and the UI in poll after QUIT. */
  if (registry)
    mpc_identity_destroy(registry);
  if (adapter)
    mpc_native_close(adapter);
}
