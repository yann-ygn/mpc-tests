#include "private.h"
#include "mpc_adapter_profile.h"

#include <stdlib.h>
#include <string.h>

struct entry {
  uint32_t address;
  uint64_t id, generation;
  unsigned index;
  struct mpc_native_lifetime lifetime;
  struct mpc_native_sequence_watch sequence;
};

struct mpc_identity_registry {
  struct mpc_native_adapter *adapter;
  enum mpc_native_error fault;
  uint64_t next, project, generation;
  uint32_t root;
  struct mpc_native_lifetime lifetime;
  struct entry tracks[MPC_IDENTITY_MAX_TRACKS];
  struct entry sequences[MPC_IDENTITY_MAX_SEQUENCES];
  struct mpc_native_project_watch project_watch;
};

static int read32(struct mpc_identity_registry *registry, uint32_t address, uint32_t *value) {
  struct mpc_native_io *io = &registry->adapter->io;
  if (!address || address % 4 || address > UINT32_MAX - 0x10000u)
    return -1;
  return io->read(io->userdata, address, value, sizeof *value);
}

static bool has_vtable(struct mpc_identity_registry *registry, uint32_t address, uint32_t rva) {
  uint32_t value;
  return !read32(registry, address, &value) && value == registry->adapter->io.image + rva;
}

/* One fixed application-owned path, never heap discovery. Check every owner
 * before traversing its child, and discard the path after this UI invocation. */
static enum mpc_native_error active_root(struct mpc_identity_registry *registry, uint32_t *root) {
  uint32_t app, window, component, engine;
  uintptr_t image = registry->adapter->io.image;
  if (
    image > UINT32_MAX - NATIVE_APPLICATION_SLOT ||
    read32(registry, (uint32_t)image + NATIVE_APPLICATION_SLOT, &app) ||
    !has_vtable(registry, app, NATIVE_APPLICATION_VTABLE) ||
    read32(registry, app + NATIVE_APPLICATION_WINDOW, &window) ||
    !has_vtable(registry, window, NATIVE_WINDOW_VTABLE) ||
    read32(registry, window + NATIVE_WINDOW_COMPONENT, &component) ||
    !has_vtable(registry, component, NATIVE_COMPONENT_VTABLE) ||
    component > UINT32_MAX - NATIVE_COMPONENT_ENGINE)
    return MPC_NATIVE_NOT_READY;
  engine = component + NATIVE_COMPONENT_ENGINE;
  if (
    !has_vtable(registry, engine, NATIVE_ENGINE_VTABLE) ||
    read32(registry, engine + NATIVE_ENGINE_PROJECT, root) ||
    !has_vtable(registry, *root, NATIVE_PROJECT_VTABLE))
    return MPC_NATIVE_NOT_READY;
  for (size_t i = 0; i < sizeof native_secondary / sizeof *native_secondary; i++)
    if (!has_vtable(registry, *root + native_secondary[i].offset, native_secondary[i].vtable))
      return MPC_NATIVE_READ_FAILED;
  return MPC_NATIVE_OK;
}

static void unwatch(struct mpc_identity_registry *registry, struct mpc_native_lifetime *lifetime) {
  if (!lifetime->vtable)
    return;
  struct mpc_native_io *io = &registry->adapter->io;
  io->unwatch(io->userdata, lifetime);
  memset(lifetime, 0, sizeof *lifetime);
}

static bool alive(const struct mpc_native_lifetime *lifetime) {
  return __atomic_load_n(&lifetime->alive, __ATOMIC_ACQUIRE);
}

static enum mpc_native_error
release_entry(struct mpc_identity_registry *registry, struct entry *entry) {
  struct mpc_native_io *io = &registry->adapter->io;
  if (entry->sequence.vtable) {
    enum mpc_native_error error =
      io->sequence_unwatch(io->userdata, &entry->sequence, alive(&entry->lifetime));
    if (error) {
      registry->fault = error;
      return error;
    }
  }
  unwatch(registry, &entry->lifetime);
  memset(entry, 0, sizeof *entry);
  return MPC_NATIVE_OK;
}

