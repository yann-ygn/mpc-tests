#ifndef MPC_NATIVE_PRIVATE_H
#define MPC_NATIVE_PRIVATE_H

#include "mpc_native.h"

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

#endif
