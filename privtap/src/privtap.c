// privtap: an LD_PRELOAD addin for MPC OS standalone devices (written on an MPC Key 37) that sits
// on the controller's own rawmidi ports as MPC opens them through alsa-lib:
//   - it logs what MPC reads from them (the hardware: buttons, pads, keys, knobs) and what it
//   writes to them (LEDs
//     and the rest of the feedback), with a timestamp and the port's name, so the protocol can be
//     mapped;
//   - it merges MIDI written to a FIFO (/dev/midi_inject) into what MPC reads from the "...
//   Private" input, as if the
//     hardware had sent it. The MPC Remote addin writes there when the device has no "Virtual
//     RawMIDI" port, which is what makes its press_button work on a stock device.
// MPC owns the private input (rawmidi inputs can't be shared), so this has to happen inside MPC.
// Its MIDI thread waits in poll() on that input and a wake fd of its own, then calls
// snd_rawmidi_read, so poll() is hooked too: while it waits on the private input it also watches a
// wake pipe, and it reports the input readable while injected messages are queued. Hardware and
// injected messages are merged at message boundaries only (running status is written out in full),
// so neither can split the other.
//
// Settings: privtap.conf next to this library (PRIVTAP_CONF overrides), key=value, re-read within a
// second of a change:
//   log=1                    log the traffic (0: only opens, closes and errors)
//   log_path=/tmp/privtap.log
//   log_max_kb=4096          past this the log moves to <log_path>.1 and a new one starts
//   inject=/dev/midi_inject  the FIFO, created when the private input opens and removed at exit;
//   none: no injection.
//                            Read once, when the private input first opens.
//   identity=                another MPC model to pass the device off as (X, LIVE, LIVE2, ONE,
//   FORCE), to see what MPC
//                            sends that model's hardware; empty: the device is itself. Read once,
//                            when MPC starts.
// Log lines: "<seconds> <in|out|inj> <port>: <hex bytes>", and "<seconds> # <event>" (monotonic
// clock). With an identity, "in" is what the hardware sent and "out" what MPC sent, before the
// rewrites below.
//
// Identity: MPC learns its model from the device tree's product code and compatible string (MPC and
// libaz0x-info both read them) and checks the controller's identity reply. The two files are
// answered from memory, and the controller's family byte is swapped in Akai sysex both ways: in the
// identity reply (F0 7E dd 06 02 47 <family> ...) and in F0 47 dd <family> .... From TheKikGen's
// tkgl_mpcmapper (--tkgl_iamX), without its virtual ports and routing.

#define _GNU_SOURCE
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
#define API __attribute__((visibility("default")))
#define MAXPORTS 16
#define QSIZE 16384   // what MPC reads next from the private input: whole messages
#define IQSIZE 4096   // injected messages waiting for the hardware to finish the message it is in
#define LINE_BYTES 48 // bytes per log line

