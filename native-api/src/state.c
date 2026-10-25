#include "api.h"
#include "mpc_native.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int valid_text(const char *text, size_t capacity) {
  const unsigned char *p = (const unsigned char *)text;
  const unsigned char *end = memchr(text, 0, capacity);
  if (!end)
    return 0;
  while (p < end) {
    unsigned c = *p++;
    if (c < 128)
      continue;
    unsigned count, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      count = 1;
      minimum = 0x80;
      c &= 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
      count = 2;
      minimum = 0x800;
      c &= 0xf;
    } else if (c >= 0xf0 && c <= 0xf4) {
      count = 3;
      minimum = 0x10000;
      c &= 7;
    } else
      return 0;
    if ((size_t)(end - p) < count)
      return 0;
    for (unsigned i = 0; i < count; i++) {
      if ((*p & 0xc0) != 0x80)
        return 0;
      c = (c << 6) | (*p++ & 0x3f);
    }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return 0;
  }
  return 1;
}

static int valid_snapshot(const struct mpc_state_snapshot *s, const char *instance) {
  if (
    s->abi_version != MPC_STATE_ABI || s->struct_bytes != sizeof *s ||
    memcmp(s->instance_id, instance, sizeof s->instance_id) ||
    s->track_count > MPC_IDENTITY_MAX_TRACKS || s->bus_count > MPC_STATE_MAX_BUSES ||
    s->sequence_count > MPC_IDENTITY_MAX_SEQUENCES ||
    s->sequence_slots > MPC_IDENTITY_MAX_SEQUENCES || s->sequence_count > s->sequence_slots ||
    !s->started_ms || !s->finished_ms || !s->attempts || s->attempts > MPC_STATE_ATTEMPTS ||
    (!s->project &&
     (s->project_generation || s->selected_track || s->selected_sequence || s->track_count ||
      s->bus_count || s->sequence_count || s->sequence_slots)) ||
    (s->project && !s->project_generation))
    return 0;
  unsigned selected_track = !s->selected_track, selected_sequence = !s->selected_sequence;
  for (unsigned i = 0; i < s->track_count + s->bus_count; i++) {
    int normal = i < s->track_count;
    const struct mpc_state_track *t = normal ? &s->tracks[i] : &s->buses[i - s->track_count];
    if (
      !valid_text(t->name, sizeof t->name) || !valid_text(t->type, sizeof t->type) ||
      (t->known && t->known != 15u) || !t->type[0] || t->muted > 1 || t->soloed > 1 ||
      t->record_armed > 1 || t->audio_soloed > 1 || t->audio_record_armed > 1 || t->silenced > 1 ||
      t->identity.index != (normal ? i : i - s->track_count) ||
      (normal && (!t->identity.id || !t->identity.generation)) ||
      (normal && (!strcmp(t->type, "plugin") || !strcmp(t->type, "drum")) && t->known != 15u) ||
      (!normal && (t->known || t->identity.id || t->identity.generation)) ||
      (t->known && strcmp(t->type, "plugin") && strcmp(t->type, "drum")))
      return 0;
    if (normal) {
      selected_track += t->identity.id == s->selected_track;
      for (unsigned j = 0; j < i; j++)
        if (t->identity.id == s->tracks[j].identity.id)
          return 0;
    }
  }
  unsigned used = 0;
  for (unsigned i = 0; i < s->sequence_count; i++) {
    const struct mpc_state_sequence *q = &s->sequences[i];
    if (
      !q->identity.id || !q->identity.generation || q->identity.index >= s->sequence_slots ||
      !valid_text(q->name, sizeof q->name) || q->used > 1 || !isfinite(q->bpm) || q->bpm <= 0 ||
      q->bpm > 1000)
      return 0;
    for (unsigned j = 0; j < i; j++)
      if (
        q->identity.id == s->sequences[j].identity.id ||
        q->identity.index == s->sequences[j].identity.index)
        return 0;
    selected_sequence += q->identity.id == s->selected_sequence;
    used += q->used;
  }
  return selected_track == 1 && selected_sequence == 1 && used == s->used_sequences;
}

static void nullable_bool(FILE *f, unsigned known, unsigned value) {
  fputs(known ? (value ? "true" : "false") : "null", f);
}

