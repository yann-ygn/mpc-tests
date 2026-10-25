#include "mpc_commands.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *boolean(bool value) {
  return value ? "true" : "false";
}
static void id(FILE *output, uint64_t value) {
  if (value)
    fprintf(output, "\"%" PRIu64 "\"", value);
  else
    fputs("null", output);
}
static const struct mpc_command_track *
find(const struct mpc_command_observation *observation, uint64_t track) {
  for (unsigned i = 0; i < observation->track_count; i++)
    if (observation->tracks[i].id == track)
      return &observation->tracks[i];
  return NULL;
}
static const char *error_name(enum mpc_native_error error) {
  return error == MPC_NATIVE_UNSUPPORTED_TRACK_TYPE ? "UNQUALIFIED_TRACK_TYPE"
    : error == MPC_NATIVE_STALE_TARGET              ? "TARGET_NOT_FOUND"
                                                    : mpc_native_error_name(error);
}
static void changes(FILE *output, const struct mpc_command_receipt *receipt) {
  fputc('[', output);
  if (
    !receipt->before_known || !receipt->after_known ||
    receipt->before.project != receipt->after.project ||
    receipt->before.generation != receipt->after.generation) {
    fputc(']', output);
    return;
  }
  bool comma = false;
  if (receipt->before.selected_track != receipt->after.selected_track) {
    fprintf(
      output,
      "{\"projectId\":\"%" PRIu64 "\",\"path\":\"selectedTrackId\",\"before\":",
      receipt->before.project);
    id(output, receipt->before.selected_track);
    fputs(",\"after\":", output);
    id(output, receipt->after.selected_track);
    fputc('}', output);
    comma = true;
  }
  if (receipt->before.selected_sequence != receipt->after.selected_sequence) {
    fprintf(
      output,
      "%s{\"projectId\":\"%" PRIu64 "\",\"path\":\"selectedSequenceId\",\"before\":",
      comma ? "," : "",
      receipt->before.project);
    id(output, receipt->before.selected_sequence);
    fputs(",\"after\":", output);
    id(output, receipt->after.selected_sequence);
    fputc('}', output);
    comma = true;
  }
  static const char *paths[] = {
    "requested.muted",
    "requested.soloed",
    "requested.recordArmed",
    "audioSide.soloed",
    "audioSide.recordArmed",
    "silenced"};
  for (unsigned i = 0; i < receipt->before.track_count; i++) {
    const struct mpc_command_track *a = &receipt->before.tracks[i];
    const struct mpc_command_track *b = find(&receipt->after, a->id);
    if (!b || a->generation != b->generation || a->known != 15 || b->known != 15)
      continue;
    const uint32_t before[] =
      {a->muted, a->soloed, a->record_armed, a->audio_soloed, a->audio_record_armed, a->silenced};
    const uint32_t after[] =
      {b->muted, b->soloed, b->record_armed, b->audio_soloed, b->audio_record_armed, b->silenced};
    for (unsigned field = 0; field < sizeof paths / sizeof *paths; field++) {
      if (before[field] == after[field])
        continue;
      fprintf(
        output,
        "%s{\"projectId\":\"%" PRIu64 "\",\"trackId\":\"%" PRIu64 "\",\"path\":\"tracks.%" PRIu64
        ".%s\",\"before\":%s,\"after\":%s}",
        comma ? "," : "",
        receipt->before.project,
        a->id,
        a->id,
        paths[field],
        boolean(before[field]),
        boolean(after[field]));
      comma = true;
    }
  }
  fputc(']', output);
}

