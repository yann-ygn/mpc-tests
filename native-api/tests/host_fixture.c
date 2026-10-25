#include "identity_fixture_backend.h"
#include "host.h"

#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

static struct identity_fixture fixture;
static volatile sig_atomic_t stopping;
static atomic_int finished;
static char gate[4096], pause_ui[4096];
static const char *installation, *runtime, *root;

static void stop_signal(int sig) {
  (void)sig;
  stopping = 1;
}

static int exiting(void *unused) {
  (void)unused;
  return stopping;
}

static enum mpc_native_error start(void *unused, struct mpc_executor **out) {
  (void)unused;
  if (!access(gate, F_OK))
    return MPC_NATIVE_NOT_READY;
  struct mpc_native_io backend = io(&fixture.native);
  backend.read = identity_read;
  backend.on_ui_thread = on_identity_ui;
  backend.post = enqueue;
  backend.watch = watch;
  backend.unwatch = unwatch;
  backend.sequence_watch = sequence_watch;
  backend.sequence_unwatch = sequence_unwatch;
  backend.project_watch = project_watch;
  backend.project_unwatch = project_unwatch;
  return mpc_executor_open_authoritative_backend(&backend, 8, out);
}

static void cancel(void *unused) {
  (void)unused;
}

static void *owner(void *unused) {
  (void)unused;
  struct mpc_host_ops ops = {NULL, root, start, cancel, exiting};
  int status = mpc_host_run(installation, runtime, &ops);
  atomic_store(&finished, status ? 2 : 1);
  return NULL;
}

int main(int argc, char **argv) {
  (void)validate_target;
  assert(argc == 5);
  installation = argv[1];
  runtime = argv[2];
  root = argv[3];
  snprintf(gate, sizeof gate, "%s/gate", installation);
  snprintf(pause_ui, sizeof pause_ui, "%s/pause-ui", installation);
  state_init(&fixture);
  if (!strcmp(argv[4], "build"))
    strcpy(fixture.native.build, "bad");
  else if (!strcmp(argv[4], "fingerprint"))
    fixture.native.corrupt = 0;
  struct sigaction action = {.sa_handler = stop_signal};
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  mode_t mask = umask(0022);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, owner, NULL));
  while (!atomic_load(&finished)) {
    pthread_mutex_lock(&fixture.mutex);
    bool queued = fixture.queued;
    pthread_mutex_unlock(&fixture.mutex);
    if (queued && (stopping || access(pause_ui, F_OK)))
      pump(&fixture);
    else {
      struct timespec delay = {0, 1000000};
      nanosleep(&delay, NULL);
    }
  }
  assert(!pthread_join(thread, NULL));
  assert(umask(mask) == 0022);
  sigaction(SIGTERM, NULL, &action);
  assert(action.sa_handler == stop_signal);
  assert(fixture.registered == fixture.removed + fixture.expired);
  assert(!fixture.native.setters);
  puts("callbacks_drained=true process_settings_preserved=true setters=0");
  return atomic_load(&finished) == 1 ? 0 : 1;
}
