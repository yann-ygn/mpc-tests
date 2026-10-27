#include "watch.h"
#include "mpc_executor.h"
#include "mpc_family_profile.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct replay {
  uint64_t revision;
  int64_t time;
  size_t bytes;
  char *frame;
};

/* All native work belongs to this one worker. The service only copies bounded
 * frames under mutex; neither native callbacks nor the producer perform I/O. */
struct api_watch {
  pthread_mutex_t mutex;
  pthread_cond_t wake;
  pthread_t worker;
  struct api context;
  char adapter[37];
  int ready, stop, valid, error;
  unsigned subscribers, offset;
  uint64_t epoch, revision, topology, requested, completed, full_revision;
  int64_t lease, audit, retry, reconcile;
  struct mpc_state_snapshot state, full_state;
  struct replay replay[API_WATCH_REPLAY];
  unsigned head, count;
  size_t bytes;
};

static int64_t monotonic_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static struct timespec deadline(int64_t ms) {
  return (struct timespec){.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};
}

static void evict(struct api_watch *p) {
  struct replay *r = &p->replay[p->head];
  p->bytes -= r->bytes;
  free(r->frame);
  memset(r, 0, sizeof *r);
  p->head = (p->head + 1) % API_WATCH_REPLAY;
  p->count--;
}

static void invalidate(struct api_watch *p) {
  while (p->count)
    evict(p);
  p->valid = 0;
  p->topology = 0;
  p->offset = 0;
  p->epoch++;
}

static struct replay *find(struct api_watch *p, uint64_t revision) {
  for (unsigned i = 0; i < p->count; i++) {
    struct replay *r = &p->replay[(p->head + i) % API_WATCH_REPLAY];
    if (r->revision == revision)
      return r;
  }
  return NULL;
}

static FILE *
frame(struct api_watch *p, const char *type, uint64_t revision, char **out, size_t *bytes) {
  FILE *f = open_memstream(out, bytes);
  if (!f)
    return NULL;
  uint64_t project = revision || p->valid ? p->state.project : 0;
  fprintf(
    f,
    "{\"apiVersion\":\"" API_VERSION "\",\"type\":\"%s\",\"instanceId\":\"%s\","
    "\"projectId\":",
    type,
    p->context.instance_id);
  if (project)
    fprintf(f, "\"%" PRIu64 "\"", project);
  else
    fputs("null", f);
  fputs(",\"projectGeneration\":", f);
  if (project)
    fprintf(f, "\"%" PRIu64 "\"", p->state.project_generation);
  else
    fputs("null", f);
  if (revision)
    fprintf(
      f,
      ",\"revision\":\"%" PRIu64 "\",\"cursor\":\"%s:%" PRIu64 "\"",
      revision,
      p->context.instance_id,
      revision);
  return f;
}

static char *finish(FILE *f, char **out, size_t *bytes) {
  if (!f)
    return NULL;
  fputs("}\n", f);
  if (fclose(f) || *bytes > MAX_RESPONSE_BYTES) {
    free(*out);
    *out = NULL;
  }
  return *out;
}

static char *resync(struct api_watch *p, const char *reason) {
  char *out = NULL;
  size_t bytes = 0;
  FILE *f = frame(p, "resync_required", 0, &out, &bytes);
  if (f) {
    fputs(",\"reason\":", f);
    api_quoted(f, reason);
    fputs(",\"recovery\":\"recovery_snapshot\"", f);
  }
  return finish(f, &out, &bytes);
}

static int retain(struct api_watch *p, char *data, size_t bytes) {
  if (!data) {
    invalidate(p);
    return -1;
  }
  while (p->count && (p->count == API_WATCH_REPLAY || p->bytes + bytes > API_WATCH_REPLAY_BYTES))
    evict(p);
  unsigned slot = (p->head + p->count++) % API_WATCH_REPLAY;
  p->replay[slot] = (struct replay){p->revision, monotonic_ms(), bytes, data};
  p->bytes += bytes;
  return 0;
}

