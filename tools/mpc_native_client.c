#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* One local lab command on stdin; relay response lines until the bridge closes. */
int main(int argc, char **argv) {
  char *end;
  long pid = argc == 2 ? strtol(argv[1], &end, 10) : 0;
  if (argc != 2 || !pid || pid < 0 || *end) {
    fprintf(stderr, "usage: native_client MPC_PID\n");
    return 2;
  }
  char command[256];
  if (!fgets(command, sizeof command, stdin) || !strchr(command, '\n'))
    return 2;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  snprintf(addr.sun_path, sizeof addr.sun_path, "/tmp/mpc-native-lab-%ld.sock", pid);
  struct timeval timeout = {25, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
    perror("connect");
    close(fd);
    return 1;
  }
  size_t n = strlen(command), sent = 0;
  while (sent < n) {
    ssize_t got = write(fd, command + sent, n - sent);
    if (got < 0) {
      perror("write");
      close(fd);
      return 1;
    }
    sent += (size_t)got;
  }
  shutdown(fd, SHUT_WR);
  char b[2048];
  ssize_t got;
  while ((got = read(fd, b, sizeof b)) > 0) {
    if (fwrite(b, 1, (size_t)got, stdout) != (size_t)got) {
      close(fd);
      return 1;
    }
    fflush(stdout);
  }
  if (got < 0)
    perror("read");
  close(fd);
  return got < 0 ? 1 : 0;
}
