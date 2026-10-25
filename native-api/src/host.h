#ifndef MPC_API_HOST_H
#define MPC_API_HOST_H

#include "mpc_executor.h"

#define MPC_HOST_DIRECTORY "/tmp/mpc-native-api"
#define MPC_HOST_VERSION "0.1.0"

/* Internal dependency boundary, also used by the isolated lifecycle fixture. */
struct mpc_host_ops {
  void *userdata;
  const char *discovery_root;
  enum mpc_native_error (*open)(void *, struct mpc_executor **);
  void (*cancel_start)(void *);
  int (*exiting)(void *);
};

int mpc_host_run(const char *installation, const char *runtime, const struct mpc_host_ops *);

#endif
