/* Controller interposition: port tracking, config/logging, FIFO worker and poll wakeups.
 * MIDI parsing/merging lives in midi_stream.c, identity rewriting in identity.c,
 * and libc entry points in file_hooks.c. See ../README.md for settings and protocol details.
 *
 * midi_lock protects ports, merger state and identity streams. The FIFO parser
 * belongs to the injection thread. Traffic logs keep the original hardware/MPC
 * bytes, before identity rewriting; injected messages merge only at message boundaries.
 */

#define _GNU_SOURCE
#include "interpose.h"
#include "identity.h"
#include "midi_stream.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PRIVTAP_VERSION "0.2.0"

typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_rawmidi_info snd_rawmidi_info_t;

#define SND_RAWMIDI_NONBLOCK 0x0002
#define MAXPORTS 16
#define LINE_BYTES 48 // bytes per log line

static int (*real_open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
static int (*real_close)(snd_rawmidi_t *);
static ssize_t (*real_read)(snd_rawmidi_t *, void *, size_t);
static ssize_t (*real_write)(snd_rawmidi_t *, const void *, size_t);
static int (*real_nonblock)(snd_rawmidi_t *, int);
static int (*real_poll)(struct pollfd *, nfds_t, int);

struct port {
  snd_rawmidi_t *handle;
  int output, fd, nonblocking;
  char name[64];                   // the subdevice's name, e.g. "MPC Private"
  struct identity_stream identity; // MPC's writes, for the identity rewrites
};

static char identity_name[16];
static const struct identity_profile *selected_identity; // NULL: the device is itself
static pthread_once_t identity_once = PTHREAD_ONCE_INIT;
static int hardware_family = -1; // the hardware's own family byte, from its first identity reply
static struct identity_stream hardware_identity_stream = {
  -1, {0}, 0}; // the hardware's stream (under lock)

static pthread_mutex_t midi_lock =
  PTHREAD_MUTEX_INITIALIZER; // the ports, the queues and the parser
static struct port ports[MAXPORTS];
static snd_rawmidi_t *private_input;
static int private_input_fd = -1;
static int wake_pipe[2] = {-1, -1};
static struct midi_merger midi;

// Settings and the log
static pthread_mutex_t conf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t conf_once = PTHREAD_ONCE_INIT;
static char conf_path[512];
static struct timespec conf_mtime;
static long long conf_checked = -1000000;
static volatile int log_traffic;
static char log_path[256] = "/tmp/privtap.log";
static long log_max = 4096L * 1024;
static int log_fd = -1;
static long log_size;
static char inject_path[256] = "/dev/midi_inject";

// The FIFO
static pthread_once_t inject_once = PTHREAD_ONCE_INIT;
static char fifo_path[256];
static pid_t fifo_owner;

// libc's own dlsym, fetched by version: another preloaded library may export a dlsym of its own
// (the commander addin does), and dlsym(RTLD_NEXT) then resolves relative to that library instead
// of this one (mpc-vst-plugins NOTES, 2026-10-13; the same fix as hwremap's).
void *privtap_next_symbol(const char *name) {
  static void *(*libc_dlsym)(void *, const char *);
  if (!libc_dlsym) {
    static const char *const vers[] =
      {"GLIBC_2.34", "GLIBC_2.4", "GLIBC_2.17", "GLIBC_2.2.5", "GLIBC_2.0"};
    for (size_t i = 0; i < sizeof vers / sizeof *vers && !libc_dlsym; i++)
      libc_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_DEFAULT, "dlsym", vers[i]);
  }
  return (libc_dlsym ? libc_dlsym : dlsym)(RTLD_NEXT, name);
}

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- log

static void log_write(const char *s, size_t n) // under log_lock
{
  if (log_fd < 0) {
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0)
      return;
    struct stat st;
    log_size = fstat(log_fd, &st) ? 0 : (long)st.st_size;
  }
  if (log_size + (long)n > log_max) {
    char old[sizeof log_path + 2];
    snprintf(old, sizeof old, "%s.1", log_path);
    close(log_fd);
    rename(log_path, old);
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_TRUNC | O_CLOEXEC, 0644);
    log_size = 0;
    if (log_fd < 0)
      return;
  }
  ssize_t w = write(log_fd, s, n);
  if (w > 0)
    log_size += w;
}

