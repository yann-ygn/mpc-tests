#include "private.h"
#include "mpc_adapter_profile.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int read32(struct mpc_native_adapter *a, uint32_t address, uint32_t *out) {
  return !address || address % 4 || address > UINT32_MAX - 0x10000u
    ? -1
    : a->io.read(a->io.userdata, address, out, 4);
}

/* Copy bounded UTF-8, including validation on the native side. A null string
 * storage is accepted only for an unloaded, empty synthetic/native descriptor. */
static int text(struct mpc_native_adapter *a, uint32_t address, char *out) {
  if (!address)
    return 0;
  if (address > UINT32_MAX - MPC_PLUGIN_TEXT_BYTES)
    return -1;
  unsigned remaining = 0, scalar = 0, minimum = 0;
  for (unsigned i = 0; i < MPC_PLUGIN_TEXT_BYTES; i++) {
    unsigned char c;
    if (a->io.read(a->io.userdata, address + i, &c, 1))
      return -1;
    out[i] = (char)c;
    if (!c)
      return remaining ? -1 : 0;
    if (remaining) {
      if ((c & 0xc0) != 0x80)
        return -1;
      scalar = (scalar << 6) | (c & 0x3f);
      if (
        !--remaining &&
        (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)))
        return -1;
    } else if (c >= 0xc2 && c <= 0xdf) {
      remaining = 1;
      scalar = c & 0x1f;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      remaining = 2;
      scalar = c & 0xf;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      remaining = 3;
      scalar = c & 7;
      minimum = 0x10000;
    } else if (c >= 0x80)
      return -1;
  }
  return -1;
}

enum mpc_native_error mpc_plugin_read(
  struct mpc_native_adapter *a,
  uint32_t program,
  unsigned family,
  struct mpc_plugin_identity *out,
  struct mpc_plugin_fence *fence) {
  memset(out, 0, sizeof *out);
  memset(fence, 0, sizeof *fence);
  if (family != MPC_NATIVE_PLUGIN)
    return MPC_NATIVE_OK;
  fence->program = program;
  if (read32(a, program, &fence->vtable))
    return MPC_NATIVE_READ_FAILED;
  if (fence->vtable != a->io.image + NATIVE_PLUGIN_VTABLE)
    return MPC_NATIVE_STATE_UNSTABLE;
  if (
    read32(a, program + PLUGIN_programInstance, &fence->instance) ||
    read32(a, program + PLUGIN_programOwner, &fence->owner) ||
    read32(a, program + PLUGIN_descriptionName, &fence->name) ||
    read32(a, program + PLUGIN_descriptionIdentifier, &fence->product) ||
    read32(a, program + PLUGIN_descriptionFormat, &fence->format))
    return MPC_NATIVE_READ_FAILED;
  if (!fence->instance) {
    /* An assigned descriptor without a loaded UI instance can mean loading or
     * a failed load. Do not relabel it as an available instrument. */
    char name[MPC_PLUGIN_TEXT_BYTES] = {0}, product[MPC_PLUGIN_TEXT_BYTES] = {0};
    if (text(a, fence->name, name) || text(a, fence->product, product))
      return MPC_NATIVE_READ_FAILED;
    out->status = name[0] || product[0] ? MPC_PLUGIN_UNAVAILABLE : MPC_PLUGIN_UNASSIGNED;
    return MPC_NATIVE_OK;
  }
  if (!fence->owner || !fence->name || !fence->product || !fence->format)
    return MPC_NATIVE_READ_FAILED;
  char format[MPC_PLUGIN_TEXT_BYTES] = {0};
  if (text(a, fence->format, format))
    return MPC_NATIVE_READ_FAILED;
  if (strcmp(format, "MPC") && strcmp(format, "VST")) {
    out->status = MPC_PLUGIN_UNSUPPORTED;
    return MPC_NATIVE_OK;
  }
  char product[MPC_PLUGIN_TEXT_BYTES] = {0};
  if (text(a, fence->name, out->display_name) || !out->display_name[0])
    return MPC_NATIVE_READ_FAILED;
  if (!strcmp(format, "VST")) {
    if (
      read32(a, program + PLUGIN_descriptionUid, &fence->uid) || text(a, fence->product, product) ||
      product[0] != '/')
      return MPC_NATIVE_READ_FAILED;
    /* Exact native library path + descriptor UID. No display name, instance
     * address, basename reduction, path normalization or silent truncation. */
    int length = snprintf(
      out->product_id,
      sizeof out->product_id,
      "VST:%08" PRIx32 ":%s",
      fence->uid,
      product);
    if (length < 0 || (size_t)length >= sizeof out->product_id)
      return MPC_NATIVE_READ_FAILED;
  } else if (
    text(a, fence->product, out->product_id) || strncmp(out->product_id, "MPC:", 4) ||
    !out->product_id[4])
    return MPC_NATIVE_READ_FAILED;
  out->status = MPC_PLUGIN_AVAILABLE;
  return MPC_NATIVE_OK;
}
