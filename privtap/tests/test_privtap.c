// privtap's host tests: run with privtap.so preloaded over libfakeasound.so (build.sh test). The
// test plays MPC (poll on a wake fd of its own and the private input, then read), the fake
// controller's hardware, and a FIFO writer. With PRIVTAP_TEST_IDENTITY=X it runs the identity tests
// instead (the identity is fixed for a process's life).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct _snd_rawmidi snd_rawmidi_t;
int snd_rawmidi_open(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
int snd_rawmidi_close(snd_rawmidi_t *);
ssize_t snd_rawmidi_read(snd_rawmidi_t *, void *, size_t);
ssize_t snd_rawmidi_write(snd_rawmidi_t *, const void *, size_t);
int snd_rawmidi_nonblock(snd_rawmidi_t *, int);
int snd_rawmidi_poll_descriptors(snd_rawmidi_t *, struct pollfd *, unsigned int);
int fake_hw_fd(int sub);
size_t fake_out(int sub, unsigned char *buf, size_t n);

static char dir[64], fifo[128], logp[128], conf[128];
static const char *identity = "";
static const char DT_CODE[] = "/sys/firmware/devicetree/base/inmusic,product-code";
static const char DT_COMPAT[] = "/sys/firmware/devicetree/base/compatible";
int __open64_2(const char *, int);
static int checks, fails;

#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    checks++;                                                                                      \
    if (!(cond)) {                                                                                 \
      fails++;                                                                                     \
      printf("FAIL line %d: ", __LINE__);                                                          \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
    }                                                                                              \
  } while (0)

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms) {
  usleep((useconds_t)ms * 1000);
}

static size_t unhex(const char *s, unsigned char *out) {
  size_t n = 0;
  unsigned v;
  int k;
  while (sscanf(s, " %2x%n", &v, &k) == 1) {
    out[n++] = (unsigned char)v;
    s += k;
  }
  return n;
}

static const char *tohex(const unsigned char *b, size_t n) {
  static char s[1024];
  s[0] = 0;
  for (size_t i = 0; i < n && i < 300; i++)
    sprintf(s + strlen(s), "%s%02X", i ? " " : "", b[i]);
  return s;
}

static void write_hex(int fd, const char *hex) {
  unsigned char b[256];
  size_t n = unhex(hex, b);
  if (write(fd, b, n) != (ssize_t)n)
    printf("write failed: %s\n", strerror(errno));
}

static void hw(const char *hex) {
  write_hex(fake_hw_fd(1), hex);
} // the hardware sends

static void inj(const char *hex) // a writer, like the remote addin
{
  int fd = open(fifo, O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd < 0) {
    printf("can't open %s: %s\n", fifo, strerror(errno));
    return;
  }
  write_hex(fd, hex);
  close(fd);
}

static int own_wake[2];

// MPC's MIDI thread: poll its own wake fd and the input, read when the input is ready, until quiet
// for ms.
static size_t collect(snd_rawmidi_t *h, int ms, unsigned char *out, size_t max, size_t rdsize) {
  struct pollfd pfd[2] = {{own_wake[0], POLLIN, 0}, {-1, 0, 0}};
  snd_rawmidi_poll_descriptors(h, &pfd[1], 1);
  size_t n = 0;
  while (n < max) {
    pfd[0].revents = pfd[1].revents = 0;
    if (poll(pfd, 2, ms) <= 0)
      break;
    if (pfd[1].revents & POLLIN) {
      ssize_t k = snd_rawmidi_read(h, out + n, rdsize < max - n ? rdsize : max - n);
      if (k > 0)
        n += (size_t)k;
      else if (k != -EAGAIN)
        break;
    }
  }
  return n;
}

static void expect(const char *what, const unsigned char *got, size_t n, const char *want_hex) {
  unsigned char want[256];
  size_t wn = unhex(want_hex, want);
  CHECK(n == wn && !memcmp(got, want, n), "%s: got [%s], want [%s]", what, tohex(got, n), want_hex);
}