static int snapshot(struct api_watch *p, const struct mpc_state_snapshot *s) {
  if (
    p->valid &&
    (p->state.project != s->project || p->state.project_generation != s->project_generation))
    invalidate(p);
  uint64_t previous = p->valid ? p->revision : 0;
  if (p->revision == UINT64_MAX)
    return -1;
  p->state = *s;
  p->full_state = *s;
  p->revision++;
  p->full_revision = p->revision;
  p->topology = 0;
  p->offset = 0;
  p->audit = monotonic_ms() + API_WATCH_AUDIT_MS;
  p->reconcile = monotonic_ms() + 1000;
  char *state = api_state_json(&p->context, s, "get_state", p->revision);
  char *out = NULL;
  size_t bytes = 0;
  FILE *f = frame(p, "snapshot", p->revision, &out, &bytes);
  if (!state || !f) {
    free(state);
    if (f)
      fclose(f);
    free(out);
    invalidate(p);
    return -1;
  }
  if (previous)
    fprintf(f, ",\"previousRevision\":\"%" PRIu64 "\"", previous);
  else
    fputs(",\"previousRevision\":null", f);
  state[strcspn(state, "\n")] = 0;
  fprintf(f, ",\"state\":%s", state);
  free(state);
  char *data = finish(f, &out, &bytes);
  if (retain(p, data, bytes))
    return -1;
  p->valid = 1;
  return 0;
}

/* Metadata stays at the last full baseline. Every controls frame declares its
 * exact interval/time; unsampled rows are never represented as fresh. */
static int controls(struct api_watch *p, const struct mpc_command_observation *o) {
  if (
    !p->valid || !o->captured_ms || !o->topology_epoch || o->offset != p->offset ||
    o->track_count > MPC_EXECUTOR_SAMPLE_TRACKS || o->total_tracks != p->state.track_count ||
    o->offset > o->total_tracks || o->track_count > o->total_tracks - o->offset ||
    (o->total_tracks && !o->track_count) || o->project != p->state.project ||
    o->generation != p->state.project_generation ||
    o->selected_sequence != p->state.selected_sequence ||
    o->selected_sequence_generation != p->state.selected_sequence_generation ||
    o->mute_scope != p->state.mute_scope || (p->topology && p->topology != o->topology_epoch))
    return -1;
  struct mpc_state_snapshot *next = malloc(sizeof *next);
  if (!next)
    return -1;
  *next = p->state;
  next->selected_track = o->selected_track;
  for (unsigned i = 0; i < o->track_count; i++) {
    const struct mpc_command_track *a = &o->tracks[i];
    struct mpc_state_track *b = &next->tracks[o->offset + i];
    if (
      a->id != b->identity.id || a->generation != b->identity.generation ||
      a->family != mpc_family_by_type(b->type)->family) {
      free(next);
      return -1;
    }
    b->known = a->known;
    b->muted = a->muted;
    b->soloed = a->soloed;
    b->record_armed = a->record_armed;
    b->audio_soloed = a->audio_soloed;
    b->audio_record_armed = a->audio_record_armed;
    b->silenced = a->silenced;
    b->mixer = a->mixer;
  }
  if (!api_valid_snapshot(next, p->adapter)) {
    free(next);
    return -1;
  }
  p->topology = o->topology_epoch;
  p->offset = o->offset + o->track_count;
  if (p->offset == o->total_tracks)
    p->offset = 0;
  if (!memcmp(next, &p->state, sizeof *next)) {
    free(next);
    return 0;
  }
  if (p->revision == UINT64_MAX) {
    free(next);
    return -1;
  }
  p->state = *next;
  free(next);
  uint64_t previous = p->revision++;
  char *out = NULL;
  size_t bytes = 0;
  FILE *f = frame(p, "change", p->revision, &out, &bytes);
  if (!f)
    return -1;
  fprintf(
    f,
    ",\"previousRevision\":\"%" PRIu64 "\",\"coverage\":{\"kind\":\"controls\","
    "\"offset\":%u,\"count\":%u,\"totalTracks\":%u,\"topologyEpoch\":\"%" PRIu64
    "\",\"capturedMs\":\"%" PRIu64 "\",\"durationNs\":\"%" PRIu64 "\"},\"selectedTrackId\":",
    previous,
    o->offset,
    o->track_count,
    o->total_tracks,
    o->topology_epoch,
    o->captured_ms,
    o->duration_ns);
  if (o->selected_track)
    fprintf(f, "\"%" PRIu64 "\"", o->selected_track);
  else
    fputs("null", f);
  fputs(",\"muteContext\":", f);
  api_mute_context(f, &p->context, &p->state);
  fputs(",\"tracks\":[", f);
  for (unsigned i = 0; i < o->track_count; i++) {
    if (i)
      fputc(',', f);
    api_track_json(f, &p->context, p->state.project, &p->state.tracks[o->offset + i], 1);
  }
  fputc(']', f);
  char *data = finish(f, &out, &bytes);
  return retain(p, data, bytes);
}

