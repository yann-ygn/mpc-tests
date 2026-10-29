#ifndef MPC_PLUGIN_IDENTITY_H
#define MPC_PLUGIN_IDENTITY_H

#include <stdint.h>

#define MPC_PLUGIN_TEXT_BYTES 256u
enum mpc_plugin_status {
  MPC_PLUGIN_NOT_APPLICABLE,
  MPC_PLUGIN_UNASSIGNED,
  MPC_PLUGIN_AVAILABLE,
  MPC_PLUGIN_UNAVAILABLE,
  MPC_PLUGIN_UNSUPPORTED
};

/* Product metadata, never a loaded-instance or persistent Track identifier. */
struct mpc_plugin_identity {
  uint32_t status;
  char display_name[MPC_PLUGIN_TEXT_BYTES];
  char product_id[MPC_PLUGIN_TEXT_BYTES];
};

#endif