static int stamp(char *buf, size_t n) {
  long long t = now_ms();
  return snprintf(buf, n, "%lld.%03lld", t / 1000, t % 1000);
}

// An event: logged whether or not the traffic is.
static void log_event(const char *fmt, ...) {
  char buf[600];
  int n = stamp(buf, sizeof buf);
  n += snprintf(buf + n, sizeof buf - (size_t)n, " # ");
  va_list ap;
  va_start(ap, fmt);
  int k = vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
  va_end(ap);
  n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
  buf[n++] = '\n';
  pthread_mutex_lock(&log_lock);
  log_write(buf, (size_t)n);
  pthread_mutex_unlock(&log_lock);
}

static void log_bytes(const char *dir, const char *name, const unsigned char *b, size_t n) {
  static const char hex[] = "0123456789ABCDEF";
  if (!log_traffic)
    return;
  char line[160 + 3 * LINE_BYTES];
  pthread_mutex_lock(&log_lock);
  for (size_t off = 0; off < n; off += LINE_BYTES) {
    size_t k = n - off < LINE_BYTES ? n - off : LINE_BYTES;
    int len = stamp(line, sizeof line);
    len += snprintf(line + len, sizeof line - (size_t)len, " %-3s %.100s:", dir, name);
    for (size_t i = 0; i < k; i++) {
      line[len++] = ' ';
      line[len++] = hex[b[off + i] >> 4];
      line[len++] = hex[b[off + i] & 15];
    }
    line[len++] = '\n';
    log_write(line, (size_t)len);
  }
  pthread_mutex_unlock(&log_lock);
}

// ---- settings

static void find_conf(void) {
  const char *env = getenv("PRIVTAP_CONF");
  Dl_info di;
  if (env && *env)
    snprintf(conf_path, sizeof conf_path, "%s", env);
  else if (dladdr((void *)find_conf, &di) && di.dli_fname && strrchr(di.dli_fname, '/'))
    snprintf(
      conf_path,
      sizeof conf_path,
      "%.*s/privtap.conf",
      (int)(strrchr(di.dli_fname, '/') - di.dli_fname),
      di.dli_fname);
}

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
    *--e = 0;
  return s;
}

static void load_conf(void) {
  FILE *f = fopen(conf_path, "re");
  if (!f)
    return;
  char line[400];
  while (fgets(line, sizeof line, f)) {
    char *hash = strchr(line, '#'), *eq;
    if (hash)
      *hash = 0;
    if (!(eq = strchr(line, '=')))
      continue;
    *eq = 0;
    char *k = trim(line), *v = trim(eq + 1);
    if (!strcmp(k, "log"))
      log_traffic = atoi(v) != 0;
    else if (!strcmp(k, "log_max_kb"))
      log_max = (atol(v) < 64 ? 64 : atol(v)) * 1024;
    else if (!strcmp(k, "inject"))
      snprintf(inject_path, sizeof inject_path, "%s", v);
    else if (!strcmp(k, "identity"))
      snprintf(identity_name, sizeof identity_name, "%s", v);
    else if (!strcmp(k, "log_path") && *v && strcmp(v, log_path)) {
      pthread_mutex_lock(&log_lock);
      snprintf(log_path, sizeof log_path, "%s", v);
      if (log_fd >= 0)
        close(log_fd);
      log_fd = -1;
      pthread_mutex_unlock(&log_lock);
    }
  }
  fclose(f);
}

// Re-read the settings if they changed: looked at once a second at most, unless forced.
static void refresh_config(int force) {
  pthread_once(&conf_once, find_conf);
  if (force)
    pthread_mutex_lock(&conf_lock);
  else if (pthread_mutex_trylock(&conf_lock))
    return;
  long long t = now_ms();
  struct stat st;
  if (
    (force || t - conf_checked >= 1000) && conf_path[0] && !stat(conf_path, &st) &&
    (force || st.st_mtim.tv_sec != conf_mtime.tv_sec || st.st_mtim.tv_nsec != conf_mtime.tv_nsec)) {
    conf_mtime = st.st_mtim;
    load_conf();
  }
  if (force || t - conf_checked >= 1000)
    conf_checked = t;
  pthread_mutex_unlock(&conf_lock);
}

static struct port *find_port(snd_rawmidi_t *handle) {
  for (int i = 0; i < MAXPORTS; i++)
    if (handle && ports[i].handle == handle)
      return &ports[i];
  return NULL;
}

