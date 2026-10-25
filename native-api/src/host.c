#include "host.h"
#include "api.h"
#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct host {
  pthread_mutex_t mutex;
  pthread_t service;
  struct mpc_executor *executor;
  const struct mpc_host_ops *ops;
  struct api_backend backend;
  char socket[108], device_id[37];
  bool service_started, wanted;
  atomic_int service_stop, service_exited;
  enum mpc_native_error error;
  const char *phase;
};

static int probe(void *data, char instance[37], int *ready) {
  struct host *h = data;
  struct mpc_executor_status status;
  pthread_mutex_lock(&h->mutex);
  int ok = h->executor && !mpc_executor_get_status(h->executor, &status) &&
    status.state == MPC_EXECUTOR_READY;
  if (ok)
    strcpy(instance, status.adapter.instance_id);
  *ready = ok;
  pthread_mutex_unlock(&h->mutex);
  return ok;
}

static int state(void *data, const char *instance, struct mpc_state_snapshot *out) {
  struct host *h = data;
  struct mpc_executor_status status;
  struct mpc_executor_job *job = NULL;
  struct mpc_executor_result result;
  pthread_mutex_lock(&h->mutex);
  enum mpc_native_error error =
    h->executor ? mpc_executor_get_status(h->executor, &status) : MPC_NATIVE_NOT_READY;
  if (!error && strcmp(instance, status.adapter.instance_id))
    error = MPC_NATIVE_STALE_TARGET;
  if (!error) {
    struct mpc_executor_request request = {
      .operation = MPC_EXECUTOR_GET_STATE,
      .queue_timeout_ms = 150};
    error = mpc_executor_submit(h->executor, &request, &job);
  }
  if (!error)
    error = mpc_executor_wait(job, 150, &result);
  if (!error)
    *out = *result.snapshot;
  if (job)
    mpc_executor_release(job);
  pthread_mutex_unlock(&h->mutex);
  return error;
}

static int service_stopping(void *data) {
  struct host *h = data;
  return atomic_load(&h->service_stop) || h->ops->exiting(h->ops->userdata);
}

static void *serve(void *data) {
  struct host *h = data;
  pthread_setname_np(pthread_self(), "mpc api service");
  /* The standalone binary uses the same transport and protocol. This entry
   * leaves MPC's signals, environment, umask and child processes alone. */
  api_serve_embedded(
    h->socket,
    h->device_id,
    &h->backend,
    service_stopping,
    h,
    h->ops->discovery_root);
  atomic_store(&h->service_exited, 1);
  return NULL;
}

static int remove_stale(int directory, const char *name) {
  struct stat s;
  if (fstatat(directory, name, &s, AT_SYMLINK_NOFOLLOW))
    return errno == ENOENT ? 0 : -1;
  /* Only under the lifetime flock. Never replace files, symlinks or a socket
   * belonging to another UID. A second host cannot acquire this lock. */
  return S_ISSOCK(s.st_mode) && s.st_uid == geteuid() ? unlinkat(directory, name, 0) : -1;
}

static int configuration(int installation) {
  int fd = openat(installation, "mpc-native-api.conf", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -1;
  FILE *f = fdopen(fd, "r");
  if (!f) {
    close(fd);
    return -1;
  }
  char line[256];
  int enabled = -1;
  bool invalid = false;
  while (fgets(line, sizeof line, f)) {
    if (!strchr(line, '\n') && !feof(f)) {
      invalid = true;
      break;
    }
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] || line[0] == '#')
      continue;
    if (enabled == -1 && (!strcmp(line, "enabled=0") || !strcmp(line, "enabled=1")))
      enabled = line[8] - '0';
    else
      invalid = true;
  }
  invalid |= ferror(f) != 0;
  fclose(f);
  return invalid ? -1 : enabled;
}

