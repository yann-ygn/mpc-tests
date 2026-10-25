#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int mpc_host_directory(const char *runtime) {
  int fd = open(runtime, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  struct stat s;
  if (fd >= 0 && !fstat(fd, &s) && s.st_uid == geteuid() && !(s.st_mode & 0077))
    return fd;
  if (fd >= 0)
    close(fd);
  return -1;
}

int mpc_host_lock(int directory) {
  int fd = openat(directory, "host.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  struct stat s;
  if (
    fd >= 0 && !fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_uid == geteuid() &&
    !flock(fd, LOCK_EX | LOCK_NB))
    return fd;
  if (fd >= 0)
    close(fd);
  return -1;
}

int mpc_host_remove_stale(int directory, const char *name) {
  struct stat s;
  if (fstatat(directory, name, &s, AT_SYMLINK_NOFOLLOW))
    return errno == ENOENT ? 0 : -1;
  /* Only under the lifetime flock. Never replace files, symlinks or a socket
   * belonging to another UID. A second host cannot acquire this lock. */
  return S_ISSOCK(s.st_mode) && s.st_uid == geteuid() ? unlinkat(directory, name, 0) : -1;
}

int mpc_host_cleanup(const char *runtime) {
  struct stat s;
  if (lstat(runtime, &s) && errno == ENOENT)
    return 0;
  int directory = mpc_host_directory(runtime);
  if (directory < 0)
    return 1;
  int lock = mpc_host_lock(directory);
  int result = 1;
  if (lock >= 0) {
    result = mpc_host_remove_stale(directory, "api.sock") |
      mpc_host_remove_stale(directory, "control.sock");
    close(lock);
  }
  close(directory);
  return result ? 1 : 0;
}