static void wake_reader(void) {
  char c = 1;
  if (wake_pipe[1] >= 0 && write(wake_pipe[1], &c, 1) < 0) {
    // full: the reader is already due to wake
  }
}

static void drain_wake_pipe(int fd) {
  char buf[64];
  while (read(fd, buf, sizeof buf) > 0)
    ;
}

// ---- injection

static void queue_injected_message(const unsigned char *bytes, size_t length) {
  pthread_mutex_lock(&midi_lock);
  int queued = private_input && midi_merger_queue_injected(&midi, bytes, length);
  pthread_mutex_unlock(&midi_lock);
  if (queued) {
    wake_reader();
  } else {
    log_event(
      "inject: %zu bytes dropped (%s)",
      length,
      private_input ? "queue full" : "the private input isn't open");
  }
}

static struct midi_injection_parser injection_parser;

static void process_injected_byte(unsigned char byte) {
  struct midi_message message;
  enum midi_parse_result result = midi_parse_injected_byte(&injection_parser, byte, &message);
  if (result == MIDI_PARSE_MESSAGE) {
    queue_injected_message(message.bytes, message.length);
  } else if (result == MIDI_PARSE_OVERSIZED_SYSEX) {
    log_event("inject: a sysex over %d bytes was dropped", MIDI_MESSAGE_CAPACITY);
  }
}

static void *inject_thread(void *arg) {
  int fd = (int)(long)arg;
  unsigned char buf[256];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      log_event("inject: reading %s stopped: %s", fifo_path, n ? strerror(errno) : "end of file");
      break;
    }
    log_bytes("inj", fifo_path, buf, (size_t)n);
    for (ssize_t i = 0; i < n; i++)
      process_injected_byte(buf[i]);
  }
  close(fd);
  return NULL;
}

static void inject_setup(void) {
  struct stat st;
  snprintf(fifo_path, sizeof fifo_path, "%s", inject_path);
  if (!fifo_path[0] || !strcmp(fifo_path, "none")) {
    log_event("inject: off");
    return;
  }
  int made = 0;
  if (!lstat(fifo_path, &st)) {
    if (!S_ISFIFO(st.st_mode)) {
      log_event("inject: %s exists and isn't a FIFO, so there is no injection", fifo_path);
      return;
    }
  } else if (!mkfifo(fifo_path, 0600))
    made = 1;
  else {
    log_event("inject: can't create %s: %s", fifo_path, strerror(errno));
    return;
  }
  // Read-write: opening doesn't wait for a writer, and a writer going away isn't an end of file.
  int fd = open(fifo_path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    log_event("inject: can't open %s: %s", fifo_path, strerror(errno));
    return;
  }
  // Ours from here on, whoever made it: removed at exit, since a FIFO nobody reads blocks whoever
  // opens it to write.
  fifo_owner = getpid();
  pthread_t th;
  pthread_attr_t at;
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old); // MPC's signals stay on MPC's threads
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 128 * 1024);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  int err = pthread_create(&th, &at, inject_thread, (void *)(long)fd);
  pthread_attr_destroy(&at);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (err) {
    log_event("inject: no thread: %s", strerror(err));
    close(fd);
    return;
  }
  pthread_setname_np(th, "privtap inject");
  log_event("inject: reading %s%s", fifo_path, made ? " (created)" : "");
}

__attribute__((destructor)) static void privtap_fini(void) {
  if (fifo_owner && fifo_owner == getpid())
    unlink(fifo_path);
}

// ---- identity

static void select_identity(void) {
  refresh_config(1);
  selected_identity = identity_lookup(identity_name);
  if (selected_identity)
    log_event(
      "identity: passing this device off as %s (%s, family %02X)",
      selected_identity->name,
      selected_identity->product_code,
      selected_identity->family);
  else if (identity_name[0] && strcasecmp(identity_name, "none"))
    log_event(
      "identity: '%s' isn't X, LIVE, LIVE2, ONE or FORCE: the device stays itself",
      identity_name);
}

#define DT "/sys/firmware/devicetree/base/"

