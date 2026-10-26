#include "api.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
  }
  return seen != 7 || token(p, '}') ? -1 : 0;
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
    SEQUENCE = 1024
  };
  unsigned seen = 0;
  for (unsigned i = 0; i < 11; i++) {
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
    } else if (!strcmp(key, "requestId")) {
      bit = ID;
      error = identifier(&p, out->command.request_id);
    } else if (!strcmp(key, "target")) {
      bit = TARGET;
      error = target(&p, &out->command.target);
    } else if (!strcmp(key, "value")) {
      bit = VALUE;
      error = boolean(&p, &out->command.value);
    } else if (!strcmp(key, "expected")) {
      bit = EXPECTED;
      error = token(&p, '{') || string(&p, key, sizeof key) || strcmp(key, "value") ||
        token(&p, ':') || boolean(&p, &out->command.expected) || token(&p, '}');
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
  } else if (!strcmp(out->op, "get_command")) {
    required |= ID | INSTANCE;
    allowed = required | WAIT;
  }
  return (seen & required) != required || (seen & ~allowed) ? -1 : 0;
}
