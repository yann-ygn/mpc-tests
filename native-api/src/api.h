#ifndef MPC_API_H
#define MPC_API_H

#include <stddef.h>
#include <stdio.h>
#include "mpc_state.h"
#include "mpc_commands.h"

#define API_VERSION "0.1"
#define MAX_REQUEST_BYTES 1024
#define MAX_RESPONSE_BYTES 1048576
#define MAX_CONNECTIONS 16
#define CONNECTION_TIMEOUT_MS 1000
#define ADAPTER_PROBE_TIMEOUT_MS 50
#define ADAPTER_STATE_TIMEOUT_MS 200
#define API_COMMAND_CAPACITY 16
#define API_RECEIPT_CAPACITY 128

struct api_request_data {
  char version[80], op[80], instance_id[37];
  struct mpc_command_request command;
  unsigned wait_ms;
};

/* A delivery wait owns no native job. Disconnecting only drops this lookup. */
struct api_delivery {
  char instance_id[37], request_id[MPC_COMMAND_ID_BYTES];
  unsigned wait_ms;
  int pending;
};

/* Optional in-process attachment. The transport joins all callers before the
 * owner retires this backend. Standalone discovery keeps the lab IPC path. */
struct api_backend {
  void *userdata;
  int (*probe)(void *, char instance_id[37], int *state_ready);
  int (*state)(void *, const char *, struct mpc_state_snapshot *);
  int (*bind_commands)(void *, const char *);
  char *(*command)(void *, const char *, uint64_t, const struct api_request_data *, int *);
};

struct api {
  const char *root;
  const char *device_id;
  char instance_id[37];
  char backend_signature[1024];
  uint64_t revision;
  const struct api_backend *backend;
};

struct discovery {
  char model[256];
  char build_id[129];
  const char *process;
  const char *profile;
  const char *unavailable_reason;
  int known_build;
  int adapter_attached;
  char adapter_id[37];
  long pid;
  int state_ready;
  int commands_ready;
};

int api_init(struct api *api, const char *root, const char *device_id);
int api_instance_new(char output[37]);
int discovery_read(struct api *api, struct discovery *result);
char *api_error(const char *code, const char *message);
int api_parse(const char *, size_t, struct api_request_data *);
char *api_respond(struct api *, uint64_t, const char *, size_t, struct api_delivery *);
char *api_command_poll(struct api *, uint64_t, struct api_delivery *);
int adapter_probe(const char *root, long pid, char instance_id[37], int *state_ready);
char *api_state(struct api *, const struct discovery *, const char *operation);
int adapter_state(const char *, long, const char *, struct mpc_state_snapshot *);
void api_quoted(FILE *, const char *);

#endif
