#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "api.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <locale.h>

struct parser {
  const char *p, *end;
};

static void whitespace(struct parser *p) {
  while (p->p < p->end && *p->p && strchr(" \t\r\n", *p->p))
    p->p++;
}

static int token(struct parser *p, char value) {
  whitespace(p);
  if (p->p == p->end || *p->p != value)
    return -1;
  p->p++;
  return 0;
}

/* Only bounded ASCII identifiers are accepted. Decode escapes before key and
 * duplicate comparisons so alternate JSON spellings have identical meaning. */
static int string(struct parser *p, char *out, size_t capacity) {
  if (token(p, '"'))
    return -1;
  size_t used = 0;
  while (p->p < p->end) {
    unsigned char c = (unsigned char)*p->p++;
    if (c == '"') {
      out[used] = 0;
      return 0;
    }
    if (c == '\\') {
      if (p->p == p->end)
        return -1;
      c = (unsigned char)*p->p++;
      if (c == 'u') {
        unsigned value = 0;
        for (int i = 0; i < 4; i++) {
          if (p->p == p->end)
            return -1;
          char h = *p->p++;
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
    out[used++] = (char)c;
  }
  return -1;
}

static int boolean(struct parser *p, bool *out) {
  whitespace(p);
  size_t remaining = (size_t)(p->end - p->p);
  if (remaining >= 4 && !memcmp(p->p, "true", 4)) {
    *out = true;
    p->p += 4;
  } else if (remaining >= 5 && !memcmp(p->p, "false", 5)) {
    *out = false;
    p->p += 5;
  } else
    return -1;
  return 0;
}

/* JSON numeric grammar, bounded conversion, and scalar type retention. The
 * operation can appear after value; boolean/number coercion is never allowed. */
static int scalar(struct parser *p, bool *boolean_out, double *number_out, bool *numeric) {
  whitespace(p);
  if (p->p < p->end && (*p->p == 't' || *p->p == 'f')) {
    *numeric = false;
    return boolean(p, boolean_out);
  }
  *numeric = true;
  const char *start = p->p;
  if (p->p < p->end && *p->p == '-')
    p->p++;
  if (p->p == p->end || *p->p < '0' || *p->p > '9')
    return -1;
  if (*p->p++ != '0')
    while (p->p < p->end && *p->p >= '0' && *p->p <= '9')
      p->p++;
  if (p->p < p->end && *p->p == '.') {
    p->p++;
    const char *digits = p->p;
    while (p->p < p->end && *p->p >= '0' && *p->p <= '9')
      p->p++;
    if (p->p == digits)
      return -1;
  }
  if (p->p < p->end && (*p->p == 'e' || *p->p == 'E')) {
    p->p++;
    if (p->p < p->end && (*p->p == '-' || *p->p == '+'))
      p->p++;
    const char *digits = p->p;
    while (p->p < p->end && *p->p >= '0' && *p->p <= '9')
      p->p++;
    if (p->p == digits)
      return -1;
  }
  size_t n = (size_t)(p->p - start);
  if (!n || n >= 128)
    return -1;
  char buffer[128], *end;
  memcpy(buffer, start, n);
  buffer[n] = 0;
  locale_t numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!numeric_locale)
    return -1;
  errno = 0;
  double value = strtod_l(buffer, &end, numeric_locale);
  int conversion_error = errno;
  freelocale(numeric_locale);
  if (*end || conversion_error || !isfinite(value))
    return -1;
  *number_out = value;
  return 0;
}

static int milliseconds(struct parser *p, unsigned *out, unsigned minimum) {
  whitespace(p);
  if (p->p == p->end || *p->p < '0' || *p->p > '9')
    return -1;
  unsigned value = 0;
  bool zero = *p->p == '0';
  do {
    value = value * 10 + (unsigned)(*p->p++ - '0');
    if (value > MPC_COMMAND_MAX_WAIT_MS)
      return -1;
    if (p->p == p->end || *p->p < '0' || *p->p > '9')
      break;
    if (zero)
      return -1;
  } while (true);
  if (value < minimum)
    return -1;
  *out = value;
  return 0;
}

static int instance(struct parser *p, char out[37]) {
  return string(p, out, 37) || strlen(out) != 36 || strncmp(out, "run_", 4) ||
      strspn(out + 4, "0123456789abcdef") != 32
    ? -1
    : 0;
}

static int identifier(struct parser *p, char out[MPC_COMMAND_ID_BYTES]) {
  return string(p, out, MPC_COMMAND_ID_BYTES) || !out[0] ||
      strspn(out, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(out)
    ? -1
    : 0;
}

static int handle(struct parser *p, uint64_t *out) {
  char value[21];
  if (string(p, value, sizeof value) || value[0] < '1' || value[0] > '9')
    return -1;
  uint64_t result = 0;
  for (const char *c = value; *c; c++) {
    if (*c < '0' || *c > '9' || result > (UINT64_MAX - (unsigned)(*c - '0')) / 10)
      return -1;
    result = result * 10 + (unsigned)(*c - '0');
  }
  *out = result;
  return 0;
}

static int target(struct parser *p, struct mpc_command_target *out) {
  if (token(p, '{'))
    return -1;
  unsigned seen = 0;
  for (unsigned i = 0; i < 3; i++) {
    char key[32];
    if ((i && token(p, ',')) || string(p, key, sizeof key) || token(p, ':'))
      return -1;
    unsigned bit;
    int error;
    if (!strcmp(key, "instanceId")) {
      bit = 1;
      error = instance(p, out->instance_id);
    } else if (!strcmp(key, "projectId")) {
      bit = 2;
      error = handle(p, &out->project);
    } else if (!strcmp(key, "trackId")) {
      bit = 4;
      error = handle(p, &out->track);
    } else
      return -1;
    if (error || (seen & bit))
      return -1;
    seen |= bit;
    whitespace(p);
    if (p->p < p->end && *p->p == '}')
      break;
  }
  return (seen & 3) != 3 || token(p, '}') ? -1 : 0;
}

static int sequence(struct parser *p, struct mpc_command_sequence *out) {
  if (token(p, '{'))
    return -1;
  unsigned seen = 0;
  for (unsigned i = 0; i < 4; i++) {
    char key[32];
    if ((i && token(p, ',')) || string(p, key, sizeof key) || token(p, ':'))
      return -1;
    unsigned bit;
    int error;
    if (!strcmp(key, "instanceId")) {
      bit = 1;
      error = instance(p, out->instance_id);
    } else if (!strcmp(key, "projectId")) {
      bit = 2;
      error = handle(p, &out->project);
    } else if (!strcmp(key, "sequenceId")) {
      bit = 4;
      error = handle(p, &out->sequence);
    } else if (!strcmp(key, "generation")) {
      bit = 8;
      error = handle(p, &out->generation);
    } else
      return -1;
    if (error || (seen & bit))
      return -1;
    seen |= bit;
  }
  return seen != 15 || token(p, '}') ? -1 : 0;
}

int api_parse(const char *input, size_t size, struct api_request_data *out) {
  memset(out, 0, sizeof *out);
  if (size > MAX_REQUEST_BYTES)
    return -1;
  struct parser p = {input, input + size};
  if (token(&p, '{'))
    return -1;
  enum {
    VERSION = 1,
    OP = 2,
    ID = 4,
    TARGET = 8,
    VALUE = 16,
    EXPECTED = 32,
    START = 64,
    WAIT = 128,
    INSTANCE = 256,
    SCOPE = 512,
    SEQUENCE = 1024,
    MIXER = 2048,
    SEND = 4096,
    SLOT = 8192,
    CURSOR = 16384,
    PULSE = 32768,
    BPM = 65536
  };
  unsigned seen = 0;
  bool value_numeric = false, expected_numeric = false;
  for (unsigned i = 0; i < 17; i++) {
    char key[32];
    if ((i && token(&p, ',')) || string(&p, key, sizeof key) || token(&p, ':'))
      return -1;
    unsigned bit;
    int error;
    if (!strcmp(key, "apiVersion")) {
      bit = VERSION;
      error = string(&p, out->version, sizeof out->version);
    } else if (!strcmp(key, "op")) {
      bit = OP;
      error = string(&p, out->op, sizeof out->op);
    } else if (!strcmp(key, "cursor")) {
      bit = CURSOR;
      char cursor[64];
      error = string(&p, cursor, sizeof cursor);
      if (!error) {
        char *colon = strchr(cursor, ':');
        error = !colon || colon - cursor != 36;
        if (!error) {
          *colon++ = 0;
          strcpy(out->cursor_instance, cursor);
          error = strncmp(cursor, "run_", 4) || strspn(cursor + 4, "0123456789abcdef") != 32 ||
            *colon < '1' || *colon > '9';
          for (; !error && *colon; colon++) {
            unsigned digit = (unsigned)(*colon - '0');
            if (digit > 9 || out->cursor_revision > (UINT64_MAX - digit) / 10)
              error = -1;
            else
              out->cursor_revision = out->cursor_revision * 10 + digit;
          }
        }
      }
    } else if (!strcmp(key, "requestId")) {
      bit = ID;
      error = identifier(&p, out->command.request_id);
    } else if (!strcmp(key, "target")) {
      bit = TARGET;
      error = target(&p, &out->command.target);
    } else if (!strcmp(key, "value")) {
      bit = VALUE;
      error = scalar(&p, &out->command.value, &out->command.mixer.value, &value_numeric);
    } else if (!strcmp(key, "expected")) {
      bit = EXPECTED;
      error = token(&p, '{') || string(&p, key, sizeof key) || strcmp(key, "value") ||
        token(&p, ':') ||
        scalar(&p, &out->command.expected, &out->command.mixer.expected, &expected_numeric) ||
        token(&p, '}');
      out->command.has_expected = true;
    } else if (!strcmp(key, "startWithinMs")) {
      bit = START;
      error = milliseconds(&p, &out->command.start_within_ms, 1);
    } else if (!strcmp(key, "waitMs")) {
      bit = WAIT;
      error = milliseconds(&p, &out->wait_ms, 0);
    } else if (!strcmp(key, "instanceId")) {
      bit = INSTANCE;
      error = instance(&p, out->instance_id);
    } else if (!strcmp(key, "muteScope")) {
      bit = SCOPE;
      char scope[16];
      error = string(&p, scope, sizeof scope);
      if (!error) {
        if (!strcmp(scope, "global"))
          out->command.mute_scope = MPC_MUTE_GLOBAL;
        else if (!strcmp(scope, "sequence"))
          out->command.mute_scope = MPC_MUTE_SEQUENCE;
        else
          error = -1;
      }
    } else if (!strcmp(key, "mixerGeneration")) {
      bit = MIXER;
      error = handle(&p, &out->command.mixer.generation);
    } else if (!strcmp(key, "sendIndex") || !strcmp(key, "slotIndex")) {
      bit = !strcmp(key, "sendIndex") ? SEND : SLOT;
      error = milliseconds(&p, &out->command.mixer.index, 0);
      if (out->command.mixer.index > 3)
        error = -1;
    } else if (!strcmp(key, "bpm")) {
      bit = BPM;
      bool ignored, numeric;
      error = scalar(&p, &ignored, &out->command.bpm, &numeric);
      if (!numeric || out->command.bpm < 20 || out->command.bpm > 999)
        error = -1;
    } else if (!strcmp(key, "pulse")) {
      bit = PULSE;
      uint64_t pulse;
      /* Absolute pulses are bounded signed integers, encoded as decimal strings. */
      char number[12];
      error = string(&p, number, sizeof number);
      pulse = 0;
      if (!error && (!number[0] || (number[0] == '0' && number[1])))
        error = -1;
      for (const char *c = number; !error && *c; c++) {
        if (*c < '0' || *c > '9' || pulse > ((uint64_t)INT32_MAX - (unsigned)(*c - '0')) / 10)
          error = -1;
        else
          pulse = pulse * 10 + (unsigned)(*c - '0');
      }
      out->command.pulse = (int64_t)pulse;
    } else if (!strcmp(key, "sequence")) {
      bit = SEQUENCE;
      error = sequence(&p, &out->command.sequence);
    } else
      return -1;
    if (error || (seen & bit))
      return -1;
    seen |= bit;
    whitespace(&p);
    if (p.p < p.end && *p.p == '}')
      break;
  }
  if (token(&p, '}'))
    return -1;
  whitespace(&p);
  if (p.p != p.end)
    return -1;
  unsigned required = VERSION | OP, allowed = required;
  if (
    !strcmp(out->op, "set_mute") || !strcmp(out->op, "set_solo") ||
    !strcmp(out->op, "set_record_armed")) {
    required |= ID | TARGET | VALUE;
    allowed = required | EXPECTED | START | WAIT;
    out->command.operation = !strcmp(out->op, "set_mute") ? MPC_COMMAND_MUTE
      : !strcmp(out->op, "set_solo")                      ? MPC_COMMAND_SOLO
                                                          : MPC_COMMAND_RECORD_ARMED;
    if (out->command.operation == MPC_COMMAND_MUTE) {
      allowed |= SCOPE;
      if (out->command.mute_scope == MPC_MUTE_SEQUENCE) {
        required |= SEQUENCE;
        allowed |= SEQUENCE;
        if (
          strcmp(out->command.sequence.instance_id, out->command.target.instance_id) ||
          out->command.sequence.project != out->command.target.project)
          return -1;
      }
    }
  } else if (
    !strcmp(out->op, "set_volume") || !strcmp(out->op, "set_pan") ||
    !strcmp(out->op, "set_send_level") || !strcmp(out->op, "set_insert_rack_enabled") ||
    !strcmp(out->op, "set_insert_enabled")) {
    required |= ID | TARGET | VALUE | MIXER;
    allowed = required | EXPECTED | START | WAIT;
    out->command.operation = !strcmp(out->op, "set_volume") ? MPC_COMMAND_VOLUME
      : !strcmp(out->op, "set_pan")                         ? MPC_COMMAND_PAN
      : !strcmp(out->op, "set_send_level")                  ? MPC_COMMAND_SEND
      : !strcmp(out->op, "set_insert_rack_enabled")         ? MPC_COMMAND_RACK
                                                            : MPC_COMMAND_INSERT;
    if (out->command.operation == MPC_COMMAND_SEND) {
      required |= SEND;
      allowed |= SEND;
    }
    if (out->command.operation == MPC_COMMAND_INSERT) {
      required |= SLOT;
      allowed |= SLOT;
    }
  } else if (
    !strcmp(out->op, "play") || !strcmp(out->op, "stop") || !strcmp(out->op, "play_start") ||
    !strcmp(out->op, "locate") || !strcmp(out->op, "select_track") ||
    !strcmp(out->op, "select_sequence") || !strcmp(out->op, "set_sequence_bpm")) {
    required |= ID | TARGET;
    out->command.operation = !strcmp(out->op, "play") ? MPC_COMMAND_PLAY
      : !strcmp(out->op, "stop")                      ? MPC_COMMAND_STOP
      : !strcmp(out->op, "play_start")                ? MPC_COMMAND_PLAY_START
      : !strcmp(out->op, "locate")                    ? MPC_COMMAND_LOCATE
      : !strcmp(out->op, "select_track")              ? MPC_COMMAND_SELECT_TRACK
      : !strcmp(out->op, "select_sequence")           ? MPC_COMMAND_SELECT_SEQUENCE
                                                      : MPC_COMMAND_SEQUENCE_BPM;
    if (out->command.operation == MPC_COMMAND_SELECT_TRACK) {
      if (!out->command.target.track)
        return -1;
    } else {
      required |= SEQUENCE;
      if (
        out->command.target.track ||
        strcmp(out->command.sequence.instance_id, out->command.target.instance_id) ||
        out->command.sequence.project != out->command.target.project)
        return -1;
    }
    if (out->command.operation == MPC_COMMAND_LOCATE)
      required |= PULSE;
    if (out->command.operation == MPC_COMMAND_SEQUENCE_BPM)
      required |= BPM;
    allowed = required | START | WAIT;
  } else if (
    !strcmp(out->op, "set_track_colour") || !strcmp(out->op, "set_track_transpose") ||
    !strcmp(out->op, "set_track_velocity_scale") || !strcmp(out->op, "set_midi_output_channel") ||
    !strcmp(out->op, "set_midi_bank_enabled") || !strcmp(out->op, "set_midi_bank_msb") ||
    !strcmp(out->op, "set_midi_bank_lsb") || !strcmp(out->op, "set_midi_program_enabled") ||
    !strcmp(out->op, "set_midi_program")) {
    static const char *names[] = {
      "set_track_colour",
      "set_track_transpose",
      "set_track_velocity_scale",
      "set_midi_output_channel",
      "set_midi_bank_enabled",
      "set_midi_bank_msb",
      "set_midi_bank_lsb",
      "set_midi_program_enabled",
      "set_midi_program"};
    unsigned field;
    for (field = 0; field < MPC_TRACK_SETUP_FIELDS; field++)
      if (!strcmp(out->op, names[field]))
        break;
    out->command.operation = MPC_COMMAND_TRACK_COLOUR + field;
    bool boolean_field = mpc_track_setup_boolean(field);
    if (
      !out->command.target.track || value_numeric == boolean_field ||
      ((seen & EXPECTED) && expected_numeric == boolean_field))
      return -1;
    out->command.edit.value = boolean_field ? out->command.value : out->command.mixer.value;
    out->command.edit.expected =
      boolean_field ? out->command.expected : out->command.mixer.expected;
    out->command.value = out->command.expected = false;
    memset(&out->command.mixer, 0, sizeof out->command.mixer);
    if (
      !mpc_track_setup_scalar_valid(field, out->command.edit.value) ||
      ((seen & EXPECTED) && !mpc_track_setup_scalar_valid(field, out->command.edit.expected)))
      return -1;
    required |= ID | TARGET | VALUE;
    allowed = required | EXPECTED | START | WAIT;
  } else if (!strcmp(out->op, "watch")) {
    allowed |= CURSOR;
  } else if (!strcmp(out->op, "get_command")) {
    required |= ID | INSTANCE;
    allowed = required | WAIT;
  }
  if (out->command.operation < MPC_COMMAND_PLAY && (seen & TARGET) && !out->command.target.track)
    return -1;
  bool numeric =
    out->command.operation >= MPC_COMMAND_VOLUME && out->command.operation <= MPC_COMMAND_SEND;
  if (
    !mpc_command_track_edit(out->command.operation) && (seen & VALUE) &&
    (numeric != value_numeric ||
     (numeric && (out->command.mixer.value < 0 || out->command.mixer.value > 1))))
    return -1;
  if (
    !mpc_command_track_edit(out->command.operation) && (seen & EXPECTED) &&
    (numeric != expected_numeric ||
     (numeric && (out->command.mixer.expected < 0 || out->command.mixer.expected > 1))))
    return -1;
  return (seen & required) != required || (seen & ~allowed) ? -1 : 0;
}