static int device_id(int installation, char id[37]) {
  int fd = openat(installation, "device-id", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0 && errno == ENOENT) {
    /* Persist one random installation identity; never derive it from a serial.
     * Atomic rename avoids a partial ID surviving an interrupted first start. */
    if (api_instance_new(id))
      return -1;
    memcpy(id, "dev_", 4);
    fd = openat(
      installation,
      "device-id.new",
      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
      0600);
    if (fd < 0)
      return -1;
    int ok = write(fd, id, 36) == 36 && !fsync(fd);
    close(fd);
    if (!ok || renameat(installation, "device-id.new", installation, "device-id")) {
      unlinkat(installation, "device-id.new", 0);
      return -1;
    }
    return fsync(installation);
  }
  if (fd < 0)
    return -1;
  char buffer[38] = {0};
  ssize_t n = read(fd, buffer, sizeof buffer);
  close(fd);
  if (n != 36 || memcmp(buffer, "dev_", 4) || strspn(buffer + 4, "0123456789abcdef") != 32)
    return -1;
  memcpy(id, buffer, 36);
  id[36] = 0;
  return 0;
}

static void stop_host(struct host *h) {
  h->wanted = false;
  h->ops->cancel_start(h->ops->userdata);
  if (h->service_started) {
    atomic_store(&h->service_stop, 1);
    pthread_join(h->service, NULL);
    h->service_started = false;
  }
  if (h->executor) {
    /* All API callers have joined. A deadline never frees pending native
     * captures or listeners; keep the executor and pinned DSO and retry. */
    h->error = mpc_executor_shutdown(h->executor, 100);
    if (!h->error)
      h->error = mpc_executor_close(h->executor);
    if (h->error) {
      h->phase = "draining";
      return;
    }
    h->executor = NULL;
  }
  h->phase = "stopped";
  h->error = MPC_NATIVE_OK;
}

static void start_host(struct host *h, int installation, int directory) {
  if (h->service_started || h->executor)
    return;
  int enabled = configuration(installation);
  if (enabled != 1) {
    h->phase = enabled == 0 ? "disabled" : "configuration_error";
    return;
  }
  if (remove_stale(directory, "api.sock")) {
    h->phase = "service_failed";
    return;
  }
  atomic_store(&h->service_stop, 0);
  atomic_store(&h->service_exited, 0);
  if (pthread_create(&h->service, NULL, serve, h)) {
    h->phase = "service_failed";
    return;
  }
  h->service_started = true;
  h->wanted = true;
  h->error = MPC_NATIVE_NOT_READY;
  h->phase = "waiting_for_ui";
}

static void advance(struct host *h) {
  if (!h->wanted) {
    if (h->executor)
      stop_host(h);
    return;
  }
  if (atomic_load(&h->service_exited)) {
    stop_host(h);
    if (!h->executor)
      h->phase = "service_failed";
    return;
  }
  if (!h->executor && !strcmp(h->phase, "waiting_for_ui")) {
    struct mpc_executor *executor = NULL;
    h->error = h->ops->open(h->ops->userdata, &executor);
    if (!h->error) {
      pthread_mutex_lock(&h->mutex);
      h->executor = executor;
      pthread_mutex_unlock(&h->mutex);
      h->phase = "starting";
    } else if (h->error != MPC_NATIVE_NOT_READY)
      h->phase = "discovery_only";
  }
  if (h->executor) {
    struct mpc_executor_status status;
    h->error = mpc_executor_get_status(h->executor, &status);
    if (!h->error) {
      if (status.state == MPC_EXECUTOR_READY)
        h->phase = "ready";
      else if (status.state == MPC_EXECUTOR_FAILED) {
        h->error = status.failure;
        h->phase = "discovery_only";
      }
    } else
      h->phase = "discovery_only";
  }
}

