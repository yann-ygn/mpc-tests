#include "api.h"
#include "watch.h"
#include "host.h"
#include "profile.h"
#include "mpc_family_profile.h"
#include "mpc_mixer_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void api_quoted(FILE *output, const char *value) {
  if (!value) {
    fputs("null", output);
    return;
  }
  fputc('"', output);
  for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
    if (*p == '"' || *p == '\\') {
      fputc('\\', output);
      fputc(*p, output);
    } else if (*p < 32 || *p == 127)
      fprintf(output, "\\u%04x", *p);
    else
      fputc(*p, output);
  }
  fputc('"', output);
}

void api_flag_support(FILE *f, const char *type, unsigned known, int normal) {
  const struct mpc_family_support *support = mpc_family_by_type(type);
  static const char *fields[] = {
    "requested.muted",
    "requested.soloed",
    "requested.recordArmed",
    "audioSide.soloed",
    "audioSide.recordArmed",
    "silenced",
    "audioSide.muted",
    "audioActivity"};
  static const char *operations[] = {"set_mute", "set_solo", "set_record_armed"};
  fputs("{\"family\":", f);
  api_quoted(f, type);
  fputs(",\"readableFields\":[", f);
  bool comma = false;
  for (unsigned i = 0; i < 6; i++)
    if (known & (1u << i)) {
      if (comma)
        fputc(',', f);
      api_quoted(f, fields[i]);
      comma = true;
    }
  fputs("],\"unsupportedReasons\":{", f);
  comma = false;
  for (unsigned i = 0; i < sizeof fields / sizeof *fields; i++)
    if (!(known & (1u << i))) {
      if (comma)
        fputc(',', f);
      api_quoted(f, fields[i]);
      fputc(':', f);
      api_quoted(
        f,
        !normal             ? "BUS_METADATA_ONLY"
          : support->reason ? support->reason
          : i == 7          ? "NOT_MEASURED"
                            : "UNQUALIFIED_OBSERVATION");
      comma = true;
    }
  fputs("},\"operations\":{", f);
  for (unsigned i = 0; i < 3; i++) {
    if (i)
      fputc(',', f);
    api_quoted(f, operations[i]);
    unsigned required = support->required[i];
    bool available =
      normal && required && (support->operations & (1u << i)) && (known & required) == required;
    fprintf(f, ":{\"available\":%s,\"reason\":", available ? "true" : "false");
    api_quoted(
      f,
      available           ? NULL
        : !normal         ? "BUS_METADATA_ONLY"
        : support->reason ? support->reason
                          : "UNQUALIFIED_OBSERVATION");
    fputs(",\"verification\":[", f);
    if (available) {
      fputs("\"model\"", f);
      if (required & (MPC_NATIVE_SOLO_COPY | MPC_NATIVE_ARM_COPY))
        fputs(",\"audio_copy\"", f);
    }
    fputs(
      "],\"conditions\":[\"qualified_screen\",\"no_popup\",\"stopped_or_sequence_playback\","
      "\"recording_off\",\"automation_off_or_qualified_read\","
      "\"recognized_solo_arm_settings\"",
      f);
    if (!i)
      fputs(",\"matching_mute_scope\",\"global_or_named_active_sequence\"", f);
    fputc(']', f);
    if (available) {
      fputs(",\"automation\":", f);
      mpc_automation_json(f, 1, i != 2, i == 1, i == 0);
    }
    fputc('}', f);
  }
  fputs("},\"outputVerified\":false}", f);
}

char *api_error(const char *code, const char *message) {
  char *buffer = NULL;
  size_t size = 0;
  FILE *output = open_memstream(&buffer, &size);
  if (!output)
    return NULL;
  fputs("{\"apiVersion\":\"" API_VERSION "\",\"error\":{\"code\":", output);
  api_quoted(output, code);
  fputs(",\"message\":", output);
  api_quoted(output, message);
  fputs(",\"details\":{}}}\n", output);
  if (fclose(output)) {
    free(buffer);
    return NULL;
  }
  return buffer;
}

