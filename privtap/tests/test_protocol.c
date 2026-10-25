#include "midi_stream.h"
#include "identity.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void hardware(struct midi_merger *stream, const unsigned char *bytes, size_t length) {
  for (size_t i = 0; i < length; i++) {
    midi_merger_feed_hardware(stream, bytes[i]);
  }
}

static void
expect_output(struct midi_merger *stream, const unsigned char *expected, size_t length) {
  unsigned char output[MIDI_OUTPUT_CAPACITY];
  size_t count = midi_merger_read(stream, output, sizeof output);
  assert(count == length);
  assert(!memcmp(output, expected, length));
  assert(!midi_merger_ready(stream));
}

static void running_status(void) {
  struct midi_merger stream = {0};
  const unsigned char input[] = {0x90, 0x40, 0x7F, 0x41, 0x01, 0xC0, 0x06, 0x07};
  const unsigned char expected[] = {0x90, 0x40, 0x7F, 0x90, 0x41, 0x01, 0xC0, 0x06, 0xC0, 0x07};
  hardware(&stream, input, sizeof input);
  expect_output(&stream, expected, sizeof expected);
}

static void injection_waits_for_message_boundary(void) {
  struct midi_merger stream = {0};
  const unsigned char partial[] = {0x90, 0x40};
  const unsigned char injected[] = {0xB0, 0x07, 0x01};
  hardware(&stream, partial, sizeof partial);
  assert(midi_merger_queue_injected(&stream, injected, sizeof injected));
  assert(!midi_merger_ready(&stream));
  unsigned char byte;
  assert(midi_merger_read(&stream, &byte, 1) == 0);
  midi_merger_feed_hardware(&stream, 0x7F);
  const unsigned char expected[] = {0x90, 0x40, 0x7F, 0xB0, 0x07, 0x01};
  expect_output(&stream, expected, sizeof expected);
  const unsigned char continuation[] = {0x41, 0x02};
  const unsigned char next[] = {0x90, 0x41, 0x02};
  hardware(&stream, continuation, sizeof continuation);
  expect_output(&stream, next, sizeof next);
}

static void sysex_and_realtime(void) {
  struct midi_merger stream = {0};
  const unsigned char prefix[] = {0xF0, 0x47, 0x7F};
  const unsigned char injected[] = {0x90, 0x40, 0x01};
  hardware(&stream, prefix, sizeof prefix);
  expect_output(&stream, prefix, sizeof prefix);
  assert(midi_merger_queue_injected(&stream, injected, sizeof injected));
  assert(!midi_merger_ready(&stream));
  const unsigned char suffix[] = {0xF8, 0x12, 0xF7};
  hardware(&stream, suffix, sizeof suffix);
  const unsigned char expected[] = {0xF8, 0x12, 0xF7, 0x90, 0x40, 0x01};
  expect_output(&stream, expected, sizeof expected);
}

static void system_common_and_reset(void) {
  struct midi_merger stream = {0};
  const unsigned char input[] = {0x90, 0x40, 0x7F, 0xF1, 0x02, 0x05};
  hardware(&stream, input, sizeof input);
  expect_output(&stream, input, sizeof input);
  midi_merger_feed_hardware(&stream, 0x90);
  midi_merger_reset(&stream);
  const unsigned char orphan[] = {0x33};
  hardware(&stream, orphan, sizeof orphan);
  expect_output(&stream, orphan, sizeof orphan);
}

static void bounded_queues_and_wraparound(void) {
  struct midi_merger stream = {0};
  unsigned char output[MIDI_OUTPUT_CAPACITY];
  for (size_t i = 0; i < MIDI_OUTPUT_CAPACITY + 3; i++) {
    midi_merger_feed_hardware(&stream, (unsigned char)(i % 128));
  }
  assert(midi_merger_take_dropped(&stream) == 4);
  assert(midi_merger_take_dropped(&stream) == 0);
  assert(midi_merger_read(&stream, output, 400) == 400);
  for (size_t i = 0; i < 400; i++) {
    assert(output[i] == i % 128);
    midi_merger_feed_hardware(&stream, 0xF8);
  }
  assert(midi_merger_read(&stream, output, sizeof output) == MIDI_OUTPUT_CAPACITY - 1);
  for (size_t i = 0; i < MIDI_OUTPUT_CAPACITY - 401; i++) {
    assert(output[i] == (i + 400) % 128);
  }
  for (size_t i = MIDI_OUTPUT_CAPACITY - 401; i < MIDI_OUTPUT_CAPACITY - 1; i++) {
    assert(output[i] == 0xF8);
  }
  midi_merger_reset(&stream);
  midi_merger_feed_hardware(&stream, 0x90);
  unsigned char injected[MIDI_INJECT_CAPACITY - 1];
  memset(injected, 0xF8, sizeof injected);
  assert(midi_merger_queue_injected(&stream, injected, sizeof injected));
  assert(!midi_merger_queue_injected(&stream, injected, 1));
  midi_merger_feed_hardware(&stream, 0x40);
  midi_merger_feed_hardware(&stream, 0x7F);
  assert(midi_merger_read(&stream, output, sizeof output) == sizeof injected + 3);
  assert(output[0] == 0x90 && output[1] == 0x40 && output[2] == 0x7F);
  assert(!memcmp(output + 3, injected, sizeof injected));
}