// An fd reading the spoofed model's product code or compatible string, if path is one of those
// files and an identity is set; else -2.
int privtap_identity_fd(const char *path) {
  static int told[2];
  if (!path || strncmp(path, DT, sizeof DT - 1))
    return -2;
  int code = !strcmp(path + sizeof DT - 1, "inmusic,product-code");
  if (!code && strcmp(path + sizeof DT - 1, "compatible"))
    return -2;
  pthread_once(&identity_once, select_identity);
  if (!selected_identity)
    return -2;
  // Device-tree strings end in a NUL, and a list of them is NUL-separated.
  char buf[96];
  int n = code ? sprintf(buf, "%s", selected_identity->product_code) + 1 : 0;
  if (!code) {
    n += sprintf(buf + n, "%s", selected_identity->compatible) + 1;
    n += sprintf(buf + n, "inmusic,az01") + 1;
    n += sprintf(buf + n, "rockchip,rk3288") + 1;
  }
  int fd = memfd_create(code ? "product-code" : "compatible", MFD_CLOEXEC);
  if (fd < 0 || write(fd, buf, (size_t)n) != n || lseek(fd, 0, SEEK_SET)) {
    if (fd >= 0)
      close(fd);
    log_event("identity: can't fake %s: %s", path, strerror(errno));
    return -2;
  }
  if (!told[code]) {
    told[code] = 1;
    log_event("identity: %s read as %s's", path, selected_identity->name);
  }
  return fd;
}

static unsigned char
rewrite_identity_byte(struct identity_stream *stream, unsigned char byte, bool incoming) {
  int previous_family = hardware_family;
  unsigned char rewritten =
    identity_rewrite_byte(selected_identity, stream, &hardware_family, byte, incoming);
  if (previous_family < 0 && hardware_family >= 0) {
    log_event(
      "identity: the hardware is family %02X, shown to MPC as %02X",
      hardware_family,
      selected_identity->family);
  }
  return rewritten;
}

// ---- the hooks

static void info_names(snd_rawmidi_t *handle, char *dev, char *sub, size_t n) {
  size_t (*info_sizeof)(void) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info_sizeof");
  int (*info)(snd_rawmidi_t *, snd_rawmidi_info_t *) = dlsym(RTLD_DEFAULT, "snd_rawmidi_info");
  const char *(*get_name)(const snd_rawmidi_info_t *) =
    dlsym(RTLD_DEFAULT, "snd_rawmidi_info_get_name");
  const char *(*get_sub)(const snd_rawmidi_info_t *) =
    dlsym(RTLD_DEFAULT, "snd_rawmidi_info_get_subdevice_name");
  dev[0] = sub[0] = 0;
  if (!info_sizeof || !info || !get_name || !get_sub)
    return;
  snd_rawmidi_info_t *ri = calloc(1, info_sizeof());
  if (ri && !info(handle, ri)) {
    snprintf(dev, n, "%s", get_name(ri) ? get_name(ri) : "");
    snprintf(sub, n, "%s", get_sub(ri) ? get_sub(ri) : "");
  }
  free(ri);
}

static int poll_fd(snd_rawmidi_t *handle) {
  int (*pdesc)(snd_rawmidi_t *, struct pollfd *, unsigned int) =
    dlsym(RTLD_DEFAULT, "snd_rawmidi_poll_descriptors");
  struct pollfd pfd;
  return pdesc && pdesc(handle, &pfd, 1) == 1 ? pfd.fd : -1;
}

static void track_port(snd_rawmidi_t *handle, int output, const char *alsa_name, int mode) {
  char dev[64], sub[64];
  info_names(handle, dev, sub, sizeof dev);
  int fd = poll_fd(handle), priv = 0;
  pthread_mutex_lock(&midi_lock);
  struct port *port = NULL;
  for (int i = 0; i < MAXPORTS && !port; i++)
    if (!ports[i].handle)
      port = &ports[i];
  if (port) {
    port->handle = handle;
    port->output = output;
    port->fd = fd;
    port->nonblocking = (mode & SND_RAWMIDI_NONBLOCK) != 0;
    port->identity.position = -1;
    snprintf(
      port->name,
      sizeof port->name,
      "%s",
      sub[0]        ? sub
        : dev[0]    ? dev
        : alsa_name ? alsa_name
                    : "?");
    if (!output && (strstr(sub, "Private") || strstr(dev, "Private"))) {
      priv = 1;
      private_input = handle;
      private_input_fd = fd;
      if (wake_pipe[0] < 0 && pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC))
        wake_pipe[0] = wake_pipe[1] = -1;
      midi_merger_reset(&midi);
    }
  }
  pthread_mutex_unlock(&midi_lock);
  log_event(
    "open %s %s: '%s' / '%s', fd %d, mode %#x%s",
    alsa_name ? alsa_name : "?",
    output ? "out" : "in",
    dev,
    sub,
    fd,
    mode,
    !port    ? ": not tracked (too many ports)"
      : priv ? ": the private input"
             : "");
  if (priv)
    pthread_once(&inject_once, inject_setup);
}

