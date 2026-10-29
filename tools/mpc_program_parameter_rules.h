#ifndef MPC_PROGRAM_PARAMETER_RULES_H
#define MPC_PROGRAM_PARAMETER_RULES_H

#include <float.h>
#include <math.h>
#include <stdbool.h>

/* Tuneable's normalized getter uses fused binary32 (native / 72 + 0.5).
 * At -36 semitones its rounded coefficient produces -3.7252903e-9. Only
 * this qualified integer parameter canonicalizes that endpoint overshoot.
 * Desired/expected request inputs must still be finite and inside [0, 1]. */
static inline bool mpc_parameter_canonical_value(unsigned id, float observed, float *out) {
  if (!isfinite(observed))
    return false;
  if (id == 1043 && observed >= -FLT_EPSILON && observed <= 1.0f + FLT_EPSILON) {
    if (observed < 0)
      observed = 0;
    if (observed > 1)
      observed = 1;
  }
  if (observed < 0 || observed > 1)
    return false;
  *out = observed;
  return true;
}

#endif