enum mpc_native_error mpc_identity_detach(struct mpc_identity_registry *registry) {
  if (!registry)
    return MPC_NATIVE_OK;
  for (unsigned i = 0; i < MPC_IDENTITY_MAX_TRACKS; i++) {
    enum mpc_native_error error = release_entry(registry, &registry->tracks[i]);
    if (error)
      return error;
  }
  for (unsigned i = 0; i < MPC_IDENTITY_MAX_SEQUENCES; i++) {
    enum mpc_native_error error = release_entry(registry, &registry->sequences[i]);
    if (error)
      return error;
  }
  if (registry->project_watch.vtable) {
    struct mpc_native_io *io = &registry->adapter->io;
    enum mpc_native_error error =
      io->project_unwatch(io->userdata, &registry->project_watch, alive(&registry->lifetime));
    if (error) {
      registry->fault = error;
      return error;
    }
    memset(&registry->project_watch, 0, sizeof registry->project_watch);
  }
  unwatch(registry, &registry->lifetime);
  registry->root = 0;
  registry->project = registry->generation = 0;
  return MPC_NATIVE_OK;
}

enum mpc_native_error
mpc_identity_create(struct mpc_native_adapter *adapter, struct mpc_identity_registry **output) {
  if (!adapter || !output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  *output = NULL;
  if (
    !adapter->io.watch || !adapter->io.unwatch || !adapter->io.sequence_watch ||
    !adapter->io.sequence_unwatch || !adapter->io.project_watch || !adapter->io.project_unwatch)
    return MPC_NATIVE_NOT_READY;
  struct mpc_identity_registry *registry = calloc(1, sizeof *registry);
  if (!registry)
    return MPC_NATIVE_NOT_READY;
  registry->adapter = adapter;
  registry->next = 1;
  *output = registry;
  return MPC_NATIVE_OK;
}

void mpc_identity_destroy(struct mpc_identity_registry *registry) {
  /* Only after UI detachment and executor capture/caller drain. */
  free(registry);
}

static enum mpc_native_error watch(
  struct mpc_identity_registry *registry, uint32_t address, struct mpc_native_lifetime *lifetime) {
  struct mpc_native_io *io = &registry->adapter->io;
  return io->watch(io->userdata, address + NATIVE_DESTRUCTION_OFFSET, lifetime);
}

static uint64_t next_id(struct mpc_identity_registry *registry) {
  /* Exhaustion permanently gates this authority instead of wrapping/reusing. */
  if (registry->next == UINT64_MAX)
    return 0;
  return registry->next++;
}

static enum mpc_native_error topology(
  struct mpc_identity_registry *registry,
  uint32_t tracks[MPC_IDENTITY_MAX_TRACKS],
  unsigned *track_count,
  uint32_t sequences[MPC_IDENTITY_MAX_SEQUENCES],
  unsigned *sequence_count) {
  uint32_t pool, begin, end, capacity, count;
  uint32_t root = registry->root;
  if (
    read32(registry, root + NATIVE_POOL_OFFSET, &pool) ||
    !has_vtable(registry, pool, NATIVE_POOL_VTABLE) ||
    read32(registry, pool + NATIVE_VECTOR_BEGIN, &begin) ||
    read32(registry, pool + NATIVE_VECTOR_END, &end) ||
    read32(registry, pool + NATIVE_VECTOR_CAPACITY, &capacity) || begin % 4 || end < begin ||
    capacity < end || (end - begin) % 4 || (capacity - begin) % 4 ||
    capacity - begin > 4 * MPC_IDENTITY_MAX_TRACKS || (!begin && (end || capacity)))
    return MPC_NATIVE_READ_FAILED;
  *track_count = (end - begin) / 4;
  struct mpc_native_io *io = &registry->adapter->io;
  if (*track_count && io->read(io->userdata, begin, tracks, *track_count * 4))
    return MPC_NATIVE_READ_FAILED;
  if (
    read32(registry, root + NATIVE_SEQUENCE_ARRAY, &begin) ||
    read32(registry, root + NATIVE_SEQUENCE_CAPACITY, &capacity) ||
    read32(registry, root + NATIVE_SEQUENCE_COUNT, &count) || begin % 4 || count > capacity ||
    count > MPC_IDENTITY_MAX_SEQUENCES || capacity > 2048 || (capacity && !begin) ||
    (count && io->read(io->userdata, begin, sequences, count * 4)))
    return MPC_NATIVE_READ_FAILED;
  *sequence_count = count;
  for (unsigned kind = 0; kind < 2; kind++) {
    uint32_t *addresses = kind ? sequences : tracks;
    unsigned length = kind ? *sequence_count : *track_count;
    for (unsigned i = 0; i < length; i++) {
      if (kind && !addresses[i])
        continue;
      if (!has_vtable(registry, addresses[i], kind ? NATIVE_SEQUENCE_VTABLE : NATIVE_TRACK_VTABLE))
        return MPC_NATIVE_READ_FAILED;
      for (unsigned j = 0; j < i; j++)
        if (addresses[j] == addresses[i])
          return MPC_NATIVE_READ_FAILED;
    }
  }
  return MPC_NATIVE_OK;
}

static enum mpc_native_error reconcile(
  struct mpc_identity_registry *registry,
  struct entry *entries,
  unsigned capacity,
  const uint32_t *addresses,
  unsigned count,
  bool sequences) {
  for (unsigned i = 0; i < capacity; i++) {
    struct entry *entry = &entries[i];
    if (!entry->address)
      continue;
    unsigned j;
    for (j = 0; j < count; j++)
      if (addresses[j] == entry->address)
        break;
    if (j == count || !alive(&entry->lifetime)) {
      enum mpc_native_error error = release_entry(registry, entry);
      if (error)
        return error;
    } else {
      entry->index = j;
      if (sequences && __atomic_exchange_n(&entry->sequence.changed, false, __ATOMIC_ACQ_REL)) {
        uint64_t id = next_id(registry);
        if (!id)
          return MPC_NATIVE_NOT_READY;
        entry->id = entry->generation = id;
      }
    }
  }
  for (unsigned i = 0; i < count; i++) {
    if (!addresses[i])
      continue;
    struct entry *empty = NULL;
    bool found = false;
    for (unsigned j = 0; j < capacity; j++) {
      if (entries[j].address == addresses[i])
        found = true;
      if (!entries[j].address && !empty)
        empty = &entries[j];
    }
    if (found)
      continue;
    if (!empty)
      return MPC_NATIVE_NOT_READY;
    uint64_t id = next_id(registry);
    if (!id)
      return MPC_NATIVE_NOT_READY;
    enum mpc_native_error error = watch(registry, addresses[i], &empty->lifetime);
    if (error)
      return error;
    empty->address = addresses[i];
    empty->id = empty->generation = id;
    empty->index = i;
    if (sequences) {
      struct mpc_native_io *io = &registry->adapter->io;
      error = io->sequence_watch(io->userdata, addresses[i], &empty->sequence);
      if (error)
        return error;
    }
  }
  return MPC_NATIVE_OK;
}

static enum mpc_native_error refresh(struct mpc_identity_registry *registry) {
  enum mpc_native_error error = mpc_native_verify_ui(registry->adapter);
  if (error)
    return error;
  if (registry->fault)
    return registry->fault;
  uint32_t root = 0;
  error = active_root(registry, &root);
  if (error) {
    enum mpc_native_error detached = mpc_identity_detach(registry);
    return detached ? detached : error;
  }
  if (
    root == registry->root && alive(&registry->lifetime) &&
    __atomic_load_n(&registry->project_watch.loading, __ATOMIC_ACQUIRE))
    return MPC_NATIVE_NOT_READY;
  if (
    root != registry->root || !alive(&registry->lifetime) ||
    __atomic_load_n(&registry->project_watch.changed, __ATOMIC_ACQUIRE)) {
    error = mpc_identity_detach(registry);
    if (error)
      return error;
    registry->project = registry->generation = next_id(registry);
    if (!registry->project)
      return MPC_NATIVE_NOT_READY;
    error = watch(registry, root, &registry->lifetime);
    if (error)
      goto invalid;
    registry->root = root;
    struct mpc_native_io *io = &registry->adapter->io;
    error = io->project_watch(io->userdata, root, &registry->project_watch);
    if (error)
      goto invalid;
  }
  uint32_t tracks[MPC_IDENTITY_MAX_TRACKS], sequences[MPC_IDENTITY_MAX_SEQUENCES];
  unsigned track_count, sequence_count;
  error = topology(registry, tracks, &track_count, sequences, &sequence_count);
  if (!error)
    error =
      reconcile(registry, registry->tracks, MPC_IDENTITY_MAX_TRACKS, tracks, track_count, false);
  if (!error)
    error = reconcile(
      registry,
      registry->sequences,
      MPC_IDENTITY_MAX_SEQUENCES,
      sequences,
      sequence_count,
      true);
  if (!error)
    return MPC_NATIVE_OK;
invalid: {
  enum mpc_native_error detached = mpc_identity_detach(registry);
  return detached ? detached : error;
}
}

static uint64_t object_id(struct entry *entries, unsigned count, uint32_t address) {
  for (unsigned i = 0; i < count; i++)
    if (entries[i].address && entries[i].address == address)
      return entries[i].id;
  return 0;
}

enum mpc_native_error
mpc_identity_capture(struct mpc_identity_registry *registry, struct mpc_identity_snapshot *output) {
  memset(output, 0, sizeof *output);
  enum mpc_native_error error = refresh(registry);
  if (error)
    return error;
  struct mpc_identity_snapshot snapshot = {0};
  uint32_t track, sequence;
  if (
    read32(registry, registry->root + NATIVE_SELECTED_TRACK, &track) ||
    read32(registry, registry->root + NATIVE_SELECTED_SEQUENCE, &sequence) ||
    read32(registry, registry->root + NATIVE_SEQUENCE_COUNT, &snapshot.sequence_slots))
    return MPC_NATIVE_READ_FAILED;
  snapshot.selected_track = object_id(registry->tracks, MPC_IDENTITY_MAX_TRACKS, track);
  snapshot.selected_sequence = object_id(registry->sequences, MPC_IDENTITY_MAX_SEQUENCES, sequence);
  if ((track && !snapshot.selected_track) || (sequence && !snapshot.selected_sequence))
    return MPC_NATIVE_READ_FAILED;
  strcpy(snapshot.instance_id, registry->adapter->instance_id);
  snapshot.project = registry->project;
  snapshot.project_generation = registry->generation;
  /* Emit current index order, independently of registry slot reuse. */
  for (unsigned index = 0; index < MPC_IDENTITY_MAX_SEQUENCES; index++) {
    for (unsigned i = 0; i < MPC_IDENTITY_MAX_TRACKS; i++) {
      const struct entry *entry = &registry->tracks[i];
      if (entry->address && entry->index == index)
        snapshot.tracks[snapshot.track_count++] =
          (struct mpc_identity_object){entry->id, entry->generation, entry->index};
    }
    for (unsigned i = 0; i < MPC_IDENTITY_MAX_SEQUENCES; i++) {
      const struct entry *entry = &registry->sequences[i];
      if (entry->address && entry->index == index)
        snapshot.sequences[snapshot.sequence_count++] =
          (struct mpc_identity_object){entry->id, entry->generation, entry->index};
    }
  }
  *output = snapshot;
  return MPC_NATIVE_OK;
}

static enum mpc_native_error check_project(
  struct mpc_identity_registry *registry, const struct mpc_executor_identity *identity) {
  /* Check the bounded session field before touching native state. */
  if (!identity || memcmp(identity->instance_id, registry->adapter->instance_id, 37))
    return MPC_NATIVE_STALE_TARGET;
  enum mpc_native_error error = refresh(registry);
  if (error)
    return error;
  return identity->project == registry->project &&
      identity->project_generation == registry->generation
    ? MPC_NATIVE_OK
    : MPC_NATIVE_STALE_TARGET;
}

static struct entry *find(struct entry *entries, unsigned count, uint64_t id, uint64_t generation) {
  for (unsigned i = 0; i < count; i++)
    if (
      entries[i].address && entries[i].id == id && entries[i].generation == generation &&
      alive(&entries[i].lifetime))
      return &entries[i];
  return NULL;
}

enum mpc_native_error mpc_identity_sequence(
  struct mpc_identity_registry *registry, const struct mpc_executor_identity *identity) {
  enum mpc_native_error error = check_project(registry, identity);
  if (error)
    return error;
  return find(
           registry->sequences,
           MPC_IDENTITY_MAX_SEQUENCES,
           identity->sequence,
           identity->sequence_generation)
    ? MPC_NATIVE_OK
    : MPC_NATIVE_STALE_TARGET;
}

enum mpc_native_error mpc_identity_resolve(
  struct mpc_identity_registry *registry,
  const struct mpc_executor_identity *identity,
  struct mpc_native_target *output) {
  memset(output, 0, sizeof *output);
  enum mpc_native_error error = check_project(registry, identity);
  if (error)
    return error;
  if (identity->sequence || identity->sequence_generation) {
    error = mpc_identity_sequence(registry, identity);
    if (error)
      return error;
  }
  struct entry *track =
    find(registry->tracks, MPC_IDENTITY_MAX_TRACKS, identity->track, identity->track_generation);
  uint32_t program;
  if (!track)
    return MPC_NATIVE_STALE_TARGET;
  if (read32(registry, track->address + NATIVE_PROGRAM_OFFSET, &program) || !program)
    return MPC_NATIVE_READ_FAILED;
  *output = (struct mpc_native_target){registry->root, track->address, program};
  return MPC_NATIVE_OK;
}