static int log_has(const char *needle) {
  FILE *f = fopen(logp, "r");
  char line[512];
  int found = 0;
  while (f && !found && fgets(line, sizeof line, f))
    found = strstr(line, needle) != NULL;
  if (f)
    fclose(f);
  return found;
}

static void write_conf(int log) {
  FILE *f = fopen(conf, "w");
  fprintf(
    f,
    "# test settings\nlog = %d\nlog_path=%s   # trailing comment\ninject=%s\nidentity=%s\n",
    log,
    logp,
    fifo,
    identity);
  fclose(f);
}

static int is_fifo(const char *p) {
  struct stat st;
  return !lstat(p, &st) && S_ISFIFO(st.st_mode);
}

// A timed script for another thread: "w100" waits 100 ms, "h..." the hardware sends, "i..." the
// FIFO gets.
struct script {
  const char *steps[8];
};

static void *run_script(void *arg) {
  struct script *s = arg;
  for (int i = 0; i < 8 && s->steps[i]; i++) {
    const char *st = s->steps[i];
    if (st[0] == 'w')
      sleep_ms(atoi(st + 1));
    else if (st[0] == 'h')
      hw(st + 1);
    else if (st[0] == 'i')
      inj(st + 1);
  }
  return NULL;
}

static size_t
collect_with(snd_rawmidi_t *h, struct script *s, int ms, unsigned char *out, size_t max) {
  pthread_t th;
  pthread_create(&th, NULL, run_script, s);
  size_t n = collect(h, ms, out, max, 64);
  pthread_join(th, NULL);
  return n;
}

struct blocked_read {
  snd_rawmidi_t *h;
  unsigned char buf[64];
  ssize_t r;
  volatile int done;
};

static void *read_once(void *arg) {
  struct blocked_read *b = arg;
  b->r = snd_rawmidi_read(b->h, b->buf, sizeof b->buf);
  b->done = 1;
  return NULL;
}

static size_t read_fd(int fd, char *buf, size_t n) {
  ssize_t r = fd >= 0 ? read(fd, buf, n) : -1;
  if (fd >= 0)
    close(fd);
  return r > 0 ? (size_t)r : 0;
}

static void expect_file_mode(int fd, const char *operation) {
  struct stat status;
  CHECK(fd >= 0, "%s creates an ordinary file", operation);
  if (fd >= 0) {
    CHECK(
      !fstat(fd, &status) && (status.st_mode & 0777) == 0640,
      "%s forwards the mode argument",
      operation);
    close(fd);
  }
}

static void file_hook_tests(void) {
  char path[128];
  mode_t previous_mask = umask(0);
  snprintf(path, sizeof path, "%s/open-mode", dir);
  expect_file_mode(open(path, O_WRONLY | O_CREAT | O_EXCL, 0640), "open");
  snprintf(path, sizeof path, "%s/open64-mode", dir);
  expect_file_mode(open64(path, O_WRONLY | O_CREAT | O_EXCL, 0640), "open64");
  int directory = open(dir, O_RDONLY | O_DIRECTORY);
  CHECK(directory >= 0, "open a directory without a mode argument");
  if (directory >= 0) {
    expect_file_mode(openat(directory, "openat-mode", O_WRONLY | O_CREAT | O_EXCL, 0640), "openat");
    expect_file_mode(
      openat64(directory, "openat64-mode", O_WRONLY | O_CREAT | O_EXCL, 0640),
      "openat64");
    close(directory);
  }
  umask(previous_mask);
}

