#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Standalone Unix datagram transport. STDIN/STDOUT carry hex-encoded CBOR packets.
 * No dynamic loading, injection, signal delivery to other processes, or setting changes. */
int main(void) {
  alarm(90);
  setvbuf(stdin, NULL, _IONBF, 0);
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_un local = {.sun_family = AF_UNIX}, server = {.sun_family = AF_UNIX};
  snprintf(local.sun_path, sizeof local.sun_path, "/tmp/az0x-webserver-client.%ld", (long)getpid());
  strcpy(server.sun_path, "/tmp/az0x-webserver");
  if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) {
    perror("bind");
    close(fd);
    return 1;
  }
  if (connect(fd, (struct sockaddr *)&server, sizeof server) < 0) {
    perror("connect");
    close(fd);
    unlink(local.sun_path);
    return 1;
  }
  printf("PID %ld\n", (long)getpid());
  fflush(stdout);
  char line[131074];
  unsigned char data[65536];
  int status = 0;
  for (;;) {
    struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {fd, POLLIN, 0}};
    if (poll(fds, 2, 5000) < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      status = 1;
      break;
    }
    if (fds[0].revents & (POLLIN | POLLHUP)) {
      if (!fgets(line, sizeof line, stdin))
        break;
      size_t n = strcspn(line, "\r\n");
      if (n % 2 || n > 2 * sizeof data) {
        status = 2;
        break;
      }
      for (size_t i = 0; i < n / 2; i++) {
        unsigned value;
        if (sscanf(line + 2 * i, "%2x", &value) != 1) {
          status = 2;
          break;
        }
        data[i] = (unsigned char)value;
      }
      if (status)
        break;
      if (send(fd, data, n / 2, 0) != (ssize_t)(n / 2)) {
        perror("send");
        status = 1;
        break;
      }
    }
    if (fds[1].revents & POLLIN) {
      ssize_t n = recv(fd, data, sizeof data, 0);
      if (n < 0) {
        perror("recv");
        status = 1;
        break;
      }
      fputs("RX ", stdout);
      for (ssize_t i = 0; i < n; i++)
        printf("%02x", data[i]);
      fputc('\n', stdout);
      fflush(stdout);
    }
  }
  close(fd);
  unlink(local.sun_path);
  return status;
}
