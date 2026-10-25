#include "api.h"
#include "profile.h"

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

struct parser {
  const char *p, *end;
};

static void whitespace(struct parser *parser) {
  while (parser->p < parser->end && strchr(" \t\r\n", *parser->p) && *parser->p)
    parser->p++;
}

static int token(struct parser *parser, char expected) {
  whitespace(parser);
  if (parser->p == parser->end || *parser->p != expected)
    return -1;
  parser->p++;
  return 0;
}

/* This endpoint accepts a flat object of ASCII identifier strings, not general RPC arguments. */
static int string(struct parser *parser, char *output, size_t capacity) {
  if (token(parser, '"'))
    return -1;
  size_t used = 0;
  while (parser->p < parser->end) {
    unsigned char c = (unsigned char)*parser->p++;
    if (c == '"') {
      output[used] = 0;
      return 0;
    }
    if (c == '\\') {
      if (parser->p == parser->end)
        return -1;
      c = (unsigned char)*parser->p++;
      if (c == 'u') {
        unsigned value = 0;
        for (int i = 0; i < 4; i++) {
          if (parser->p == parser->end)
            return -1;
          char h = *parser->p++;
          unsigned digit = h >= '0' && h <= '9' ? (unsigned)(h - '0')
            : h >= 'a' && h <= 'f'              ? (unsigned)(h - 'a' + 10)
            : h >= 'A' && h <= 'F'              ? (unsigned)(h - 'A' + 10)
                                                : 16;
          if (digit == 16)
            return -1;
          value = value * 16 + digit;
        }
        if (value < 32 || value > 126)
          return -1;
        c = (unsigned char)value;
      } else if (c != '"' && c != '\\' && c != '/')
        return -1;
    }
    if (c < 32 || c > 126 || used + 1 >= capacity)
      return -1;
    output[used++] = (char)c;
  }
  return -1;
}

static int request_parse(const char *input, size_t size, char version[80], char op[80]) {
  struct parser parser = {input, input + size};
  if (token(&parser, '{'))
    return -1;
  int seen_version = 0, seen_op = 0;
  for (int i = 0; i < 2; i++) {
    char key[80], value[80];
    if (
      (i && token(&parser, ',')) || string(&parser, key, sizeof key) || token(&parser, ':') ||
      string(&parser, value, sizeof value))
      return -1;
    if (!strcmp(key, "apiVersion") && !seen_version) {
      strcpy(version, value);
      seen_version = 1;
    } else if (!strcmp(key, "op") && !seen_op) {
      strcpy(op, value);
      seen_op = 1;
    } else
      return -1;
  }
  if (token(&parser, '}'))
    return -1;
  whitespace(&parser);
  return !seen_version || !seen_op || parser.p != parser.end ? -1 : 0;
}

char *api_respond(struct api *api, const char *request, size_t length) {
  char version[80], op[80];
  if (length > MAX_REQUEST_BYTES || request_parse(request, length, version, op))
    return api_error(
      "INVALID_ARGUMENT",
      "Expected apiVersion and op strings, with no duplicate or extra fields");
  if (strcmp(version, API_VERSION))
    return api_error("UNSUPPORTED_API_VERSION", "Supported API version is " API_VERSION);
  struct discovery discovery;
  if (discovery_read(api, &discovery))
    return api_error("NOT_READY", "Could not establish a fresh instance identity");
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
  if (strcmp(op, "describe")) {
    static const char *reserved[] = {
      "get_state",
      "current_track",
      "set_mute",
      "set_solo",
      "set_record_armed",
      "get_command",
      "watch"};
    for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++)
      if (!strcmp(op, reserved[i]))
        return api_error("NOT_READY", "This operation is not implemented; consult describe");
    return api_error("UNSUPPORTED_OPERATION", "Unknown operation");
  }
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
    ",\"implementation\":{\"name\":\"mpc-native-api\",\"version\":\"0.1.0-dev\"},\"firmware\":{"
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
    fputs("{\"abiVersion\":1,\"instanceId\":", output);
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
      ? "[\"track.name\",\"track.type\",\"track.requested\",\"track.audioSide.soloed\",\"track."
        "audioSide.recordArmed\",\"track.silenced\",\"sequence.name\",\"sequence.used\",\"sequence."
        "bpm\",\"selection\",\"buses.metadata\"]"
      : "[]",
    output);
  fputs(",\"capabilities\":{\"describe\":{\"available\":true,\"reason\":null}", output);
  for (size_t i = 0; i < sizeof reads / sizeof *reads; i++) {
    fputc(',', output);
    api_quoted(output, reads[i]);
    fprintf(output, ":{\"available\":%s,\"reason\":", discovery.state_ready ? "true" : "false");
    api_quoted(output, discovery.state_ready ? NULL : discovery.unavailable_reason);
    fputs(
      ",\"trackTypes\":[\"plugin\",\"drum\"],\"verification\":[\"model\",\"audio_copy\"]}",
      output);
  }
  static const char *operations[] =
    {"set_mute", "set_solo", "set_record_armed", "get_command", "watch"};
  for (size_t i = 0; i < sizeof operations / sizeof *operations; i++) {
    fputc(',', output);
    api_quoted(output, operations[i]);
    fputs(":{\"available\":false,\"reason\":", output);
    api_quoted(output, discovery.unavailable_reason);
    if (i <= 2)
      fputs(",\"trackTypes\":[],\"verification\":[]", output);
    fputc('}', output);
  }
  fprintf(
    output,
    "},\"limits\":{\"maxRequestBytes\":%d,\"maxResponseBytes\":%d,\"maxConnections\":%d,"
    "\"connectionTimeoutMs\":%d,"
    "\"queueCapacity\":0,\"requestRetentionMs\":0,\"defaultStartWithinMs\":null,"
    "\"maxStartWithinMs\":null,"
    "\"verificationTimeoutMs\":null,\"adapterProbeTimeoutMs\":%d}}\n",
    MAX_REQUEST_BYTES,
    MAX_RESPONSE_BYTES,
    MAX_CONNECTIONS,
    CONNECTION_TIMEOUT_MS,
    ADAPTER_PROBE_TIMEOUT_MS);
  if (fclose(output)) {
    free(buffer);
    return NULL;
  }
  return buffer;
}
