#ifndef MPC_EXECUTOR_H
#define MPC_EXECUTOR_H

#include "mpc_native.h"
#include "mpc_identity.h"
#include "mpc_state.h"

/* Trusted in-process interface, separate from the public command protocol. All
 * client calls are for non-realtime threads. Only the resolver runs on the UI
 * thread. The executor owns its adapter, scheduler and copied requests. */
#define MPC_EXECUTOR_ABI 6u
#define MPC_EXECUTOR_MAX_JOBS 128u
#define MPC_EXECUTOR_MAX_WAIT_MS 30000u

struct mpc_executor;
struct mpc_executor_job;

/* Opaque identities, never native addresses. The host's resolver must check the
 * active project and both generations on every invocation (#20). */
struct mpc_executor_identity {
  uint64_t project, project_generation, track, track_generation;
  char instance_id[37];
  /* Optional active-sequence precondition; zero/zero leaves it unset. */
  uint64_t sequence, sequence_generation;
};

struct mpc_executor_config {
  uint32_t abi_version;
  size_t struct_bytes;
  unsigned capacity;
  void *userdata;
  /* Bounded UI-thread hook: check current identities/generations, resolve for
   * this call only, and perform no blocking waits or client I/O. */
  enum mpc_native_error (*resolve)(
    void *, const struct mpc_executor_identity *, struct mpc_native_target *);
};

enum mpc_executor_operation {
  MPC_EXECUTOR_READ_TRACK,
  MPC_EXECUTOR_SET_MUTE,
  MPC_EXECUTOR_SET_SOLO,
  MPC_EXECUTOR_SET_RECORD_ARMED,
  MPC_EXECUTOR_IDENTITIES,
  MPC_EXECUTOR_VALIDATE_SEQUENCE,
  MPC_EXECUTOR_GET_STATE,
  /* For the managed command host only; require an attached handler authority. */
  MPC_EXECUTOR_COMMAND_MUTE,
  MPC_EXECUTOR_COMMAND_SOLO,
  MPC_EXECUTOR_COMMAND_ARM,
  MPC_EXECUTOR_COMMAND_OBSERVE
};

struct mpc_executor_request {
  enum mpc_executor_operation operation;
  struct mpc_executor_identity identity;
  bool desired, has_expected, expected;
  /* Optional acceptance-relative queue deadline, at most MAX_WAIT_MS. Zero
   * leaves it unset. The UI callback checks it before starting model access. */
  unsigned queue_timeout_ms;
  /* COMMAND_OBSERVE rechecks the qualification of the original operation. */
  unsigned command_property;
  enum mpc_mute_scope mute_scope;
  /* Managed commands carry their original absolute monotonic deadline. This
   * prevents allocation/dispatch overhead from renewing the acceptance budget. */
  uint64_t command_deadline_ms;
};

enum mpc_executor_state {
  MPC_EXECUTOR_STARTING,
  MPC_EXECUTOR_READY,
  MPC_EXECUTOR_FAILED,
  MPC_EXECUTOR_STOPPING,
  MPC_EXECUTOR_STOPPED
};

enum mpc_executor_job_state {
  MPC_EXECUTOR_QUEUED = 1,
  MPC_EXECUTOR_RUNNING,
  MPC_EXECUTOR_COMPLETED,
  MPC_EXECUTOR_CANCELLED
};

struct mpc_executor_result {
  enum mpc_executor_job_state state;
  enum mpc_native_error error;
  struct mpc_native_track_state observed;
  /* Final observations/write metadata are valid only when COMPLETED. RUNNING
   * with OUTCOME_UNKNOWN does not report whether a native setter has begun. */
  struct mpc_native_write_result write;
  struct mpc_identity_snapshot identities;
  /* Valid only on successful COMPLETED GET_STATE; borrowed until job release.
   * Wait/cancel never exposes a buffer while a UI capture is still running. */
  const struct mpc_state_snapshot *snapshot;
  /* Internal managed-command observation, borrowed under the same job lifetime. */
  const struct mpc_command_native_result *command;
};

struct mpc_executor_status {
  enum mpc_executor_state state;
  enum mpc_native_error failure;
  unsigned live_jobs, queued_jobs, callback_captures;
  uint32_t ui_thread_id;
  struct mpc_native_status adapter;
};

/* Open starts an asynchronous dispatch probe. Even a failed probe leaves a
 * closeable instance; wait_ready/status reports its failure. NULL resolve allows
 * discovery only. userdata must remain valid until successful close. */
MPC_NATIVE_API enum mpc_native_error
mpc_executor_open(const struct mpc_executor_config *, struct mpc_executor **);
/* Firmware-owned root resolution and destruction observers. No external
 * resolver or raw-address registry. Snapshot/sequence requests require this
 * mode. Shutdown detaches observers on the UI thread before draining. */
MPC_NATIVE_API enum mpc_native_error
mpc_executor_open_authoritative(unsigned capacity, struct mpc_executor **);
MPC_NATIVE_API enum mpc_native_error mpc_executor_wait_ready(struct mpc_executor *, unsigned);
MPC_NATIVE_API enum mpc_native_error
mpc_executor_get_status(struct mpc_executor *, struct mpc_executor_status *);
MPC_NATIVE_API enum mpc_native_error mpc_executor_submit(
  struct mpc_executor *, const struct mpc_executor_request *, struct mpc_executor_job **);

/* A wait deadline can only shorten a queued job's execution deadline. A running job returns
 * OUTCOME_UNKNOWN and remains queryable by waiting again. No result implies audio completion. Zero
 * milliseconds is an immediate deadline, not a non-mutating poll. */
MPC_NATIVE_API enum mpc_native_error
mpc_executor_wait(struct mpc_executor_job *, unsigned, struct mpc_executor_result *);
MPC_NATIVE_API enum mpc_native_error
mpc_executor_cancel(struct mpc_executor_job *, struct mpc_executor_result *);
/* Non-mutating status read: unlike wait(0), never shortens a queue deadline. */
MPC_NATIVE_API enum mpc_native_error
mpc_executor_poll(struct mpc_executor_job *, struct mpc_executor_result *);
/* Client disconnect: cancel if queued, detach if running. Join other users of
 * this job before release; the handle becomes invalid immediately. */
MPC_NATIVE_API void mpc_executor_release(struct mpc_executor_job *);

/* Stop acceptance, cancel queued work, and wait off the UI thread for captures
 * and the scheduler to drain. Timeout keeps the instance alive; retry later.
 * Close refuses undrained instances and unreleased job handles. The host must
 * stop/join API callers before close. Before dlclose of the DSO, it must also
 * establish external UI quiescence: the final capture destructor can still be
 * returning through library code after its reference count reaches zero. */
MPC_NATIVE_API enum mpc_native_error mpc_executor_shutdown(struct mpc_executor *, unsigned);
MPC_NATIVE_API enum mpc_native_error mpc_executor_close(struct mpc_executor *);

#endif
