#ifndef MPC_NATIVE_H
#define MPC_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Trusted, in-process C ABI. Addresses here are private implementation details:
 * none of these types is a public wire/SDK target or a lifetime authority. */
#define MPC_NATIVE_ABI 1u
#define MPC_NATIVE_API __attribute__((visibility("default")))

enum mpc_native_error {
  MPC_NATIVE_OK,
  MPC_NATIVE_INVALID_ARGUMENT,
  MPC_NATIVE_ABI_MISMATCH,
  MPC_NATIVE_BUILD_MISMATCH,
  MPC_NATIVE_MODEL_MISMATCH,
  MPC_NATIVE_FINGERPRINT_MISMATCH,
  MPC_NATIVE_READ_FAILED,
  MPC_NATIVE_NOT_READY,
  MPC_NATIVE_WRONG_THREAD,
  MPC_NATIVE_STALE_TARGET,
  MPC_NATIVE_UNSUPPORTED_TRACK_TYPE,
  MPC_NATIVE_PRECONDITION_FAILED,
  MPC_NATIVE_FILTER_MISSING,
  MPC_NATIVE_VALUE_REJECTED,
  MPC_NATIVE_QUEUE_FULL,
  MPC_NATIVE_CANCELLED,
  MPC_NATIVE_OUTCOME_UNKNOWN,
  MPC_NATIVE_TIMED_OUT,
  MPC_NATIVE_SHUTTING_DOWN,
  MPC_NATIVE_DISPATCH_FAILED,
  MPC_NATIVE_STATE_UNSTABLE,
  MPC_NATIVE_STALE_INSTANCE,
  MPC_NATIVE_STALE_PROJECT,
  MPC_NATIVE_TARGET_NOT_FOUND,
  MPC_NATIVE_NO_ACTIVE_PROJECT,
  MPC_NATIVE_UNSUPPORTED_PRECONDITION,
  MPC_NATIVE_REQUEST_ID_CONFLICT,
  MPC_NATIVE_RETENTION_CAPACITY,
  MPC_NATIVE_OUTCOME_UNAVAILABLE,
  MPC_NATIVE_VERIFICATION_TIMEOUT,
  MPC_NATIVE_STALE_SEQUENCE,
  MPC_NATIVE_SEQUENCE_NOT_ACTIVE,
  MPC_NATIVE_MUTE_SCOPE_MISMATCH
};

enum mpc_native_family { MPC_NATIVE_PLUGIN = 1, MPC_NATIVE_DRUM = 2 };
enum mpc_native_audio_known { MPC_NATIVE_AUDIO_SOLO = 1, MPC_NATIVE_AUDIO_ARM = 2 };

struct mpc_native_adapter;
struct mpc_native_target {
  uint32_t project, track, program;
};

struct mpc_native_hooks {
  uint32_t abi_version;
  size_t struct_bytes;
  void *userdata;
  /* Called on the verified UI thread immediately before traversal. Must check
   * active project/object generations. NULL gates all target operations.
   * The retained lab supplies an explicitly experimental ownership check;
   * it does not establish public handle lifetime correctness (#20). */
  enum mpc_native_error (*validate_target)(void *, const struct mpc_native_target *);
};

struct mpc_native_track_state {
  enum mpc_native_family family;
  bool muted, soloed, record_armed, silenced;
  unsigned audio_known;
  bool audio_soloed, audio_record_armed;
};

struct mpc_native_write_result {
  bool native_invoked, changed_known, changed;
  struct mpc_native_track_state observed;
};

struct mpc_native_status {
  uint32_t abi_version;
  bool code_verified, target_validator_present;
  char instance_id[37];
  char profile_id[64];
  char build_id[129];
};

MPC_NATIVE_API enum mpc_native_error
mpc_native_open(const struct mpc_native_hooks *, struct mpc_native_adapter **);
/* Direct users must drain before close. mpc_executor owns its adapter lifetime. */
MPC_NATIVE_API void mpc_native_close(struct mpc_native_adapter *);
MPC_NATIVE_API enum mpc_native_error
mpc_native_get_status(struct mpc_native_adapter *, struct mpc_native_status *);
MPC_NATIVE_API enum mpc_native_error mpc_native_read_track(
  struct mpc_native_adapter *, const struct mpc_native_target *, struct mpc_native_track_state *);
MPC_NATIVE_API enum mpc_native_error mpc_native_set_mute(
  struct mpc_native_adapter *,
  const struct mpc_native_target *,
  bool,
  const bool *,
  struct mpc_native_write_result *);
MPC_NATIVE_API enum mpc_native_error mpc_native_set_solo(
  struct mpc_native_adapter *,
  const struct mpc_native_target *,
  bool,
  const bool *,
  struct mpc_native_write_result *);
MPC_NATIVE_API enum mpc_native_error mpc_native_set_record_armed(
  struct mpc_native_adapter *,
  const struct mpc_native_target *,
  bool,
  const bool *,
  struct mpc_native_write_result *);
MPC_NATIVE_API const char *mpc_native_error_name(enum mpc_native_error);

#endif
