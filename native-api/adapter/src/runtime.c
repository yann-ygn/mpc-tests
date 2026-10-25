#include "private.h"
#include "elf_id.h"
#include "mpc_adapter_profile.h"

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

static void expired(struct mpc_native_lifetime *lifetime) {
  __atomic_store_n(&lifetime->alive, false, __ATOMIC_RELEASE);
}

static void (*const lifetime_vtable[])(struct mpc_native_lifetime *) = {expired, expired, expired};

static enum mpc_native_error
watch(void *data, uint32_t subject, struct mpc_native_lifetime *lifetime) {
  if (!on_ui_thread(data))
    return MPC_NATIVE_WRONG_THREAD;
  uint32_t head, previous;
  if (
    !subject || subject % 4 || memory_read(data, subject, &head, 4) ||
    (head && (head % 4 || memory_read(data, head, &previous, 4) || previous != subject)))
    return MPC_NATIVE_READ_FAILED;
  /* Validated UIThreadOnly destruction-list ABI. This registers an observer;
   * it does not patch code, vtables, or model values. Subject never owns it. */
  struct mpc_native_lifetime_link **owner = (void *)(uintptr_t)subject;
  lifetime->vtable = lifetime_vtable;
  lifetime->link.previous = owner;
  lifetime->link.next = *owner;
  __atomic_store_n(&lifetime->alive, true, __ATOMIC_RELEASE);
  if (*owner)
    (*owner)->previous = &lifetime->link.next;
  *owner = &lifetime->link;
  return MPC_NATIVE_OK;
}

static void unwatch(void *data, struct mpc_native_lifetime *lifetime) {
  (void)data;
  /* Native destruction unlinks before expired(). Never dereference a destroyed
   * subject, including an address that has already been allocated again. */
  if (__atomic_load_n(&lifetime->alive, __ATOMIC_ACQUIRE)) {
    *lifetime->link.previous = lifetime->link.next;
    if (lifetime->link.next)
      lifetime->link.next->previous = lifetime->link.previous;
  }
  __atomic_store_n(&lifetime->alive, false, __ATOMIC_RELEASE);
}

static void sequence_noop(struct mpc_native_sequence_watch *watch, uint32_t sequence) {
  (void)watch;
  (void)sequence;
}

static void sequence_structure_changed(struct mpc_native_sequence_watch *watch, uint32_t sequence) {
  if (sequence == watch->sequence)
    __atomic_store_n(&watch->changed, true, __ATOMIC_RELEASE);
}

static void sequence_used_changed(struct mpc_native_sequence_watch *watch, uint32_t sequence) {
  if (sequence != watch->sequence)
    return;
  /* The native callback owns a live Sequence. Slot 5 also fires for rename, so
   * only an actual used/unused transition can retire the handle here. */
  unsigned char used = *(const unsigned char *)(uintptr_t)(sequence + NATIVE_SEQUENCE_USED);
  if (__atomic_exchange_n(&watch->used, used, __ATOMIC_ACQ_REL) != used)
    __atomic_store_n(&watch->changed, true, __ATOMIC_RELEASE);
}

/* Reset always emits slots 2, 3, 4, including slot zero which can remain unused
 * before and after Clear All. They also cover destructive content/length edits:
 * conservatively retire sequence refs for these events. Metadata alone does
 * not retire them; slot 5 is separately checked for an actual used transition. */
static void (*const sequence_vtable[])(struct mpc_native_sequence_watch *, uint32_t) = {
  sequence_noop,
  sequence_noop,
  sequence_structure_changed,
  sequence_structure_changed,
  sequence_structure_changed,
  sequence_used_changed,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop,
  sequence_noop};

static enum mpc_native_error
sequence_watch(void *data, uint32_t sequence, struct mpc_native_sequence_watch *watch) {
  struct runtime *runtime = data;
  if (!on_ui_thread(data))
    return MPC_NATIVE_WRONG_THREAD;
  if (memory_read(data, sequence + NATIVE_SEQUENCE_USED, &watch->used, 1) || watch->used > 1)
    return MPC_NATIVE_READ_FAILED;
  watch->vtable = sequence_vtable;
  watch->sequence = sequence;
  ((void (*)(void *, void *, bool))(runtime->image + NATIVE_SEQUENCE_LISTENER_ADD))(
    (void *)(uintptr_t)(sequence + NATIVE_SEQUENCE_NOTIFIER),
    watch,
    false);
  if (!watch->destruction_observers)
    return MPC_NATIVE_NOT_READY;
  watch->attached = true;
  return MPC_NATIVE_OK;
}

