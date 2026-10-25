#include "api.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop(int sig) {
  (void)sig;
  stopping = 1;
}

static int64_t now_ms(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value))
    abort();
  return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

struct client {
  int fd;
  char input[MAX_REQUEST_BYTES + 1];
  size_t used, sent, length;
  char *output;
  int64_t deadline;
};

static void client_close(struct client *client) {
  close(client->fd);
  free(client->output);
  memset(client, 0, sizeof *client);
  client->fd = -1;
}

static void client_reply(struct client *client, char *response) {
  if (!response) {
    client_close(client);
    return;
  }
  client->output = response;
  client->length = strlen(response);
  client->deadline = now_ms() + CONNECTION_TIMEOUT_MS;
}

static int private_parent(const char *path) {
  char parent[sizeof(((struct sockaddr_un *)0)->sun_path)];
  strcpy(parent, path);
  char *slash = strrchr(parent, '/');
  if (!slash || slash == parent)
    return -1;
  *slash = 0;
  struct stat status;
  return lstat(parent, &status) || !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0077)
    ? -1
    : 0;
}

static int serve(const char *path, const char *device_id, const char *root) {
  if (private_parent(path)) {
    fputs("Socket parent must be an existing private directory owned by this user\n", stderr);
    return 1;
  }
  struct api api;
  if (api_init(&api, root, device_id)) {
    perror("instance identity");
    return 1;
  }
  int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  strcpy(address.sun_path, path);
  umask(0077);
  /* Never unlink a pre-existing path, including a live service's socket. */
  if (bind(listener, (struct sockaddr *)&address, sizeof address)) {
    perror("bind");
    close(listener);
    return 1;
  }
  struct stat owned;
  if (lstat(path, &owned)) {
    perror("lstat socket");
    close(listener);
    return 1;
  }
  int status = 1;
  struct client clients[MAX_CONNECTIONS];
  memset(clients, 0, sizeof clients);
  for (size_t i = 0; i < MAX_CONNECTIONS; i++)
    clients[i].fd = -1;
  if (chmod(path, 0600) || listen(listener, MAX_CONNECTIONS)) {
    perror("listen");
    goto cleanup;
  }
  struct sigaction action = {.sa_handler = stop};
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGINT, &action, NULL);
  fprintf(stderr, "Listening on %s (describe only)\n", path);
  status = 0;
  while (!stopping) {
    struct pollfd fds[MAX_CONNECTIONS + 1];
    fds[0] = (struct pollfd){listener, POLLIN, 0};
    for (size_t i = 0; i < MAX_CONNECTIONS; i++)
      fds[i + 1] = (struct pollfd){clients[i].fd, clients[i].output ? POLLOUT : POLLIN, 0};
    int ready = poll(fds, MAX_CONNECTIONS + 1, 100);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      status = 1;
      break;
    }
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
      struct client *client = &clients[i];
      if (client->fd < 0)
        continue;
      short events = fds[i + 1].revents;
      if (events & (POLLERR | POLLNVAL)) {
        client_close(client);
        continue;
      }
      if (client->output && (events & POLLOUT)) {
        ssize_t n = send(
          client->fd,
          client->output + client->sent,
          client->length - client->sent,
          MSG_NOSIGNAL);
        if (n > 0)
          client->sent += (size_t)n;
        else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
          client_close(client);
        if (client->fd >= 0 && client->sent == client->length)
          client_close(client);
      } else if (!client->output && (events & (POLLIN | POLLHUP))) {
        ssize_t n =
          recv(client->fd, client->input + client->used, sizeof client->input - client->used, 0);
        if (n > 0) {
          client->used += (size_t)n;
          char *newline = memchr(client->input, '\n', client->used);
          if (newline)
            client_reply(
              client,
              api_respond(&api, client->input, (size_t)(newline - client->input)));
          else if (client->used > MAX_REQUEST_BYTES)
            client_reply(client, api_error("INVALID_ARGUMENT", "Request exceeds maxRequestBytes"));
        } else if (!n)
          client_reply(client, api_error("INVALID_ARGUMENT", "Request must end with a newline"));
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
          client_close(client);
      }
      if (client->fd >= 0 && now_ms() >= client->deadline) {
        if (client->output)
          client_close(client);
        else
          client_reply(client, api_error("INVALID_ARGUMENT", "Request line timed out"));
      }
    }
    if (fds[0].revents & POLLIN) {
      int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
          perror("accept");
          status = 1;
          break;
        }
        continue;
      }
      struct ucred peer;
      socklen_t length = sizeof peer;
      const char *error = NULL;
      if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) || peer.uid != geteuid())
        error = "UNAUTHORIZED";
      size_t slot;
      for (slot = 0; slot < MAX_CONNECTIONS && clients[slot].fd >= 0; slot++) {
      }
      if (!error && slot == MAX_CONNECTIONS)
        error = "NOT_READY";
      if (error) {
        char *response = api_error(error, "Connection not admitted");
        if (response) {
          (void)send(fd, response, strlen(response), MSG_NOSIGNAL);
          free(response);
        }
        close(fd);
      } else {
        clients[slot].fd = fd;
        clients[slot].deadline = now_ms() + CONNECTION_TIMEOUT_MS;
      }
    }
  }
cleanup:
  for (size_t i = 0; i < MAX_CONNECTIONS; i++)
    if (clients[i].fd >= 0)
      client_close(&clients[i]);
  close(listener);
  struct stat current;
  if (!lstat(path, &current) && current.st_dev == owned.st_dev && current.st_ino == owned.st_ino)
    unlink(path);
  return status;
}

static int request(const char *path) {
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

int main(int argc, char **argv) {
  const char *path = NULL, *device_id = NULL, *root = "";
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
    return request(path);
  }
  if (!device_id || !*device_id || strlen(device_id) > 80)
    goto usage;
  for (const char *p = device_id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
          *p == '_' || *p == '-'))
      goto usage;
  return serve(path, device_id, root);
usage:
  fputs(
    "Usage: mpc-api serve --socket PATH --device-id ID [--root FIXTURE_ROOT]\n"
    "       mpc-api request --socket PATH < request.json\n",
    stderr);
  return 2;
}
