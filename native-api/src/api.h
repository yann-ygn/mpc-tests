#ifndef MPC_API_H
#define MPC_API_H

#include <stddef.h>

#define API_VERSION "0.1"
#define MAX_REQUEST_BYTES 1024
#define MAX_RESPONSE_BYTES 16384
#define MAX_CONNECTIONS 16
#define CONNECTION_TIMEOUT_MS 1000
#define ADAPTER_PROBE_TIMEOUT_MS 50

struct api {
  const char *root;
  const char *device_id;
  char instance_id[37];
  char backend_signature[1024];
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
};

int api_init(struct api *api, const char *root, const char *device_id);
int discovery_read(struct api *api, struct discovery *result);
char *api_error(const char *code, const char *message);
char *api_respond(struct api *api, const char *request, size_t length);
int adapter_probe(const char *root, long pid, char instance_id[37]);

#endif