static void *produce(void *data) {
  struct api_watch *p = data;
  struct mpc_state_snapshot *s = calloc(1, sizeof *s);
  struct mpc_command_observation *o = calloc(1, sizeof *o);
  pthread_mutex_lock(&p->mutex);
  while (!p->stop && s && o) {
    int64_t now = monotonic_ms();
    while (p->count && now - p->replay[p->head].time >= API_WATCH_RETENTION_MS)
      evict(p);
    int active = p->ready && (p->subscribers || now < p->lease);
    if (!active && p->valid)
      invalidate(p);
    if (active && now >= p->retry) {
      int full = !p->valid || p->completed < p->requested || now >= p->audit;
      uint64_t epoch = p->epoch, ticket = p->requested;
      unsigned offset = p->offset;
      uint64_t topology = p->topology;
      char adapter[37];
      strcpy(adapter, p->adapter);
      pthread_mutex_unlock(&p->mutex);
      memset(s, 0, sizeof *s);
      memset(o, 0, sizeof *o);
      int error = full
        ? p->context.backend->state(p->context.backend->userdata, adapter, s)
        : p->context.backend->sample(p->context.backend->userdata, adapter, offset, topology, o);
      pthread_mutex_lock(&p->mutex);
      if (epoch != p->epoch)
        continue;
      p->error = error;
      if (full) {
        if (error || !api_valid_snapshot(s, adapter) || snapshot(p, s)) {
          invalidate(p);
          p->error = error ? error : MPC_NATIVE_READ_FAILED;
        }
        p->completed = ticket;
        pthread_cond_broadcast(&p->wake);
      } else if (error == MPC_NATIVE_STATE_UNSTABLE || (!error && controls(p, o))) {
        /* Reconcile topology/context using a full capture before more pages.
         * Same-project replacement snapshots keep the ordered replay chain. */
        p->audit = p->reconcile;
      } else if (error) {
        invalidate(p);
      }
      p->retry = monotonic_ms() + (p->valid ? API_WATCH_SAMPLE_MS : 1000);
    }
    if (active) {
      struct timespec until = deadline(monotonic_ms() + API_WATCH_SAMPLE_MS);
      pthread_cond_timedwait(&p->wake, &p->mutex, &until);
    } else
      pthread_cond_wait(&p->wake, &p->mutex);
  }
  pthread_mutex_unlock(&p->mutex);
  free(s);
  free(o);
  return NULL;
}

struct api_watch *api_watch_create(const struct api *api) {
  if (!api->backend || !api->backend->sample)
    return NULL;
  struct api_watch *p = calloc(1, sizeof *p);
  if (!p)
    return NULL;
  p->context = *api;
  pthread_mutex_init(&p->mutex, NULL);
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&p->wake, &attr);
  pthread_condattr_destroy(&attr);
  if (pthread_create(&p->worker, NULL, produce, p)) {
    pthread_mutex_destroy(&p->mutex);
    pthread_cond_destroy(&p->wake);
    free(p);
    return NULL;
  }
  return p;
}

void api_watch_destroy(struct api_watch *p) {
  if (!p)
    return;
  pthread_mutex_lock(&p->mutex);
  p->stop = 1;
  pthread_cond_broadcast(&p->wake);
  pthread_mutex_unlock(&p->mutex);
  pthread_join(p->worker, NULL);
  while (p->count)
    evict(p);
  pthread_cond_destroy(&p->wake);
  pthread_mutex_destroy(&p->mutex);
  free(p);
}

void api_watch_sync(struct api *api, const struct discovery *d) {
  struct api_watch *p = api->watch;
  if (!p)
    return;
  pthread_mutex_lock(&p->mutex);
  if (
    strcmp(p->context.instance_id, api->instance_id) || strcmp(p->adapter, d->adapter_id) ||
    p->ready != d->state_ready) {
    invalidate(p);
    strcpy(p->context.instance_id, api->instance_id);
    strcpy(p->adapter, d->adapter_id);
    p->ready = d->state_ready;
    p->retry = 0;
    pthread_cond_broadcast(&p->wake);
  }
  pthread_mutex_unlock(&p->mutex);
}

char *api_watch_read(struct api_watch *p, const char *operation) {
  pthread_mutex_lock(&p->mutex);
  uint64_t ticket = ++p->requested;
  p->lease = monotonic_ms() + API_WATCH_RETENTION_MS;
  p->retry = 0;
  pthread_cond_broadcast(&p->wake);
  struct timespec until = deadline(monotonic_ms() + ADAPTER_STATE_TIMEOUT_MS);
  while (!p->stop && p->completed < ticket)
    if (pthread_cond_timedwait(&p->wake, &p->mutex, &until))
      break;
  char *result = p->valid && p->completed >= ticket
    ? api_state_json(&p->context, &p->full_state, operation, p->full_revision)
    : api_error("STATE_UNSTABLE", "Shared snapshot capture unavailable; retry a fresh read");
  pthread_mutex_unlock(&p->mutex);
  return result;
}

