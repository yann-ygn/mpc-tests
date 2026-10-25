#include "api.h"
#include "transport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

int api_request(const char *path) {
  char input[MAX_REQUEST_BYTES + 2];
  size_t used = fread(input, 1, sizeof input, stdin);
  if (
    ferror(stdin) || !used || used > MAX_REQUEST_BYTES + 1 ||
    (used == MAX_REQUEST_BYTES + 1 && input[used - 1] != '\n')) {
    fputs("Expected one request of at most maxRequestBytes bytes\n", stderr);
    return 1;
  }
  if (input[used - 1] != '\n')
    input[used++] = '\n';
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  struct timeval timeout = {.tv_sec = 2};
  if (
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ||
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout)) {
    perror("socket timeout");
    close(fd);
    return 1;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  strcpy(address.sun_path, path);
  if (connect(fd, (struct sockaddr *)&address, sizeof address)) {
    perror("connect");
    close(fd);
    return 1;
  }
  size_t sent = 0;
  while (sent < used) {
    ssize_t n = send(fd, input + sent, used - sent, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      perror("send");
      close(fd);
      return 1;
    }
    sent += (size_t)n;
  }
  char output[MAX_RESPONSE_BYTES + 1];
  used = 0;
  while (used < MAX_RESPONSE_BYTES) {
    ssize_t n = recv(fd, output + used, MAX_RESPONSE_BYTES - used, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      perror("receive");
      close(fd);
      return 1;
    }
    if (!n)
      break;
    used += (size_t)n;
    if (memchr(output, '\n', used))
      break;
  }
  close(fd);
  if (!used || used == MAX_RESPONSE_BYTES || output[used - 1] != '\n') {
    fputs("Incomplete response\n", stderr);
    return 1;
  }
  return fwrite(output, 1, used, stdout) == used && !fflush(stdout) ? 0 : 1;
}