static enum mpc_native_error
sequence_unwatch(void *data, struct mpc_native_sequence_watch *watch, bool object_alive) {
  struct runtime *runtime = data;
  if (!on_ui_thread(data))
    return MPC_NATIVE_WRONG_THREAD;
  if (watch->attached && object_alive)
    ((void (*)(void *, void *))(runtime->image + NATIVE_SEQUENCE_LISTENER_REMOVE))(
      (void *)(uintptr_t)(watch->sequence + NATIVE_SEQUENCE_NOTIFIER),
      watch);
  if (watch->destruction_observers)
    return MPC_NATIVE_NOT_READY; /* Keep the observer/DSO alive; never free a live registration. */
  watch->attached = false;
  return MPC_NATIVE_OK;
}

static void project_noop(struct mpc_native_project_watch *watch, uint32_t project) {
  (void)watch;
  (void)project;
}

static void project_begin(struct mpc_native_project_watch *watch, uint32_t project) {
  (void)project; /* The listener is registered with exactly one native subject. */
  __atomic_store_n(&watch->loading, true, __ATOMIC_RELEASE);
  __atomic_store_n(&watch->changed, true, __ATOMIC_RELEASE);
}

static void project_end(struct mpc_native_project_watch *watch, uint32_t project) {
  (void)project; /* The installed-data notification has no extra argument. */
  __atomic_store_n(&watch->changed, true, __ATOMIC_RELEASE);
  __atomic_store_n(&watch->loading, false, __ATOMIC_RELEASE);
}

/* ProjectListener: reset begin/end (5/6), followed by data installation (7).
 * A project file load can retain the Project address throughout all of them. */
static void (*const project_vtable[])(struct mpc_native_project_watch *, uint32_t) = {
  project_noop,
  project_noop,
  project_noop,
  project_noop,
  project_noop,
  project_begin,
  project_end,
  project_end,
  project_noop,
  project_noop};

static enum mpc_native_error
project_watch(void *data, uint32_t project, struct mpc_native_project_watch *watch) {
  struct runtime *runtime = data;
  if (!on_ui_thread(data))
    return MPC_NATIVE_WRONG_THREAD;
  watch->vtable = project_vtable;
  watch->project = project;
  ((void (*)(void *, void *, bool))(runtime->image + NATIVE_PROJECT_LISTENER_ADD))(
    (void *)(uintptr_t)(project + NATIVE_PROJECT_NOTIFIER),
    watch,
    false);
  if (!watch->destruction_observers)
    return MPC_NATIVE_NOT_READY;
  watch->attached = true;
  return MPC_NATIVE_OK;
}

static enum mpc_native_error
project_unwatch(void *data, struct mpc_native_project_watch *watch, bool object_alive) {
  struct runtime *runtime = data;
  if (!on_ui_thread(data))
    return MPC_NATIVE_WRONG_THREAD;
  if (watch->attached && object_alive && watch->destruction_observers) {
    uint32_t depth;
    if (memory_read(data, watch->project + NATIVE_PROJECT_NOTIFIER + 8, &depth, 4) || depth)
      return MPC_NATIVE_NOT_READY;
    /* Destroy the listener base, not the registration node. The native weak
     * listener protocol marks its receiver dead and clears the listener's head.
     * The notifier then unlinks/frees its own node on its next dispatch. Freeing
     * the node directly would leave its notifier-chain link dangling. */
    ((void (*)(void *))(runtime->image + NATIVE_PROJECT_LISTENER_DETACH))(watch);
  }
  if (watch->destruction_observers)
    return MPC_NATIVE_NOT_READY;
  watch->attached = false;
  return MPC_NATIVE_OK;
}

static bool post(void *data, uintptr_t address, struct mpc_native_function *function) {
  (void)data;
#if defined(__arm__)
  _Static_assert(sizeof(struct mpc_native_function) == 16, "GNU ARM std::function ABI");
  _Static_assert(offsetof(struct mpc_native_function, manager) == 8, "manager offset");
  _Static_assert(offsetof(struct mpc_native_function, invoke) == 12, "invoker offset");
  _Static_assert(offsetof(struct mpc_native_lifetime, link) == 4, "destruction observer link");
  _Static_assert(sizeof(struct mpc_native_lifetime_link) == 8, "destruction observer ABI");
  _Static_assert(
    offsetof(struct mpc_native_sequence_watch, destruction_observers) == 4,
    "SequenceListener destruction subject");
  _Static_assert(
    offsetof(struct mpc_native_project_watch, destruction_observers) == 4,
    "ProjectListener destruction subject");
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
    .watch = watch,
    .unwatch = unwatch,
    .sequence_watch = sequence_watch,
    .sequence_unwatch = sequence_unwatch,
    .project_watch = project_watch,
    .project_unwatch = project_unwatch,
    .destroy = destroy};
  enum mpc_native_error error = mpc_native_open_backend(&io, hooks, output);
  if (error)
    destroy(runtime);
  return error;
}