static void track(
  FILE *f, const struct api *api, uint64_t project, const struct mpc_state_track *t, int normal) {
  if (!t) {
    fputs("null", f);
    return;
  }
  fputs("{\"ref\":", f);
  if (normal)
    fprintf(
      f,
      "{\"instanceId\":\"%s\",\"projectId\":\"%" PRIu64 "\",\"trackId\":\"%" PRIu64 "\"}",
      api->instance_id,
      project,
      t->identity.id);
  else
    fputs("null", f);
  fprintf(f, ",\"generation\":");
  if (normal)
    fprintf(f, "\"%" PRIu64 "\"", t->identity.generation);
  else
    fputs("null", f);
  fprintf(
    f,
    ",\"kind\":\"%s\",\"index\":%u,\"name\":",
    normal ? "track" : "bus",
    t->identity.index);
  api_quoted(f, t->name);
  fputs(",\"type\":", f);
  api_quoted(f, strcmp(t->type, "unknown") ? t->type : NULL);
  fputs(",\"requested\":{\"muted\":", f);
  nullable_bool(f, t->known & MPC_STATE_REQUESTED, t->muted);
  fputs(",\"soloed\":", f);
  nullable_bool(f, t->known & MPC_STATE_REQUESTED, t->soloed);
  fputs(",\"recordArmed\":", f);
  nullable_bool(f, t->known & MPC_STATE_REQUESTED, t->record_armed);
  fputs("},\"audioSide\":{\"muted\":null,\"soloed\":", f);
  nullable_bool(f, t->known & MPC_STATE_AUDIO_SOLO, t->audio_soloed);
  fputs(",\"recordArmed\":", f);
  nullable_bool(f, t->known & MPC_STATE_AUDIO_ARM, t->audio_record_armed);
  fputs("},\"silenced\":", f);
  nullable_bool(f, t->known & MPC_STATE_SILENCED, t->silenced);
  fputs(",\"unsupportedFields\":", f);
  fputs(
    t->known
      ? "[\"audioSide.muted\",\"audioActivity\"]"
      : "[\"requested.muted\",\"requested.soloed\",\"requested.recordArmed\",\"audioSide.muted\","
        "\"audioSide.soloed\",\"audioSide.recordArmed\",\"silenced\",\"audioActivity\"]",
    f);
  fputc('}', f);
}

static void sequence(FILE *f, const struct mpc_state_sequence *q) {
  if (!q) {
    fputs("null", f);
    return;
  }
  fprintf(
    f,
    "{\"id\":\"%" PRIu64 "\",\"generation\":\"%" PRIu64 "\",\"index\":%u,\"name\":",
    q->identity.id,
    q->identity.generation,
    q->identity.index);
  api_quoted(f, q->name);
  fprintf(
    f,
    ",\"used\":%s,\"bpm\":%.9g,\"bpmSource\":\"sequence_property\"}",
    q->used ? "true" : "false",
    (double)q->bpm);
}

static void timestamp(FILE *f, uint64_t milliseconds) {
  time_t seconds = (time_t)(milliseconds / 1000);
  struct tm time;
  char date[32];
  if (!gmtime_r(&seconds, &time) || !strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &time)) {
    fputs("null", f);
    return;
  }
  fprintf(f, "\"%s.%03uZ\"", date, (unsigned)(milliseconds % 1000));
}

