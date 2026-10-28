#ifndef MPC_MIXER_WIRE_H
#define MPC_MIXER_WIRE_H

#include "mpc_mixer.h"
#include <stdio.h>
#include <inttypes.h>

static inline void
mpc_automation_json(FILE *f, int read, int sources, int peers, int sequence_mute) {
  fprintf(
    f,
    "{\"off\":\"qualified_writes\",\"read\":\"%s\",\"write\":\"unsupported\","
    "\"unknown\":\"unsupported\",\"readBehavior\":\"%s\",\"readConditions\":[",
    read ? "qualified_writes" : "unsupported",
    !read       ? "unsupported"
      : sources ? "manual_change_until_native_update"
                : "native_record_arm");
  if (read && sources)
    fputs("\"qualified_native_sources\",\"stable_active_sequence\"", f);
  if (read && sequence_mute)
    fputs(",\"sequence_mute_active_clip\"", f);
  if (read && peers)
    fputs(",\"solo_peers_off_or_read\"", f);
  fputs("],\"timelineEdits\":false}", f);
}

static inline void mpc_mixer_scalar_json(FILE *f, int numeric, double value) {
  if (numeric)
    fprintf(f, "%.17g", value);
  else
    fputs(value ? "true" : "false", f);
}

static inline void mpc_mixer_json(FILE *f, const struct mpc_mixer_state *s) {
  fputs("{\"generation\":", f);
  if (s->generation)
    fprintf(f, "\"%" PRIu64 "\"", s->generation);
  else
    fputs("null", f);
  fprintf(f, ",\"known\":%u,\"scope\":\"global\",\"unit\":\"normalized\",\"volume\":", s->known);
  if (s->known & MPC_MIXER_VOLUME)
    fprintf(f, "%.9g", (double)s->volume);
  else
    fputs("null", f);
  fputs(",\"pan\":", f);
  if (s->known & MPC_MIXER_PAN)
    fprintf(f, "%.9g", (double)s->pan);
  else
    fputs("null", f);
  fputs(",\"sends\":[", f);
  for (unsigned i = 0; i < 4; i++) {
    fprintf(
      f,
      "%s{\"index\":%u,\"destination\":\"return_%c\",\"value\":",
      i ? "," : "",
      i,
      'A' + i);
    if (s->known & (MPC_MIXER_SEND_0 << i))
      fprintf(f, "%.9g", (double)s->sends[i]);
    else
      fputs("null", f);
    fputc('}', f);
  }
  fputs("],\"insertRackEnabled\":", f);
  if (s->known & MPC_MIXER_RACK)
    fputs(s->rack_enabled ? "true" : "false", f);
  else
    fputs("null", f);
  fputs(",\"inserts\":[", f);
  for (unsigned i = 0; i < 4; i++) {
    fprintf(f, "%s{\"index\":%u,\"occupied\":", i ? "," : "", i);
    if (s->generation)
      fputs((s->occupied & (1u << i)) ? "true" : "false", f);
    else
      fputs("null", f);
    fputs(",\"enabled\":", f);
    if (s->known & (MPC_MIXER_INSERT_0 << i))
      fputs((s->inserts_enabled & (1u << i)) ? "true" : "false", f);
    else
      fputs("null", f);
    fputc('}', f);
  }
  fputs("],\"verification\":[", f);
  if (s->known)
    fputs("\"model\"", f);
  fputs("],\"audioCompletionKnown\":false,\"operations\":{", f);
  static const char *ops[] =
    {"set_volume", "set_pan", "set_send_level", "set_insert_rack_enabled", "set_insert_enabled"};
  static const unsigned fields[] =
    {MPC_MIXER_VOLUME, MPC_MIXER_PAN, MPC_MIXER_SEND_0, MPC_MIXER_RACK, MPC_MIXER_INSERT_0};
  for (unsigned i = 0; i < 5; i++) {
    unsigned mask = i == 4 ? (MPC_MIXER_INSERT_0 | (MPC_MIXER_INSERT_0 << 1) |
                              (MPC_MIXER_INSERT_0 << 2) | (MPC_MIXER_INSERT_0 << 3))
                           : fields[i];
    fprintf(
      f,
      "%s\"%s\":{\"available\":%s,\"automation\":",
      i ? "," : "",
      ops[i],
      (s->known & mask) ? "true" : "false");
    mpc_automation_json(f, i <= 1 && (s->known & mask), i <= 1, 0, 0);
    fputc('}', f);
  }
  fputs("}}", f);
}

#endif
