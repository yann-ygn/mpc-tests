#define _GNU_SOURCE
#include "interpose.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// The file opens MPC and libaz0x-info use. Only reading the two device-tree files is ever answered
// differently.
static int (*libc_open)(const char *, int, ...);
static int (*libc_open64)(const char *, int, ...);
static int (*libc_open_2)(const char *, int);
static int (*libc_open64_2)(const char *, int);
static int (*libc_openat)(int, const char *, int, ...);
static int (*libc_openat64)(int, const char *, int, ...);
static FILE *(*libc_fopen)(const char *, const char *);
static FILE *(*libc_fopen64)(const char *, const char *);

#define READ_ONLY(flags) (((flags) & O_ACCMODE) == O_RDONLY && !((flags) & O_CREAT))
#define NEEDS_MODE(flags) (((flags) & O_CREAT) || ((flags) & O_TMPFILE) == O_TMPFILE)
static mode_t open_mode(int flags, va_list arguments) {
  return NEEDS_MODE(flags) ? (mode_t)va_arg(arguments, int) : 0;
}

typedef int (*open_function)(const char *, int, ...);
typedef int (*openat_function)(int, const char *, int, ...);

static int
open_file(const char *path, int flags, mode_t mode, open_function *original, const char *symbol) {
  int fd;
  if (READ_ONLY(flags) && (fd = privtap_identity_fd(path)) != -2) {
    return fd;
  }
  if (!RESOLVE(*original, symbol)) {
    errno = ENOSYS;
    return -1;
  }
  return (*original)(path, flags, mode);
}

static int open_file_at(
  int directory,
  const char *path,
  int flags,
  mode_t mode,
  openat_function *original,
  const char *symbol) {
  int fd;
  if (READ_ONLY(flags) && (fd = privtap_identity_fd(path)) != -2) {
    return fd;
  }
  if (!RESOLVE(*original, symbol)) {
    errno = ENOSYS;
    return -1;
  }
  return (*original)(directory, path, flags, mode);
}

API int open(const char *path, int flags, ...) {
  va_list arguments;
  va_start(arguments, flags);
  mode_t mode = open_mode(flags, arguments);
  va_end(arguments);
  return open_file(path, flags, mode, &libc_open, "open");
}

API int open64(const char *path, int flags, ...) {
  va_list arguments;
  va_start(arguments, flags);
  mode_t mode = open_mode(flags, arguments);
  va_end(arguments);
  return open_file(path, flags, mode, &libc_open64, "open64");
}

API int openat(int directory, const char *path, int flags, ...) {
  va_list arguments;
  va_start(arguments, flags);
  mode_t mode = open_mode(flags, arguments);
  va_end(arguments);
  return open_file_at(directory, path, flags, mode, &libc_openat, "openat");
}

API int openat64(int directory, const char *path, int flags, ...) {
  va_list arguments;
  va_start(arguments, flags);
  mode_t mode = open_mode(flags, arguments);
  va_end(arguments);
  return open_file_at(directory, path, flags, mode, &libc_openat64, "openat64");
}

// The fortified forms (_FORTIFY_SOURCE), which take no mode.
API int __open_2(const char *path, int flags) {
  int fd;
  if (READ_ONLY(flags) && (fd = privtap_identity_fd(path)) != -2)
    return fd;
  if (!RESOLVE(libc_open_2, "__open_2")) {
    errno = ENOSYS;
    return -1;
  }
  return libc_open_2(path, flags);
}

API int __open64_2(const char *path, int flags) {
  int fd;
  if (READ_ONLY(flags) && (fd = privtap_identity_fd(path)) != -2)
    return fd;
  if (!RESOLVE(libc_open64_2, "__open64_2")) {
    errno = ENOSYS;
    return -1;
  }
  return libc_open64_2(path, flags);
}

static FILE *fopen_hook(
  const char *path, const char *mode, FILE *(**libc)(const char *, const char *), const char *sym) {
  int fd;
  if (mode && mode[0] == 'r' && !strchr(mode, '+') && (fd = privtap_identity_fd(path)) != -2) {
    FILE *f = fdopen(fd, "r");
    if (!f)
      close(fd);
    return f;
  }
  if (!RESOLVE(*libc, sym)) {
    errno = ENOSYS;
    return NULL;
  }
  return (*libc)(path, mode);
}

API FILE *fopen(const char *path, const char *mode) {
  return fopen_hook(path, mode, &libc_fopen, "fopen");
}
API FILE *fopen64(const char *path, const char *mode) {
  return fopen_hook(path, mode, &libc_fopen64, "fopen64");
}