char *api_state(struct api *api, const struct discovery *discovery, const char *operation) {
  struct mpc_state_snapshot *s = calloc(1, sizeof *s);
  if (!s)
    return api_error("NOT_READY", "Snapshot allocation failed");
  int error = adapter_state(api->root, discovery->pid, discovery->adapter_id, s);
  if (error || !valid_snapshot(s, discovery->adapter_id)) {
    free(s);
    return api_error(
      error == MPC_NATIVE_STATE_UNSTABLE ? "STATE_UNSTABLE"
        : error == MPC_NATIVE_NOT_READY  ? "NOT_READY"
                                         : "READ_FAILED",
      "Could not capture a validated fresh snapshot");
  }
  /* Recheck attachment/process identity after transfer; never publish a sample
   * from an attachment replaced while the request was in flight. */
  char instance[37];
  strcpy(instance, api->instance_id);
  struct discovery after;
  if (
    discovery_read(api, &after) || !after.state_ready || strcmp(instance, api->instance_id) ||
    strcmp(discovery->adapter_id, after.adapter_id)) {
    free(s);
    return api_error("STATE_UNSTABLE", "MPC or snapshot authority changed during capture");
  }
  if (api->revision == UINT64_MAX) {
    free(s);
    return api_error("NOT_READY", "Observation revision exhausted");
  }
  char *buffer = NULL;
  size_t bytes = 0;
  FILE *f = open_memstream(&buffer, &bytes);
  if (!f) {
    free(s);
    return NULL;
  }
  fprintf(f, "{\"apiVersion\":\"" API_VERSION "\",\"deviceId\":");
  api_quoted(f, api->device_id);
  fprintf(f, ",\"instanceId\":\"%s\",\"projectId\":", api->instance_id);
  if (s->project)
    fprintf(f, "\"%" PRIu64 "\"", s->project);
  else
    fputs("null", f);
  fprintf(f, ",\"projectGeneration\":");
  if (s->project)
    fprintf(f, "\"%" PRIu64 "\"", s->project_generation);
  else
    fputs("null", f);
  fprintf(f, ",\"revision\":\"%" PRIu64 "\",\"sample\":{\"startedAt\":", ++api->revision);
  timestamp(f, s->started_ms);
  fputs(",\"finishedAt\":", f);
  timestamp(f, s->finished_ms);
  fprintf(
    f,
    ",\"durationMs\":%.3f,\"attempts\":%u,\"atomic\":false,\"consistency\":\"identity_topology_"
    "selection_checked\",\"freshness\":\"fresh\",\"source\":\"native_ui_thread\"}",
    (double)s->duration_ns / 1000000,
    s->attempts);
  const struct mpc_state_track *selected_track = NULL;
  const struct mpc_state_sequence *selected_sequence = NULL;
  for (unsigned i = 0; i < s->track_count; i++)
    if (s->tracks[i].identity.id == s->selected_track)
      selected_track = &s->tracks[i];
  for (unsigned i = 0; i < s->sequence_count; i++)
    if (s->sequences[i].identity.id == s->selected_sequence)
      selected_sequence = &s->sequences[i];
  if (!strcmp(operation, "current_track")) {
    fputs(",\"track\":", f);
    track(f, api, s->project, selected_track, 1);
  } else if (!strcmp(operation, "current_sequence")) {
    fputs(",\"sequence\":", f);
    sequence(f, selected_sequence);
  } else {
    if (strcmp(operation, "list_tracks")) {
      fputs(",\"project\":", f);
      fputs(s->project ? "{\"name\":null}" : "null", f);
      fputs(",\"selectedTrackId\":", f);
      if (s->selected_track)
        fprintf(f, "\"%" PRIu64 "\"", s->selected_track);
      else
        fputs("null", f);
      fputs(",\"selectedSequence\":", f);
      sequence(f, selected_sequence);
      fprintf(
        f,
        ",\"sequenceSlotCount\":%u,\"sequenceObjectCount\":%u,\"usedSequenceCount\":%u,"
        "\"sequences\":[",
        s->sequence_slots,
        s->sequence_count,
        s->used_sequences);
      for (unsigned i = 0; i < s->sequence_count; i++) {
        if (i)
          fputc(',', f);
        sequence(f, &s->sequences[i]);
      }
      fputs(
        "],\"transport\":{\"effectiveBpm\":null,\"tempoSource\":null,\"playing\":null,"
        "\"recording\":null}",
        f);
    }
    fputs(",\"tracks\":[", f);
    for (unsigned i = 0; i < s->track_count; i++) {
      if (i)
        fputc(',', f);
      track(f, api, s->project, &s->tracks[i], 1);
    }
    fputs("],\"buses\":[", f);
    for (unsigned i = 0; i < s->bus_count; i++) {
      if (i)
        fputc(',', f);
      track(f, api, s->project, &s->buses[i], 0);
    }
    fputc(']', f);
  }
  fputs(
    ",\"unsupportedFields\":[\"project.name\",\"transport.effectiveBpm\",\"transport.tempoSource\","
    "\"transport.playing\",\"transport.recording\"]}\n",
    f);
  free(s);
  if (fclose(f) || bytes > MAX_RESPONSE_BYTES) {
    free(buffer);
    return api_error("READ_FAILED", "Snapshot response exceeds limits");
  }
  return buffer;
}
