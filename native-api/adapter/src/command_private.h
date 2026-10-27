#ifndef MPC_COMMAND_PRIVATE_H
#define MPC_COMMAND_PRIVATE_H

#include "mpc_commands.h"
#include "mpc_executor.h"
#include "private.h"

struct mpc_identity_registry;

struct mpc_command_native_result {
  uint64_t dispatched_ms, finished_ms, ui_work_us;
  struct mpc_ui_profile profile;
  bool before_known, after_known;
  bool context_known;
  bool last_readback_known, readbacks_consistent;
  struct mpc_native_track_state last_readback;
  struct mpc_command_context context;
  struct mpc_command_observation before, after;
};

enum mpc_native_error mpc_identity_flags(
  struct mpc_identity_registry *,
  unsigned offset,
  unsigned limit,
  uint64_t expected_topology,
  struct mpc_command_observation *);

enum mpc_native_error mpc_executor_command_submit(
  struct mpc_executor *,
  const struct mpc_executor_request *,
  const struct mpc_command_context *,
  struct mpc_executor_job **);

enum mpc_native_error
mpc_executor_commands_attach(struct mpc_executor *, void *owner, const struct mpc_command_policy *);
void mpc_executor_commands_detach(struct mpc_executor *, void *owner);
bool mpc_executor_on_ui(struct mpc_executor *);
enum mpc_native_error
mpc_identity_bind(struct mpc_identity_registry *, struct mpc_executor_identity *);

enum mpc_native_error mpc_commands_open_clock(
  const struct mpc_commands_config *, uint64_t (*)(void *), void *, struct mpc_commands **);

#endif