static int (*real_open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
static int (*real_close)(snd_rawmidi_t *);
static ssize_t (*real_read)(snd_rawmidi_t *, void *, size_t);
static ssize_t (*real_write)(snd_rawmidi_t *, const void *, size_t);
static int (*real_nonblock)(snd_rawmidi_t *, int);
static int (*real_poll)(struct pollfd *, nfds_t, int);

struct sx { // where a stream is in a sysex message, for the identity rewrites
  int pos;  // the next byte's index after F0; -1 outside a sysex
  unsigned char hdr[8];
  int ident; // this is the hardware's identity reply
};

struct port {
  snd_rawmidi_t *h;
  int out, fd, nonblock;
  char name[64]; // the subdevice's name, e.g. "MPC Private"
  struct sx sx;  // MPC's writes, for the identity rewrites
};

// The models a device can pass for (tkgl_mpcmapper's table). Only X has been tried.
struct identity {
  const char *name, *code, *compatible;
  unsigned char family, reply[7];
};
static const struct identity IDENTITIES[] = {
  {"X", "ACV5", "inmusic,acv5", 0x3A, {0x3A, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01}},
  {"LIVE", "ACV8", "inmusic,acv8", 0x3B, {0x3B, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01}},
  {"FORCE", "ADA2", "inmusic,ada2", 0x40, {0x40, 0x00, 0x19, 0x00, 0x00, 0x04, 0x03}},
  {"ONE", "ACVA", "inmusic,acva", 0x46, {0x46, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01}},
  {"LIVE2", "ACVB", "inmusic,acvb", 0x47, {0x47, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01}},
};
static char identity_name[16];
static const struct identity *spoof; // NULL: the device is itself
static pthread_once_t spoof_once = PTHREAD_ONCE_INIT;
static int hw_family = -1; // the hardware's own family byte, from its first identity reply
static struct sx in_sx = {-1, {0}, 0}; // the hardware's stream (under lock)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // the ports, the queues and the parser
static struct port ports[MAXPORTS];
static snd_rawmidi_t *priv_in;
static int priv_fd = -1;
static int wake_fd[2] = {-1, -1};
static unsigned char q[QSIZE], iq[IQSIZE];
static int qh, qt, ih, it;
static unsigned long dropped;

// The hardware's stream, parsed: cur is the status of the message being put together (0 between
// messages), run the running status a data byte continues.
static unsigned char cur, run, data[2];
static int have, need, sysex;

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
static void *next_sym(const char *name) {
  static void *(*libc_dlsym)(void *, const char *);
  if (!libc_dlsym) {
    static const char *const vers[] =
      {"GLIBC_2.34", "GLIBC_2.4", "GLIBC_2.17", "GLIBC_2.2.5", "GLIBC_2.0"};
    for (size_t i = 0; i < sizeof vers / sizeof *vers && !libc_dlsym; i++)
      libc_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_DEFAULT, "dlsym", vers[i]);
  }
  return (libc_dlsym ? libc_dlsym : dlsym)(RTLD_NEXT, name);
}