static void say_hello(void) {
  char exe[256], when[32];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  exe[n > 0 ? n : 0] = 0;
  time_t t = time(NULL);
  struct tm tm;
  strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime_r(&t, &tm));
  log_event(
    "privtap %s in pid %d (%s) at %s, settings %s, traffic log %s%s%s",
    PRIVTAP_VERSION,
    (int)getpid(),
    exe,
    when,
    conf_path[0] ? conf_path : "(none)",
    log_traffic ? "on" : "off",
    selected_identity ? ", identity " : "",
    selected_identity ? selected_identity->name : "");
}

API int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **output, const char *name, int mode) {
  static pthread_once_t hello = PTHREAD_ONCE_INIT;
  if (!RESOLVE(real_open, "snd_rawmidi_open"))
    return -ENOSYS;
  pthread_once(&identity_once, select_identity);
  int r = real_open(in, output, name, mode);
  if (r == 0) {
    refresh_config(1);
    pthread_once(&hello, say_hello);
    if (in && *in)
      track_port(*in, 0, name, mode);
    if (output && *output)
      track_port(*output, 1, name, mode);
  }
  return r;
}

API int snd_rawmidi_close(snd_rawmidi_t *handle) {
  if (!RESOLVE(real_close, "snd_rawmidi_close"))
    return -ENOSYS;
  char name[64] = "";
  pthread_mutex_lock(&midi_lock);
  struct port *port = find_port(handle);
  if (port) {
    snprintf(name, sizeof name, "%s", port->name);
    if (handle == private_input) {
      private_input = NULL;
      private_input_fd = -1;
      midi_merger_clear_queues(&midi);
    }
    port->handle = NULL;
  }
  unsigned long lost = midi_merger_take_dropped(&midi);
  pthread_mutex_unlock(&midi_lock);
  if (port)
    log_event("close %s%s", name, lost ? " (bytes were dropped: the queue was full)" : "");
  return real_close(handle);
}

API int snd_rawmidi_nonblock(snd_rawmidi_t *handle, int nonblocking) {
  if (!RESOLVE(real_nonblock, "snd_rawmidi_nonblock"))
    return -ENOSYS;
  int r = real_nonblock(handle, nonblocking);
  if (r == 0) {
    pthread_mutex_lock(&midi_lock);
    struct port *port = find_port(handle);
    if (port)
      port->nonblocking = nonblocking != 0;
    pthread_mutex_unlock(&midi_lock);
  }
  return r;
}

API ssize_t snd_rawmidi_write(snd_rawmidi_t *handle, const void *buffer, size_t size) {
  if (!RESOLVE(real_write, "snd_rawmidi_write"))
    return -ENOSYS;
  const void *send = buffer;
  unsigned char local[1024], *copy = NULL;
  if (selected_identity && size) {
    pthread_mutex_lock(&midi_lock);
    struct port *port = find_port(handle);
    if (port && (copy = size <= sizeof local ? local : malloc(size))) {
      for (size_t i = 0; i < size; i++)
        copy[i] = rewrite_identity_byte(&port->identity, ((const unsigned char *)buffer)[i], 0);
      send = copy;
    }
    pthread_mutex_unlock(&midi_lock);
  }
  ssize_t r = real_write(handle, send, size);
  if (copy && copy != local)
    free(copy);
  if (r > 0) {
    refresh_config(0);
    if (log_traffic) {
      char name[64];
      pthread_mutex_lock(&midi_lock);
      struct port *port = find_port(handle);
      if (port)
        snprintf(name, sizeof name, "%s", port->name);
      pthread_mutex_unlock(&midi_lock);
      if (port)
        log_bytes("out", name, buffer, (size_t)r);
    }
  }
  return r;
}