static char *command_reply(
  struct api *api,
  uint64_t caller,
  const struct api_request_data *request,
  struct api_delivery *delivery) {
  if (!api->backend || !api->backend->command)
    return api_error("NOT_READY", "Command authority is unavailable; consult describe");
  delivery->pending = 0;
  char *response =
    api->backend
      ->command(api->backend->userdata, api->instance_id, caller, request, &delivery->pending);
  delivery->wait_ms = request->wait_ms;
  strcpy(
    delivery->instance_id,
    !strcmp(request->op, "get_command") ? request->instance_id
                                        : request->command.target.instance_id);
  strcpy(delivery->request_id, request->command.request_id);
  return response;
}

char *api_command_poll(struct api *api, uint64_t caller, struct api_delivery *delivery) {
  struct api_request_data request = {0};
  strcpy(request.op, "get_command");
  strcpy(request.instance_id, delivery->instance_id);
  strcpy(request.command.request_id, delivery->request_id);
  request.wait_ms = delivery->wait_ms;
  return command_reply(api, caller, &request, delivery);
}

char *api_respond(
  struct api *api,
  uint64_t caller,
  const char *request,
  size_t length,
  struct api_delivery *delivery) {
  memset(delivery, 0, sizeof *delivery);
  struct api_request_data parsed;
  if (api_parse(request, length, &parsed))
    return api_error("INVALID_ARGUMENT", "Request does not match the operation schema");
  if (strcmp(parsed.version, API_VERSION))
    return api_error("UNSUPPORTED_API_VERSION", "Supported API version is " API_VERSION);
  const char *op = parsed.op;
  struct discovery discovery;
  if (discovery_read(api, &discovery))
    return api_error("NOT_READY", "Could not establish a fresh instance identity");
  api_watch_sync(api, &discovery);
  static const char *reads[] =
    {"get_state", "list_tracks", "current_track", "current_sequence", "recovery_snapshot"};
  for (size_t i = 0; i < sizeof reads / sizeof *reads; i++)
    if (!strcmp(op, reads[i])) {
      if (!discovery.state_ready)
        return api_error(
          "NOT_READY",
          "Authoritative snapshot attachment is unavailable; consult describe");
      return api_state(api, &discovery, op);
    }
  if (
    !strcmp(op, "set_mute") || !strcmp(op, "set_solo") || !strcmp(op, "set_record_armed") ||
    !strcmp(op, "set_volume") || !strcmp(op, "set_pan") || !strcmp(op, "set_send_level") ||
    !strcmp(op, "set_insert_rack_enabled") || !strcmp(op, "set_insert_enabled") ||
    !strcmp(op, "play") || !strcmp(op, "stop") || !strcmp(op, "play_start") ||
    !strcmp(op, "locate") || !strcmp(op, "select_track") || !strcmp(op, "select_sequence") ||
    !strcmp(op, "set_sequence_bpm") || mpc_command_track_edit(parsed.command.operation) ||
    !strcmp(op, "get_command")) {
    if (!discovery.commands_ready)
      return api_error("NOT_READY", "Command authority is unavailable; consult describe");
    return command_reply(api, caller, &parsed, delivery);
  }
  if (!strcmp(op, "watch"))
    return api_error("NOT_READY", "Watch requires the persistent Unix transport");
  if (strcmp(op, "describe"))
    return api_error("UNSUPPORTED_OPERATION", "Unknown operation");
  char *buffer = NULL;
  size_t size = 0;
  FILE *output = open_memstream(&buffer, &size);
  if (!output)
    return NULL;
  fputs("{\"apiVersion\":\"" API_VERSION "\",\"deviceId\":", output);
  api_quoted(output, api->device_id);
  fputs(",\"instanceId\":", output);
  api_quoted(output, api->instance_id);
  fputs(
    ",\"implementation\":{\"name\":\"mpc-native-api\",\"version\":\"" MPC_HOST_VERSION
    "\"},\"firmware\":{"
    "\"model\":",
    output);
  api_quoted(output, discovery.model[0] ? discovery.model : NULL);
  fputs(",\"modelSource\":", output);
  api_quoted(output, discovery.model[0] ? "device_tree" : NULL);
  fputs(",\"version\":", output);
  api_quoted(output, discovery.known_build ? PROFILE_VERSION : NULL);
  fputs(",\"versionSource\":", output);
  api_quoted(output, discovery.known_build ? "build_profile" : NULL);
  fputs(",\"buildId\":", output);
  api_quoted(output, discovery.build_id[0] ? discovery.build_id : NULL);
  fputs(",\"buildIdSource\":", output);
  api_quoted(output, discovery.build_id[0] ? "elf_note" : NULL);
  fprintf(
    output,
    "},\"readiness\":{\"status\":\"%s\",\"nativeAdapter\":",
    discovery.state_ready ? "ready" : "discovery_only");
  api_quoted(output, discovery.adapter_attached ? "attached" : "not_attached");
  fputs(",\"adapter\":", output);
  if (discovery.adapter_attached) {
    fprintf(output, "{\"abiVersion\":%u,\"instanceId\":", MPC_NATIVE_ABI);
    api_quoted(output, discovery.adapter_id);
    fputs(",\"codeVerified\":true,\"qualification\":\"bounded_lab_proof\"}", output);
  } else
    fputs("null", output);
  fputs(",\"mpcProcess\":", output);
  api_quoted(output, discovery.process);
  fputs(",\"firmwareProfile\":", output);
  api_quoted(output, discovery.profile);
  fputs(",\"profileId\":", output);
  api_quoted(output, !strcmp(discovery.profile, "matched") ? PROFILE_ID : NULL);
  fputs("},\"readableFields\":", output);
  fputs(
    discovery.state_ready
      ? "[\"track.uiIndex\",\"track.collection\",\"track.index\",\"track.name\",\"track.type\","
        "\"track.requested\",\"track.audioSide.soloed\",\"track."
        "audioSide.recordArmed\",\"track.silenced\",\"sequence.name\",\"sequence.used\",\"sequence."
        "bpm\",\"muteContext\",\"sequence.ref\",\"selection\",\"buses.metadata\",\"track.mixer\","
        "\"transport.playing\",\"transport.recording\",\"transport.recordMode\",\"transport."
        "pulse\",\"transport.sequenceBpm\",\"transport.masterBpm\",\"transport."
        "masterTempoEnabled\",\"track.instrument.displayName\",\"track.instrument.productId\"]"
      : "[]",
    output);
  fputs(",\"instrumentIdentity\":{\"available\":", output);
  fputs(discovery.state_ready ? "true" : "false", output);
  fputs(",\"reason\":", output);
  api_quoted(output, discovery.state_ready ? NULL : discovery.unavailable_reason);
  fputs(
    ",\"trackTypes\":[\"plugin\"],\"formats\":[\"MPC\"],"
    "\"fields\":[\"displayName\",\"productId\"],\"instanceIdentity\":false,"
    "\"presets\":false,\"parameters\":false,\"productIdScope\":\"native_format_catalog\"}",
    output);
  fputs(",\"flagFamilies\":[", output);
  if (!strcmp(discovery.profile, "matched"))
    for (unsigned i = 0; i < sizeof mpc_family_matrix / sizeof *mpc_family_matrix; i++) {
      if (i)
        fputc(',', output);
      api_flag_support(output, mpc_family_matrix[i].type, mpc_family_matrix[i].fields, 1);
    }
  fputs("],\"capabilities\":{\"describe\":{\"available\":true,\"reason\":null}", output);
  for (size_t i = 0; i < sizeof reads / sizeof *reads; i++) {
    fputc(',', output);
    api_quoted(output, reads[i]);
    fprintf(output, ":{\"available\":%s,\"reason\":", discovery.state_ready ? "true" : "false");
    api_quoted(output, discovery.state_ready ? NULL : discovery.unavailable_reason);
    fputs(
      ",\"trackTypes\":[\"plugin\",\"drum\",\"keygroup\",\"clip\",\"midi\",\"cv\",\"audio\"],"
      "\"verification\":[\"model\",\"audio_copy\"]}",
      output);
  }
  static const char *operations[] =
    {"set_mute", "set_solo", "set_record_armed", "get_command", "watch"};
  for (size_t i = 0; i < sizeof operations / sizeof *operations; i++) {
    fputc(',', output);
    api_quoted(output, operations[i]);
    int available = i == 4 ? api->watch && discovery.state_ready : discovery.commands_ready;
    fprintf(output, ":{\"available\":%s,\"reason\":", available ? "true" : "false");
    api_quoted(output, available ? NULL : discovery.unavailable_reason);
    if (i == 4)
      fprintf(
        output,
        ",\"maxSubscribers\":%u,\"replayCapacity\":%u,\"replayBytes\":%u,"
        "\"replayRetentionMs\":%u,\"sampleIntervalMs\":%u,\"samplePageTracks\":%u,"
        "\"metadataAuditMs\":%u,\"heartbeatMs\":%u,\"testedTracks\":8,"
        "\"maximumDensityQualified\":false",
        API_WATCH_SUBSCRIBERS,
        API_WATCH_REPLAY,
        API_WATCH_REPLAY_BYTES,
        API_WATCH_RETENTION_MS,
        API_WATCH_SAMPLE_MS,
        16u,
        API_WATCH_AUDIT_MS,
        API_WATCH_HEARTBEAT_MS);
    if (i <= 2) {
      fputs(",\"trackTypes\":[", output);
      bool comma = false;
      if (available)
        for (unsigned family = 0; family < sizeof mpc_family_matrix / sizeof *mpc_family_matrix;
             family++)
          if (mpc_family_matrix[family].operations & (1u << i)) {
            if (comma)
              fputc(',', output);
            api_quoted(output, mpc_family_matrix[family].type);
            comma = true;
          }
      fputs(available ? "],\"verification\":[\"model\"" : "],\"verification\":[]", output);
      if (available) {
        fputs(i ? ",\"audio_copy\"]" : "]", output);
        fputs(
          ",\"screens\":[\"main\",\"mixer\",\"track_mute\"],"
          "\"conditions\":[\"qualified_screen\",\"no_popup\",\"stopped_or_sequence_playback\","
          "\"recording_off\",\"automation_off_or_qualified_read\","
          "\"recognized_solo_arm_settings\"",
          output);
        if (!i)
          fputs(",\"matching_mute_scope\",\"global_or_named_active_sequence\"", output);
        fputc(']', output);
        fputs(",\"automation\":", output);
        mpc_automation_json(output, 1, i != 2, i == 1, i == 0);
        fputs(
          ",\"transportStates\":[\"stopped\",\"sequence_playback\"],"
          "\"excludedContexts\":{\"recording\":\"RECORDING_UNQUALIFIED\","
          "\"overdub\":\"RECORDING_UNQUALIFIED\",\"automationWriteUnknown\":\"AUTOMATION_"
          "UNQUALIFIED\","
          "\"clipSong\":\"PLAYBACK_CONTEXT_UNQUALIFIED\"},"
          "\"timingSemantics\":\"immediate_model_at_ui_dispatch\",\"sampleAccurate\":false,"
          "\"quantizedScheduling\":false",
          output);
        if (!i)
          fputs(
            ",\"muteScopes\":[\"global\",\"sequence\"],\"inactiveSequence\":\"SEQUENCE_NOT_"
            "ACTIVE\",\"muteReadCoverage\":\"active_sequence\"",
            output);
      }
    }
    fputc('}', output);
  }
  static const char *mixers[] =
    {"set_volume", "set_pan", "set_send_level", "set_insert_rack_enabled", "set_insert_enabled"};
  for (unsigned i = 0; i < 5; i++) {
    fputc(',', output);
    api_quoted(output, mixers[i]);
    fprintf(output, ":{\"available\":%s,\"reason\":", discovery.commands_ready ? "true" : "false");
    api_quoted(output, discovery.commands_ready ? NULL : discovery.unavailable_reason);
    fputs(
      ",\"trackTypes\":[\"plugin\",\"drum\",\"keygroup\",\"audio\"],"
      "\"screens\":[\"main\",\"mixer\",\"track_mute\"],\"scope\":\"global\","
      "\"verification\":[\"model\"],\"audioCompletionKnown\":false,\"busWrites\":false,"
      "\"requiresMixerGeneration\":true,\"conditions\":[\"qualified_screen\",\"no_popup\","
      "\"stopped_or_sequence_playback\",\"recording_off\",\"current_mixer_generation\",",
      output);
    api_quoted(output, i <= 1 ? "automation_off_or_qualified_read" : "automation_off");
    fputs("],\"automation\":", output);
    mpc_automation_json(output, i <= 1, i <= 1, 0, 0);
    fputs(",\"transportStates\":[\"stopped\",\"sequence_playback\"]", output);
    if (i < 3)
      fputs(
        ",\"unit\":\"normalized\",\"minimum\":0,\"maximum\":1,\"quantization\":\"ieee_binary32\","
        "\"expectedComparison\":\"binary32_exact\"",
        output);
    else
      fputs(",\"unit\":\"boolean\"", output);
    if (i == 1)
      fputs(",\"centre\":0.5", output);
    if (i == 2 || i == 4)
      fputs(",\"indexBase\":0,\"indexCount\":4", output);
    fputc('}', output);
  }
  static const char *navigation[] =
    {"play", "stop", "play_start", "locate", "select_track", "select_sequence", "set_sequence_bpm"};
  for (unsigned i = 0; i < 7; i++) {
    fputc(',', output);
    api_quoted(output, navigation[i]);
    fprintf(output, ":{\"available\":%s,\"reason\":", discovery.commands_ready ? "true" : "false");
    api_quoted(output, discovery.commands_ready ? NULL : discovery.unavailable_reason);
    fputs(
      ",\"verification\":[\"model\"],\"viewIndependent\":true,\"conditions\":[\"stable_view\","
      "\"no_popup_or_menu\",\"recording_off\",\"sequence_mode\",",
      output);
    api_quoted(output, i == 4 ? "current_track_incarnation" : "explicit_sequence_generation");
    fputs(
      ",\"local_transport_source\",\"transport_controller_ready\",\"native_transport_queue_idle\"",
      output);
    if (i == 2 || i == 3 || i >= 5)
      fputs(",\"stopped\"", output);
    if (i == 6)
      fputs(",\"used_sequence\"", output);
    fputs(
      "],\"sampleAccurate\":false,\"quantizedScheduling\":false,\"requestReplay\":\"retained_"
      "result\"",
      output);
    if (i == 5)
      fputs(",\"queuedSelection\":false", output);
    if (i == 6)
      fputs(
        ",\"tempoScope\":\"stored_sequence_bpm\",\"effectiveTempoKnown\":false,"
        "\"minimum\":20,\"maximum\":999,\"quantization\":\"ieee_binary32\"",
        output);
    fputc('}', output);
  }
  static const char *setup_ops[] = {
    "set_track_colour",
    "set_track_transpose",
    "set_track_velocity_scale",
    "set_midi_output_channel",
    "set_midi_bank_enabled",
    "set_midi_bank_msb",
    "set_midi_bank_lsb",
    "set_midi_program_enabled",
    "set_midi_program"};
  static const double setup_min[] = {0, -24, (double)0.01f, 0, 0, 0, 0, 0, 0};
  static const double setup_max[] = {16777215, 24, 2, 16, 1, 127, 127, 1, 127};
  static const char *units[] = {
    "rgb24",
    "semitones",
    "velocity_multiplier",
    "midi_channel_or_follow",
    "boolean",
    "midi_7bit",
    "midi_7bit",
    "boolean",
    "midi_7bit"};
  for (unsigned i = 0; i < MPC_TRACK_SETUP_FIELDS; i++) {
    fputc(',', output);
    api_quoted(output, setup_ops[i]);
    fprintf(output, ":{\"available\":%s,\"reason\":", discovery.commands_ready ? "true" : "false");
    api_quoted(output, discovery.commands_ready ? NULL : discovery.unavailable_reason);
    fputs(",\"families\":[", output);
    if (i < 3)
      fputs("\"plugin\",\"drum\",\"keygroup\",\"midi\",\"cv\"", output);
    else
      fputs("\"midi\"", output);
    if (i == 0)
      fputs(",\"audio\"", output);
    fprintf(
      output,
      "],\"verification\":[\"model\"],\"conditions\":[\"stopped\",\"recording_off\","
      "\"sequence_mode\",\"stable_view\",\"main_mixer_or_track_mute\",\"engine_popup_clear_or_"
      "menu\","
      "\"current_track_incarnation\",\"local_transport_source\",\"native_transport_queue_idle\","
      "\"target_automation_off_or_read\"],"
      "\"minimum\":%.17g,\"maximum\":%.17g,\"unit\":\"%s\",\"requestReplay\":\"retained_result\","
      "\"undo\":\"not_registered\",\"outputVerified\":false,\"expectedComparison\":\"%s\"}",
      setup_min[i],
      setup_max[i],
      units[i],
      i == 2 ? "binary32_exact" : "exact");
  }
  static const char *setup_unavailable[] = {
    "rename_track",
    "create_track",
    "copy_track",
    "remove_track",
    "set_midi_input_port",
    "set_midi_output_port",
    "set_midi_input_channel",
    "set_cv_port",
    "set_gate_port",
    "set_parameter"};
  for (unsigned i = 0; i < sizeof setup_unavailable / sizeof *setup_unavailable; i++) {
    fputc(',', output);
    api_quoted(output, setup_unavailable[i]);
    fputs(":{\"available\":false,\"reason\":", output);
    api_quoted(
      output,
      discovery.unavailable_reason && !discovery.commands_ready ? discovery.unavailable_reason
                                                                : "NOT_QUALIFIED");
    fputc('}', output);
  }
  fprintf(
    output,
    "},\"limits\":{\"maxRequestBytes\":%d,\"maxResponseBytes\":%d,\"maxConnections\":%d,"
    "\"connectionTimeoutMs\":%d,\"queueCapacity\":%d,\"receiptCapacity\":%d,"
    "\"requestRetentionMs\":%d,\"defaultStartWithinMs\":%s,\"maxStartWithinMs\":%s,"
    "\"maxWaitMs\":%s,\"verificationTimeoutMs\":%s,\"adapterProbeTimeoutMs\":%d,\"maxTracks\":%u,"
    "\"maxMidiTracks\":%u,\"maxAudioTracks\":%u,\"maxBuses\":%u,\"snapshotAbiVersion\":%u,"
    "\"maxJobsPerUiCallback\":%s}}\n",
    MAX_REQUEST_BYTES,
    MAX_RESPONSE_BYTES,
    MAX_CONNECTIONS,
    CONNECTION_TIMEOUT_MS,
    discovery.commands_ready ? API_COMMAND_CAPACITY : 0,
    discovery.commands_ready ? API_RECEIPT_CAPACITY : 0,
    discovery.commands_ready ? MPC_COMMAND_MIN_RETENTION_MS : 0,
    discovery.commands_ready ? "1000" : "null",
    discovery.commands_ready ? "30000" : "null",
    discovery.commands_ready ? "30000" : "null",
    discovery.commands_ready ? "500" : "null",
    ADAPTER_PROBE_TIMEOUT_MS,
    MPC_IDENTITY_MAX_TRACKS,
    MPC_IDENTITY_MAX_MIDI_TRACKS,
    MPC_IDENTITY_MAX_AUDIO_TRACKS,
    MPC_STATE_MAX_BUSES,
    MPC_STATE_ABI,
    discovery.commands_ready ? "1" : "null");
  if (fclose(output)) {
    free(buffer);
    return NULL;
  }
  return buffer;
}
