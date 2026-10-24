#ifndef MPC_API_H
#define MPC_API_H

#include <stddef.h>

#define API_VERSION "0.1"
#define MAX_REQUEST_BYTES 1024
#define MAX_RESPONSE_BYTES 16384
#define MAX_CONNECTIONS 16
#define CONNECTION_TIMEOUT_MS 1000

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
};

int api_init(struct api *api, const char *root, const char *device_id);
int discovery_read(struct api *api, struct discovery *result);
char *api_error(const char *code, const char *message);
char *api_respond(struct api *api, const char *request, size_t length);

#endif