API ssize_t snd_rawmidi_read(snd_rawmidi_t *handle, void *buffer, size_t size) {
  if (!RESOLVE(real_read, "snd_rawmidi_read") || !RESOLVE(real_poll, "poll"))
    return -ENOSYS;
  refresh_config(0);
  char name[64];
  pthread_mutex_lock(&midi_lock);
  struct port *port = find_port(handle);
  if (port)
    snprintf(name, sizeof name, "%s", port->name);
  if (!port || handle != private_input || !size) {
    pthread_mutex_unlock(&midi_lock);
    ssize_t r = real_read(handle, buffer, size);
    if (port && r > 0)
      log_bytes("in", name, buffer, (size_t)r);
    return r;
  }
  unsigned char *output = buffer;
  size_t n = 0;
  for (;;) {
    n = midi_merger_read(&midi, output, size);
    if (n)
      break;
    int nb = port->nonblocking, fd = private_input_fd, wfd = wake_pipe[0];
    pthread_mutex_unlock(&midi_lock);
    if (!nb && fd >= 0 && wfd >= 0) {
      // A blocking read waits for the hardware or for an injected message, whichever comes first.
      struct pollfd w[2] = {{fd, POLLIN, 0}, {wfd, POLLIN, 0}};
      int r = real_poll(w, 2, -1);
      if (r >= 0 || errno == EINTR) {
        if (r > 0 && w[1].revents)
          drain_wake_pipe(wfd);
        if (r <= 0 || !w[0].revents) {
          pthread_mutex_lock(&midi_lock);
          continue;
        }
      }
    }
    unsigned char tmp[256];
    ssize_t r = real_read(handle, tmp, size < sizeof tmp ? size : sizeof tmp);
    if (r <= 0)
      return r;
    log_bytes("in", name, tmp, (size_t)r);
    pthread_mutex_lock(&midi_lock);
    if (handle != private_input) { // closed meanwhile: hand over what was read as it is
      pthread_mutex_unlock(&midi_lock);
      memcpy(buffer, tmp, (size_t)r);
      return r;
    }
    for (ssize_t i = 0; i < r; i++)
      midi_merger_feed_hardware(
        &midi,
        selected_identity ? rewrite_identity_byte(&hardware_identity_stream, tmp[i], 1) : tmp[i]);
  }
  pthread_mutex_unlock(&midi_lock);
  return (ssize_t)n;
}

// While MPC waits on the private input, it also waits on the wake pipe, and the input reads as
// ready while injected messages are queued for it.
API int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  if (!RESOLVE(real_poll, "poll")) {
    errno = ENOSYS;
    return -1;
  }
  int fd = private_input_fd, wfd = wake_pipe[0], idx = -1;
  if (fd >= 0 && wfd >= 0 && nfds < 16)
    for (nfds_t i = 0; i < nfds && idx < 0; i++)
      if (fds[i].fd == fd && (fds[i].events & POLLIN))
        idx = (int)i;
  if (idx < 0)
    return real_poll(fds, nfds, timeout);

  struct pollfd all[17];
  long long deadline = timeout > 0 ? now_ms() + timeout : 0;
  for (;;) {
    pthread_mutex_lock(&midi_lock);
    int rdy = midi_merger_ready(&midi);
    pthread_mutex_unlock(&midi_lock);
    memcpy(all, fds, nfds * sizeof *fds);
    all[nfds] = (struct pollfd){wfd, POLLIN, 0};
    int r = real_poll(all, nfds + 1, rdy ? 0 : timeout);
    if (r < 0)
      return r;
    int woke = all[nfds].revents != 0;
    if (woke) {
      drain_wake_pipe(wfd);
      r--;
    }
    for (nfds_t i = 0; i < nfds; i++)
      fds[i].revents = all[i].revents;
    if (!rdy && woke) {
      pthread_mutex_lock(&midi_lock);
      rdy = midi_merger_ready(&midi);
      pthread_mutex_unlock(&midi_lock);
    }
    if (rdy) {
      if (!fds[idx].revents)
        r++;
      fds[idx].revents |= POLLIN;
      return r;
    }
    if (r > 0 || !woke || timeout == 0)
      return r;
    // Woken for a message that waits for the end of the hardware's current one: keep waiting, to
    // the deadline.
    if (timeout > 0 && (timeout = (int)(deadline - now_ms())) <= 0)
      return 0;
  }
}

API int __poll_chk(struct pollfd *fds, nfds_t nfds, int timeout, size_t fdslen) {
  (void)fdslen;
  return poll(fds, nfds, timeout);
}
