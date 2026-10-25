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
  void (*destroy)(void *);
};

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