char *mpc_commands_receipt_json(const struct mpc_command_receipt *receipt) {
  if (
    !receipt || (unsigned)receipt->status > MPC_COMMAND_UNKNOWN ||
    (unsigned)receipt->execution > MPC_COMMAND_COMPLETED ||
    (unsigned)receipt->request.operation > MPC_COMMAND_RECORD_ARMED ||
    receipt->before.track_count > MPC_IDENTITY_MAX_TRACKS ||
    receipt->after.track_count > MPC_IDENTITY_MAX_TRACKS ||
    strnlen(receipt->request.request_id, MPC_COMMAND_ID_BYTES) >= MPC_COMMAND_ID_BYTES ||
    strspn(
      receipt->request.request_id,
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
      strlen(receipt->request.request_id) ||
    strnlen(receipt->request.target.instance_id, 37) != 36 ||
    strncmp(receipt->request.target.instance_id, "run_", 4) ||
    strspn(receipt->request.target.instance_id + 4, "0123456789abcdef") != 32)
    return NULL;
  static const char *operations[] = {"set_mute", "set_solo", "set_record_armed"};
  static const char *statuses[] = {"accepted", "applied", "rejected", "cancelled", "unknown"};
  static const char *stages[] = {"not_started", "started", "completed"};
  char *buffer = NULL;
  size_t bytes = 0;
  FILE *output = open_memstream(&buffer, &bytes);
  if (!output)
    return NULL;
  fprintf(
    output,
    "{\"apiVersion\":\"0.1\",\"requestId\":\"%s\",\"instanceId\":\"%s\","
    "\"status\":\"%s\",\"execution\":\"%s\",\"target\":{\"instanceId\":\"%s\","
    "\"projectId\":\"%" PRIu64 "\",\"trackId\":\"%" PRIu64
    "\"},\"op\":\"%s\",\"value\":%s,\"changed\":%s,\"nativeInvoked\":%s,\"verification\":",
    receipt->request.request_id,
    receipt->request.target.instance_id,
    statuses[receipt->status],
    stages[receipt->execution],
    receipt->request.target.instance_id,
    receipt->request.target.project,
    receipt->request.target.track,
    operations[receipt->request.operation],
    boolean(receipt->request.value),
    receipt->changed_known ? boolean(receipt->changed) : "null",
    receipt->native_known ? boolean(receipt->native_invoked) : "null");
  const struct mpc_command_track *track =
    receipt->after_known && receipt->after.project == receipt->request.target.project
    ? find(&receipt->after, receipt->request.target.track)
    : NULL;
  bool requested = false;
  if (track && track->known == 15) {
    bool mute = receipt->request.operation == MPC_COMMAND_MUTE;
    requested = mute                                   ? track->muted
      : receipt->request.operation == MPC_COMMAND_SOLO ? track->soloed
                                                       : track->record_armed;
    fprintf(
      output,
      "{\"by\":[\"model\"%s],\"requestedValue\":%s,\"audioSideValue\":%s,\"observedAt\":",
      mute ? "" : ",\"audio_copy\"",
      boolean(requested),
      mute ? "null"
           : boolean(
               receipt->request.operation == MPC_COMMAND_SOLO ? track->audio_soloed
                                                              : track->audio_record_armed));
    time_t seconds = (time_t)(receipt->after.captured_ms / 1000);
    struct tm time;
    char date[32];
    if (gmtime_r(&seconds, &time) && strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &time))
      fprintf(output, "\"%s.%03uZ\"", date, (unsigned)(receipt->after.captured_ms % 1000));
    else
      fputs("null", output);
    fputc('}', output);
  } else
    fputs("null", output);
  fputs(",\"observedChanges\":", output);
  changes(output, receipt);
  fputs(",\"error\":", output);
  if (receipt->error == MPC_NATIVE_OK)
    fputs("null", output);
  else {
    fprintf(
      output,
      "{\"code\":\"%s\",\"message\":\"Command did not establish the requested state\","
      "\"details\":{\"execution\":\"%s\"",
      error_name(receipt->error),
      stages[receipt->execution]);
    if (receipt->error == MPC_NATIVE_PRECONDITION_FAILED && track)
      fprintf(
        output,
        ",\"expectedValue\":%s,\"observedValue\":%s",
        boolean(receipt->request.expected),
        boolean(requested));
    fputs("}}", output);
  }
  fputs("}\n", output);
  if (fclose(output)) {
    free(buffer);
    return NULL;
  }
  return buffer;
}
