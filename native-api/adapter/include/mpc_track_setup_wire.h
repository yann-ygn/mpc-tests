#ifndef MPC_TRACK_SETUP_WIRE_H
#define MPC_TRACK_SETUP_WIRE_H

#include "mpc_track_setup.h"
#include <stdio.h>

static const char *const mpc_track_setup_names[MPC_TRACK_SETUP_FIELDS] = {
  "colour",
  "transposeSemitones",
  "velocityScale",
  "midiOutputChannel",
  "midiBankEnabled",
  "midiBankMsb",
  "midiBankLsb",
  "midiProgramEnabled",
  "midiProgram"};

static inline void mpc_track_setup_json(FILE *f, const struct mpc_track_setup *s) {
  fputc('{', f);
  for (unsigned i = 0; i < MPC_TRACK_SETUP_FIELDS; i++) {
    fprintf(f, "%s\"%s\":", i ? "," : "", mpc_track_setup_names[i]);
    if (!(s->known & (1u << i)))
      fputs("null", f);
    else if (mpc_track_setup_boolean(i))
      fputs(s->values[i] ? "true" : "false", f);
    else
      fprintf(f, "%.17g", s->values[i]);
  }
  fputc('}', f);
}

#endif
