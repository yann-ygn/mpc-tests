#ifndef MPC_API_TRANSPORT_H
#define MPC_API_TRANSPORT_H

/* Internal CLI operations. Protocol and discovery remain behind api.h. */
int api_serve(const char *path, const char *device_id, const char *root);
int api_request(const char *path);

#endif