// identity=X: the device tree reads as an MPC X's, and the controller's family byte is swapped both
// ways.
static void identity_tests(void) {
  unsigned char got[1024];
  char buf[128];
  size_t n;
  static const char want_compat[] =
    "inmusic,acv5\0inmusic,az01\0rockchip,rk3288"; // sizeof counts the last NUL

  // Whichever open MPC or libaz0x-info uses.
  n = read_fd(open(DT_CODE, O_RDONLY), buf, sizeof buf);
  CHECK(n == 5 && !memcmp(buf, "ACV5", 5), "open: the product code read %zu bytes", n);
  n = read_fd(open64(DT_CODE, O_RDONLY | O_CLOEXEC), buf, sizeof buf);
  CHECK(n == 5 && !memcmp(buf, "ACV5", 5), "open64: the product code read %zu bytes", n);
  n = read_fd(__open64_2(DT_CODE, O_RDONLY), buf, sizeof buf);
  CHECK(n == 5 && !memcmp(buf, "ACV5", 5), "__open64_2: the product code read %zu bytes", n);
  n = read_fd(openat64(AT_FDCWD, DT_CODE, O_RDONLY), buf, sizeof buf);
  CHECK(n == 5 && !memcmp(buf, "ACV5", 5), "openat64: the product code read %zu bytes", n);
  n = read_fd(openat(AT_FDCWD, DT_COMPAT, O_RDONLY), buf, sizeof buf);
  CHECK(
    n == sizeof want_compat && !memcmp(buf, want_compat, n),
    "openat: compatible read %zu bytes",
    n);
  FILE *f = fopen64(DT_COMPAT, "r");
  n = f ? fread(buf, 1, sizeof buf, f) : 0;
  if (f)
    fclose(f);
  CHECK(
    n == sizeof want_compat && !memcmp(buf, want_compat, n),
    "fopen64: compatible read %zu bytes",
    n);
  f = fopen(DT_CODE, "re");
  n = f ? fread(buf, 1, sizeof buf, f) : 0;
  if (f)
    fclose(f);
  CHECK(n == 5 && !memcmp(buf, "ACV5", 5), "fopen: the product code read %zu bytes", n);
  int fd = open(DT_CODE, O_RDWR);
  CHECK(fd < 0, "opening the product code to write isn't answered from memory");
  if (fd >= 0)
    close(fd);
  errno = 0;
  CHECK(
    open("/sys/firmware/devicetree/base/no-such-file", O_RDONLY) < 0 && errno == ENOENT,
    "other files: untouched");

  snd_rawmidi_t *pin, *pubout;
  CHECK(snd_rawmidi_open(&pin, NULL, "hw:9,0,1", 0) == 0, "open private in");
  CHECK(snd_rawmidi_open(NULL, &pubout, "hw:9,0,0", 0) == 0, "open public out");

  hw("F0 47 7F 57 53 00 01 01 F7");
  n = collect(pin, 200, got, sizeof got, 64);
  expect("Akai sysex before the identity reply: as it is", got, n, "F0 47 7F 57 53 00 01 01 F7");

  hw("F0 7E 00 06 02 47 57 00 19 00 00 03 00 7F 41 31 32 F7");
  n = collect(pin, 200, got, sizeof got, 64);
  expect(
    "the identity reply: the X's family and version",
    got,
    n,
    "F0 7E 00 06 02 47 3A 00 19 00 01 01 01 7F 41 31 32 F7");

  hw("F0 47 7F 57 53 00 05 6E 6B 2D 79 01 F7");
  n = collect(pin, 200, got, sizeof got, 64);
  expect(
    "Akai sysex from the hardware: the X's family",
    got,
    n,
    "F0 47 7F 3A 53 00 05 6E 6B 2D 79 01 F7");

  hw("F0 47");
  struct script s1 = {{"w100", "h7F 57 53 F7"}};
  n = collect_with(pin, &s1, 400, got, sizeof got);
  expect("a sysex split across reads", got, n, "F0 47 7F 3A 53 F7");

  hw("F0 47 F8 7F 57 53 F7");
  n = collect(pin, 200, got, sizeof got, 64);
  expect("real time inside a sysex", got, n, "F0 47 F8 7F 3A 53 F7");

  hw("F0 41 7F 57 F7 90 57 7F");
  n = collect(pin, 200, got, sizeof got, 64);
  expect("other sysex and notes: untouched", got, n, "F0 41 7F 57 F7 90 57 7F");

  unsigned char pad[] = {0xF0, 0x47, 0x7F, 0x3A, 0x65, 0x00, 0x04, 0x00, 0x7F, 0x00, 0x00, 0xF7};
  CHECK(snd_rawmidi_write(pubout, pad, sizeof pad) == (ssize_t)sizeof pad, "write");
  n = fake_out(0, got, sizeof got);
  expect("MPC's sysex: the hardware's family", got, n, "F0 47 7F 57 65 00 04 00 7F 00 00 F7");
  unsigned char a[] = {0xF0, 0x47, 0x7F}, b[] = {0x3A, 0x65, 0x00, 0xF7},
                other[] = {0xF0, 0x00, 0x20, 0x3A, 0xF7};
  snd_rawmidi_write(pubout, a, sizeof a);
  snd_rawmidi_write(pubout, b, sizeof b);
  snd_rawmidi_write(pubout, other, sizeof other);
  n = fake_out(0, got, sizeof got);
  expect(
    "split across writes; other sysex untouched",
    got,
    n,
    "F0 47 7F 57 65 00 F7 F0 00 20 3A F7");

  CHECK(log_has("# identity: passing this device off as X (ACV5, family 3A)"), "log: the identity");
  CHECK(
    log_has("# identity: /sys/firmware/devicetree/base/inmusic,product-code read as X's"),
    "log: product code");
  CHECK(
    log_has("# identity: the hardware is family 57, shown to MPC as 3A"),
    "log: the hardware's family");
  CHECK(log_has(", identity X"), "log: hello");
  CHECK(
    log_has(" out Fake Public: F0 47 7F 3A 65 00 04 00 7F 00 00 F7"),
    "log: MPC's sysex as MPC sent it");
  CHECK(
    log_has(" in  Fake Private: F0 7E 00 06 02 47 57 00 19"),
    "log: the hardware's reply as it sent it");
}

