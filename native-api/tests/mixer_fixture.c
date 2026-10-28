/* Reuse the command harness so numeric commands exercise the same real FIFO,
 * policy, lifetime registry, retained worker and draining shutdown as flags. */
#define main flag_fixture_main
#include "commands_fixture.c"
#undef main

static struct mpc_mixer_state mixer_sample(struct commands_fixture *f, unsigned index) {
  struct mpc_executor_request request = {.operation = MPC_EXECUTOR_SAMPLE_FLAGS};
  struct mpc_executor_job *job;
  assert(!mpc_executor_submit(f->executor, &request, &job));
  pump(&f->identity);
  struct mpc_executor_result result;
  assert(!mpc_executor_wait(job, 1000, &result));
  assert(result.flags && result.flags->tracks[index].mixer.generation);
  struct mpc_mixer_state copy = result.flags->tracks[index].mixer;
  mpc_executor_release(job);
  return copy;
}
static struct mpc_command_request mixer_request(
  struct commands_fixture *f,
  unsigned op,
  unsigned target_index,
  unsigned control_index,
  double value,
  const char *id) {
  struct mpc_command_request r = request(f, id, op, target_index, value != 0);
  r.mixer.generation = mixer_sample(f, target_index).generation;
  r.mixer.index = control_index;
  if (mpc_mixer_numeric(op)) {
    r.value = false;
    r.mixer.value = value;
  }
  return r;
}
static void controls_only(struct identity_fixture *fixture, uintptr_t address) {
  (void)fixture;
  assert(address != TRACK + NATIVE_TRACK_NAME && address != TRACK2 + NATIVE_TRACK_NAME);
  assert(address != SEQUENCE + NATIVE_SEQUENCE_NAME && address != SEQUENCE + NATIVE_SEQUENCE_BPM);
}

