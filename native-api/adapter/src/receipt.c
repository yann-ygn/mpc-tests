#include "mpc_commands.h"
#include "private.h"
#include "mpc_mixer_wire.h"
#include "mpc_track_setup_wire.h"
#include "mpc_family_profile.h"

#include <inttypes.h>
#include <math.h>
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
static void sequence_ref(
  FILE *output, const char *instance, uint64_t project, uint64_t sequence, uint64_t generation) {
  fprintf(
    output,
    "{\"instanceId\":\"%s\",\"projectId\":\"%" PRIu64 "\",\"sequenceId\":\"%" PRIu64
    "\",\"generation\":\"%" PRIu64 "\"}",
    instance,
    project,
    sequence,
    generation);
}
static void mute_context(
  FILE *output,
  const struct mpc_command_receipt *receipt,
  const struct mpc_command_observation *observation) {
  fprintf(
    output,
    "{\"scope\":%s,\"sequence\":",
    observation->mute_scope == MPC_MUTE_GLOBAL       ? "\"global\""
      : observation->mute_scope == MPC_MUTE_SEQUENCE ? "\"sequence\""
                                                     : "null");
  if (observation->mute_scope == MPC_MUTE_SEQUENCE && observation->selected_sequence)
    sequence_ref(
      output,
      receipt->request.target.instance_id,
      observation->project,
      observation->selected_sequence,
      observation->selected_sequence_generation);
  else
    fputs("null", output);
  fputc('}', output);
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
  static const unsigned masks[] = {
    MPC_NATIVE_MUTED,
    MPC_NATIVE_SOLOED,
    MPC_NATIVE_ARMED,
    MPC_NATIVE_SOLO_COPY,
    MPC_NATIVE_ARM_COPY,
    MPC_NATIVE_SILENCED};
  for (unsigned i = 0; i < receipt->before.track_count; i++) {
    const struct mpc_command_track *a = &receipt->before.tracks[i];
    const struct mpc_command_track *b = find(&receipt->after, a->id);
    if (!b || a->generation != b->generation || a->family != b->family)
      continue;
    const uint32_t before[] =
      {a->muted, a->soloed, a->record_armed, a->audio_soloed, a->audio_record_armed, a->silenced};
    const uint32_t after[] =
      {b->muted, b->soloed, b->record_armed, b->audio_soloed, b->audio_record_armed, b->silenced};
    for (unsigned field = 0; field < sizeof paths / sizeof *paths; field++) {
      if (!(a->known & b->known & masks[field]) || before[field] == after[field])
        continue;
      if (
        !field &&
        (receipt->before.mute_scope != receipt->after.mute_scope ||
         (receipt->before.mute_scope == MPC_MUTE_SEQUENCE &&
          (receipt->before.selected_sequence != receipt->after.selected_sequence ||
           receipt->before.selected_sequence_generation !=
             receipt->after.selected_sequence_generation))))
        continue;
      fprintf(
        output,
        "%s{\"projectId\":\"%" PRIu64 "\",\"trackId\":\"%" PRIu64 "\",\"path\":\"tracks.%" PRIu64
        ".%s\",\"before\":%s,\"after\":%s",
        comma ? "," : "",
        receipt->before.project,
        a->id,
        a->id,
        paths[field],
        boolean(before[field]),
        boolean(after[field]));
      if (!field) {
        fputs(",\"muteContext\":", output);
        mute_context(output, receipt, &receipt->after);
      }
      fputc('}', output);
      comma = true;
    }
    if (a->id == receipt->request.target.track) {
      for (unsigned field = 0; field < MPC_TRACK_SETUP_FIELDS; field++) {
        double left = receipt->setup_before.values[field],
               right = receipt->setup_after.values[field];
        if (
          !(receipt->setup_before.known & receipt->setup_after.known & (1u << field)) ||
          left == right)
          continue;
        fprintf(
          output,
          "%s{\"projectId\":\"%" PRIu64 "\",\"trackId\":\"%" PRIu64 "\",\"path\":\"tracks.%" PRIu64
          ".setup.%s\",\"before\":",
          comma ? "," : "",
          receipt->before.project,
          a->id,
          a->id,
          mpc_track_setup_names[field]);
        mpc_mixer_scalar_json(output, !mpc_track_setup_boolean(field), left);
        fputs(",\"after\":", output);
        mpc_mixer_scalar_json(output, !mpc_track_setup_boolean(field), right);
        fputc('}', output);
        comma = true;
      }
    }
    if (a->mixer.generation && a->mixer.generation == b->mixer.generation) {
      for (unsigned op = MPC_COMMAND_VOLUME; op <= MPC_COMMAND_INSERT; op++) {
        unsigned count = op == MPC_COMMAND_SEND || op == MPC_COMMAND_INSERT ? 4 : 1;
        for (unsigned index = 0; index < count; index++) {
          double left, right;
          if (
            !mpc_mixer_value(&a->mixer, op, index, &left) ||
            !mpc_mixer_value(&b->mixer, op, index, &right) || left == right)
            continue;
          const char *name = op == MPC_COMMAND_VOLUME ? "volume"
            : op == MPC_COMMAND_PAN                   ? "pan"
            : op == MPC_COMMAND_SEND                  ? "sends"
            : op == MPC_COMMAND_RACK                  ? "insertRackEnabled"
                                                      : "inserts";
          fprintf(
            output,
            "%s{\"projectId\":\"%" PRIu64 "\",\"trackId\":\"%" PRIu64
            "\",\"path\":\"tracks.%" PRIu64 ".mixer.%s",
            comma ? "," : "",
            receipt->before.project,
            a->id,
            a->id,
            name);
          if (count == 4)
            fprintf(output, ".%u", index);
          fputs("\",\"before\":", output);
          mpc_mixer_scalar_json(output, mpc_mixer_numeric(op), left);
          fputs(",\"after\":", output);
          mpc_mixer_scalar_json(output, mpc_mixer_numeric(op), right);
          fputc('}', output);
          comma = true;
        }
      }
    }
  }
  fputc(']', output);
}

