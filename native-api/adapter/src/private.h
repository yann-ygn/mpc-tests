#ifndef MPC_NATIVE_PRIVATE_H
#define MPC_NATIVE_PRIVATE_H

#include "mpc_native.h"
#include "mpc_executor.h"

/* Exact GNU ARM std::function<void()> layout qualified on this firmware. The
 * fixture exercises the same manager protocol without invoking native code. */
struct mpc_native_function {
  void *data[2];
  int (*manager)(void *, const void *, int);
  void (*invoke)(const void *);
};

/* Native destruction-observer ABI: the subject holds &link, and invokes virtual
 * slot 2 with the complete observer after unlinking it. It never owns/free()s it.
 * Only the atomic alive flag is changed by a destruction callback. */
struct mpc_native_lifetime;
struct mpc_native_lifetime_link {
  struct mpc_native_lifetime_link **previous;
  struct mpc_native_lifetime_link *next;
};
struct mpc_native_lifetime {
  void (*const *vtable)(struct mpc_native_lifetime *);
  struct mpc_native_lifetime_link link;
  bool alive;
};

/* SequenceListener's first two words are a vtable and its destruction subject.
 * Native Notifier<SequenceListener> owns the registration node, not this object. */
struct mpc_native_sequence_watch {
  void (*const *vtable)(struct mpc_native_sequence_watch *, uint32_t);
  struct mpc_native_lifetime_link *destruction_observers;
  uint32_t sequence;
  unsigned char used;
  bool changed, attached;
};

struct mpc_native_project_watch {
  void (*const *vtable)(struct mpc_native_project_watch *, uint32_t);
  struct mpc_native_lifetime_link *destruction_observers;
  uint32_t project;
  bool changed, loading, attached;
};

/* Backend injection is private to the isolated fixture, absent from the DSO's
 * exports. Production always supplies runtime.c's read-only memory backend. */
struct mpc_native_io {
  void *userdata;
  uintptr_t image;
  enum mpc_native_error (*identity)(void *, char[129], char[256]);
  int (*read)(void *, uintptr_t, void *, size_t);
  bool (*on_ui_thread)(void *);
  enum mpc_native_error (*setter)(void *, uintptr_t, uintptr_t, bool);
  enum mpc_native_error (*filter)(void *, uintptr_t, uintptr_t, bool *);
  bool (*post)(void *, uintptr_t, struct mpc_native_function *);
  enum mpc_native_error (*watch)(void *, uint32_t, struct mpc_native_lifetime *);
  void (*unwatch)(void *, struct mpc_native_lifetime *);
  enum mpc_native_error (*sequence_watch)(void *, uint32_t, struct mpc_native_sequence_watch *);
  enum mpc_native_error (*sequence_unwatch)(void *, struct mpc_native_sequence_watch *, bool);
  enum mpc_native_error (*project_watch)(void *, uint32_t, struct mpc_native_project_watch *);
  enum mpc_native_error (*project_unwatch)(void *, struct mpc_native_project_watch *, bool);
  void (*destroy)(void *);
};

struct mpc_native_adapter {
  struct mpc_native_io io;
  struct mpc_native_hooks hooks;
  int fault;
  char instance_id[37];
};

struct mpc_identity_registry;
enum mpc_native_error mpc_native_verify_ui(struct mpc_native_adapter *);
enum mpc_native_error
mpc_identity_create(struct mpc_native_adapter *, struct mpc_identity_registry **);
void mpc_identity_destroy(struct mpc_identity_registry *);
enum mpc_native_error mpc_identity_detach(struct mpc_identity_registry *);
enum mpc_native_error
mpc_identity_capture(struct mpc_identity_registry *, struct mpc_identity_snapshot *);
enum mpc_native_error mpc_identity_resolve(
  struct mpc_identity_registry *, const struct mpc_executor_identity *, struct mpc_native_target *);
enum mpc_native_error
mpc_identity_sequence(struct mpc_identity_registry *, const struct mpc_executor_identity *);
enum mpc_native_error mpc_executor_open_authoritative_backend(
  const struct mpc_native_io *, unsigned, struct mpc_executor **);

struct native_binding {
  uint32_t property, requested, observed, setter;
  uint32_t filter_storage, filter_manager, filter_invoker, filter_rva;
  bool owner_is_track;
};

struct native_fingerprint {
  uint32_t rva;
  unsigned char bytes[32];
};
struct native_secondary {
  uint32_t offset, vtable;
};

enum mpc_native_error mpc_native_open_backend(
  const struct mpc_native_io *, const struct mpc_native_hooks *, struct mpc_native_adapter **);
bool mpc_native_on_ui_thread(struct mpc_native_adapter *);
enum mpc_native_error mpc_native_post(struct mpc_native_adapter *, struct mpc_native_function *);
enum mpc_native_error mpc_executor_open_backend(
  const struct mpc_native_io *, const struct mpc_executor_config *, struct mpc_executor **);

#endif
