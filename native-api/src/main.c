#include "transport.h"
#include "host.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/un.h>

int main(int argc, char **argv) {
  const char *path = NULL, *device_id = NULL, *root = "";
  if (argc >= 2 && !strcmp(argv[1], "host-cleanup")) {
    path = MPC_HOST_DIRECTORY;
    if (argc == 4 && !strcmp(argv[2], "--directory"))
      path = argv[3];
    else if (argc != 2)
      goto usage;
    if (path[0] != '/')
      goto usage;
    int result = mpc_host_cleanup(path);
    fputs(
      result ? "Runtime cleanup refused: host active or path not owned\n"
             : "Native host runtime sockets cleaned\n",
      result ? stderr : stdout);
    return result;
  }
  if (
    argc >= 2 &&
    (!strcmp(argv[1], "host-status") || !strcmp(argv[1], "host-start") ||
     !strcmp(argv[1], "host-stop"))) {
    if (argc == 2)
      path = MPC_HOST_DIRECTORY "/control.sock";
    else if (argc == 4 && !strcmp(argv[2], "--socket"))
      path = argv[3];
    if (!path || path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
      goto usage;
    return api_control(
      path,
      !strcmp(argv[1], "host-status")    ? "STATUS\n"
        : !strcmp(argv[1], "host-start") ? "START\n"
                                         : "STOP\n");
  }
  if (argc < 2 || (strcmp(argv[1], "serve") && strcmp(argv[1], "request")))
    goto usage;
  for (int i = 2; i < argc; i += 2) {
    if (i + 1 == argc)
      goto usage;
    if (!strcmp(argv[i], "--socket") && !path)
      path = argv[i + 1];
    else if (!strcmp(argv[i], "--device-id") && !device_id)
      device_id = argv[i + 1];
    else if (!strcmp(argv[i], "--root") && !*root)
      root = argv[i + 1];
    else
      goto usage;
  }
  if (
    !path || path[0] != '/' || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
    strlen(root) > PATH_MAX - 128)
    goto usage;
  if (!strcmp(argv[1], "request")) {
    if (device_id || *root)
      goto usage;
    return api_request(path);
  }
  if (!device_id || !*device_id || strlen(device_id) > 80)
    goto usage;
  for (const char *p = device_id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
          *p == '_' || *p == '-'))
      goto usage;
  return api_serve(path, device_id, root);
usage:
  fputs(
    "Usage: mpc-api serve --socket PATH --device-id ID [--root FIXTURE_ROOT]\n"
    "       mpc-api request --socket PATH < request.json\n"
    "       mpc-api host-status|host-start|host-stop [--socket PATH]\n"
    "       mpc-api host-cleanup [--directory PATH]\n",
    stderr);
  return 2;
}
