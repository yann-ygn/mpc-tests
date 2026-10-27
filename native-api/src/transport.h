#ifndef MPC_API_TRANSPORT_H
#define MPC_API_TRANSPORT_H

/* Internal CLI operations. Protocol and discovery remain behind api.h. */
int api_serve(const char *path, const char *device_id, const char *root);
struct api_backend;
/* Embedded service: no process signal handlers or process-wide umask changes.
 * stop is called only by the serving thread; backend lives until return. */
int api_serve_embedded(
  const char *path,
  const char *device_id,
  const struct api_backend *,
  int (*stop)(void *),
  void *,
  const char *root);
int api_control(const char *path, const char *request);
struct stat;
void api_socket_remember(unsigned slot, const char *path, const struct stat *);
void api_sockets_cleanup(void);
int api_request(const char *path);
int api_follow(const char *path, const char *cursor);

#endif
