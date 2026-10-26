#include "api.h"
#include "elf_id.h"
#include "profile.h"

#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int api_instance_new(char output[37]) {
  unsigned char bytes[16];
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  size_t used = 0;
  while (used < sizeof bytes) {
    ssize_t n = read(fd, bytes + used, sizeof bytes - used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      return -1;
    }
    used += (size_t)n;
  }
  close(fd);
  memcpy(output, "run_", 4);
  for (size_t i = 0; i < sizeof bytes; i++)
    sprintf(output + 4 + i * 2, "%02x", bytes[i]);
  return 0;
}

int api_init(struct api *api, const char *root, const char *device_id) {
  memset(api, 0, sizeof *api);
  api->root = root;
  api->device_id = device_id;
  return api_instance_new(api->instance_id);
}

/* Bounded metadata reads only. Never open /proc/PID/mem or attach to MPC. */
static int text_read(const char *path, char *output, size_t capacity) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return -1;
  size_t used = 0;
  while (used < capacity - 1) {
    ssize_t n = read(fd, output + used, capacity - 1 - used);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      close(fd);
      return -1;
    }
    if (!n)
      break;
    used += (size_t)n;
  }
  char extra;
  ssize_t tail = read(fd, &extra, 1);
  close(fd);
  if (tail != 0)
    return -1;
  output[used] = 0;
  return 0;
}

static unsigned long long start_time(const char *root, long pid) {
  char path[PATH_MAX], text[4096];
  snprintf(path, sizeof path, "%s/proc/%ld/stat", root, pid);
  if (text_read(path, text, sizeof text))
    return 0;
  char *p = strrchr(text, ')');
  if (!p || p[1] != ' ')
    return 0;
  p += 2;
  for (int field = 3; field < 22; field++) {
    p = strchr(p, ' ');
    if (!p)
      return 0;
    while (*p == ' ')
      p++;
  }
  char *end;
  errno = 0;
  unsigned long long value = strtoull(p, &end, 10);
  return errno || end == p || (*end != ' ' && *end != '\n' && *end) ? 0 : value;
}

static int valid_utf8(const char *text) {
  const unsigned char *p = (const unsigned char *)text;
  while (*p) {
    unsigned c = *p++;
    if (c < 128)
      continue;
    unsigned count, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      count = 1;
      minimum = 0x80;
      c &= 0x1f;
    } else if (c >= 0xe0 && c <= 0xef) {
      count = 2;
      minimum = 0x800;
      c &= 0xf;
    } else if (c >= 0xf0 && c <= 0xf4) {
      count = 3;
      minimum = 0x10000;
      c &= 7;
    } else
      return 0;
    for (unsigned i = 0; i < count; i++) {
      if ((*p & 0xc0) != 0x80)
        return 0;
      c = (c << 6) | (*p++ & 0x3f);
    }
    if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
      return 0;
  }
  return 1;
}

int discovery_read(struct api *api, struct discovery *result) {
  memset(result, 0, sizeof *result);
  char path[PATH_MAX], executable[PATH_MAX];
  snprintf(path, sizeof path, "%s/sys/firmware/devicetree/base/model", api->root);
  if (text_read(path, result->model, sizeof result->model) || !valid_utf8(result->model))
    result->model[0] = 0;
  size_t length = strlen(result->model);
  while (length && (result->model[length - 1] == '\n' || result->model[length - 1] == '\r'))
    result->model[--length] = 0;

  snprintf(path, sizeof path, "%s/proc", api->root);
  DIR *directory = opendir(path);
  int count = 0;
  long pid = 0;
  struct dirent *entry;
  if (directory) {
    while ((entry = readdir(directory))) {
      char *end;
      errno = 0;
      long candidate = strtol(entry->d_name, &end, 10);
      if (errno || end == entry->d_name || *end || candidate <= 0)
        continue;
      /* JUCE renames MPC's main thread; /proc/PID/comm is not its executable identity. */
      snprintf(path, sizeof path, "%s/proc/%ld/exe", api->root, candidate);
      ssize_t n = readlink(path, executable, sizeof executable - 1);
      if (n <= 0 || n == sizeof executable - 1)
        continue;
      executable[n] = 0;
      const char deleted[] = " (deleted)";
      if (
        (size_t)n >= sizeof deleted - 1 && !strcmp(executable + n - (sizeof deleted - 1), deleted))
        executable[n - (sizeof deleted - 1)] = 0;
      const char *base = strrchr(executable, '/');
      base = base ? base + 1 : executable;
      if (!strcmp(base, "MPC")) {
        count++;
        pid = candidate;
      }
    }
    closedir(directory);
  }
  unsigned long long started = 0;
  struct stat before = {0}, after;
  int valid = 0;
  if (count == 1) {
    started = start_time(api->root, pid);
    snprintf(path, sizeof path, "%s/proc/%ld/exe", api->root, pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd >= 0) {
      valid = started && !fstat(fd, &before) && S_ISREG(before.st_mode) &&
        !mpc_elf_build_id(fd, result->build_id) && !stat(path, &after) &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        started == start_time(api->root, pid);
      close(fd);
    }
  }
  if (!valid)
    result->build_id[0] = 0;
  result->process = !directory ? "unreadable"
    : !count                   ? "not_running"
    : count > 1                ? "ambiguous"
    : valid                    ? "running"
                               : "unreadable";
  result->known_build = valid && !strcmp(result->build_id, PROFILE_BUILD_ID);
  int matched = result->known_build && !strcmp(result->model, PROFILE_MODEL);
  if (matched) {
    result->adapter_attached = api->backend
      ? api->backend->probe(api->backend->userdata, result->adapter_id, &result->state_ready)
      : adapter_probe(api->root, pid, result->adapter_id, &result->state_ready);
    result->pid = pid;
    if (
      started != start_time(api->root, pid) || stat(path, &after) ||
      before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
      valid = matched = result->known_build = result->adapter_attached = 0;
      result->adapter_id[0] = result->build_id[0] = 0;
      result->state_ready = 0;
      result->process = "unreadable";
    }
  }
  result->profile = !valid || !result->model[0] ? "unknown" : matched ? "matched" : "unsupported";
  result->unavailable_reason = !strcmp(result->profile, "unknown") ? "NOT_READY"
    : matched                                                      ? "NOT_IMPLEMENTED"
                                                                   : "UNSUPPORTED_FIRMWARE";

  char signature[1024];
  snprintf(
    signature,
    sizeof signature,
    "%d:%ld:%llu:%llu:%llu:%d:%s:%s:%s",
    count,
    pid,
    started,
    (unsigned long long)before.st_dev,
    (unsigned long long)before.st_ino,
    valid,
    result->build_id,
    result->model,
    result->adapter_id);
  if (api->backend_signature[0] && strcmp(api->backend_signature, signature)) {
    if (api_instance_new(api->instance_id))
      return -1;
    api->revision = 0;
  }
  strcpy(api->backend_signature, signature);
  result->commands_ready = result->state_ready && api->backend && api->backend->bind_commands &&
    api->backend->bind_commands(api->backend->userdata, api->instance_id);
  return 0;
}