static int64_t now_ms(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static void control(struct host *h, int listener, int installation, int directory) {
  int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (fd < 0)
    return;
  struct ucred peer;
  socklen_t size = sizeof peer;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != geteuid()) {
    close(fd);
    return;
  }
  char line[16];
  size_t used = 0;
  int64_t deadline = now_ms() + 200;
  while (used < sizeof line - 1 && now_ms() < deadline) {
    struct pollfd p = {fd, POLLIN, 0};
    int64_t remaining = deadline - now_ms();
    if (remaining <= 0 || poll(&p, 1, (int)remaining) <= 0)
      break;
    ssize_t n = recv(fd, line + used, sizeof line - 1 - used, 0);
    if (n <= 0)
      break;
    used += (size_t)n;
    if (memchr(line, '\n', used))
      break;
  }
  line[used] = 0;
  bool valid = used == 7 && !memcmp(line, "STATUS\n", 7);
  if (used == 5 && !memcmp(line, "STOP\n", 5)) {
    stop_host(h);
    valid = true;
  } else if (used == 6 && !memcmp(line, "START\n", 6)) {
    start_host(h, installation, directory);
    valid = true;
  }
  char response[768];
  int n = snprintf(
    response,
    sizeof response,
    "{\"ok\":%s,\"hostVersion\":\"" MPC_HOST_VERSION "\",\"pid\":%ld,\"status\":\"%s\","
    "\"nativeError\":\"%s\",\"deviceId\":\"%s\",\"restartRequiredToUnload\":true}\n",
    valid ? "true" : "false",
    (long)getpid(),
    h->phase,
    mpc_native_error_name(h->error),
    h->device_id);
  if (n > 0 && (size_t)n < sizeof response)
    (void)send(fd, response, (size_t)n, MSG_NOSIGNAL);
  close(fd);
}

int mpc_host_run(const char *installation, const char *runtime, const struct mpc_host_ops *ops) {
  int install = open(installation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (install < 0)
    return 1;
  if (mkdir(runtime, 0700) && errno != EEXIST) {
    close(install);
    return 1;
  }
  int directory = open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat s;
  if (directory < 0 || fstat(directory, &s) || s.st_uid != geteuid() || (s.st_mode & 0077)) {
    if (directory >= 0)
      close(directory);
    close(install);
    return 1;
  }
  int lock = openat(directory, "host.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  int listener = -1, result = 1;
  struct host h = {.ops = ops, .phase = "stopped"};
  pthread_mutex_init(&h.mutex, NULL);
  h.backend = (struct api_backend){&h, probe, state};
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) || device_id(install, h.device_id))
    goto done;
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  int n = snprintf(address.sun_path, sizeof address.sun_path, "%s/control.sock", runtime);
  int m = snprintf(h.socket, sizeof h.socket, "%s/api.sock", runtime);
  if (n < 0 || n >= (int)sizeof address.sun_path || m < 0 || m >= (int)sizeof h.socket)
    goto done;
  if (remove_stale(directory, "control.sock"))
    goto done;
  listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof address))
    goto done;
  struct stat owned;
  if (fstatat(directory, "control.sock", &owned, AT_SYMLINK_NOFOLLOW))
    goto done;
  api_socket_remember(1, address.sun_path, &owned);
  if (fchmodat(directory, "control.sock", 0600, 0) || listen(listener, 8))
    goto cleanup;
  pthread_setname_np(pthread_self(), "mpc api host");
  start_host(&h, install, directory);
  while (!ops->exiting(ops->userdata)) {
    advance(&h);
    struct pollfd p = {listener, POLLIN, 0};
    int ready = poll(&p, 1, 100);
    if (ready < 0 && errno != EINTR)
      break;
    if (ready > 0 && (p.revents & POLLIN))
      control(&h, listener, install, directory);
  }
  stop_host(&h);
  /* The process-exit path cannot wait for a UI thread which is itself exiting.
   * The production DSO is NODELETE; pending callbacks remain mapped until exit.
   * The isolated harness exits only after its UI pump has drained this owner. */
  if (h.executor)
    return 1;
  result = 0;
cleanup:
  if (
    !fstatat(directory, "control.sock", &s, AT_SYMLINK_NOFOLLOW) && s.st_ino == owned.st_ino &&
    s.st_dev == owned.st_dev)
    unlinkat(directory, "control.sock", 0);
done:
  if (listener >= 0)
    close(listener);
  if (lock >= 0)
    close(lock);
  close(directory);
  close(install);
  pthread_mutex_destroy(&h.mutex);
  return result;
}