char *api_watch_open(
  struct api *api, const struct api_request_data *request, struct api_subscription *s) {
  struct discovery d;
  if (discovery_read(api, &d) || !d.state_ready || !api->watch)
    return api_error("NOT_READY", "Shared observation attachment is unavailable");
  api_watch_sync(api, &d);
  struct api_watch *p = api->watch;
  pthread_mutex_lock(&p->mutex);
  if (p->subscribers == API_WATCH_SUBSCRIBERS) {
    pthread_mutex_unlock(&p->mutex);
    return api_error("NOT_READY", "Subscriber capacity reached");
  }
  *s = (struct api_subscription){.publisher = p,
                                 .revision = request->cursor_revision,
                                 .epoch = p->epoch};
  struct replay *cursor = find(p, request->cursor_revision);
  if (
    request->cursor_revision &&
    (strcmp(request->cursor_instance, p->context.instance_id) || !p->valid || !cursor ||
     monotonic_ms() - cursor->time >= API_WATCH_RETENTION_MS))
    s->terminal = 1;
  else if (!request->cursor_revision) {
    s->ticket = ++p->requested;
    p->retry = 0;
  }
  p->subscribers++;
  p->lease = monotonic_ms() + API_WATCH_RETENTION_MS;
  pthread_cond_broadcast(&p->wake);
  pthread_mutex_unlock(&p->mutex);
  return NULL;
}

char *api_watch_end(struct api_subscription *s, const char *reason) {
  struct api_watch *p = s->publisher;
  if (!p)
    return NULL;
  pthread_mutex_lock(&p->mutex);
  char *out = NULL;
  size_t bytes = 0;
  FILE *f = frame(p, "terminated", 0, &out, &bytes);
  if (f) {
    fputs(",\"reason\":", f);
    api_quoted(f, reason);
  }
  char *result = finish(f, &out, &bytes);
  s->terminal = 1;
  pthread_mutex_unlock(&p->mutex);
  return result;
}

char *api_watch_next(struct api_subscription *s) {
  struct api_watch *p = s->publisher;
  pthread_mutex_lock(&p->mutex);
  char *out = NULL;
  size_t bytes = 0;
  FILE *f = NULL;
  int64_t now = monotonic_ms();
  if (
    s->terminal || ((s->stage || !s->ticket) && s->epoch != p->epoch) ||
    (s->ticket && p->completed >= s->ticket && !p->valid)) {
    s->terminal = 1;
    out = resync(p, "CONTINUITY_LOST");
  } else if (!s->stage) {
    if (!s->ticket || p->completed >= s->ticket) {
      if (s->ticket) {
        s->revision = p->full_revision - 1;
        s->epoch = p->epoch;
      }
      uint64_t acknowledged = s->ticket ? p->full_revision : s->revision;
      f = frame(p, "ack", acknowledged, &out, &bytes);
      if (f)
        fprintf(f, ",\"resumed\":%s,\"qualified\":false", s->ticket ? "false" : "true");
      s->stage = 1;
    }
  } else if (s->revision < p->revision) {
    struct replay *r = find(p, s->revision + 1);
    if (!r || now - r->time >= API_WATCH_RETENTION_MS) {
      s->terminal = 1;
      out = resync(p, "REPLAY_GAP");
    } else {
      out = strdup(r->frame);
      if (out)
        s->revision = r->revision;
    }
  } else if (now >= s->heartbeat) {
    f = frame(p, "heartbeat", s->revision, &out, &bytes);
  }
  if (f)
    finish(f, &out, &bytes);
  if (out)
    s->heartbeat = now + API_WATCH_HEARTBEAT_MS;
  pthread_mutex_unlock(&p->mutex);
  return out;
}

void api_watch_close(struct api_subscription *s) {
  if (!s->publisher)
    return;
  struct api_watch *p = s->publisher;
  pthread_mutex_lock(&p->mutex);
  p->subscribers--;
  p->lease = monotonic_ms() + API_WATCH_RETENTION_MS;
  pthread_cond_broadcast(&p->wake);
  pthread_mutex_unlock(&p->mutex);
  memset(s, 0, sizeof *s);
}
