// A stand-in for alsa-lib's rawmidi API, for privtap's host tests. "hw:9,0,N" (N = 0..2) is a port
// of a fake controller: its input is a pipe the test writes the hardware's bytes into (fake_hw_fd),
// its output a buffer the test reads back (fake_out). Subdevice 1 is "Fake Private".
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct _snd_rawmidi {
  int in, fd, sub;
} snd_rawmidi_t;
typedef struct _snd_rawmidi_info {
  char name[64], sub[64];
} snd_rawmidi_info_t;

static const char *const SUBS[3] = {"Fake Public", "Fake Private", "Fake MIDI Port A"};
static int hw_fd[3] = {-1, -1, -1};
static unsigned char out_buf[3][8192];
static size_t out_len[3];

int fake_hw_fd(int sub) {
  return hw_fd[sub];
}

size_t fake_out(int sub, unsigned char *buf, size_t n) {
  size_t k = out_len[sub] < n ? out_len[sub] : n;
  memcpy(buf, out_buf[sub], k);
  out_len[sub] = 0;
  return k;
}

int snd_rawmidi_open(snd_rawmidi_t **in, snd_rawmidi_t **out, const char *name, int mode) {
  int sub;
  if (!name || sscanf(name, "hw:9,0,%d", &sub) != 1 || sub < 0 || sub > 2)
    return -ENOENT;
  if (in) {
    int p[2];
    if (pipe2(p, O_CLOEXEC))
      return -errno;
    if (mode & 2)
      fcntl(p[0], F_SETFL, O_NONBLOCK);
    *in = calloc(1, sizeof **in);
    (*in)->in = 1;
    (*in)->fd = p[0];
    (*in)->sub = sub;
    hw_fd[sub] = p[1];
  }
  if (out) {
    *out = calloc(1, sizeof **out);
    (*out)->fd = -1;
    (*out)->sub = sub;
  }
  return 0;
}

int snd_rawmidi_close(snd_rawmidi_t *h) {
  if (h->in) {
    close(h->fd);
    close(hw_fd[h->sub]);
    hw_fd[h->sub] = -1;
  }
  free(h);
  return 0;
}

ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buf, size_t size) {
  ssize_t r = read(h->fd, buf, size);
  return r < 0 ? -errno : r;
}

ssize_t snd_rawmidi_write(snd_rawmidi_t *h, const void *buf, size_t size) {
  size_t room = sizeof out_buf[0] - out_len[h->sub], k = size < room ? size : room;
  memcpy(out_buf[h->sub] + out_len[h->sub], buf, k);
  out_len[h->sub] += k;
  return (ssize_t)size;
}

int snd_rawmidi_nonblock(snd_rawmidi_t *h, int nonblock) {
  if (h->in)
    fcntl(h->fd, F_SETFL, nonblock ? O_NONBLOCK : 0);
  return 0;
}

int snd_rawmidi_poll_descriptors(snd_rawmidi_t *h, struct pollfd *pfds, unsigned int space) {
  if (!h->in || space < 1)
    return 0;
  pfds[0] = (struct pollfd){h->fd, POLLIN | POLLERR | POLLNVAL, 0};
  return 1;
}

size_t snd_rawmidi_info_sizeof(void) {
  return sizeof(snd_rawmidi_info_t);
}

int snd_rawmidi_info(snd_rawmidi_t *h, snd_rawmidi_info_t *info) {
  snprintf(info->name, sizeof info->name, "Fake Key");
  snprintf(info->sub, sizeof info->sub, "%s", SUBS[h->sub]);
  return 0;
}

const char *snd_rawmidi_info_get_name(const snd_rawmidi_info_t *info) {
  return info->name;
}
const char *snd_rawmidi_info_get_subdevice_name(const snd_rawmidi_info_t *info) {
  return info->sub;
}
