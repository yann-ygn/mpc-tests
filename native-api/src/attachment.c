#include "api.h"
#include "profile.h"
#include "mpc_native.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Private attachment handshake with the retained lab host. No native
 * address or target is accepted. PID/UID verification binds it to this MPC. */
/* Deliberately identical on the host and ARM. Any layout change needs a new
 * transfer version, including changes to padding, capacities or identity rows. */
_Static_assert(sizeof(struct mpc_state_track) == 344, "snapshot ABI 3 track layout");
_Static_assert(sizeof(struct mpc_state_snapshot) == 127824, "snapshot ABI 3 transfer layout");

static long long monotonic_ms(void) {
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time))
    return -1;
  return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int ready(int fd, short events, long long deadline) {
  for (;;) {
    long long remaining = deadline - monotonic_ms();
    if (remaining <= 0 || remaining > ADAPTER_STATE_TIMEOUT_MS)
      return -1;
    struct pollfd pollfd = {fd, events, 0};
    int result = poll(&pollfd, 1, (int)remaining);
    if (result < 0 && errno == EINTR)
      continue;
    return result > 0 && (pollfd.revents & events) ? 0 : -1;
  }
}

int adapter_probe(const char *root, long pid, char instance_id[37], int *state_ready) {
  instance_id[0] = 0;
  *state_ready = 0;
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  int length = snprintf(
    address.sun_path,
    sizeof address.sun_path,
    "%s/tmp/mpc-native-lab-%ld.sock",
    root,
    pid);
  if (length < 0 || (size_t)length >= sizeof address.sun_path)
    return 0;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return 0;
  int valid = 0;
  long long deadline = monotonic_ms() + ADAPTER_PROBE_TIMEOUT_MS;
  if (connect(fd, (struct sockaddr *)&address, sizeof address)) {
    if (errno != EINPROGRESS || ready(fd, POLLOUT, deadline))
      goto done;
    int error;
    socklen_t size = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error)
      goto done;
  }
  struct ucred peer;
  socklen_t size = sizeof peer;
  if (
    getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.pid != pid ||
    peer.uid != geteuid())
    goto done;
  const char request[] = "ADAPTER_STATUS\n";
  size_t sent = 0;
  while (sent < sizeof request - 1) {
    if (ready(fd, POLLOUT, deadline))
      goto done;
    ssize_t n = send(fd, request + sent, sizeof request - 1 - sent, MSG_NOSIGNAL);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (n <= 0)
      goto done;
    sent += (size_t)n;
  }
  char response[512];
  size_t used = 0;
  while (used < sizeof response - 1) {
    if (ready(fd, POLLIN, deadline))
      goto done;
    ssize_t n = recv(fd, response + used, sizeof response - 1 - used, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (n <= 0)
      goto done;
    used += (size_t)n;
    if (memchr(response, '\n', used))
      break;
  }
  if (
    !used || response[used - 1] != '\n' || memchr(response, 0, used) ||
    memchr(response, '\n', used) != response + used - 1)
    goto done;
  response[used] = 0;
  char tag[32], epoch[37], profile[64], build[129], verification[16], extra;
  if (
    sscanf(
      response,
      "%31s %36s %63s %128s %15s %c",
      tag,
      epoch,
      profile,
      build,
      verification,
      &extra) != 5 ||
    (strcmp(tag, "MPC_ADAPTER_V1") && strcmp(tag, "MPC_ADAPTER_STATE_V3")) ||
    strcmp(profile, PROFILE_ID) || strcmp(build, PROFILE_BUILD_ID) ||
    strcmp(verification, "VERIFIED") || strlen(epoch) != 36 || strncmp(epoch, "adp_", 4) ||
    strspn(epoch + 4, "0123456789abcdef") != 32)
    goto done;
  strcpy(instance_id, epoch);
  *state_ready = !strcmp(tag, "MPC_ADAPTER_STATE_V3");
  valid = 1;
done:
  close(fd);
  return valid;
}

/* Fixed-size, pointer-free private transfer. Never forward the lab's diagnostic
 * commands or raw targets. Every connection is bound to MPC's PID and UID. */
int adapter_state(
  const char *root, long pid, const char *instance, struct mpc_state_snapshot *snapshot) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  int length = snprintf(
    address.sun_path,
    sizeof address.sun_path,
    "%s/tmp/mpc-native-lab-%ld.sock",
    root,
    pid);
  if (length < 0 || (size_t)length >= sizeof address.sun_path)
    return MPC_NATIVE_NOT_READY;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return MPC_NATIVE_NOT_READY;
  int result = MPC_NATIVE_READ_FAILED;
  long long deadline = monotonic_ms() + ADAPTER_STATE_TIMEOUT_MS;
  if (connect(fd, (struct sockaddr *)&address, sizeof address)) {
    if (errno != EINPROGRESS || ready(fd, POLLOUT, deadline))
      goto done;
    int error;
    socklen_t size = sizeof error;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error)
      goto done;
  }
  struct ucred peer;
  socklen_t size = sizeof peer;
  if (
    getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.pid != pid ||
    peer.uid != geteuid())
    goto done;
  char request[64];
  length = snprintf(request, sizeof request, "STATE_V3 %s\n", instance);
  size_t sent = 0;
  while (sent < (size_t)length) {
    if (ready(fd, POLLOUT, deadline))
      goto done;
    ssize_t n = send(fd, request + sent, (size_t)length - sent, MSG_NOSIGNAL);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (n <= 0)
      goto done;
    sent += (size_t)n;
  }
  uint32_t header[3];
  for (unsigned part = 0; part < 2; part++) {
    char *buffer = part ? (char *)snapshot : (char *)header;
    size_t bytes = part ? sizeof *snapshot : sizeof header;
    size_t received = 0;
    while (received < bytes) {
      if (ready(fd, POLLIN, deadline))
        goto done;
      ssize_t n = recv(fd, buffer + received, bytes - received, 0);
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (n <= 0)
        goto done;
      received += (size_t)n;
    }
    if (!part && (header[0] != MPC_STATE_ABI || header[1] != sizeof *snapshot))
      goto done;
    if (!part && header[2]) {
      result = header[2] <= MPC_NATIVE_STATE_UNSTABLE ? (int)header[2] : MPC_NATIVE_READ_FAILED;
      goto done;
    }
  }
  result = MPC_NATIVE_OK;
done:
  close(fd);
  return result;
}