static size_t parse_injected(
  struct midi_injection_parser *parser,
  const unsigned char *bytes,
  size_t length,
  unsigned char *output,
  size_t capacity,
  unsigned *oversized) {
  size_t used = 0;
  for (size_t i = 0; i < length; i++) {
    struct midi_message message;
    enum midi_parse_result result = midi_parse_injected_byte(parser, bytes[i], &message);
    if (result == MIDI_PARSE_MESSAGE) {
      assert(message.length <= capacity - used);
      memcpy(output + used, message.bytes, message.length);
      used += message.length;
    } else if (result == MIDI_PARSE_OVERSIZED_SYSEX) {
      (*oversized)++;
    }
  }
  return used;
}

static void injected_messages(void) {
  struct midi_injection_parser parser = {0};
  const unsigned char input[] =
    {0x40, 0x90, 0x40, 0x01, 0x41, 0x02, 0xF6, 0x42, 0xF0, 0x47, 0xF8, 0x12, 0xF7};
  const unsigned char expected[] =
    {0x90, 0x40, 0x01, 0x90, 0x41, 0x02, 0xF6, 0xF8, 0xF0, 0x47, 0x12, 0xF7};
  unsigned char output[64];
  unsigned oversized = 0;
  size_t length = parse_injected(&parser, input, sizeof input, output, sizeof output, &oversized);
  assert(length == sizeof expected && !memcmp(output, expected, length));
  assert(oversized == 0);
}

static void injected_sysex_limits(void) {
  unsigned char input[MIDI_MESSAGE_CAPACITY + 1], output[MIDI_MESSAGE_CAPACITY + 1];
  struct midi_injection_parser parser = {0};
  unsigned oversized = 0;
  memset(input, 0x01, sizeof input);
  input[0] = 0xF0;
  input[MIDI_MESSAGE_CAPACITY - 1] = 0xF7;
  assert(
    parse_injected(&parser, input, MIDI_MESSAGE_CAPACITY, output, sizeof output, &oversized) ==
    MIDI_MESSAGE_CAPACITY);
  assert(!memcmp(input, output, MIDI_MESSAGE_CAPACITY));
  assert(oversized == 0);
  input[MIDI_MESSAGE_CAPACITY - 1] = 0x01;
  input[MIDI_MESSAGE_CAPACITY] = 0xF7;
  assert(parse_injected(&parser, input, sizeof input, output, sizeof output, &oversized) == 0);
  assert(oversized == 1);
  const unsigned char recovery[] = {0xC0, 0x05};
  assert(
    parse_injected(&parser, recovery, sizeof recovery, output, sizeof output, &oversized) ==
    sizeof recovery);
  assert(!memcmp(recovery, output, sizeof recovery));
}

static void rewrite_identity(
  const struct identity_profile *profile,
  int *family,
  bool incoming,
  const unsigned char *input,
  unsigned char *output,
  size_t length) {
  struct identity_stream stream = {.position = -1};
  for (size_t i = 0; i < length; i++) {
    output[i] = identity_rewrite_byte(profile, &stream, family, input[i], incoming);
  }
}

static void identity_profiles(void) {
  const char *names[] = {"x", "LIVE", "FORCE", "ONE", "LIVE2"};
  const unsigned families[] = {0x3A, 0x3B, 0x40, 0x46, 0x47};
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    const struct identity_profile *profile = identity_lookup(names[i]);
    assert(profile && profile->family == families[i]);
  }
  assert(!identity_lookup("") && !identity_lookup("none") && !identity_lookup("unknown"));
}

static void identity_streams(void) {
  const struct identity_profile *profile = identity_lookup("X");
  int family = -1;
  unsigned char output[32];
  const unsigned char outgoing[] = {0xF0, 0x47, 0x7F, 0x3A, 0x01, 0xF7};
  rewrite_identity(profile, &family, false, outgoing, output, sizeof outgoing);
  assert(!memcmp(output, outgoing, sizeof outgoing) && family == -1);
  const unsigned char invalid[] = {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x46, 0x57, 0xF7};
  rewrite_identity(profile, &family, true, invalid, output, sizeof invalid);
  assert(!memcmp(output, invalid, sizeof invalid) && family == -1);
  const unsigned char reply[] =
    {0xF0, 0x7E, 0x7F, 0xF8, 0x06, 0x02, 0x47, 0x57, 0x00, 0x19, 0x00, 0x01, 0x01, 0x01, 0xF7};
  rewrite_identity(profile, &family, true, reply, output, sizeof reply);
  assert(family == 0x57 && output[3] == 0xF8);
  assert(!memcmp(output + 7, profile->reply, sizeof profile->reply));
  rewrite_identity(profile, &family, false, outgoing, output, sizeof outgoing);
  assert(output[3] == 0x57);
  const unsigned char incoming[] = {0xF0, 0x47, 0x7F, 0x57, 0x12, 0xF7};
  rewrite_identity(profile, &family, true, incoming, output, sizeof incoming);
  assert(output[3] == 0x3A && output[4] == 0x12);
}

int main(void) {
  running_status();
  injection_waits_for_message_boundary();
  sysex_and_realtime();
  system_common_and_reset();
  bounded_queues_and_wraparound();
  injected_messages();
  injected_sysex_limits();
  identity_profiles();
  identity_streams();
  puts("privtap_protocol_cases=9");
  return 0;
}
