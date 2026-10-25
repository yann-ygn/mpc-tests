#include "host.h"
#include "transport.h"
#include "private.h"
#include "mpc_adapter_profile.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static char installation[PATH_MAX];
static struct mpc_native_adapter *verifier;
static atomic_int exiting;

static bool read32(uintptr_t address, uint32_t *value) {
  return address && address % 4 == 0 && address <= UINT32_MAX - 0x10000u &&
    !verifier->io.read(verifier->io.userdata, address, value, 4);
}

static bool object(uint32_t address, uint32_t rva) {
  uint32_t value;
  return read32(address, &value) && value == verifier->io.image + rva;
}

static enum mpc_native_error start_executor(void *unused, struct mpc_executor **out) {
  (void)unused;
  *out = NULL;
  if (!verifier) {
    enum mpc_native_error error = mpc_native_open(NULL, &verifier);
    if (error)
      return error;
  }
  /* No native calls during ELF constructors or before the application creates
   * its UI. These are disposable read-only hints, never retained object refs.
   * The authoritative executor revalidates on the UI thread after dispatch. */
  uintptr_t image = verifier->io.image;
  uint32_t app, window, component;
  if (
    !read32(image + NATIVE_APPLICATION_SLOT, &app) || !object(app, NATIVE_APPLICATION_VTABLE) ||
    !read32(app + NATIVE_APPLICATION_WINDOW, &window) || !object(window, NATIVE_WINDOW_VTABLE) ||
    !read32(window + NATIVE_WINDOW_COMPONENT, &component) ||
    !object(component, NATIVE_COMPONENT_VTABLE) ||
    !object(component + NATIVE_COMPONENT_ENGINE, NATIVE_ENGINE_VTABLE))
    return MPC_NATIVE_NOT_READY;
  /* Observe the main/UI task in its event-loop poll, as in the validated lab
   * quiescence check. getpid() discovers this task; no recorded PID or worker
   * from another addin participates. Busy/loading UI simply postpones startup. */
  char path[96], buffer[256];
  snprintf(path, sizeof path, "/proc/self/task/%ld/syscall", (long)getpid());
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return MPC_NATIVE_NOT_READY;
  ssize_t n = read(fd, buffer, sizeof buffer - 1);
  close(fd);
  if (n <= 0)
    return MPC_NATIVE_NOT_READY;
  buffer[n] = 0;
  char *end;
  long call = strtol(buffer, &end, 10);
  if (end == buffer || (call != SYS_poll && call != SYS_ppoll))
    return MPC_NATIVE_NOT_READY;
  mpc_native_close(verifier);
  verifier = NULL;
  return mpc_executor_open_authoritative(16, out);
}

static void cancel_start(void *unused) {
  (void)unused;
  if (verifier)
    mpc_native_close(verifier);
  verifier = NULL;
}

static int process_exiting(void *unused) {
  (void)unused;
  return atomic_load(&exiting);
}

static void *worker(void *unused) {
  (void)unused;
  const struct mpc_host_ops ops = {NULL, "", start_executor, cancel_start, process_exiting};
  if (mpc_host_run(installation, MPC_HOST_DIRECTORY, &ops))
    fputs("mpc-native-api: host unavailable; MPC continues without native API\n", stderr);
  return NULL;
}

__attribute__((constructor)) static void start(void) {
  char executable[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
  if (n <= 0)
    return;
  executable[n] = 0;
  const char *base = strrchr(executable, '/');
  if (!base || strcmp(base + 1, "MPC"))
    return;
  Dl_info info;
  if (!dladdr((void *)start, &info) || !realpath(info.dli_fname, installation))
    return;
  char *slash = strrchr(installation, '/');
  if (!slash)
    return;
  *slash = 0;
  /* Only the new thread inherits blocked signals; MPC retains its dispositions
   * and mask. There are no hooks, child processes, ptrace or dlopen calls. */
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 512 * 1024);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  int rc = pthread_create(&thread, &attributes, worker, NULL);
  pthread_attr_destroy(&attributes);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (rc)
    fputs("mpc-native-api: could not start host worker\n", stderr);
}

__attribute__((destructor)) static void finish(void) {
  /* Never block MPC's exiting UI thread waiting for its own callbacks. The DSO
   * is linked NODELETE, so code/observers cannot be dlclosed while alive. Use
   * host-stop while the UI is running for a complete, observable drain. */
  atomic_store(&exiting, 1);
  api_sockets_cleanup();
}