#define RESOLVE(var, name) (var || (var = next_sym(name)))

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
static void logf_(const char *fmt, ...) {
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
static void conf_check(int force) {
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

// ---- the private input's queue (all under lock)

static int data_len(unsigned char s) {
  switch (s & 0xF0) {
  case 0xC0:
  case 0xD0:
    return 1;
  case 0xF0:
    return s == 0xF2 ? 2 : s == 0xF1 || s == 0xF3 ? 1 : 0;
  default:
    return 2;
  }
}

static int qfree(void) {
  return QSIZE - 1 - (qt - qh + QSIZE) % QSIZE;
}
static int iqlen(void) {
  return (it - ih + IQSIZE) % IQSIZE;
}
static int at_boundary(void) {
  return !cur && !sysex;
}
static int ready(void) {
  return qh != qt || (iqlen() && at_boundary());
}

static void qput(unsigned char b) {
  if (qfree() > 0) {
    q[qt] = b;
    qt = (qt + 1) % QSIZE;
  } else
    dropped++;
}

// The injected messages go next, unless the hardware is in the middle of one.
static void flush_inject(void) {
  int n = iqlen();
  if (!n || !at_boundary() || qfree() < n)
    return;
  while (ih != it) {
    q[qt] = iq[ih];
    qt = (qt + 1) % QSIZE;
    ih = (ih + 1) % IQSIZE;
  }
}

static void real_byte(unsigned char b) {
  if (b >= 0xF8) { // real time: allowed anywhere, even inside another message
    qput(b);
    return;
  }
  if (sysex) {
    if (b < 0x80 || b == 0xF7) {
      qput(b);
      if (b == 0xF7) {
        sysex = 0;
        flush_inject();
      }
      return;
    }
    sysex = 0; // a status byte also ends a sysex
  }
  if (b == 0xF0) {
    qput(b);
    sysex = 1;
    cur = run = 0;
    return;
  }
  if (b & 0x80) {
    run = b < 0xF0 ? b : 0; // system common messages cancel running status
    cur = b;
    have = 0;
    need = data_len(b);
    if (!need) {
      qput(b);
      cur = 0;
      flush_inject();
    }
    return;
  }
  if (!cur) {
    if (!run) { // a data byte with no status: passed on as it is
      qput(b);
      return;
    }
    cur = run;
    have = 0;
    need = data_len(run);
  }
  data[have++] = b;
  if (have == need) {
    qput(cur);
    for (int i = 0; i < need; i++)
      qput(data[i]);
    cur = 0;
    flush_inject();
  }
}

static struct port *find(snd_rawmidi_t *h) {
  for (int i = 0; i < MAXPORTS; i++)
    if (h && ports[i].h == h)
      return &ports[i];
  return NULL;
}

static void wake(void) {
  char c = 1;
  if (wake_fd[1] >= 0 && write(wake_fd[1], &c, 1) < 0) {
    // full: the reader is already due to wake
  }
}

static void drain(int fd) {
  char buf[64];
  while (read(fd, buf, sizeof buf) > 0)
    ;
}

// ---- injection

static void push_inject(const unsigned char *m, int n) {
  int ok = 0;
  pthread_mutex_lock(&lock);
  if (priv_in && IQSIZE - 1 - iqlen() >= n) {
    for (int i = 0; i < n; i++) {
      iq[it] = m[i];
      it = (it + 1) % IQSIZE;
    }
    flush_inject();
    ok = 1;
  }
  pthread_mutex_unlock(&lock);
  if (ok)
    wake();
  else
    logf_(
      "inject: %d bytes dropped (%s)",
      n,
      priv_in ? "queue full" : "the private input isn't open");
}

// The FIFO's stream, put together into whole messages (the inject thread's own state).
static unsigned char im[512];
static int im_len, im_need, im_run, im_sysex, im_long;

static void inject_byte(unsigned char b) {
  if (b >= 0xF8) {
    push_inject(&b, 1);
    return;
  }
  if (im_sysex) {
    if (b < 0x80) {
      if (im_len < (int)sizeof im - 1)
        im[im_len++] = b;
      else
        im_long = 1;
      return;
    }
    if (b == 0xF7) {
      im[im_len++] = b;
      if (im_long)
        logf_("inject: a sysex over %d bytes was dropped", (int)sizeof im);
      else
        push_inject(im, im_len);
      im_sysex = im_len = 0;
      return;
    }
    im_sysex = im_len = 0;
  }
  if (b == 0xF0) {
    im[0] = b;
    im_len = 1;
    im_sysex = 1;
    im_long = im_run = 0;
    return;
  }
  if (b & 0x80) {
    im_run = b < 0xF0 ? b : 0;
    im[0] = b;
    im_len = 1;
    im_need = data_len(b);
    if (!im_need) {
      push_inject(im, 1);
      im_len = 0;
    }
    return;
  }
  if (!im_len) {
    if (!im_run) // a data byte with no status: dropped
      return;
    im[0] = (unsigned char)im_run;
    im_len = 1;
    im_need = data_len((unsigned char)im_run);
  }
  im[im_len++] = b;
  if (im_len == 1 + im_need) {
    push_inject(im, im_len);
    im_len = 0;
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
      logf_("inject: reading %s stopped: %s", fifo_path, n ? strerror(errno) : "end of file");
      break;
    }
    log_bytes("inj", fifo_path, buf, (size_t)n);
    for (ssize_t i = 0; i < n; i++)
      inject_byte(buf[i]);
  }
  close(fd);
  return NULL;
}

static void inject_setup(void) {
  struct stat st;
  snprintf(fifo_path, sizeof fifo_path, "%s", inject_path);
  if (!fifo_path[0] || !strcmp(fifo_path, "none")) {
    logf_("inject: off");
    return;
  }
  int made = 0;
  if (!lstat(fifo_path, &st)) {
    if (!S_ISFIFO(st.st_mode)) {
      logf_("inject: %s exists and isn't a FIFO, so there is no injection", fifo_path);
      return;
    }
  } else if (!mkfifo(fifo_path, 0600))
    made = 1;
  else {
    logf_("inject: can't create %s: %s", fifo_path, strerror(errno));
    return;
  }
  // Read-write: opening doesn't wait for a writer, and a writer going away isn't an end of file.
  int fd = open(fifo_path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    logf_("inject: can't open %s: %s", fifo_path, strerror(errno));
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
    logf_("inject: no thread: %s", strerror(err));
    close(fd);
    return;
  }
  pthread_setname_np(th, "privtap inject");
  logf_("inject: reading %s%s", fifo_path, made ? " (created)" : "");
}

__attribute__((destructor)) static void privtap_fini(void) {
  if (fifo_owner && fifo_owner == getpid())
    unlink(fifo_path);
}

// ---- identity

static void spoof_init(void) {
  conf_check(1);
  for (size_t i = 0; i < sizeof IDENTITIES / sizeof *IDENTITIES; i++)
    if (!strcasecmp(identity_name, IDENTITIES[i].name))
      spoof = &IDENTITIES[i];
  if (spoof)
    logf_(
      "identity: passing this device off as %s (%s, family %02X)",
      spoof->name,
      spoof->code,
      spoof->family);
  else if (identity_name[0] && strcasecmp(identity_name, "none"))
    logf_(
      "identity: '%s' isn't X, LIVE, LIVE2, ONE or FORCE: the device stays itself",
      identity_name);
}

#define DT "/sys/firmware/devicetree/base/"

// An fd reading the spoofed model's product code or compatible string, if path is one of those
// files and an identity is set; else -2.
static int fake_fd(const char *path) {
  static int told[2];
  if (!path || strncmp(path, DT, sizeof DT - 1))
    return -2;
  int code = !strcmp(path + sizeof DT - 1, "inmusic,product-code");
  if (!code && strcmp(path + sizeof DT - 1, "compatible"))
    return -2;
  pthread_once(&spoof_once, spoof_init);
  if (!spoof)
    return -2;
  // Device-tree strings end in a NUL, and a list of them is NUL-separated.
  char buf[96];
  int n = code ? sprintf(buf, "%s", spoof->code) + 1 : 0;
  if (!code) {
    n += sprintf(buf + n, "%s", spoof->compatible) + 1;
    n += sprintf(buf + n, "inmusic,az01") + 1;
    n += sprintf(buf + n, "rockchip,rk3288") + 1;
  }
  int fd = memfd_create(code ? "product-code" : "compatible", MFD_CLOEXEC);
  if (fd < 0 || write(fd, buf, (size_t)n) != n || lseek(fd, 0, SEEK_SET)) {
    if (fd >= 0)
      close(fd);
    logf_("identity: can't fake %s: %s", path, strerror(errno));
    return -2;
  }
  if (!told[code]) {
    told[code] = 1;
    logf_("identity: %s read as %s's", path, spoof->name);
  }
  return fd;
}

// One byte of a stream, rewritten for the identity: to_mpc is the hardware's stream, else MPC's.
static unsigned char sx_byte(struct sx *s, unsigned char b, int to_mpc) {
  if (b >= 0xF8) // real time, which may sit inside a sysex
    return b;
  if (b == 0xF0) {
    s->pos = 1;
    s->hdr[0] = b;
    s->ident = 0;
    return b;
  }
  if (b & 0x80) { // F7, or any status, ends it
    s->pos = -1;
    return b;
  }
  if (s->pos < 0)
    return b;
  int p = s->pos++;
  if (p < (int)sizeof s->hdr)
    s->hdr[p] = b;
  const unsigned char *h = s->hdr;
  if (to_mpc) {
    if (p == 6 && h[1] == 0x7E && h[3] == 0x06 && h[4] == 0x02 && h[5] == 0x47) { // an Akai
                                                                                  // identity reply
      if (hw_family < 0) {
        hw_family = b;
        logf_("identity: the hardware is family %02X, shown to MPC as %02X", b, spoof->family);
      }
      s->ident = b == hw_family;
    }
    if (s->ident && p >= 6 && p <= 12)
      return spoof->reply[p - 6];
    if (p == 3 && h[1] == 0x47 && b == hw_family)
      return spoof->family;
  } else if (p == 3 && h[1] == 0x47 && b == spoof->family && hw_family >= 0)
    return (unsigned char)hw_family;
  return b;
}

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
#define OPEN_HOOK(libc, sym, ...)                                                                  \
  {                                                                                                \
    mode_t mode = 0;                                                                               \
    if (NEEDS_MODE(flags)) {                                                                       \
      va_list ap;                                                                                  \
      va_start(ap, flags);                                                                         \
      mode = (mode_t)va_arg(ap, int);                                                              \
      va_end(ap);                                                                                  \
    }                                                                                              \
    int fd;                                                                                        \
    if (READ_ONLY(flags) && (fd = fake_fd(path)) != -2)                                            \
      return fd;                                                                                   \
    if (!RESOLVE(libc, sym)) {                                                                     \
      errno = ENOSYS;                                                                              \
      return -1;                                                                                   \
    }                                                                                              \
    return libc(__VA_ARGS__, mode);                                                                \
  }

API int open(const char *path, int flags, ...) OPEN_HOOK(libc_open, "open", path, flags) API
  int open64(const char *path, int flags, ...) OPEN_HOOK(libc_open64, "open64", path, flags) API
  int openat(int dir, const char *path, int flags, ...)
    OPEN_HOOK(libc_openat, "openat", dir, path, flags) API
  int openat64(int dir, const char *path, int flags, ...)
    OPEN_HOOK(libc_openat64, "openat64", dir, path, flags)

  // The fortified forms (_FORTIFY_SOURCE), which take no mode.
  API int __open_2(const char *path, int flags) {
  int fd;
  if (READ_ONLY(flags) && (fd = fake_fd(path)) != -2)
    return fd;
  if (!RESOLVE(libc_open_2, "__open_2")) {
    errno = ENOSYS;
    return -1;
  }
  return libc_open_2(path, flags);
}

API int __open64_2(const char *path, int flags) {
  int fd;
  if (READ_ONLY(flags) && (fd = fake_fd(path)) != -2)
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
  if (mode && mode[0] == 'r' && !strchr(mode, '+') && (fd = fake_fd(path)) != -2) {
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

// ---- the hooks

static void info_names(snd_rawmidi_t *h, char *dev, char *sub, size_t n) {
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
  if (ri && !info(h, ri)) {
    snprintf(dev, n, "%s", get_name(ri) ? get_name(ri) : "");
    snprintf(sub, n, "%s", get_sub(ri) ? get_sub(ri) : "");
  }
  free(ri);
}

static int poll_fd(snd_rawmidi_t *h) {
  int (*pdesc)(snd_rawmidi_t *, struct pollfd *, unsigned int) =
    dlsym(RTLD_DEFAULT, "snd_rawmidi_poll_descriptors");
  struct pollfd pfd;
  return pdesc && pdesc(h, &pfd, 1) == 1 ? pfd.fd : -1;
}

static void track(snd_rawmidi_t *h, int out, const char *alsa_name, int mode) {
  char dev[64], sub[64];
  info_names(h, dev, sub, sizeof dev);
  int fd = poll_fd(h), priv = 0;
  pthread_mutex_lock(&lock);
  struct port *p = NULL;
  for (int i = 0; i < MAXPORTS && !p; i++)
    if (!ports[i].h)
      p = &ports[i];
  if (p) {
    p->h = h;
    p->out = out;
    p->fd = fd;
    p->nonblock = (mode & SND_RAWMIDI_NONBLOCK) != 0;
    p->sx.pos = -1;
    snprintf(
      p->name,
      sizeof p->name,
      "%s",
      sub[0]        ? sub
        : dev[0]    ? dev
        : alsa_name ? alsa_name
                    : "?");
    if (!out && (strstr(sub, "Private") || strstr(dev, "Private"))) {
      priv = 1;
      priv_in = h;
      priv_fd = fd;
      if (wake_fd[0] < 0 && pipe2(wake_fd, O_NONBLOCK | O_CLOEXEC))
        wake_fd[0] = wake_fd[1] = -1;
      qh = qt = ih = it = 0;
      cur = run = 0;
      have = need = sysex = 0;
    }
  }
  pthread_mutex_unlock(&lock);
  logf_(
    "open %s %s: '%s' / '%s', fd %d, mode %#x%s",
    alsa_name ? alsa_name : "?",
    out ? "out" : "in",
    dev,
    sub,
    fd,
    mode,
    !p       ? ": not tracked (too many ports)"
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
  logf_(
    "privtap %s in pid %d (%s) at %s, settings %s, traffic log %s%s%s",
    PRIVTAP_VERSION,
    (int)getpid(),
    exe,
    when,
    conf_path[0] ? conf_path : "(none)",
    log_traffic ? "on" : "off",
    spoof ? ", identity " : "",
    spoof ? spoof->name : "");
}

API int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode) {
  static pthread_once_t hello = PTHREAD_ONCE_INIT;
  if (!RESOLVE(real_open, "snd_rawmidi_open"))
    return -ENOSYS;
  pthread_once(&spoof_once, spoof_init);
  int r = real_open(in, out, name, mode);
  if (r == 0) {
    conf_check(1);
    pthread_once(&hello, say_hello);
    if (in && *in)
      track(*in, 0, name, mode);
    if (out && *out)
      track(*out, 1, name, mode);
  }
  return r;
}

API int snd_rawmidi_close(snd_rawmidi_t *h) {
  if (!RESOLVE(real_close, "snd_rawmidi_close"))
    return -ENOSYS;
  char name[64] = "";
  pthread_mutex_lock(&lock);
  struct port *p = find(h);
  if (p) {
    snprintf(name, sizeof name, "%s", p->name);
    if (h == priv_in) {
      priv_in = NULL;
      priv_fd = -1;
      qh = qt = ih = it = 0;
    }
    p->h = NULL;
  }
  unsigned long lost = dropped;
  dropped = 0;
  pthread_mutex_unlock(&lock);
  if (p)
    logf_("close %s%s", name, lost ? " (bytes were dropped: the queue was full)" : "");
  return real_close(h);
}

API int snd_rawmidi_nonblock(snd_rawmidi_t *h, int nonblock) {
  if (!RESOLVE(real_nonblock, "snd_rawmidi_nonblock"))
    return -ENOSYS;
  int r = real_nonblock(h, nonblock);
  if (r == 0) {
    pthread_mutex_lock(&lock);
    struct port *p = find(h);
    if (p)
      p->nonblock = nonblock != 0;
    pthread_mutex_unlock(&lock);
  }
  return r;
}

API ssize_t snd_rawmidi_write(snd_rawmidi_t *h, const void *buffer, size_t size) {
  if (!RESOLVE(real_write, "snd_rawmidi_write"))
    return -ENOSYS;
  const void *send = buffer;
  unsigned char local[1024], *copy = NULL;
  if (spoof && size) {
    pthread_mutex_lock(&lock);
    struct port *p = find(h);
    if (p && (copy = size <= sizeof local ? local : malloc(size))) {
      for (size_t i = 0; i < size; i++)
        copy[i] = sx_byte(&p->sx, ((const unsigned char *)buffer)[i], 0);
      send = copy;
    }
    pthread_mutex_unlock(&lock);
  }
  ssize_t r = real_write(h, send, size);
  if (copy && copy != local)
    free(copy);
  if (r > 0) {
    conf_check(0);
    if (log_traffic) {
      char name[64];
      pthread_mutex_lock(&lock);
      struct port *p = find(h);
      if (p)
        snprintf(name, sizeof name, "%s", p->name);
      pthread_mutex_unlock(&lock);
      if (p)
        log_bytes("out", name, buffer, (size_t)r);
    }
  }
  return r;
}

API ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buffer, size_t size) {
  if (!RESOLVE(real_read, "snd_rawmidi_read") || !RESOLVE(real_poll, "poll"))
    return -ENOSYS;
  conf_check(0);
  char name[64];
  pthread_mutex_lock(&lock);
  struct port *p = find(h);
  if (p)
    snprintf(name, sizeof name, "%s", p->name);
  if (!p || h != priv_in || !size) {
    pthread_mutex_unlock(&lock);
    ssize_t r = real_read(h, buffer, size);
    if (p && r > 0)
      log_bytes("in", name, buffer, (size_t)r);
    return r;
  }
  unsigned char *out = buffer;
  size_t n = 0;
  for (;;) {
    flush_inject();
    while (n < size && qh != qt) {
      out[n++] = q[qh];
      qh = (qh + 1) % QSIZE;
    }
    if (n)
      break;
    int nb = p->nonblock, fd = priv_fd, wfd = wake_fd[0];
    pthread_mutex_unlock(&lock);
    if (!nb && fd >= 0 && wfd >= 0) {
      // A blocking read waits for the hardware or for an injected message, whichever comes first.
      struct pollfd w[2] = {{fd, POLLIN, 0}, {wfd, POLLIN, 0}};
      int r = real_poll(w, 2, -1);
      if (r >= 0 || errno == EINTR) {
        if (r > 0 && w[1].revents)
          drain(wfd);
        if (r <= 0 || !w[0].revents) {
          pthread_mutex_lock(&lock);
          continue;
        }
      }
    }
    unsigned char tmp[256];
    ssize_t r = real_read(h, tmp, size < sizeof tmp ? size : sizeof tmp);
    if (r <= 0)
      return r;
    log_bytes("in", name, tmp, (size_t)r);
    pthread_mutex_lock(&lock);
    if (h != priv_in) { // closed meanwhile: hand over what was read as it is
      pthread_mutex_unlock(&lock);
      memcpy(buffer, tmp, (size_t)r);
      return r;
    }
    for (ssize_t i = 0; i < r; i++)
      real_byte(spoof ? sx_byte(&in_sx, tmp[i], 1) : tmp[i]);
  }
  pthread_mutex_unlock(&lock);
  return (ssize_t)n;
}

// While MPC waits on the private input, it also waits on the wake pipe, and the input reads as
// ready while injected messages are queued for it.
API int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  if (!RESOLVE(real_poll, "poll")) {
    errno = ENOSYS;
    return -1;
  }
  int fd = priv_fd, wfd = wake_fd[0], idx = -1;
  if (fd >= 0 && wfd >= 0 && nfds < 16)
    for (nfds_t i = 0; i < nfds && idx < 0; i++)
      if (fds[i].fd == fd && (fds[i].events & POLLIN))
        idx = (int)i;
  if (idx < 0)
    return real_poll(fds, nfds, timeout);

  struct pollfd all[17];
  long long deadline = timeout > 0 ? now_ms() + timeout : 0;
  for (;;) {
    pthread_mutex_lock(&lock);
    int rdy = ready();
    pthread_mutex_unlock(&lock);
    memcpy(all, fds, nfds * sizeof *fds);
    all[nfds] = (struct pollfd){wfd, POLLIN, 0};
    int r = real_poll(all, nfds + 1, rdy ? 0 : timeout);
    if (r < 0)
      return r;
    int woke = all[nfds].revents != 0;
    if (woke) {
      drain(wfd);
      r--;
    }
    for (nfds_t i = 0; i < nfds; i++)
      fds[i].revents = all[i].revents;
    if (!rdy && woke) {
      pthread_mutex_lock(&lock);
      rdy = ready();
      pthread_mutex_unlock(&lock);
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
