#ifndef MPC_COMMANDS_H
#define MPC_COMMANDS_H

#include "mpc_native.h"
#include "mpc_identity.h"
#include "mpc_state.h"
#include "mpc_observation.h"

#define MPC_COMMANDS_ABI 6u
#define MPC_COMMAND_ID_BYTES 81u
#define MPC_COMMAND_MIN_RETENTION_MS 600000u
#define MPC_COMMAND_MAX_RECEIPTS 256u
#define MPC_COMMAND_MAX_PENDING 128u
#define MPC_COMMAND_MAX_START_MS 30000u
#define MPC_COMMAND_MAX_WAIT_MS 30000u
#define MPC_COMMAND_DEFAULT_START_MS 1000u
#define MPC_COMMAND_VERIFICATION_MS 500u

struct mpc_executor;
struct mpc_commands;

enum mpc_command_operation { MPC_COMMAND_MUTE, MPC_COMMAND_SOLO, MPC_COMMAND_RECORD_ARMED };
enum mpc_command_status {
  MPC_COMMAND_ACCEPTED,
  MPC_COMMAND_APPLIED,
  MPC_COMMAND_REJECTED,
  MPC_COMMAND_CANCELLED,
  MPC_COMMAND_UNKNOWN
};
enum mpc_command_execution { MPC_COMMAND_NOT_STARTED, MPC_COMMAND_STARTED, MPC_COMMAND_COMPLETED };

struct mpc_command_target {
  char instance_id[37];
  uint64_t project, track;
};
struct mpc_command_sequence {
  char instance_id[37];
  uint64_t project, sequence, generation;
};
struct mpc_command_request {
  char request_id[MPC_COMMAND_ID_BYTES];
  struct mpc_command_target target;
  enum mpc_command_operation operation;
  /* Zero-initialized requests retain global semantics. Sequence scope is mute
   * only and requires a complete, same-instance/project reference. */
  enum mpc_mute_scope mute_scope;
  struct mpc_command_sequence sequence;
  bool value, has_expected, expected;
  /* Observation revisions cannot be transaction preconditions. Reject before
   * acceptance rather than silently ignoring a caller's requested safeguard. */
  bool has_expected_revision;
  unsigned start_within_ms;
};

/* A trusted host must check LIVE mode/settings on the UI thread at dispatch and
 * each verification. No callback means NOT_READY, including for no-op commands.
 * It must reject recording/overdub, automation Write/unknown, Clip/Song and
 * unqualified playback combinations. Ordinary sequence playback is qualified.
 * Mute must match the request's scope and named active sequence. The production
 * callback reads this context from the active executor request.
 * Callback userdata lives through successful commands_close. This callback is
 * host configuration, never an untrusted request field. */
struct mpc_command_policy {
  void *userdata;
  enum mpc_native_error (*qualify)(void *, enum mpc_command_operation);
};

/* Production live policy for the exact profiled firmware. UI-thread only,
 * called from qualify while a managed command is active on this executor.
 * Checks qualified Main/Mixer/Track Mute, no popup, sequence transport, recording Off, settings and
 * target automation. Read qualifies verified no-ops for all flags and native
 * record-arm writes; Read mute/solo writes remain unqualified. Samples a
 * command-local context fence, including at native invocation boundaries; never
 * invokes a setter or accepts saved settings. */
MPC_NATIVE_API enum mpc_native_error
mpc_executor_qualify_command(struct mpc_executor *, enum mpc_command_operation);

struct mpc_commands_config {
  uint32_t abi_version;
  size_t struct_bytes;
  struct mpc_executor *executor; /* Borrowed authoritative executor. Close handlers first. */
  char instance_id[37]; /* Transport authority's run_<32 hex> ID. Never reuse after restart. */
  unsigned pending_capacity, receipt_capacity, retention_ms;
  struct mpc_command_policy policy;
};

struct mpc_command_receipt {
  uint32_t abi_version, struct_bytes;
  struct mpc_command_request request;
  enum mpc_command_status status;
  enum mpc_command_execution execution;
  enum mpc_native_error error;
  bool native_known, native_invoked, changed_known, changed;
  bool before_known, after_known;
  struct mpc_command_observation before, after;
  /* Monotonic milliseconds, private to the authority's lifetime. Wire timings
   * are durations, never wall-clock or audio/sample scheduling timestamps.
   * Zero means the corresponding stage has not been observed. */
  uint64_t accepted_ms, dispatched_ms, dispatch_finished_ms, finished_ms;
  uint64_t ui_work_us, max_ui_callback_us, verification_ui_us;
  /* Exclusive UI stages: identity, policy, filter/setter, target readback,
   * before observation, after observation and other executor work. */
  uint64_t stages_us[7];
  uint32_t verification_passes;
};

/* All calls are for non-realtime, non-UI threads. Caller is the authenticated
 * transport identity, not a request parameter. Same caller/request ID/canonical
 * command is deduplicated; wait_ms only waits and never cancels/renews a command.
 * Missing start budget (zero) means DEFAULT_START_MS. Results remain retained
 * for at least ten minutes from acceptance, and pending commands never expire
 * from retention. Pressure rejects new work; it cannot evict an unexpired result. */
MPC_NATIVE_API enum mpc_native_error
mpc_commands_open(const struct mpc_commands_config *, struct mpc_commands **);
MPC_NATIVE_API enum mpc_native_error mpc_commands_submit(
  struct mpc_commands *,
  uint64_t caller,
  const struct mpc_command_request *,
  unsigned wait_ms,
  struct mpc_command_receipt *);
MPC_NATIVE_API enum mpc_native_error mpc_commands_get(
  struct mpc_commands *,
  uint64_t caller,
  const char *instance_id,
  const char *request_id,
  unsigned wait_ms,
  struct mpc_command_receipt *);
/* Stop acceptance and cancel work that has not begun. Running callbacks keep
 * their receipt/job ownership until drained; timeout never makes close safe.
 * The host must stop/join API callers before close, as for executor close. */
MPC_NATIVE_API enum mpc_native_error mpc_commands_shutdown(struct mpc_commands *, unsigned);
MPC_NATIVE_API enum mpc_native_error mpc_commands_close(struct mpc_commands *);
/* Caller frees the returned compact JSON response. No native addresses. */
MPC_NATIVE_API char *mpc_commands_receipt_json(const struct mpc_command_receipt *);

#endif
