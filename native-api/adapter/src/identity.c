#include "private.h"
#include "mpc_adapter_profile.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

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
    read32(registry, engine + NATIVE_ENGINE_PROJECT, root))
    return MPC_NATIVE_NOT_READY;
  if (!*root)
    return MPC_NATIVE_OK;
  if (!has_vtable(registry, *root, NATIVE_PROJECT_VTABLE))
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
    read32(registry, pool + NATIVE_USER_BEGIN, &begin) ||
    read32(registry, pool + NATIVE_USER_END, &end) ||
    read32(registry, pool + NATIVE_USER_CAPACITY, &capacity) || begin % 4 || end < begin ||
    capacity < end || (end - begin) % 4 || (capacity - begin) % 4 ||
    (end - begin) / 4 > MPC_IDENTITY_MAX_TRACKS || capacity - begin > 4 * 256 ||
    (!begin && (end || capacity)))
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
  if (!root)
    return mpc_identity_detach(registry);
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
  strcpy(snapshot.instance_id, registry->adapter->instance_id);
  if (!registry->root) {
    *output = snapshot;
    return MPC_NATIVE_OK;
  }
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
  for (unsigned index = 0; index < MPC_IDENTITY_MAX_TRACKS; index++) {
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
  struct entry *entry = find(
    registry->sequences,
    MPC_IDENTITY_MAX_SEQUENCES,
    identity->sequence,
    identity->sequence_generation);
  if (!entry)
    return MPC_NATIVE_STALE_SEQUENCE;
  uint32_t selected, mirror;
  if (
    read32(registry, registry->root + NATIVE_SELECTED_SEQUENCE, &selected) ||
    read32(registry, registry->root + NATIVE_SELECTED_SEQUENCE_MIRROR, &mirror))
    return MPC_NATIVE_READ_FAILED;
  if (selected != mirror)
    return MPC_NATIVE_STATE_UNSTABLE;
  return selected == entry->address ? MPC_NATIVE_OK : MPC_NATIVE_SEQUENCE_NOT_ACTIVE;
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

static uint64_t nanoseconds(clockid_t clock) {
  struct timespec time;
  if (clock_gettime(clock, &time))
    return 0;
  return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static int state_bool(struct mpc_identity_registry *registry, uint32_t address, uint32_t *value) {
  unsigned char byte;
  struct mpc_native_io *io = &registry->adapter->io;
  if (io->read(io->userdata, address, &byte, 1) || byte > 1)
    return -1;
  *value = byte;
  return 0;
}

static int state_name(struct mpc_identity_registry *registry, uint32_t field, char *name) {
  uint32_t address;
  if (read32(registry, field, &address) || !address || address > UINT32_MAX - MPC_STATE_NAME_BYTES)
    return -1;
  struct mpc_native_io *io = &registry->adapter->io;
  for (unsigned i = 0; i < MPC_STATE_NAME_BYTES; i++) {
    if (io->read(io->userdata, address + i, name + i, 1))
      return -1;
    if (!name[i])
      return 0;
  }
  return -1; /* Never silently truncate a required name. */
}

#define ALL_TRACKS (MPC_IDENTITY_MAX_TRACKS + MPC_STATE_MAX_BUSES)
struct inventory {
  uint32_t all[ALL_TRACKS], users[MPC_IDENTITY_MAX_TRACKS];
  uint32_t midi[MPC_IDENTITY_MAX_MIDI_TRACKS], audio[MPC_IDENTITY_MAX_AUDIO_TRACKS];
  uint32_t masters[MPC_STATE_MAX_BUSES], programs[ALL_TRACKS], vtables[ALL_TRACKS];
  unsigned all_count, user_count, midi_count, audio_count, master_count;
};

static int position(const uint32_t *addresses, unsigned count, uint32_t address) {
  for (unsigned i = 0; i < count; i++)
    if (addresses[i] == address)
      return (int)i;
  return -1;
}

static enum mpc_native_error inventory_vector(
  struct mpc_identity_registry *registry,
  uint32_t pool,
  unsigned offset,
  uint32_t *addresses,
  unsigned maximum,
  unsigned *count) {
  uint32_t begin, end, capacity;
  if (
    read32(registry, pool + offset, &begin) || read32(registry, pool + offset + 4, &end) ||
    read32(registry, pool + offset + 8, &capacity) || begin % 4 || end < begin || capacity < end ||
    (end - begin) % 4 || (capacity - begin) % 4 || (end - begin) / 4 > maximum ||
    capacity - begin > 512 * 4 || (!begin && (end || capacity)))
    return MPC_NATIVE_READ_FAILED;
  *count = (end - begin) / 4;
  struct mpc_native_io *io = &registry->adapter->io;
  if (*count && io->read(io->userdata, begin, addresses, *count * 4))
    return MPC_NATIVE_READ_FAILED;
  for (unsigned i = 0; i < *count; i++) {
    if (!has_vtable(registry, addresses[i], NATIVE_TRACK_VTABLE))
      return MPC_NATIVE_READ_FAILED;
    if (position(addresses, i, addresses[i]) >= 0)
      return MPC_NATIVE_READ_FAILED;
  }
  return MPC_NATIVE_OK;
}

static const char *program_type(struct mpc_identity_registry *registry, uint32_t vtable) {
  for (unsigned i = 0; i < sizeof native_program_types / sizeof *native_program_types; i++)
    if (vtable == registry->adapter->io.image + native_program_types[i].vtable)
      return native_program_types[i].name;
  return "unknown";
}

static bool master_type(const char *type) {
  return !strcmp(type, "input") || !strcmp(type, "output") || !strcmp(type, "master-output") ||
    !strcmp(type, "return") || !strcmp(type, "submix");
}

/* The explicit normal/master collections partition all tracks. MIDI-triggered
 * and audio partition normal. Never infer a bus from absence in MIDI. */
static enum mpc_native_error
inventory_read(struct mpc_identity_registry *registry, struct inventory *inventory) {
  memset(inventory, 0, sizeof *inventory);
  uint32_t pool;
  if (
    read32(registry, registry->root + NATIVE_POOL_OFFSET, &pool) ||
    !has_vtable(registry, pool, NATIVE_POOL_VTABLE))
    return MPC_NATIVE_READ_FAILED;
  struct {
    unsigned offset, maximum;
    uint32_t *addresses;
    unsigned *count;
  } vectors[] = {
    {NATIVE_ALL_BEGIN, ALL_TRACKS, inventory->all, &inventory->all_count},
    {NATIVE_USER_BEGIN, MPC_IDENTITY_MAX_TRACKS, inventory->users, &inventory->user_count},
    {NATIVE_MIDI_BEGIN, MPC_IDENTITY_MAX_MIDI_TRACKS, inventory->midi, &inventory->midi_count},
    {NATIVE_AUDIO_BEGIN, MPC_IDENTITY_MAX_AUDIO_TRACKS, inventory->audio, &inventory->audio_count},
    {NATIVE_MASTER_BEGIN, MPC_STATE_MAX_BUSES, inventory->masters, &inventory->master_count}};
  for (unsigned i = 0; i < sizeof vectors / sizeof *vectors; i++) {
    enum mpc_native_error error = inventory_vector(
      registry,
      pool,
      vectors[i].offset,
      vectors[i].addresses,
      vectors[i].maximum,
      vectors[i].count);
    if (error)
      return error;
  }
  if (
    inventory->all_count != inventory->user_count + inventory->master_count ||
    inventory->user_count != inventory->midi_count + inventory->audio_count)
    return MPC_NATIVE_READ_FAILED;
  for (unsigned i = 0; i < inventory->all_count; i++) {
    uint32_t address = inventory->all[i];
    bool user = position(inventory->users, inventory->user_count, address) >= 0;
    bool master = position(inventory->masters, inventory->master_count, address) >= 0;
    bool midi = position(inventory->midi, inventory->midi_count, address) >= 0;
    bool audio = position(inventory->audio, inventory->audio_count, address) >= 0;
    if (
      user == master || (user && midi == audio) || (master && (midi || audio)) ||
      read32(registry, address + NATIVE_PROGRAM_OFFSET, &inventory->programs[i]) ||
      read32(registry, inventory->programs[i], &inventory->vtables[i]))
      return MPC_NATIVE_READ_FAILED;
    const char *type = program_type(registry, inventory->vtables[i]);
    if (strcmp(type, "unknown") && (master != master_type(type) || audio != !strcmp(type, "audio")))
      return MPC_NATIVE_READ_FAILED;
  }
  return MPC_NATIVE_OK;
}

static bool same_inventory(const struct inventory *a, const struct inventory *b) {
  return a->all_count == b->all_count && a->user_count == b->user_count &&
    a->midi_count == b->midi_count && a->audio_count == b->audio_count &&
    a->master_count == b->master_count && !memcmp(a->all, b->all, a->all_count * 4) &&
    !memcmp(a->users, b->users, a->user_count * 4) &&
    !memcmp(a->midi, b->midi, a->midi_count * 4) &&
    !memcmp(a->audio, b->audio, a->audio_count * 4) &&
    !memcmp(a->masters, b->masters, a->master_count * 4) &&
    !memcmp(a->programs, b->programs, a->all_count * 4) &&
    !memcmp(a->vtables, b->vtables, a->all_count * 4);
}

static enum mpc_native_error selection_check(
  struct mpc_identity_registry *registry, const struct mpc_identity_snapshot *identity) {
  uint32_t track, mirror, sequence, sequence_mirror, index;
  if (
    read32(registry, registry->root + NATIVE_SELECTED_TRACK, &track) ||
    read32(registry, registry->root + NATIVE_SELECTED_TRACK_MIRROR, &mirror) ||
    read32(registry, registry->root + NATIVE_SELECTED_SEQUENCE, &sequence) ||
    read32(registry, registry->root + NATIVE_SELECTED_SEQUENCE_MIRROR, &sequence_mirror) ||
    read32(registry, registry->root + NATIVE_SELECTED_TRACK_INDEX, &index))
    return MPC_NATIVE_READ_FAILED;
  if (track != mirror || sequence != sequence_mirror)
    return MPC_NATIVE_STATE_UNSTABLE;
  for (unsigned i = 0; i < identity->track_count; i++)
    if (identity->tracks[i].id == identity->selected_track && identity->tracks[i].index != index)
      return MPC_NATIVE_STATE_UNSTABLE;
  return MPC_NATIVE_OK;
}

static enum mpc_native_error state_track(
  struct mpc_identity_registry *registry,
  uint32_t address,
  uint32_t *program,
  bool normal,
  struct mpc_state_track *output) {
  uint32_t vtable;
  if (
    state_name(registry, address + NATIVE_TRACK_NAME, output->name) ||
    read32(registry, address + NATIVE_PROGRAM_OFFSET, program) ||
    read32(registry, *program, &vtable))
    return MPC_NATIVE_READ_FAILED;
  strcpy(output->type, "unknown");
  for (unsigned i = 0; i < sizeof native_program_types / sizeof *native_program_types; i++)
    if (vtable == registry->adapter->io.image + native_program_types[i].vtable)
      strcpy(output->type, native_program_types[i].name);
  if (!normal || (strcmp(output->type, "plugin") && strcmp(output->type, "drum")))
    return MPC_NATIVE_OK;
  uint32_t *requested[] = {&output->muted, &output->soloed, &output->record_armed};
  uint32_t *audio[] = {NULL, &output->audio_soloed, &output->audio_record_armed};
  for (unsigned i = 0; i < 3; i++) {
    const struct native_binding *binding = &native_bindings[i];
    uint32_t owner = binding->owner_is_track ? address : *program;
    if (
      state_bool(registry, owner + binding->requested, requested[i]) ||
      (audio[i] && state_bool(registry, owner + binding->observed, audio[i])))
      return MPC_NATIVE_READ_FAILED;
  }
  if (state_bool(registry, *program + NATIVE_SILENCED_OFFSET, &output->silenced))
    return MPC_NATIVE_READ_FAILED;
  output->known =
    MPC_STATE_REQUESTED | MPC_STATE_AUDIO_SOLO | MPC_STATE_AUDIO_ARM | MPC_STATE_SILENCED;
  return MPC_NATIVE_OK;
}

static bool
same_identities(const struct mpc_identity_snapshot *a, const struct mpc_identity_snapshot *b) {
  if (
    memcmp(a->instance_id, b->instance_id, sizeof a->instance_id) || a->project != b->project ||
    a->project_generation != b->project_generation || a->selected_track != b->selected_track ||
    a->selected_sequence != b->selected_sequence || a->track_count != b->track_count ||
    a->sequence_count != b->sequence_count || a->sequence_slots != b->sequence_slots)
    return false;
  /* Compare fields, never C structure padding. */
  for (unsigned kind = 0; kind < 2; kind++) {
    const struct mpc_identity_object *left = kind ? a->sequences : a->tracks;
    const struct mpc_identity_object *right = kind ? b->sequences : b->tracks;
    unsigned count = kind ? a->sequence_count : a->track_count;
    for (unsigned i = 0; i < count; i++)
      if (
        left[i].id != right[i].id || left[i].generation != right[i].generation ||
        left[i].index != right[i].index)
        return false;
  }
  return true;
}

static enum mpc_native_error
capture_state(struct mpc_identity_registry *registry, struct mpc_state_snapshot *output) {
  struct mpc_identity_snapshot before, after;
  enum mpc_native_error error = mpc_identity_capture(registry, &before);
  if (error)
    return error;
  strcpy(output->instance_id, before.instance_id);
  output->project = before.project;
  output->project_generation = before.project_generation;
  output->selected_track = before.selected_track;
  output->selected_sequence = before.selected_sequence;
  output->sequence_slots = before.sequence_slots;
  output->mute_scope = MPC_MUTE_UNKNOWN;
  struct inventory inventory, verify;
  if (before.project) {
    error = mpc_native_mute_scope(registry->adapter, registry->root, &output->mute_scope);
    if (error)
      return error;
    error = selection_check(registry, &before);
    if (error)
      return error;
    error = inventory_read(registry, &inventory);
    if (error)
      return error;
    if (inventory.user_count != before.track_count)
      return MPC_NATIVE_STATE_UNSTABLE;
    for (unsigned i = 0; i < inventory.all_count; i++) {
      uint32_t address = inventory.all[i];
      int ui_index = position(inventory.users, inventory.user_count, address);
      struct mpc_state_track *track;
      if (ui_index >= 0) {
        struct entry *entry = NULL;
        for (unsigned j = 0; j < MPC_IDENTITY_MAX_TRACKS; j++)
          if (registry->tracks[j].address == address)
            entry = &registry->tracks[j];
        if (!entry || entry->index != (unsigned)ui_index)
          return MPC_NATIVE_STATE_UNSTABLE;
        track = &output->tracks[ui_index];
        track->identity = (struct mpc_identity_object){entry->id, entry->generation, entry->index};
        int midi_index = position(inventory.midi, inventory.midi_count, address);
        track->collection = midi_index >= 0 ? MPC_STATE_MIDI_TRIGGERED : MPC_STATE_AUDIO;
        track->collection_index =
          midi_index >= 0 ? midi_index : position(inventory.audio, inventory.audio_count, address);
        output->track_count++;
      } else {
        unsigned index = position(inventory.masters, inventory.master_count, address);
        track = &output->buses[index];
        track->identity.index = track->collection_index = index;
        track->collection = MPC_STATE_MASTER;
        output->bus_count++;
      }
      uint32_t program;
      error = state_track(registry, address, &program, ui_index >= 0, track);
      if (error)
        return error;
      if (
        program != inventory.programs[i] ||
        strcmp(track->type, program_type(registry, inventory.vtables[i])))
        return MPC_NATIVE_STATE_UNSTABLE;
    }
    for (unsigned i = 0; i < before.sequence_count; i++) {
      const struct mpc_identity_object *id = &before.sequences[i];
      struct entry *entry =
        find(registry->sequences, MPC_IDENTITY_MAX_SEQUENCES, id->id, id->generation);
      struct mpc_state_sequence *sequence = &output->sequences[output->sequence_count++];
      sequence->identity.id = id->id;
      sequence->identity.generation = id->generation;
      sequence->identity.index = id->index;
      if (id->id == before.selected_sequence)
        output->selected_sequence_generation = id->generation;
      struct mpc_native_io *io = &registry->adapter->io;
      if (
        !entry || state_name(registry, entry->address + NATIVE_SEQUENCE_NAME, sequence->name) ||
        state_bool(registry, entry->address + NATIVE_SEQUENCE_USED, &sequence->used) ||
        io->read(
          io->userdata,
          entry->address + NATIVE_SEQUENCE_BPM,
          &sequence->bpm,
          sizeof sequence->bpm) ||
        !isfinite(sequence->bpm) || sequence->bpm <= 0 || sequence->bpm > 1000)
        return MPC_NATIVE_READ_FAILED;
      output->used_sequences += sequence->used;
    }
  }
  error = mpc_identity_capture(registry, &after);
  if (error)
    return error;
  if (!same_identities(&before, &after))
    return MPC_NATIVE_STATE_UNSTABLE;
  if (!before.project)
    return MPC_NATIVE_OK;
  uint32_t scope;
  error = mpc_native_mute_scope(registry->adapter, registry->root, &scope);
  if (error)
    return error;
  if (scope != output->mute_scope)
    return MPC_NATIVE_STATE_UNSTABLE;
  error = selection_check(registry, &after);
  if (error)
    return error;
  error = inventory_read(registry, &verify);
  if (error)
    return error;
  if (!same_inventory(&inventory, &verify))
    return MPC_NATIVE_STATE_UNSTABLE;
  return MPC_NATIVE_OK;
}

enum mpc_native_error
mpc_identity_state(struct mpc_identity_registry *registry, struct mpc_state_snapshot *output) {
  if (!registry || !output)
    return MPC_NATIVE_INVALID_ARGUMENT;
  uint64_t started = nanoseconds(CLOCK_REALTIME) / 1000000;
  uint64_t monotonic = nanoseconds(CLOCK_MONOTONIC);
  enum mpc_native_error error = MPC_NATIVE_STATE_UNSTABLE;
  for (unsigned attempt = 1; attempt <= MPC_STATE_ATTEMPTS; attempt++) {
    memset(output, 0, sizeof *output);
    error = capture_state(registry, output);
    if (error == MPC_NATIVE_STATE_UNSTABLE)
      continue;
    if (error)
      break;
    output->abi_version = MPC_STATE_ABI;
    output->struct_bytes = sizeof *output;
    output->started_ms = started;
    output->finished_ms = nanoseconds(CLOCK_REALTIME) / 1000000;
    output->duration_ns = nanoseconds(CLOCK_MONOTONIC) - monotonic;
    output->attempts = attempt;
    return MPC_NATIVE_OK;
  }
  memset(output, 0, sizeof *output);
  return error;
}

/* Bind public incarnation IDs to their current generations on the UI thread.
 * Never resolve names/indices or reuse a generation from a service cache. */
enum mpc_native_error
mpc_identity_bind(struct mpc_identity_registry *registry, struct mpc_executor_identity *identity) {
  if (!registry || memcmp(identity->instance_id, registry->adapter->instance_id, 37))
    return MPC_NATIVE_STALE_INSTANCE;
  enum mpc_native_error error = refresh(registry);
  if (error)
    return error;
  if (!registry->project)
    return MPC_NATIVE_NO_ACTIVE_PROJECT;
  if (identity->project != registry->project)
    return MPC_NATIVE_STALE_PROJECT;
  if (identity->sequence || identity->sequence_generation) {
    if (!find(
          registry->sequences,
          MPC_IDENTITY_MAX_SEQUENCES,
          identity->sequence,
          identity->sequence_generation))
      return MPC_NATIVE_STALE_SEQUENCE;
  }
  for (unsigned i = 0; i < MPC_IDENTITY_MAX_TRACKS; i++) {
    const struct entry *entry = &registry->tracks[i];
    if (entry->address && entry->id == identity->track && alive(&entry->lifetime)) {
      identity->project_generation = registry->generation;
      identity->track_generation = entry->generation;
      return MPC_NATIVE_OK;
    }
  }
  return MPC_NATIVE_TARGET_NOT_FOUND;
}
