#include "host.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Temporary qualification entry point. Production host/server/adapter code is
 * unchanged; only startup, private paths and off-UI measurement differ. The
 * operator stops the installed host first and checks finished + UI quiescence
 * before unloading. No NODELETE, preload installation, native hooks or exports. */
static char installation[108], runtime[108], exit_path[128];
static pthread_t thread;
static atomic_int finished;
static int started;
static FILE *metrics;

static enum mpc_native_error open_executor(void *unused, struct mpc_executor **out) {
  (void)unused;
  return mpc_executor_open_authoritative(16, out);
}

static void cancel(void *unused) {
  (void)unused;
}

static int exiting(void *unused) {
  (void)unused;
  return !access(exit_path, F_OK);
}

static void observed(
  void *unused, bool full, enum mpc_native_error error, const struct mpc_executor_result *result) {
  (void)unused;
  if (!metrics)
    return;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t native_ns = 0;
  unsigned tracks = 0;
  if (!error && full) {
    native_ns = result->snapshot->duration_ns;
    tracks = result->snapshot->track_count;
  } else if (!error) {
    native_ns = result->flags->duration_ns;
    tracks = result->flags->track_count;
  }
  fprintf(
    metrics,
    "{\"atMs\":%" PRIu64 ",\"full\":%s,\"error\":%u,\"nativeNs\":%" PRIu64
    ",\"callbackUs\":%" PRIu64 ",\"tracks\":%u,\"shared\":%s}\n",
    (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000,
    full ? "true" : "false",
    (unsigned)error,
    native_ns,
    result->ui_callback_us,
    tracks,
    result->flags_shared ? "true" : "false");
  fflush(metrics);
}

static void *run(void *unused) {
  (void)unused;
  char path[128];
  snprintf(path, sizeof path, "%s/metrics.jsonl", installation);
  metrics = fopen(path, "w");
  const struct mpc_host_ops ops = {
    .discovery_root = "",
    .open = open_executor,
    .cancel_start = cancel,
    .exiting = exiting,
    .native_command_policy = true,
    .thread_name = "mpc native lab",
    .observed = observed};
  int status = mpc_host_run(installation, runtime, &ops);
  if (metrics)
    fclose(metrics);
  metrics = NULL;
  snprintf(path, sizeof path, "%s/finished", installation);
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f, "%d\n", status);
    fclose(f);
  }
  atomic_store(&finished, 1);
  return NULL;
}

__attribute__((constructor)) static void start(void) {
  Dl_info info;
  char executable[256];
  ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
  if (n <= 0)
    return;
  executable[n] = 0;
  if (
    strcmp(executable, "/usr/bin/MPC") || !dladdr((void *)start, &info) || !info.dli_fname ||
    strlen(info.dli_fname) >= sizeof installation)
    return;
  strcpy(installation, info.dli_fname);
  char *slash = strrchr(installation, '/');
  if (!slash)
    return;
  *slash = 0;
  if (snprintf(runtime, sizeof runtime, "%s/run", installation) >= (int)sizeof runtime)
    return;
  snprintf(exit_path, sizeof exit_path, "%s/exit", installation);
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  started = !pthread_create(&thread, NULL, run, NULL);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
}

__attribute__((destructor)) static void stop(void) {
  if (started && atomic_load(&finished))
    pthread_join(thread, NULL);
}
