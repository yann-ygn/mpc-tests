#include "private.h"
#include "elf_id.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct runtime {
  int memory;
  uintptr_t image;
  bool found;
};

static int main_image(struct dl_phdr_info *entry, size_t size, void *data) {
  (void)size;
  struct runtime *runtime = data;
  if (entry->dlpi_name && *entry->dlpi_name)
    return 0;
  runtime->image = entry->dlpi_addr;
  runtime->found = true;
  return 1;
}

static enum mpc_native_error identity(void *data, char build[129], char model[256]) {
  (void)data;
  int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return MPC_NATIVE_READ_FAILED;
  int rc = mpc_elf_build_id(fd, build);
  close(fd);
  if (rc)
    return MPC_NATIVE_READ_FAILED;
  fd = open("/sys/firmware/devicetree/base/model", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return MPC_NATIVE_READ_FAILED;
  ssize_t n;
  do {
    n = read(fd, model, 255);
  } while (n < 0 && errno == EINTR);
  close(fd);
  if (n <= 0 || n == 255)
    return MPC_NATIVE_READ_FAILED;
  model[n] = 0;
  size_t length = strlen(model);
  while (length && (model[length - 1] == '\n' || model[length - 1] == '\r'))
    model[--length] = 0;
  return MPC_NATIVE_OK;
}

static int memory_read(void *data, uintptr_t address, void *output, size_t length) {
  struct runtime *runtime = data;
  size_t used = 0;
  if (address > UINT32_MAX || length > UINT32_MAX - address)
    return -1;
  while (used < length) {
    ssize_t n =
      pread(runtime->memory, (char *)output + used, length - used, (off_t)(address + used));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return -1;
    used += (size_t)n;
  }
  return 0;
}

static bool on_ui_thread(void *data) {
  (void)data;
  return syscall(SYS_gettid) == getpid();
}

static bool post(void *data, uintptr_t address, struct mpc_native_function *function) {
  (void)data;
#if defined(__arm__)
  _Static_assert(sizeof(struct mpc_native_function) == 16, "GNU ARM std::function ABI");
  _Static_assert(offsetof(struct mpc_native_function, manager) == 8, "manager offset");
  _Static_assert(offsetof(struct mpc_native_function, invoke) == 12, "invoker offset");
#endif
  return ((bool (*)(struct mpc_native_function *))address)(function);
}

static enum mpc_native_error
setter(void *data, uintptr_t function, uintptr_t property, bool value) {
  (void)data;
  ((void (*)(void *, bool))function)((void *)property, value);
  return MPC_NATIVE_OK;
}

static enum mpc_native_error
filter(void *data, uintptr_t storage, uintptr_t function, bool *value) {
  (void)data;
  *value = ((bool (*)(void *, const bool *))function)((void *)storage, value);
  return MPC_NATIVE_OK;
}

static void destroy(void *data) {
  struct runtime *runtime = data;
  close(runtime->memory);
  free(runtime);
}

enum mpc_native_error
mpc_native_open(const struct mpc_native_hooks *hooks, struct mpc_native_adapter **output) {
  if (!output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  *output = NULL;
  /* The injected fixture backend lives outside this exported entry point. */
#if defined(__arm__) && defined(__ARM_PCS_VFP) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  const bool supported_abi = sizeof(void *) == 4;
#else
  const bool supported_abi = false;
#endif
  if (!supported_abi)
    return MPC_NATIVE_ABI_MISMATCH;
  struct runtime *runtime = calloc(1, sizeof *runtime);
  if (!runtime)
    return MPC_NATIVE_NOT_READY;
  dl_iterate_phdr(main_image, runtime);
  if (!runtime->found) {
    free(runtime);
    return MPC_NATIVE_NOT_READY;
  }
  runtime->memory = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
  if (runtime->memory < 0) {
    free(runtime);
    return MPC_NATIVE_READ_FAILED;
  }
  struct mpc_native_io io = {
    .userdata = runtime,
    .image = runtime->image,
    .identity = identity,
    .read = memory_read,
    .on_ui_thread = on_ui_thread,
    .setter = setter,
    .filter = filter,
    .post = post,
    .destroy = destroy};
  enum mpc_native_error error = mpc_native_open_backend(&io, hooks, output);
  if (error)
    destroy(runtime);
  return error;
}
