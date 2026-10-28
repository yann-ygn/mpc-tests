/* Exercise replay boundaries and producer ownership identically on host/ARM.
 * Including the private publisher permits deterministic expiry/pressure tests
 * without adding a clock override or arbitrary native subscriptions to the API. */
#include "../src/watch.c"

#undef NDEBUG
#include <assert.h>
#include <stdatomic.h>

static const char instance_id[] = "run_0123456789abcdef0123456789abcdef";

static void baseline(struct mpc_state_snapshot *s) {
  memset(s, 0, sizeof *s);
  s->abi_version = MPC_STATE_ABI;
  s->struct_bytes = sizeof *s;
  strcpy(s->instance_id, instance_id);
  s->project = s->project_generation = 1;
  s->started_ms = s->finished_ms = 1;
  s->attempts = 1;
  s->track_count = 1;
  s->tracks[0].identity.id = s->tracks[0].identity.generation = 1;
  s->tracks[0].collection = MPC_STATE_MIDI_TRIGGERED;
  strcpy(s->tracks[0].type, "plugin");
  s->tracks[0].known = MPC_STATE_REQUESTED;
}

static void initialize(struct api_watch *p) {
  memset(p, 0, sizeof *p);
  pthread_mutex_init(&p->mutex, NULL);
  p->context.device_id = "fixture";
  strcpy(p->context.instance_id, instance_id);
  strcpy(p->adapter, instance_id);
}

static void dispose(struct api_watch *p) {
  while (p->count)
    evict(p);
  pthread_mutex_destroy(&p->mutex);
}

static void replay_tests(void) {
  struct api_watch *p = calloc(1, sizeof *p);
  struct mpc_state_snapshot *s = malloc(sizeof *s);
  assert(p && s);
  initialize(p);
  baseline(s);
  assert(api_valid_snapshot(s, instance_id));
  assert(!snapshot(p, s));
  assert(p->revision == 1 && p->count == 1 && p->bytes > 0);
  assert(strchr(p->replay[0].frame, '\n') == p->replay[0].frame + strlen(p->replay[0].frame) - 1);
  struct api_subscription subscriber =
    {.publisher = p, .revision = 1, .epoch = p->epoch, .stage = 1};
  struct mpc_command_observation o = {
    .project = 1,
    .generation = 1,
    .topology_epoch = 1,
    .captured_ms = 2,
    .total_tracks = 1,
    .track_count = 1};
  o.tracks[0] = (struct mpc_command_track){.id = 1,
                                           .generation = 1,
                                           .known = MPC_STATE_REQUESTED,
                                           .family = MPC_NATIVE_PLUGIN,
                                           .muted = 1};
  assert(!controls(p, &o));
  char *next = api_watch_next(&subscriber);
  assert(next && strstr(next, "\"type\":\"change\"") && strstr(next, "\"previousRevision\":\"1\""));
  free(next);
  assert(subscriber.revision == 2);
  assert(!controls(p, &o) && p->revision == 2); /* No revision for unchanged sampling. */
  /* A topology discontinuity cannot be stitched into the metadata baseline. */
  o.topology_epoch = 2;
  assert(controls(p, &o) == -1 && p->revision == 2);
  /* Snapshot/read publication and controls use the same ordered replay ring. */
  for (unsigned i = 0; i < API_WATCH_REPLAY + 1; i++)
    assert(!snapshot(p, s));
  assert(p->count == API_WATCH_REPLAY && p->bytes <= API_WATCH_REPLAY_BYTES);
  next = api_watch_next(&subscriber);
  assert(subscriber.terminal && next && strstr(next, "REPLAY_GAP"));
  free(next);
  subscriber = (struct api_subscription){.publisher = p,
                                         .revision = p->revision - 1,
                                         .epoch = p->epoch,
                                         .stage = 1};
  find(p, p->revision)->time = monotonic_ms() - API_WATCH_RETENTION_MS;
  next = api_watch_next(&subscriber);
  assert(subscriber.terminal && next && strstr(next, "REPLAY_GAP"));
  free(next);
  uint64_t old_epoch = p->epoch;
  s->project_generation++;
  assert(!snapshot(p, s) && p->epoch != old_epoch && p->count == 1);
  subscriber = (struct api_subscription){.publisher = p, .epoch = old_epoch, .stage = 1};
  next = api_watch_next(&subscriber);
  assert(subscriber.terminal && next && strstr(next, "CONTINUITY_LOST"));
  free(next);
  /* Byte pressure evicts history even before reaching the frame-count limit. */
  for (unsigned i = 0; i < 6; i++) {
    char *large = malloc(MAX_RESPONSE_BYTES + 1);
    assert(large);
    memset(large, ' ', MAX_RESPONSE_BYTES);
    memcpy(large, "{}", 2);
    large[MAX_RESPONSE_BYTES - 1] = '\n';
    large[MAX_RESPONSE_BYTES] = 0;
    p->revision++;
    assert(!retain(p, large, MAX_RESPONSE_BYTES));
  }
  assert(p->count == 4 && p->bytes == API_WATCH_REPLAY_BYTES);
  dispose(p);
  free(s);
  free(p);
}

