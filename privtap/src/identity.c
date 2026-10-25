#include "identity.h"

#include <stddef.h>
#include <strings.h>

// The models a device can pass for (tkgl_mpcmapper's table). Only X has been tried.
static const struct identity_profile profiles[] = {
  {"X", "ACV5", "inmusic,acv5", 0x3A, {0x3A, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01}},
  {"LIVE", "ACV8", "inmusic,acv8", 0x3B, {0x3B, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01}},
  {"FORCE", "ADA2", "inmusic,ada2", 0x40, {0x40, 0x00, 0x19, 0x00, 0x00, 0x04, 0x03}},
  {"ONE", "ACVA", "inmusic,acva", 0x46, {0x46, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01}},
  {"LIVE2", "ACVB", "inmusic,acvb", 0x47, {0x47, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01}},
};

const struct identity_profile *identity_lookup(const char *name) {
  for (size_t i = 0; i < sizeof profiles / sizeof *profiles; i++) {
    if (!strcasecmp(name, profiles[i].name)) {
      return &profiles[i];
    }
  }
  return NULL;
}

unsigned char identity_rewrite_byte(
  const struct identity_profile *profile,
  struct identity_stream *stream,
  int *hardware_family,
  unsigned char byte,
  bool incoming) {
  if (byte >= 0xF8) { // Real-time bytes do not advance SysEx position.
    return byte;
  }
  if (byte == 0xF0) {
    stream->position = 1;
    stream->header[0] = byte;
    stream->identity_reply = false;
    return byte;
  }
  if (byte & 0x80) {
    stream->position = -1;
    return byte;
  }
  if (stream->position < 0) {
    return byte;
  }
  int position = stream->position++;
  if (position < (int)sizeof stream->header) {
    stream->header[position] = byte;
  }
  const unsigned char *header = stream->header;
  if (incoming) {
    if (
      position == 6 && header[1] == 0x7E && header[3] == 0x06 && header[4] == 0x02 &&
      header[5] == 0x47) {
      if (*hardware_family < 0) {
        *hardware_family = byte;
      }
      stream->identity_reply = byte == *hardware_family;
    }
    if (stream->identity_reply && position >= 6 && position <= 12) {
      return profile->reply[position - 6];
    }
    if (position == 3 && header[1] == 0x47 && byte == *hardware_family) {
      return profile->family;
    }
  } else if (
    position == 3 && header[1] == 0x47 && byte == profile->family && *hardware_family >= 0) {
    return (unsigned char)*hardware_family;
  }
  return byte;
}
