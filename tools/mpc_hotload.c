#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ARM-only bounded dlopen/dlclose experiment. Never attaches a UI/audio thread.
 * First run --self-test PAYLOAD.so on a disposable child on the device. */
_Static_assert(sizeof(void *) == 4, "Build for 32-bit ARM");
typedef struct {
  uint32_t r[18];
} arm_regs;
typedef struct {
  uint64_t d[32];
  uint32_t fpscr;
} arm_vfp;
struct trace {
  pid_t pid, tid;
  int mem, attached, saved;
  arm_regs regs;
  arm_vfp vfp;
  uint32_t stack;
  unsigned char stack_bytes[256];
};
static int getregs(pid_t t, arm_regs *r) {
  return ptrace(PTRACE_GETREGS, t, 0, r);
}
static int wait_stop(pid_t t, int timeout_ms) {
  int status;
  for (int n = 0; n < timeout_ms; n += 10) {
    pid_t p = waitpid(t, &status, __WALL | WNOHANG);
    if (p == t)
      return WIFSTOPPED(status) ? WSTOPSIG(status) : -1;
    if (p < 0 && errno != EINTR)
      return -1;
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, 0);
  }
  errno = ETIMEDOUT;
  return -1;
}
static int copy_mem(struct trace *t, uint32_t a, void *b, size_t n, int write_) {
  ssize_t r = write_ ? pwrite(t->mem, b, n, a) : pread(t->mem, b, n, a);
  return r == (ssize_t)n ? 0 : -1;
}
static uintptr_t image_base(pid_t pid, const char *filename) {
  char path[64], line[1024], name[768];
  uintptr_t start, offset, result = 0;
  snprintf(path, sizeof path, "/proc/%d/maps", pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  while (fgets(line, sizeof line, f)) {
    if (
      sscanf(line, "%" SCNxPTR "-%*x %*s %" SCNxPTR " %*s %*s %767s", &start, &offset, name) == 3 &&
      offset == 0 && !strcmp(name, filename)) {
      result = start;
      break;
    }
  }
  fclose(f);
  return result;
}
static uint32_t symbol(struct trace *t, const char *name) {
  void *p = dlsym(RTLD_DEFAULT, name);
  Dl_info info;
  if (!p || !dladdr(p, &info))
    return 0;
  uintptr_t b = image_base(t->pid, info.dli_fname);
  if (!b)
    return 0;
  return (uint32_t)(b + (uintptr_t)p - (uintptr_t)info.dli_fbase);
}
static int call(
  struct trace *t,
  uint32_t f,
  uint32_t a,
  uint32_t b,
  uint32_t c,
  uint32_t d,
  uint32_t e,
  uint32_t g,
  uint32_t *result) {
  if (!f) {
    errno = ENOENT;
    return -1;
  }
  arm_regs r = t->regs;
  r.r[0] = a;
  r.r[1] = b;
  r.r[2] = c;
  r.r[3] = d;
  r.r[13] = t->stack;
  r.r[14] = 0;
  r.r[15] = f & ~1u;
  r.r[16] = (r.r[16] & ~(0x20u | 0x0600fc00u)) | ((f & 1u) ? 0x20u : 0);
  r.r[17] = (uint32_t)-1;
  uint32_t extra[2] = {e, g};
  if (
    copy_mem(t, t->stack, extra, sizeof extra, 1) || ptrace(PTRACE_SETREGS, t->tid, 0, &r) ||
    ptrace(PTRACE_CONT, t->tid, 0, 0))
    return -1;
  int sig = wait_stop(t->tid, 5000);
  if (sig < 0) {
    ptrace(PTRACE_INTERRUPT, t->tid, 0, 0);
    wait_stop(t->tid, 1000);
    return -1;
  }
  if (getregs(t->tid, &r))
    return -1;
  /* LR=0 returns to an unmapped sentinel. Suppress this traced fault on detach. */
  if (sig != SIGSEGV || r.r[15] != 0) {
    fprintf(stderr, "unexpected call stop signal=%d pc=%08x function=%08x\n", sig, r.r[15], f);
    errno = EPROTO;
    return -1;
  }
  *result = r.r[0];
  return 0;
}
static int restore(struct trace *t) {
  int rc = 0;
  if (t->saved) {
    if (copy_mem(t, t->stack, t->stack_bytes, sizeof t->stack_bytes, 1))
      rc = -1;
    if (ptrace(PTRACE_SETVFPREGS, t->tid, 0, &t->vfp))
      rc = -1;
    if (ptrace(PTRACE_SETREGS, t->tid, 0, &t->regs))
      rc = -1;
  }
  if (t->attached && ptrace(PTRACE_DETACH, t->tid, 0, 0))
    rc = -1;
  if (t->mem >= 0)
    close(t->mem);
  return rc;
}
static int allowed(pid_t pid, pid_t tid, int fixture) {
  char path[80], comm[64], exe[256];
  snprintf(path, sizeof path, "/proc/%d/task/%d/comm", pid, tid);
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  int ok = fgets(comm, sizeof comm, f) != NULL;
  fclose(f);
  if (!ok || strcmp(comm, fixture ? "native fixture\n" : "privtap inject\n"))
    return 0;
  if (fixture)
    return pid == tid;
  snprintf(path, sizeof path, "/proc/%d/exe", pid);
  ssize_t n = readlink(path, exe, sizeof exe - 1);
  if (n < 0)
    return 0;
  exe[n] = 0;
  return pid != tid && !strcmp(exe, "/usr/bin/MPC");
}
static int quiescent(pid_t pid) {
  char path[128], comm[64];
  snprintf(path, sizeof path, "/proc/%d/task", pid);
  DIR *dir = opendir(path);
  if (!dir)
    return 0;
  struct dirent *entry;
  int ok = 1;
  while ((entry = readdir(dir))) {
    char *end;
    long tid = strtol(entry->d_name, &end, 10);
    if (!tid || *end)
      continue;
    snprintf(path, sizeof path, "/proc/%d/task/%ld/comm", pid, tid);
    FILE *f = fopen(path, "r");
    if (!f) {
      ok = 0;
      break;
    }
    if (
      !fgets(comm, sizeof comm, f) || !strcmp(comm, "mpc native lab\n") ||
      !strcmp(comm, "mpc executor\n"))
      ok = 0;
    fclose(f);
    if (!ok)
      break;
  }
  closedir(dir);
  if (!ok)
    return 0;
  snprintf(path, sizeof path, "/proc/%d/task/%d/syscall", pid, pid);
  FILE *f = fopen(path, "r");
  unsigned long sc = 0;
  if (!f)
    return 0;
  ok = fscanf(f, "%lu", &sc) == 1 && sc == 168;
  fclose(f);
  return ok;
}
static int
run(pid_t pid, pid_t tid, const char *path, uint32_t unload, int fixture, uint32_t *handle) {
  struct trace t = {.pid = pid, .tid = tid, .mem = -1};
  char proc[80];
  int rc = -1;
  uint32_t scratch = 0, value;
  if (!allowed(pid, tid, fixture)) {
    fprintf(stderr, "refused: expected an idle privtap worker in MPC\n");
    return -1;
  }
  if (unload && !fixture && !quiescent(pid)) {
    fprintf(
      stderr,
      "refused unload: stop the lab/executor workers and wait for the UI poll loop\n");
    return -1;
  }
  snprintf(proc, sizeof proc, "/proc/%d/task/%d/syscall", pid, tid);
  FILE *f = fopen(proc, "r");
  unsigned long sc = 0;
  if (!f || fscanf(f, "%lu", &sc) != 1 || sc != 3) {
    if (f)
      fclose(f);
    fprintf(stderr, "worker is not blocked in read\n");
    return -1;
  }
  fclose(f);
  if (ptrace(PTRACE_SEIZE, tid, 0, 0)) {
    perror("seize");
    return -1;
  }
  t.attached = 1;
  if (
    ptrace(PTRACE_INTERRUPT, tid, 0, 0) || wait_stop(tid, 1000) < 0 || getregs(tid, &t.regs) ||
    ptrace(PTRACE_GETVFPREGS, tid, 0, &t.vfp))
    goto done;
  snprintf(proc, sizeof proc, "/proc/%d/mem", pid);
  t.mem = open(proc, O_RDWR | O_CLOEXEC);
  t.stack = (t.regs.r[13] - 4096u) & ~7u;
  if (t.mem < 0 || copy_mem(&t, t.stack, t.stack_bytes, sizeof t.stack_bytes, 0))
    goto done;
  t.saved = 1;
  if (call(&t, symbol(&t, "getpid"), 0, 0, 0, 0, 0, 0, &value) || value != (uint32_t)pid)
    goto done;
  if (unload) {
    if (call(&t, symbol(&t, "dlclose"), unload, 0, 0, 0, 0, 0, &value) || value)
      goto done;
    rc = 0;
    goto done;
  }
  if (
    call(
      &t,
      symbol(&t, "mmap"),
      0,
      4096,
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS,
      (uint32_t)-1,
      0,
      &scratch) ||
    scratch == (uint32_t)-1) {
    scratch = 0;
    goto done;
  }
  size_t n = strlen(path) + 1;
  if (n > 1024 || copy_mem(&t, scratch, (void *)path, n, 1))
    goto done;
  if (call(&t, symbol(&t, "dlopen"), scratch, RTLD_NOW | RTLD_LOCAL, 0, 0, 0, 0, &value))
    goto done;
  if (!value) {
    uint32_t err = 0;
    if (!call(&t, symbol(&t, "dlerror"), 0, 0, 0, 0, 0, 0, &err) && err) {
      char b[256] = {0};
      if (!copy_mem(&t, err, b, sizeof b - 1, 0))
        fprintf(stderr, "dlopen: %s\n", b);
    }
    goto done;
  }
  *handle = value;
  rc = 0;
done:
  if (scratch && call(&t, symbol(&t, "munmap"), scratch, 4096, 0, 0, 0, 0, &value))
    rc = -1;
  if (restore(&t)) {
    perror("restore/detach");
    rc = -1;
  }
  if (rc)
    perror("hotload");
  return rc;
}
int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "--self-test")) {
    int p[2];
    if (pipe(p))
      return 1;
    pid_t child = fork();
    if (child < 0)
      return 1;
    if (!child) {
      close(p[1]);
      prctl(PR_SET_NAME, "native fixture", 0, 0, 0);
      char b;
      ssize_t n = read(p[0], &b, 1);
      _exit(n == 1 && b == 'Q' ? 42 : 1);
    }
    close(p[0]);
    usleep(100000);
    uint32_t h = 0;
    int loaded = !run(child, child, argv[2], 0, 1, &h) && h;
    fprintf(stderr, "self-test load=%d handle=%08x\n", loaded, h);
    usleep(100000);
    int ok = loaded && !run(child, child, 0, h, 1, &h);
    if (write(p[1], "Q", 1) != 1)
      ok = 0;
    close(p[1]);
    int status;
    waitpid(child, &status, 0);
    ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 42;
    printf("self_test_restored_read=%s\n", ok ? "true" : "false");
    return ok ? 0 : 1;
  }
  if (argc != 5 || (strcmp(argv[1], "load") && strcmp(argv[1], "unload"))) {
    fprintf(
      stderr,
      "usage: hotload --self-test PAYLOAD.so | load PID TID /tmp/PAYLOAD.so | unload PID TID "
      "HEX_HANDLE\n");
    return 2;
  }
  pid_t pid = (pid_t)strtol(argv[2], 0, 10), tid = (pid_t)strtol(argv[3], 0, 10);
  uint32_t h = 0;
  int unloading = !strcmp(argv[1], "unload");
  if (!unloading && (strncmp(argv[4], "/tmp/", 5) || strlen(argv[4]) > 1000))
    return 2;
  if (run(
        pid,
        tid,
        unloading ? 0 : argv[4],
        unloading ? (uint32_t)strtoul(argv[4], 0, 16) : 0,
        0,
        &h))
    return 1;
  printf(
    "{\"pid\":%d,\"tid\":%d,\"action\":\"%s\",\"handle\":\"%08x\",\"restored\":true}\n",
    pid,
    tid,
    argv[1],
    h);
  return 0;
}
