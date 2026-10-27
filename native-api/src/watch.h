#ifndef MPC_API_WATCH_H
#define MPC_API_WATCH_H

#include "api.h"

#define API_WATCH_SUBSCRIBERS 8
#define API_WATCH_REPLAY 64
#define API_WATCH_REPLAY_BYTES (4u * 1024u * 1024u)
#define API_WATCH_RETENTION_MS 30000
#define API_WATCH_SAMPLE_MS 100
#define API_WATCH_AUDIT_MS 5000
#define API_WATCH_HEARTBEAT_MS 1000

struct api_subscription {
  struct api_watch *publisher;
  uint64_t revision, epoch, ticket;
  int stage, terminal;
  int64_t heartbeat;
};

struct api_watch *api_watch_create(const struct api *);
void api_watch_destroy(struct api_watch *);
void api_watch_sync(struct api *, const struct discovery *);
char *api_watch_read(struct api_watch *, const char *);
char *api_watch_open(struct api *, const struct api_request_data *, struct api_subscription *);
char *api_watch_next(struct api_subscription *);
char *api_watch_end(struct api_subscription *, const char *);
void api_watch_close(struct api_subscription *);

#endif