static atomic_uint full_count, sample_count, manual_mute;

static void paging_tests(void) {
  struct api_watch *p = calloc(1, sizeof *p);
  struct mpc_state_snapshot *s = malloc(sizeof *s);
  assert(p && s);
  initialize(p);
  baseline(s);
  s->track_count = 17;
  for (unsigned i = 1; i < 17; i++) {
    s->tracks[i] = s->tracks[0];
    s->tracks[i].identity.id = i + 1;
    s->tracks[i].identity.index = i;
    s->tracks[i].collection_index = i;
  }
  strcpy(s->tracks[16].type, "audio");
  s->tracks[16].collection = MPC_STATE_AUDIO;
  s->tracks[16].collection_index = 0;
  assert(api_valid_snapshot(s, instance_id));
  assert(!snapshot(p, s));
  struct mpc_command_observation o = {
    .project = 1,
    .generation = 1,
    .topology_epoch = 1,
    .captured_ms = 2,
    .total_tracks = 17,
    .track_count = 16};
  for (unsigned i = 0; i < 16; i++)
    o.tracks[i] = (struct mpc_command_track){.id = i + 1,
                                             .generation = 1,
                                             .known = MPC_STATE_REQUESTED,
                                             .family = MPC_NATIVE_PLUGIN,
                                             .muted = 1};
  assert(!controls(p, &o) && p->offset == 16 && p->revision == 2);
  assert(controls(p, &o) == -1); /* A duplicated/omitted page cannot claim coverage. */
  o.offset = 16;
  o.track_count = 1;
  o.tracks[0] = (struct mpc_command_track){.id = 17,
                                           .generation = 1,
                                           .known = MPC_STATE_REQUESTED,
                                           .family = MPC_NATIVE_AUDIO,
                                           .muted = 1};
  assert(!controls(p, &o) && !p->offset && p->revision == 3);
  assert(p->state.tracks[16].muted && p->state.tracks[0].muted);
  assert(!p->full_state.tracks[0].muted); /* Never relabel a partial merge as a fresh full read. */
  dispose(p);
  free(s);
  free(p);
}

static int fake_state(void *unused, const char *instance, struct mpc_state_snapshot *s) {
  (void)unused;
  assert(!strcmp(instance, instance_id));
  baseline(s);
  s->tracks[0].muted = atomic_load(&manual_mute);
  atomic_fetch_add(&full_count, 1);
  return 0;
}

static int fake_sample(
  void *unused,
  const char *instance,
  unsigned offset,
  uint64_t topology,
  struct mpc_command_observation *o) {
  (void)unused;
  assert(!strcmp(instance, instance_id) && !offset && topology <= 1);
  *o = (struct mpc_command_observation){.project = 1,
                                        .generation = 1,
                                        .topology_epoch = 1,
                                        .captured_ms = 2,
                                        .total_tracks = 1,
                                        .track_count = 1};
  o->tracks[0] = (struct mpc_command_track){.id = 1,
                                            .generation = 1,
                                            .known = MPC_STATE_REQUESTED,
                                            .family = MPC_NATIVE_PLUGIN,
                                            .muted = atomic_load(&manual_mute)};
  atomic_fetch_add(&sample_count, 1);
  return 0;
}

static void producer_tests(void) {
  struct api_backend backend = {.state = fake_state, .sample = fake_sample};
  struct api api = {.device_id = "fixture", .backend = &backend};
  strcpy(api.instance_id, instance_id);
  struct api_watch *p = api_watch_create(&api);
  assert(p);
  struct discovery d = {.state_ready = 1};
  strcpy(d.adapter_id, instance_id);
  api.watch = p;
  api_watch_sync(&api, &d);
  char *read = api_watch_read(p, "get_state");
  assert(read && strstr(read, "\"revision\":\"1\""));
  free(read);
  pthread_mutex_lock(&p->mutex);
  p->subscribers = API_WATCH_SUBSCRIBERS;
  uint64_t epoch = p->epoch;
  pthread_mutex_unlock(&p->mutex);
  struct api_subscription subscriber = {.publisher = p, .revision = 1, .epoch = epoch, .stage = 1};
  atomic_store(&manual_mute, 1);
  int observed = 0;
  for (unsigned i = 0; i < 100 && !observed; i++) {
    char *next = api_watch_next(&subscriber);
    observed = next && strstr(next, "\"type\":\"change\"");
    free(next);
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
  }
  assert(observed);
  assert(atomic_load(&full_count) == 1); /* Subscribers never multiply full captures. */
  assert(atomic_load(&sample_count) > 0 && atomic_load(&sample_count) < 5);
  api_watch_destroy(p); /* Joins the single worker before retiring its backend. */
}

int main(void) {
  replay_tests();
  paging_tests();
  producer_tests();
  puts("watch replay, expiry, context fencing and shared producer passed");
  return 0;
}
