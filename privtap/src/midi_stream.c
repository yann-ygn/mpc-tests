#include "midi_stream.h"

static int data_bytes(unsigned char status) {
  switch (status & 0xF0) {
  case 0xC0:
  case 0xD0:
    return 1;
  case 0xF0:
    return status == 0xF2 ? 2 : status == 0xF1 || status == 0xF3 ? 1 : 0;
  default:
    return 2;
  }
}

static int output_free(const struct midi_merger *stream) {
  return MIDI_OUTPUT_CAPACITY - 1 -
    (stream->output_tail - stream->output_head + MIDI_OUTPUT_CAPACITY) % MIDI_OUTPUT_CAPACITY;
}

static int injected_length(const struct midi_merger *stream) {
  return (stream->injected_tail - stream->injected_head + MIDI_INJECT_CAPACITY) %
    MIDI_INJECT_CAPACITY;
}

static bool at_message_boundary(const struct midi_merger *stream) {
  return !stream->current_status && !stream->in_sysex;
}

bool midi_merger_ready(const struct midi_merger *stream) {
  return stream->output_head != stream->output_tail ||
    (injected_length(stream) && at_message_boundary(stream));
}

static void output_push(struct midi_merger *stream, unsigned char byte) {
  if (output_free(stream) > 0) {
    stream->output[stream->output_tail] = byte;
    stream->output_tail = (stream->output_tail + 1) % MIDI_OUTPUT_CAPACITY;
  } else {
    stream->dropped++;
  }
}

/* Wait for the hardware's current message to finish before inserting messages. */
static void flush_injected(struct midi_merger *stream) {
  int length = injected_length(stream);
  if (!length || !at_message_boundary(stream) || output_free(stream) < length) {
    return;
  }
  while (stream->injected_head != stream->injected_tail) {
    stream->output[stream->output_tail] = stream->injected[stream->injected_head];
    stream->output_tail = (stream->output_tail + 1) % MIDI_OUTPUT_CAPACITY;
    stream->injected_head = (stream->injected_head + 1) % MIDI_INJECT_CAPACITY;
  }
}

void midi_merger_clear_queues(struct midi_merger *stream) {
  stream->output_head = stream->output_tail = 0;
  stream->injected_head = stream->injected_tail = 0;
}

void midi_merger_reset(struct midi_merger *stream) {
  midi_merger_clear_queues(stream);
  stream->current_status = stream->running_status = 0;
  stream->received = stream->required = 0;
  stream->in_sysex = false;
}

unsigned long midi_merger_take_dropped(struct midi_merger *stream) {
  unsigned long dropped = stream->dropped;
  stream->dropped = 0;
  return dropped;
}

bool midi_merger_queue_injected(
  struct midi_merger *stream, const unsigned char *bytes, size_t length) {
  if (length > (size_t)(MIDI_INJECT_CAPACITY - 1 - injected_length(stream))) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    stream->injected[stream->injected_tail] = bytes[i];
    stream->injected_tail = (stream->injected_tail + 1) % MIDI_INJECT_CAPACITY;
  }
  flush_injected(stream);
  return true;
}

void midi_merger_feed_hardware(struct midi_merger *stream, unsigned char byte) {
  if (byte >= 0xF8) { // Real-time messages may occur inside any other message.
    output_push(stream, byte);
    return;
  }
  if (stream->in_sysex) {
    if (byte < 0x80 || byte == 0xF7) {
      output_push(stream, byte);
      if (byte == 0xF7) {
        stream->in_sysex = false;
        flush_injected(stream);
      }
      return;
    }
    stream->in_sysex = false; // Another status byte also ends a SysEx.
  }
  if (byte == 0xF0) {
    output_push(stream, byte);
    stream->in_sysex = true;
    stream->current_status = stream->running_status = 0;
    return;
  }
  if (byte & 0x80) {
    stream->running_status = byte < 0xF0 ? byte : 0;
    stream->current_status = byte;
    stream->received = 0;
    stream->required = data_bytes(byte);
    if (!stream->required) {
      output_push(stream, byte);
      stream->current_status = 0;
      flush_injected(stream);
    }
    return;
  }
  if (!stream->current_status) {
    if (!stream->running_status) { // Preserve hardware bytes without a status.
      output_push(stream, byte);
      return;
    }
    stream->current_status = stream->running_status;
    stream->received = 0;
    stream->required = data_bytes(stream->running_status);
  }
  stream->pending_data[stream->received++] = byte;
  if (stream->received == stream->required) {
    output_push(stream, stream->current_status);
    for (int i = 0; i < stream->required; i++) {
      output_push(stream, stream->pending_data[i]);
    }
    stream->current_status = 0;
    flush_injected(stream);
  }
}

size_t midi_merger_read(struct midi_merger *stream, unsigned char *output, size_t capacity) {
  flush_injected(stream);
  size_t length = 0;
  while (length < capacity && stream->output_head != stream->output_tail) {
    output[length++] = stream->output[stream->output_head];
    stream->output_head = (stream->output_head + 1) % MIDI_OUTPUT_CAPACITY;
  }
  return length;
}

static enum midi_parse_result
complete_message(struct midi_injection_parser *parser, struct midi_message *message) {
  message->bytes = parser->message;
  message->length = (size_t)parser->length;
  parser->length = 0;
  return MIDI_PARSE_MESSAGE;
}

enum midi_parse_result midi_parse_injected_byte(
  struct midi_injection_parser *parser, unsigned char byte, struct midi_message *message) {
  *message = (struct midi_message){0};
  if (byte >= 0xF8) {
    parser->realtime_byte = byte;
    message->bytes = &parser->realtime_byte;
    message->length = 1;
    return MIDI_PARSE_MESSAGE;
  }
  if (parser->in_sysex) {
    if (byte < 0x80) {
      if (parser->length < MIDI_MESSAGE_CAPACITY - 1) {
        parser->message[parser->length++] = byte;
      } else {
        parser->oversized = true;
      }
      return MIDI_PARSE_PENDING;
    }
    if (byte == 0xF7) {
      parser->message[parser->length++] = byte;
      parser->in_sysex = false;
      if (parser->oversized) {
        parser->length = 0;
        return MIDI_PARSE_OVERSIZED_SYSEX;
      }
      return complete_message(parser, message);
    }
    parser->in_sysex = false;
    parser->length = 0;
  }
  if (byte == 0xF0) {
    parser->message[0] = byte;
    parser->length = 1;
    parser->in_sysex = true;
    parser->oversized = false;
    parser->running_status = 0;
    return MIDI_PARSE_PENDING;
  }
  if (byte & 0x80) {
    parser->running_status = byte < 0xF0 ? byte : 0;
    parser->message[0] = byte;
    parser->length = 1;
    parser->required = data_bytes(byte);
    return parser->required ? MIDI_PARSE_PENDING : complete_message(parser, message);
  }
  if (!parser->length) {
    if (!parser->running_status) { // An injected data byte without a status is dropped.
      return MIDI_PARSE_PENDING;
    }
    parser->message[0] = (unsigned char)parser->running_status;
    parser->length = 1;
    parser->required = data_bytes((unsigned char)parser->running_status);
  }
  parser->message[parser->length++] = byte;
  return parser->length == 1 + parser->required ? complete_message(parser, message)
                                                : MIDI_PARSE_PENDING;
}