int main(void) {
  unsigned char got[1024];
  size_t n;
  snprintf(dir, sizeof dir, "/tmp/privtap-test-XXXXXX");
  if (!mkdtemp(dir))
    return 2;
  snprintf(fifo, sizeof fifo, "%s/midi_inject", dir);
  snprintf(logp, sizeof logp, "%s/privtap.log", dir);
  snprintf(conf, sizeof conf, "%s/privtap.conf", dir);
  if (getenv("PRIVTAP_TEST_IDENTITY"))
    identity = getenv("PRIVTAP_TEST_IDENTITY");
  write_conf(1);
  setenv("PRIVTAP_CONF", conf, 1);
  file_hook_tests();
  if (pipe(own_wake))
    return 2;
  if (identity[0]) {
    identity_tests();
    goto done;
  }

  // Without an identity, the device tree is the device's own.
  char dt[16];
  int dtfd = open(DT_CODE, O_RDONLY);
  size_t dtn = dtfd >= 0 ? read_fd(dtfd, dt, sizeof dt) : 0;
  CHECK(
    dtfd >= 0 ? dtn < 4 || memcmp(dt, "ACV5", 4) : errno == ENOENT,
    "no identity: the product code isn't faked");

  // The FIFO exists while a process has the private input open, and goes when it exits.
  pid_t pid = fork();
  if (pid == 0) {
    snd_rawmidi_t *in;
    if (snd_rawmidi_open(&in, NULL, "hw:9,0,1", 0))
      exit(3);
    exit(is_fifo(fifo) ? 0 : 4);
  }
  int status = -1;
  waitpid(pid, &status, 0);
  CHECK(
    WIFEXITED(status) && WEXITSTATUS(status) == 0,
    "child: the FIFO wasn't there (status %d)",
    status);
  CHECK(!is_fifo(fifo), "the FIFO is still there after the child exited");

  snd_rawmidi_t *pin, *pout, *pubin, *pubout;
  CHECK(snd_rawmidi_open(&pin, NULL, "hw:9,0,1", 0) == 0, "open private in");
  CHECK(snd_rawmidi_open(NULL, &pout, "hw:9,0,1", 0) == 0, "open private out");
  CHECK(snd_rawmidi_open(&pubin, &pubout, "hw:9,0,0", 2) == 0, "open public in+out");
  CHECK(is_fifo(fifo), "the FIFO was created");
  CHECK(
    snd_rawmidi_open(&pubin, NULL, "hw:7,0,0", 0) == -ENOENT,
    "a failed open passes its error on");

  hw("90 30 7F 90 30 00");
  n = collect(pin, 200, got, sizeof got, 64);
  expect("hardware passes through", got, n, "90 30 7F 90 30 00");

  hw("90 30 7F 31 7F 31 00");
  n = collect(pin, 200, got, sizeof got, 64);
  expect("running status written out", got, n, "90 30 7F 90 31 7F 90 31 00");

  hw("90 30 7F 90 31 7F");
  n = collect(pin, 200, got, sizeof got, 1);
  expect("one byte per read", got, n, "90 30 7F 90 31 7F");

  hw("90 F8 30 7F");
  n = collect(pin, 200, got, sizeof got, 64);
  expect("real time inside a message", got, n, "F8 90 30 7F");

  hw("F0 01 F7 05 B0 10 20");
  n = collect(pin, 200, got, sizeof got, 64);
  expect("sysex, a stray data byte, a CC", got, n, "F0 01 F7 05 B0 10 20");

  // A poll() already waiting wakes for an injected message.
  struct script s1 = {{"w100", "i90 52 7F"}};
  long long t0 = now_ms();
  pthread_t th;
  pthread_create(&th, NULL, run_script, &s1);
  struct pollfd pfd[2] = {{own_wake[0], POLLIN, 0}, {-1, 0, 0}};
  snd_rawmidi_poll_descriptors(pin, &pfd[1], 1);
  int pr = poll(pfd, 2, 2000);
  long long waited = now_ms() - t0;
  pthread_join(th, NULL);
  CHECK(
    pr == 1 && (pfd[1].revents & POLLIN) && !pfd[0].revents,
    "poll: %d, revents %#x / %#x",
    pr,
    pfd[0].revents,
    pfd[1].revents);
  CHECK(waited >= 90 && waited < 1000, "poll woke after %lld ms", waited);
  n = collect(pin, 200, got, sizeof got, 64);
  expect("injected", got, n, "90 52 7F");

  // So does a blocking read with no poll before it.
  struct blocked_read br = {pin, {0}, 0, 0};
  pthread_create(&th, NULL, read_once, &br);
  sleep_ms(100);
  CHECK(!br.done, "the read waited");
  inj("90 53 7F 90 53 00");
  for (int i = 0; i < 100 && !br.done; i++)
    sleep_ms(10);
  CHECK(br.done, "a blocking read woke for an injected message");
  if (!br.done) {
    hw("FE"); // let it go
    pthread_join(th, NULL);
  } else
    pthread_join(th, NULL);
  n = br.r > 0 ? (size_t)br.r : 0;
  memcpy(got, br.buf, n);
  n += collect(pin, 200, got + n, sizeof got - n, 64);
  expect("injected into a blocking read", got, n, "90 53 7F 90 53 00");

  // An injected message never splits the hardware's.
  hw("90 30");
  struct script s2 = {{"w100", "i90 52 7F", "w100", "h7F"}};
  n = collect_with(pin, &s2, 600, got, sizeof got);
  expect("injected mid-message waits", got, n, "90 30 7F 90 52 7F");

  hw("F0 00 20 6B");
  struct script s3 = {{"w100", "i90 52 7F", "w100", "h01 F7"}};
  n = collect_with(pin, &s3, 600, got, sizeof got);
  expect("injected mid-sysex waits", got, n, "F0 00 20 6B 01 F7 90 52 7F");

  hw("90 30 7F 31");
  struct script s4 = {{"w100", "i90 52 7F", "w100", "h7F"}};
  n = collect_with(pin, &s4, 600, got, sizeof got);
  expect("injected inside running status", got, n, "90 30 7F 90 31 7F 90 52 7F");

  inj("90 52 7F 52 00 F8 B0 64 01");
  n = collect(pin, 300, got, sizeof got, 64);
  expect("the writer's running status and real time", got, n, "90 52 7F 90 52 00 F8 B0 64 01");

  inj("F0 7E 7F 06 01 F7");
  n = collect(pin, 300, got, sizeof got, 64);
  expect("injected sysex", got, n, "F0 7E 7F 06 01 F7");

  // Non-blocking mode.
  snd_rawmidi_nonblock(pin, 1);
  CHECK(
    snd_rawmidi_read(pin, got, sizeof got) == -EAGAIN,
    "non-blocking read with nothing: -EAGAIN");
  hw("90 30");
  CHECK(
    snd_rawmidi_read(pin, got, sizeof got) == -EAGAIN,
    "non-blocking read of half a message: -EAGAIN");
  inj("90 52 7F");
  hw("7F");
  n = collect(pin, 300, got, sizeof got, 64);
  expect("non-blocking", got, n, "90 30 7F 90 52 7F");
  snd_rawmidi_nonblock(pin, 0);

  // MPC's writes reach the hardware unchanged, and are logged.
  unsigned char cc[3] = {0xB0, 0x10, 0x7F};
  CHECK(snd_rawmidi_write(pubout, cc, 3) == 3, "write");
  n = fake_out(0, got, sizeof got);
  expect("write passes through", got, n, "B0 10 7F");
  CHECK(snd_rawmidi_write(pout, cc, 3) == 3, "write private");

  // The log.
  CHECK(log_has("# privtap "), "log: hello");
  CHECK(
    log_has("# open hw:9,0,1 in: 'Fake Key' / 'Fake Private', fd") &&
      log_has(": the private input"),
    "log: the private input's open");
  CHECK(
    log_has("# open hw:9,0,0 out: 'Fake Key' / 'Fake Public'"),
    "log: the public output's open");
  CHECK(log_has("# inject: reading "), "log: inject");
  CHECK(log_has(" in  Fake Private: 90 30 7F 31 7F 31 00"), "log: the hardware's bytes as read");
  CHECK(log_has(" out Fake Public: B0 10 7F"), "log: an output");
  CHECK(log_has(" out Fake Private: B0 10 7F"), "log: the private output");
  char needle[200];
  snprintf(needle, sizeof needle, " inj %s: 90 52 7F 52 00 F8 B0 64 01", fifo);
  CHECK(log_has(needle), "log: injected bytes");

  // The settings are re-read when they change.
  sleep_ms(1100);
  write_conf(0);
  sleep_ms(1100);
  hw("90 41 7F");
  n = collect(pin, 200, got, sizeof got, 64);
  expect("still passes through with the log off", got, n, "90 41 7F");
  CHECK(!log_has("90 41 7F"), "log off: no traffic");
  write_conf(1);
  sleep_ms(1100);
  hw("90 42 7F");
  n = collect(pin, 200, got, sizeof got, 64);
  CHECK(log_has(" in  Fake Private: 90 42 7F"), "log back on");

  // Closed: injection has nowhere to go, and says so.
  CHECK(snd_rawmidi_close(pin) == 0, "close");
  inj("90 52 7F");
  sleep_ms(200);
  CHECK(log_has("# close Fake Private"), "log: close");
  CHECK(
    log_has("inject: 3 bytes dropped (the private input isn't open)"),
    "log: dropped after close");

done:
  printf("%d checks, %d failed%s%s\n", checks, fails, identity[0] ? ", identity " : "", identity);
  if (!fails) {
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
      return 1;
  } else
    printf("log: %s\n", logp);
  return fails ? 1 : 0;
}