static void coverage(FILE *output, const struct mpc_command_observation *sample, bool known) {
  if (!known) {
    fputs("null", output);
    return;
  }
  fprintf(
    output,
    "{\"kind\":\"user_track_flags\",\"offset\":%u,\"count\":%u,"
    "\"fields\":[\"flags\",\"mixer\"],\"totalTracks\":%u,\"topologyEpoch\":\"%" PRIu64
    "\",\"durationUs\":%" PRIu64 "}",
    sample->offset,
    sample->track_count,
    sample->total_tracks,
    sample->topology_epoch,
    sample->duration_ns / 1000);
}

char *mpc_commands_receipt_json(const struct mpc_command_receipt *receipt) {
  if (
    !receipt || receipt->abi_version != MPC_COMMANDS_ABI ||
    receipt->struct_bytes != sizeof *receipt || !mpc_navigation_request_valid(&receipt->request) ||
    !mpc_mixer_request_valid(
      receipt->request.operation,
      &receipt->request.mixer,
      receipt->request.has_expected) ||
    (unsigned)receipt->status > MPC_COMMAND_UNKNOWN ||
    (unsigned)receipt->execution > MPC_COMMAND_COMPLETED ||
    (unsigned)receipt->request.operation > MPC_COMMAND_MIDI_PROGRAM ||
    (receipt->request.continuous && !mpc_mixer_numeric(receipt->request.operation)) ||
    (unsigned)receipt->request.mute_scope > MPC_MUTE_SEQUENCE ||
    (receipt->request.mute_scope == MPC_MUTE_SEQUENCE &&
     (receipt->request.operation != MPC_COMMAND_MUTE ||
      memcmp(receipt->request.sequence.instance_id, receipt->request.target.instance_id, 37) ||
      receipt->request.sequence.project != receipt->request.target.project ||
      !receipt->request.sequence.sequence || !receipt->request.sequence.generation)) ||
    !mpc_track_setup_valid(&receipt->setup_after) || receipt->navigation_after.known > 1 ||
    (receipt->navigation_after.known &&
     (receipt->navigation_after.playing > 1 || receipt->navigation_after.record_mode > 4 ||
      !isfinite(receipt->navigation_after.sequence_bpm) ||
      receipt->navigation_after.sequence_bpm < 20 ||
      receipt->navigation_after.sequence_bpm > 999)) ||
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
  static const char *operations[] = {
    "set_mute",
    "set_solo",
    "set_record_armed",
    "set_volume",
    "set_pan",
    "set_send_level",
    "set_insert_rack_enabled",
    "set_insert_enabled",
    "play",
    "stop",
    "play_start",
    "locate",
    "select_track",
    "select_sequence",
    "set_sequence_bpm",
    "set_track_colour",
    "set_track_transpose",
    "set_track_velocity_scale",
    "set_midi_output_channel",
    "set_midi_bank_enabled",
    "set_midi_bank_msb",
    "set_midi_bank_lsb",
    "set_midi_program_enabled",
    "set_midi_program"};
  static const char *statuses[] = {"accepted", "applied", "rejected", "cancelled", "unknown"};
  static const char *stages[] = {"not_started", "started", "completed"};
  char *buffer = NULL;
  size_t bytes = 0;
  FILE *output = open_memstream(&buffer, &bytes);
  if (!output)
    return NULL;
  bool numeric = mpc_mixer_numeric(receipt->request.operation);
  char desired[64];
  if (mpc_command_track_edit(receipt->request.operation)) {
    unsigned field = receipt->request.operation - MPC_COMMAND_TRACK_COLOUR;
    if (mpc_track_setup_boolean(field))
      strcpy(desired, boolean(receipt->request.edit.value != 0));
    else
      snprintf(desired, sizeof desired, "%.17g", receipt->request.edit.value);
  } else if (numeric)
    snprintf(desired, sizeof desired, "%.17g", receipt->request.mixer.value);
  else if (receipt->request.operation >= MPC_COMMAND_PLAY)
    strcpy(desired, "null");
  else
    strcpy(desired, boolean(receipt->request.value));
  char track_field[64] = "";
  if (receipt->request.target.track)
    snprintf(
      track_field,
      sizeof track_field,
      ",\"trackId\":\"%" PRIu64 "\"",
      receipt->request.target.track);
  fprintf(
    output,
    "{\"apiVersion\":\"0.1\",\"requestId\":\"%s\",\"instanceId\":\"%s\","
    "\"status\":\"%s\",\"execution\":\"%s\",\"target\":{\"instanceId\":\"%s\","
    "\"projectId\":\"%" PRIu64
    "\"%s},\"op\":\"%s\",\"value\":%s,\"changed\":%s,\"nativeInvoked\":%s,\"retention\":",
    receipt->request.request_id,
    receipt->request.target.instance_id,
    statuses[receipt->status],
    stages[receipt->execution],
    receipt->request.target.instance_id,
    receipt->request.target.project,
    track_field,
    operations[receipt->request.operation],
    desired,
    receipt->changed_known ? boolean(receipt->changed) : "null",
    receipt->native_known ? boolean(receipt->native_invoked) : "null");
  fprintf(output, "\"%s\"", receipt->request.continuous ? "supersedable" : "retained");
  fputs(",\"verification\":", output);
  const struct mpc_command_track *track =
    receipt->after_known && receipt->after.project == receipt->request.target.project
    ? find(&receipt->after, receipt->request.target.track)
    : NULL;
  bool requested = false;
  unsigned property = receipt->request.operation;
  if (mpc_command_track_edit(property)) {
    unsigned field = property - MPC_COMMAND_TRACK_COLOUR;
    if (receipt->setup_after.known & (1u << field)) {
      fputs("{\"by\":[\"model\"],\"requestedValue\":", output);
      mpc_mixer_scalar_json(
        output,
        !mpc_track_setup_boolean(field),
        receipt->setup_after.values[field]);
      fputs(",\"audioSideValue\":null,\"observedAt\":null}", output);
    } else
      fputs("null", output);
  } else if (property >= MPC_COMMAND_PLAY) {
    if (receipt->navigation_after.known) {
      fprintf(
        output,
        "{\"by\":[\"model\"],\"requestedValue\":null,\"audioSideValue\":null,"
        "\"observedAt\":null,\"transport\":{\"playing\":%s,\"recordMode\":%u,\"pulse\":\"%" PRId64
        "\",\"selectedTrackId\":",
        boolean(receipt->navigation_after.playing),
        receipt->navigation_after.record_mode,
        receipt->navigation_after.pulse);
      id(output, receipt->navigation_after.selected_track);
      fputs(",\"selectedSequenceId\":", output);
      id(output, receipt->navigation_after.selected_sequence);
      fprintf(output, ",\"sequenceBpm\":%.9g}}", receipt->navigation_after.sequence_bpm);
    } else
      fputs("null", output);
  } else if (property >= MPC_COMMAND_VOLUME && track) {
    double observed;
    if (mpc_mixer_value(&track->mixer, property, receipt->request.mixer.index, &observed)) {
      fputs("{\"by\":[\"model\"],\"requestedValue\":", output);
      mpc_mixer_scalar_json(output, numeric, observed);
      fputs(",\"audioSideValue\":null,\"observedAt\":", output);
      time_t seconds = (time_t)(receipt->after.captured_ms / 1000);
      struct tm time;
      char date[32];
      if (gmtime_r(&seconds, &time) && strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &time))
        fprintf(output, "\"%s.%03uZ\"", date, (unsigned)(receipt->after.captured_ms % 1000));
      else
        fputs("null", output);
      fputs(",\"mixer\":", output);
      mpc_mixer_json(output, &track->mixer);
      fputc('}', output);
    } else
      fputs("null", output);
  } else if (property < MPC_COMMAND_VOLUME && track && (track->known & (1u << property))) {
    bool mute = receipt->request.operation == MPC_COMMAND_MUTE;
    bool copy_known = !mute &&
      (track->known & (property == MPC_COMMAND_SOLO ? MPC_NATIVE_SOLO_COPY : MPC_NATIVE_ARM_COPY));
    requested = mute                                   ? track->muted
      : receipt->request.operation == MPC_COMMAND_SOLO ? track->soloed
                                                       : track->record_armed;
    fprintf(
      output,
      "{\"by\":[\"model\"%s],\"requestedValue\":%s,\"audioSideValue\":%s,\"observedAt\":",
      copy_known ? ",\"audio_copy\"" : "",
      boolean(requested),
      !copy_known ? "null"
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
    fputs(",\"muteContext\":", output);
    mute_context(output, receipt, &receipt->after);
    fputc('}', output);
  } else
    fputs("null", output);
  if (property >= MPC_COMMAND_VOLUME && property < MPC_COMMAND_PLAY) {
    fprintf(output, ",\"mixerGeneration\":\"%" PRIu64 "\"", receipt->request.mixer.generation);
    if (property == MPC_COMMAND_SEND)
      fprintf(output, ",\"sendIndex\":%u", receipt->request.mixer.index);
    if (property == MPC_COMMAND_INSERT)
      fprintf(output, ",\"slotIndex\":%u", receipt->request.mixer.index);
  }
  if (property >= MPC_COMMAND_PLAY && !mpc_command_track_target(property)) {
    fputs(",\"sequence\":", output);
    sequence_ref(
      output,
      receipt->request.sequence.instance_id,
      receipt->request.sequence.project,
      receipt->request.sequence.sequence,
      receipt->request.sequence.generation);
  }
  if (property == MPC_COMMAND_SEQUENCE_BPM)
    fprintf(output, ",\"bpm\":%.17g", receipt->request.bpm);
  if (property == MPC_COMMAND_LOCATE)
    fprintf(output, ",\"pulse\":\"%" PRId64 "\"", receipt->request.pulse);
  if (receipt->request.operation == MPC_COMMAND_MUTE) {
    fprintf(
      output,
      ",\"muteScope\":\"%s\",\"sequence\":",
      receipt->request.mute_scope == MPC_MUTE_SEQUENCE ? "sequence" : "global");
    if (receipt->request.mute_scope == MPC_MUTE_SEQUENCE)
      sequence_ref(
        output,
        receipt->request.sequence.instance_id,
        receipt->request.sequence.project,
        receipt->request.sequence.sequence,
        receipt->request.sequence.generation);
    else
      fputs("null", output);
  }
  fputs(",\"observationCoverage\":{\"before\":", output);
  coverage(output, &receipt->before, receipt->before_known);
  fputs(",\"after\":", output);
  coverage(output, &receipt->after, receipt->after_known);
  fputc('}', output);
  fputs(",\"observedChanges\":", output);
  changes(output, receipt);
  fputs(",\"timing\":{\"queueMs\":", output);
  if (receipt->dispatched_ms >= receipt->accepted_ms && receipt->dispatched_ms)
    fprintf(output, "%" PRIu64, receipt->dispatched_ms - receipt->accepted_ms);
  else
    fputs("null", output);
  fputs(",\"dispatchMs\":", output);
  if (receipt->dispatch_finished_ms >= receipt->dispatched_ms && receipt->dispatched_ms)
    fprintf(output, "%" PRIu64, receipt->dispatch_finished_ms - receipt->dispatched_ms);
  else
    fputs("null", output);
  fputs(",\"verificationMs\":", output);
  if (receipt->finished_ms >= receipt->dispatch_finished_ms && receipt->dispatch_finished_ms)
    fprintf(output, "%" PRIu64, receipt->finished_ms - receipt->dispatch_finished_ms);
  else
    fputs("null", output);
  fputs(",\"totalMs\":", output);
  if (receipt->finished_ms >= receipt->accepted_ms && receipt->finished_ms)
    fprintf(output, "%" PRIu64, receipt->finished_ms - receipt->accepted_ms);
  else
    fputs("null", output);
  fprintf(
    output,
    ",\"uiWorkUs\":%" PRIu64 ",\"verificationPasses\":%u",
    receipt->ui_work_us,
    receipt->verification_passes);
  fprintf(output, ",\"maxUiCallbackUs\":%" PRIu64 ",\"stagesUs\":{", receipt->max_ui_callback_us);
  static const char *stage_names[] = {
    "identity",
    "policy",
    "filterSetter",
    "targetReadback",
    "beforeObservation",
    "afterObservation",
    "other"};
  for (unsigned i = 0; i < 7; i++)
    fprintf(output, "%s\"%s\":%" PRIu64, i ? "," : "", stage_names[i], receipt->stages_us[i]);
  fprintf(output, "},\"verificationUiUs\":%" PRIu64 "}", receipt->verification_ui_us);
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
    if (receipt->error == MPC_NATIVE_PRECONDITION_FAILED && track && property < MPC_COMMAND_VOLUME)
      fprintf(
        output,
        ",\"expectedValue\":%s,\"observedValue\":%s",
        boolean(receipt->request.expected),
        boolean(requested));
    if (
      receipt->error == MPC_NATIVE_PRECONDITION_FAILED && track && property >= MPC_COMMAND_VOLUME) {
      double observed;
      if (mpc_mixer_value(&track->mixer, property, receipt->request.mixer.index, &observed)) {
        fputs(",\"expectedValue\":", output);
        mpc_mixer_scalar_json(
          output,
          numeric,
          numeric ? receipt->request.mixer.expected : receipt->request.expected);
        fputs(",\"observedValue\":", output);
        mpc_mixer_scalar_json(output, numeric, observed);
      }
    }
    fputs("}}", output);
  }
  fputs("}\n", output);
  if (fclose(output)) {
    free(buffer);
    return NULL;
  }
  return buffer;
}
