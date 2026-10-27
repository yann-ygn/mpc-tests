#ifndef MPC_MIXER_WIRE_H
#define MPC_MIXER_WIRE_H

#include "mpc_mixer.h"
#include <stdio.h>
#include <inttypes.h>

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
  fputs("],\"audioCompletionKnown\":false}", f);
}

#endif
