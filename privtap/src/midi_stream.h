#ifndef PRIVTAP_MIDI_STREAM_H
#define PRIVTAP_MIDI_STREAM_H

#include <stdbool.h>
#include <stddef.h>

enum {
  MIDI_OUTPUT_CAPACITY = 16384,
  MIDI_INJECT_CAPACITY = 4096,
  MIDI_MESSAGE_CAPACITY = 512,
};

/* The caller owns this state and serializes access. No I/O or locking here. */
struct midi_merger {
  unsigned char output[MIDI_OUTPUT_CAPACITY];
  unsigned char injected[MIDI_INJECT_CAPACITY];
  int output_head, output_tail, injected_head, injected_tail;
  unsigned long dropped;
  unsigned char current_status, running_status, pending_data[2];
  int received, required;
  bool in_sysex;
};

void midi_merger_reset(struct midi_merger *stream);
void midi_merger_clear_queues(struct midi_merger *stream);
unsigned long midi_merger_take_dropped(struct midi_merger *stream);
bool midi_merger_ready(const struct midi_merger *stream);
bool midi_merger_queue_injected(
  struct midi_merger *stream, const unsigned char *bytes, size_t length);
void midi_merger_feed_hardware(struct midi_merger *stream, unsigned char byte);
size_t midi_merger_read(struct midi_merger *stream, unsigned char *output, size_t capacity);

/* A FIFO writer has its own parser; only complete messages reach the merger. */
struct midi_injection_parser {
  unsigned char message[MIDI_MESSAGE_CAPACITY];
  unsigned char realtime_byte;
  int length, required, running_status;
  bool in_sysex, oversized;
};

enum midi_parse_result {
  MIDI_PARSE_PENDING,
  MIDI_PARSE_MESSAGE,
  MIDI_PARSE_OVERSIZED_SYSEX,
};

/* Borrowed parser storage: consume/copy it before feeding the next byte. */
struct midi_message {
  const unsigned char *bytes;
  size_t length;
};

enum midi_parse_result midi_parse_injected_byte(
  struct midi_injection_parser *parser, unsigned char byte, struct midi_message *message);

#endif
