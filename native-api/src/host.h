#ifndef MPC_API_HOST_H
#define MPC_API_HOST_H

#include "mpc_executor.h"
#include "mpc_commands.h"

#define MPC_HOST_DIRECTORY "/tmp/mpc-native-api"
#define MPC_HOST_VERSION "0.14.0"

/* Internal dependency boundary, also used by the isolated lifecycle fixture. */
struct mpc_host_ops {
  void *userdata;
  const char *discovery_root;
  enum mpc_native_error (*open)(void *, struct mpc_executor **);
  void (*cancel_start)(void *);
  int (*exiting)(void *);
  struct mpc_command_policy command_policy;
  bool native_command_policy;
  /* Optional off-UI diagnostics used by the temporary qualification host. */
  const char *thread_name;
  void (*observed)(void *, bool full, enum mpc_native_error, const struct mpc_executor_result *);
};

int mpc_host_run(const char *installation, const char *runtime, const struct mpc_host_ops *);
int mpc_host_directory(const char *runtime);
/* The shared lock inode is never unlinked: hosts and cleanup must serialize on
 * the same inode, including across MPC restarts and deferred uninstallation. */
int mpc_host_lock(int directory);
int mpc_host_remove_stale(int directory, const char *name);
int mpc_host_cleanup(const char *runtime);

#endif
