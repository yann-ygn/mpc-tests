#include "../mpc_program_parameter_rules.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

int main(void) {
  float value = 99;
  assert(mpc_parameter_canonical_value(1043, -3.7252903e-9f, &value) && value == 0);
  assert(mpc_parameter_canonical_value(1043, 1 + FLT_EPSILON, &value) && value == 1);
  assert(mpc_parameter_canonical_value(1043, 0.25f, &value) && value == 0.25f);
  assert(mpc_parameter_canonical_value(514, 0, &value) && value == 0);
  assert(mpc_parameter_canonical_value(514, 1, &value) && value == 1);
  assert(!mpc_parameter_canonical_value(514, -3.7252903e-9f, &value));
  assert(!mpc_parameter_canonical_value(1043, -2 * FLT_EPSILON, &value));
  assert(!mpc_parameter_canonical_value(1043, 1 + 2 * FLT_EPSILON, &value));
  assert(!mpc_parameter_canonical_value(1043, NAN, &value));
  assert(!mpc_parameter_canonical_value(1043, INFINITY, &value));
  puts("parameter binary32 endpoints: passed");
  return 0;
}