static bool verification_entered, verification_release;
static void blocking_verification(struct identity_fixture *f, uintptr_t address) {
  if (address != PROGRAM + MIXER_VOLUME_VALUE)
    return;
  pthread_mutex_lock(&f->mutex);
  verification_entered = true;
  pthread_cond_broadcast(&f->changed);
  struct timespec until = deadline();
  while (!verification_release)
    assert(pthread_cond_timedwait(&f->changed, &f->mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&f->mutex);
}
static void *verification_pump(void *data) {
  pump(data);
  return NULL;
}

static void read_without_clip(void) {
  struct commands_fixture f;
  setup(&f, 8, 64, true);
  f.live_policy = true;
  struct fixture *n = &f.identity.native;
  mixer_init(n, PROGRAM, OBJECTS + 0x30000);
  put32(n, PROGRAM + native_command_context.programAutomation, 1);
  put32(n, PROGRAM + native_bindings[0].property + 8, PROGRAM + native_bindings[0].property + 4);
  /* A newly added Track may have its canonical Clip before the mute updater
   * is bound. Global volume still qualifies that real populated owner path. */
  struct mpc_command_request initial =
    mixer_request(&f, MPC_COMMAND_VOLUME, 0, 0, .5, "clip_without_subscription");
  struct mpc_command_receipt first = send(&f, initial);
  assert(first.status == MPC_COMMAND_APPLIED && first.native_invoked);
  put32(n, POLICY_SEQUENCE_MAP + 20, 0);
  put32(n, POLICY_SEQUENCE_MAP + 24, 0);
  for (unsigned op = MPC_COMMAND_MUTE; op <= MPC_COMMAND_SOLO; op++) {
    struct mpc_command_request input =
      request(&f, op ? "no_clip_solo" : "no_clip_mute", op, 0, true);
    struct mpc_command_receipt r = send(&f, input);
    assert(r.status == MPC_COMMAND_APPLIED && r.native_invoked);
  }
  struct mpc_command_request input =
    mixer_request(&f, MPC_COMMAND_VOLUME, 0, 0, .25, "no_clip_volume");
  struct mpc_command_receipt r = send(&f, input);
  assert(r.status == MPC_COMMAND_APPLIED && r.native_invoked);
  input = mixer_request(&f, MPC_COMMAND_PAN, 0, 0, .25, "no_clip_pan");
  r = send(&f, input);
  assert(r.status == MPC_COMMAND_APPLIED && r.native_invoked);
  put_bool(n, POLICY_MUTE + native_command_context.perSequenceMute, true);
  input = request(&f, "no_clip_sequence", MPC_COMMAND_MUTE, 0, false);
  input.mute_scope = MPC_MUTE_SEQUENCE;
  memcpy(input.sequence.instance_id, service, 37);
  input.sequence.project = f.ids.project;
  input.sequence.sequence = f.ids.selected_sequence;
  input.sequence.generation = f.ids.sequences[0].generation;
  r = send(&f, input);
  assert(
    r.status == MPC_COMMAND_REJECTED && !r.native_invoked &&
    r.error == MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED);
  cleanup(&f);
}

static void read_numeric(unsigned operation) {
  const uint32_t families[] =
    {NATIVE_PLUGIN_VTABLE, NATIVE_DRUM_VTABLE, NATIVE_KEYGROUP_VTABLE, NATIVE_AUDIO_VTABLE};
  const uint32_t screens[] = {
    native_command_context.mainMode,
    native_command_context.mixerMode,
    native_command_context.trackMuteMode};
  for (unsigned family = 0; family < 4; family++)
    for (unsigned playing = 0; playing < 2; playing++)
      for (unsigned screen = 0; screen < 3; screen++) {
        struct commands_fixture f;
        setup(&f, 8, 128, true);
        f.live_policy = true;
        struct fixture *n = &f.identity.native;
        family_pair(&f, families[family], families[(family + 1) % 4]);
        mixer_init(n, PROGRAM, OBJECTS + 0x30000);
        mixer_init(n, PROGRAM2, OBJECTS + 0x32000);
        put32(n, PROGRAM + native_command_context.programAutomation, 1);
        put32(n, PROGRAM2 + native_command_context.programAutomation, 1);
        put32(n, POLICY_MODE + native_command_context.modeValue, screens[screen]);
        put_bool(n, POLICY_TIMELINE + native_command_context.timelinePlaying, playing);
        put32(n, POLICY_SOURCE + 0x400 + READ_eventTree + 16, 7);
        put32(n, POLICY_SOURCE2 + 0x400 + READ_eventTree + 16, 11);
        const double values[] = {0, 1, .123456789, .123456790, .8};
        for (unsigned target_index = 0; target_index < 2; target_index++)
          for (unsigned selected = 0; selected < 2; selected++) {
            choose(&f, selected ? TRACK : TRACK2);
            for (unsigned v = 0; v < 5; v++) {
              char id[80];
              snprintf(id, sizeof id, "read_volume_%u_%u_%u", target_index, selected, v);
              struct mpc_command_request r =
                mixer_request(&f, operation, target_index, 0, values[v], id);
              struct mpc_command_receipt out = send(&f, r), replay;
              assert(out.status == MPC_COMMAND_APPLIED);
              assert(
                (operation == MPC_COMMAND_PAN
                   ? out.after.tracks[target_index].mixer.pan
                   : out.after.tracks[target_index].mixer.volume) == (float)values[v]);
              unsigned setters = n->setters;
              assert(!mpc_commands_submit(f.commands, 7, &r, 0, &replay));
              assert(!memcmp(&out, &replay, sizeof out));
              r.request_id[0] = 'n';
              r.has_expected = true;
              r.mixer.expected = values[v];
              out = send(&f, r);
              assert(out.status == MPC_COMMAND_APPLIED && !out.native_invoked && !out.changed);
              r.request_id[0] = 'c';
              r.mixer.expected = values[v] < .5 ? 1 : 0;
              out = send(&f, r);
              assert(out.error == MPC_NATIVE_PRECONDITION_FAILED && !out.native_invoked);
              assert(n->setters == setters);
            }
          }
        for (unsigned op = MPC_COMMAND_SEND; op <= MPC_COMMAND_INSERT; op++) {
          struct mpc_command_request r = mixer_request(&f, op, 0, 0, .5, "read_other");
          r.request_id[0] = (char)('a' + op);
          struct mpc_command_receipt out = send(&f, r);
          assert(out.error == MPC_NATIVE_MIXER_AUTOMATION_UNQUALIFIED && !out.native_invoked);
        }
        cleanup(&f);
      }
}
static void read_pan_fences(void) {
  for (unsigned scenario = 0; scenario < 8; scenario++) {
    struct commands_fixture f;
    setup(&f, 8, 64, true);
    f.live_policy = true;
    struct fixture *n = &f.identity.native;
    mixer_init(n, PROGRAM, OBJECTS + 0x30000);
    put32(n, PROGRAM + native_command_context.programAutomation, 1);
    if (scenario == 0) {
      put32(n, PROGRAM + MIXER_PAN_INVOKER, IMAGE + 4);
      struct mpc_command_request noop = mixer_request(&f, MPC_COMMAND_PAN, 0, 0, .5, "source_noop");
      struct mpc_command_receipt receipt = send(&f, noop);
      assert(receipt.status == MPC_COMMAND_APPLIED && !receipt.native_invoked && !n->filters);
    } else if (scenario < 5) {
      n->pan_change_address = scenario == 1 ? PROGRAM + MIXER_PAN_MANAGER
        : scenario == 2                     ? POLICY_PLAYBACK + READ_levelPanPlayValue
        : scenario == 3                     ? PROGRAM + native_command_context.programAutomation
                                            : POLICY_SOURCE + 0x400 + READ_eventTree + 16;
      n->pan_change_value = scenario == 1 ? IMAGE + 8 : scenario == 2 ? 0 : scenario == 3 ? 2 : 1;
    } else if (scenario == 7) {
      n->pan_change_address = OBJECTS + 0x30200;
      n->pan_change_value = OBJECTS + 0x30700;
    } else {
      n->pan_normalize = scenario == 5;
      n->pan_reject = scenario == 6;
    }
    struct mpc_command_request request = mixer_request(&f, MPC_COMMAND_PAN, 0, 0, 1, "pan_fence");
    struct mpc_command_receipt receipt = send(&f, request);
    if (scenario == 0) {
      assert(receipt.error == MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED && !receipt.native_invoked);
    } else if (scenario < 5) {
      assert(receipt.native_invoked && n->filters == 1 && !n->setters);
      assert(
        receipt.error ==
        (scenario == 3 ? MPC_NATIVE_MIXER_AUTOMATION_UNQUALIFIED
                       : MPC_NATIVE_AUTOMATION_SOURCE_CHANGED));
    } else if (scenario == 7) {
      assert(receipt.native_invoked && n->filters == 1 && !n->setters);
      assert(receipt.error == MPC_NATIVE_STALE_MIXER);
    } else if (scenario == 5) {
      /* Normalization preserves the original desired request. Verification
       * cannot claim it applied when the filter selected a different value. */
      assert(receipt.native_invoked && n->filters == 1 && n->setters == 1);
      assert(receipt.status == MPC_COMMAND_UNKNOWN);
      assert(receipt.after.tracks[0].mixer.pan == .25f);
    } else {
      assert(receipt.native_invoked && n->filters == 1 && !n->setters);
      assert(receipt.error == MPC_NATIVE_VALUE_REJECTED);
    }
    cleanup(&f);
  }
}
int main(void) {
  read_pan_fences();
  read_numeric(MPC_COMMAND_VOLUME);
  read_numeric(MPC_COMMAND_PAN);
  read_without_clip();
  struct commands_fixture f;
  setup(&f, 8, 128, true);
  f.live_policy = true;
  struct fixture *n = &f.identity.native;
  mixer_init(n, PROGRAM, OBJECTS + 0x30000);
  mixer_init(n, PROGRAM2, OBJECTS + 0x32000);
  f.identity.before_read = controls_only;
  unsigned serial = 0;
  for (unsigned target_index = 0; target_index < 2; target_index++) {
    for (unsigned op = MPC_COMMAND_VOLUME; op <= MPC_COMMAND_INSERT; op++) {
      unsigned count = op == MPC_COMMAND_SEND || op == MPC_COMMAND_INSERT ? 4 : 1;
      for (unsigned index = 0; index < count; index++) {
        for (unsigned endpoint = 0; endpoint < 2; endpoint++) {
          char id[80];
          snprintf(id, sizeof id, "mixer_%u", serial++);
          struct mpc_command_request r = mixer_request(&f, op, target_index, index, endpoint, id);
          struct mpc_command_receipt result = send(&f, r);
          if (op == MPC_COMMAND_INSERT && index != 0 && index != 3) {
            assert(
              result.status == MPC_COMMAND_REJECTED && result.error == MPC_NATIVE_EMPTY_INSERT);
            assert(!result.native_invoked);
            continue;
          }
          assert(result.status == MPC_COMMAND_APPLIED && result.after_known);
          emit(&result);
          unsigned setters = n->setters, filters = n->filters;
          struct mpc_command_receipt replay;
          assert(!mpc_commands_submit(f.commands, 7, &r, 0, &replay));
          assert(replay.status == MPC_COMMAND_APPLIED && n->setters == setters);
          r.mixer.index = (r.mixer.index + 1) % 4;
          assert(mpc_commands_submit(f.commands, 7, &r, 0, &replay) != MPC_NATIVE_OK);
          r = mixer_request(&f, op, target_index, index, endpoint, "noop");
          snprintf(r.request_id, sizeof r.request_id, "noop_%u", serial++);
          result = send(&f, r);
          assert(result.status == MPC_COMMAND_APPLIED && !result.native_invoked && !result.changed);
          assert(n->setters == setters && n->filters == filters);
          r.has_expected = true;
          r.expected = !endpoint;
          r.mixer.expected = !endpoint;
          if (!mpc_mixer_numeric(op))
            r.mixer.expected = 0;
          snprintf(r.request_id, sizeof r.request_id, "conflict_%u", serial++);
          result = send(&f, r);
          assert(
            result.status == MPC_COMMAND_REJECTED &&
            result.error == MPC_NATIVE_PRECONDITION_FAILED);
          assert(n->setters == setters && n->filters == filters);
          emit(&result);
        }
      }
    }
  }
  /* Preserve the requested double, compare native float steps exactly, and
   * report quantized values instead of requiring unrepresentable equality. */
  struct mpc_command_request r =
    mixer_request(&f, MPC_COMMAND_VOLUME, 0, 0, .123456789, "quantized");
  struct mpc_command_receipt result = send(&f, r);
  assert(result.status == MPC_COMMAND_APPLIED);
  assert(result.after.tracks[0].mixer.volume == (float).123456789);
  emit(&result);
  r = mixer_request(&f, MPC_COMMAND_VOLUME, 0, 0, .123456789, "float_noop");
  r.has_expected = true;
  r.mixer.expected = .123456789;
  result = send(&f, r);
  assert(result.status == MPC_COMMAND_APPLIED && !result.native_invoked);
  /* The current mixer generation is not adopted by a request already queued. */
  r = mixer_request(&f, MPC_COMMAND_VOLUME, 0, 0, .5, "stale_program");
  assert(!mpc_commands_submit(f.commands, 7, &r, 0, &result));
  destroy_object(&f.identity, PROGRAM + MIXER_VOLUME_PROPERTY - NATIVE_DESTRUCTION_OFFSET);
  result = drive(&f, r.request_id);
  assert(result.status == MPC_COMMAND_REJECTED && result.error == MPC_NATIVE_STALE_MIXER);
  assert(!result.native_invoked);
  emit(&result);
  r = mixer_request(&f, MPC_COMMAND_INSERT, 0, 0, 0, "slot_reordered");
  assert(!mpc_commands_submit(f.commands, 7, &r, 0, &result));
  put32(n, OBJECTS + 0x30200, OBJECTS + 0x30700);
  put32(n, OBJECTS + 0x3020c, OBJECTS + 0x30400);
  result = drive(&f, r.request_id);
  assert(result.status == MPC_COMMAND_REJECTED && result.error == MPC_NATIVE_STALE_MIXER);
  emit(&result);
  /* Automation restrictions cover no-ops as well as notifying writes. */
  for (unsigned mode = 2; mode <= 3; mode++) {
    r = mixer_request(&f, MPC_COMMAND_PAN, 0, 0, 1, mode == 2 ? "write" : "unknown");
    put32(n, PROGRAM + native_command_context.programAutomation, mode);
    result = send(&f, r);
    assert(
      result.status == MPC_COMMAND_REJECTED &&
      result.error == MPC_NATIVE_MIXER_AUTOMATION_UNQUALIFIED);
    assert(!result.native_invoked);
    emit(&result);
  }
  put32(n, PROGRAM + native_command_context.programAutomation, 0);
  /* Lagging native model work remains bounded read-only verification. */
  n->propagate_audio = false;
  r = mixer_request(&f, MPC_COMMAND_RACK, 0, 0, 0, "lag");
  assert(!mpc_commands_submit(f.commands, 7, &r, 0, &result));
  while (!queued(&f))
    tiny_pause();
  pump(&f.identity);
  while (!queued(&f))
    tiny_pause();
  f.identity.before_read = blocking_verification;
  pthread_t verification;
  assert(!pthread_create(&verification, NULL, verification_pump, &f.identity));
  pthread_mutex_lock(&f.identity.mutex);
  struct timespec until = deadline();
  while (!verification_entered)
    assert(pthread_cond_timedwait(&f.identity.changed, &f.identity.mutex, &until) != ETIMEDOUT);
  pthread_mutex_unlock(&f.identity.mutex);
  atomic_fetch_add(&f.time_offset, MPC_COMMAND_VERIFICATION_MS + 1);
  result = drive(&f, r.request_id);
  assert(result.status == MPC_COMMAND_UNKNOWN && result.native_invoked);
  char *frozen = mpc_commands_receipt_json(&result);
  assert(frozen);
  assert(!mpc_commands_submit(f.commands, 7, &r, 0, &result));
  char *replay = mpc_commands_receipt_json(&result);
  assert(replay && !strcmp(frozen, replay));
  free(replay);
  pthread_mutex_lock(&f.identity.mutex);
  verification_release = true;
  pthread_cond_broadcast(&f.identity.changed);
  pthread_mutex_unlock(&f.identity.mutex);
  assert(!pthread_join(verification, NULL));
  f.identity.before_read = NULL;
  while (mpc_commands_shutdown(f.commands, 1) == MPC_NATIVE_TIMED_OUT)
    tiny_pause();
  assert(!mpc_commands_get(f.commands, 7, service, r.request_id, 0, &result));
  replay = mpc_commands_receipt_json(&result);
  assert(replay && !strcmp(frozen, replay));
  free(replay);
  free(frozen);
  emit(&result);
  cleanup(&f);
  return 0;
}
