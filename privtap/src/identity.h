#ifndef PRIVTAP_IDENTITY_H
#define PRIVTAP_IDENTITY_H

#include <stdbool.h>

struct identity_profile {
  const char *name, *product_code, *compatible;
  unsigned char family, reply[7];
};

struct identity_stream {
  int position; // Next byte after F0; -1 outside SysEx.
  unsigned char header[8];
  bool identity_reply;
};

const struct identity_profile *identity_lookup(const char *name);

/* The caller synchronizes both the stream and the shared hardware family. */
unsigned char identity_rewrite_byte(
  const struct identity_profile *profile,
  struct identity_stream *stream,
  int *hardware_family,
  unsigned char byte,
  bool incoming);

#endif
