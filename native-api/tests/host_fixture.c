#include "mixer_fixture_backend.h"
#include "host.h"
#include "mpc_family_profile.h"

#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

static struct identity_fixture fixture;
static volatile sig_atomic_t stopping;
static atomic_int finished;
static char gate[4096], pause_ui[4096];
static const char *installation, *runtime, *root;
static bool commands_enabled;
static bool native_policy, navigation_enabled;
static unsigned policy_reads;
static bool sequence_mutes[2];
static bool scope_changed_after_set;

static void select_sequence(unsigned index) {
  put32(&fixture.native, OBJECTS + NATIVE_SELECTED_SEQUENCE, index ? SEQUENCE2 : SEQUENCE);
  put32(&fixture.native, OBJECTS + NATIVE_SELECTED_SEQUENCE_MIRROR, index ? SEQUENCE2 : SEQUENCE);
  put_bool(&fixture.native, PROGRAM + native_bindings[0].requested, sequence_mutes[index]);
}

static bool marker(const char *name) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", installation, name);
  return !access(path, F_OK);
}

/* Fixture-only policy and manual edits; no test switch exists in production. */
static enum mpc_native_error qualify(void *unused, enum mpc_command_operation operation) {
  (void)unused;
  assert(ui);
  if (marker("deny-policy") || (operation == MPC_COMMAND_MUTE && marker("sequence-mute")))
    return MPC_NATIVE_NOT_READY;
  return MPC_NATIVE_OK;
}

static enum mpc_native_error
wire_setter(void *data, uintptr_t function, uintptr_t property, bool value) {
  assert(ui);
  fixture.native.propagate_audio = !marker("lag-audio");
  enum mpc_native_error error = setter(data, function, property, value);
  if (function == IMAGE + native_bindings[0].setter) {
    if (marker("scope-after-set"))
      scope_changed_after_set = true;
    sequence_mutes[marker("sequence-two")] = value;
    if (marker("sequence-after-set"))
      select_sequence(1);
  }
  if (marker("policy-change-after-set"))
    put32(
      &fixture.native,
      POLICY_MODE + native_command_context.modeValue,
      native_command_context.trackMuteMode);
  if (marker("replace-after-set"))
    select_project(&fixture, PROJECT2);
  return error;
}

static enum mpc_native_error
wire_float_filter(void *data, uintptr_t storage, uintptr_t function, float *value) {
  if (function == IMAGE + NAV_navigationTempoFilter) {
    assert(
      ui &&
      (storage == SEQUENCE + NAV_sequenceTempoStorage ||
       storage == SEQUENCE2 + NAV_sequenceTempoStorage));
    return *value >= 20 && *value <= 999 ? MPC_NATIVE_OK : MPC_NATIVE_VALUE_REJECTED;
  }
  return fixture_float_filter(data, storage, function, value);
}
static enum mpc_native_error wire_mixer_set(
  void *data, unsigned op, uintptr_t function, uintptr_t receiver, unsigned index, double value) {
  if (
    function == IMAGE + NAV_navigationTempoSetter &&
    (receiver == SEQUENCE + NAV_sequenceTempoProperty ||
     receiver == SEQUENCE2 + NAV_sequenceTempoProperty)) {
    assert(ui && op == MPC_COMMAND_PAN && index == 0);
    float bpm = (float)value;
    memcpy(
      fixture.native.objects + receiver - NAV_sequenceTempoProperty + NATIVE_SEQUENCE_BPM - OBJECTS,
      &bpm,
      4);
    for (unsigned i = 0; i < MPC_IDENTITY_MAX_SEQUENCES; i++) {
      struct mpc_native_sequence_watch *watch = fixture.sequence_watches[i];
      if (watch && watch->sequence == receiver - NAV_sequenceTempoProperty)
        mpc_sequence_callbacks[4](watch, watch->sequence);
    }
    return MPC_NATIVE_OK;
  }
  return fixture_mixer_set(data, op, function, receiver, index, value);
}

static enum mpc_native_error
wire_track_edit(void *data, unsigned kind, uintptr_t function, uintptr_t receiver, double value) {
  (void)data;
  assert(ui);
  static const unsigned properties[] = {0x5f0, 0x4a4, 0x5c4, 0x3d0, 8, 0x34, 0x60, 0x8c, 0xb8};
  static const unsigned setters[] =
    {0xe5243c, 0xe4fb24, 0xe5af48, 0x1462c7c, 0xe298bc, 0x1ac2e0c, 0x1ac2e0c, 0xe298bc, 0x1ac2e0c};
  static const unsigned kinds[] = {0, 1, 2, 3, 4, 5, 5, 4, 5};
  unsigned field;
  uint32_t track = 0;
  for (field = 0; field < MPC_TRACK_SETUP_FIELDS; field++) {
    if (receiver == TRACK + properties[field])
      track = TRACK;
    if (receiver == TRACK2 + properties[field])
      track = TRACK2;
    if (track)
      break;
  }
  assert(
    track && field < MPC_TRACK_SETUP_FIELDS && function == IMAGE + setters[field] &&
    kind == kinds[field]);
  fixture.native.setters++;
  uint32_t raw = (int32_t)value;
  if (kind == 2) {
    float number = value;
    memcpy(&raw, &number, 4);
  }
  if (!marker("setup-diverged"))
    memcpy(fixture.native.objects + receiver + 0x28 - OBJECTS, &raw, kind >= 4 ? 1 : 4);
  if (marker("setup-replace-after-set"))
    select_project(&fixture, PROJECT2);
  if (marker("setup-family-after-set"))
    put32(&fixture.native, PROGRAM, IMAGE + mpc_family_by_type("audio")->vtable);
  if (marker("setup-context-after-set"))
    put32(&fixture.native, POLICY_MODE + native_command_context.modeValue, 999);
  return MPC_NATIVE_OK;
}

static enum mpc_native_error
wire_navigate(void *data, unsigned op, uintptr_t function, uintptr_t receiver, int64_t value) {
  (void)data;
  assert(ui);
  struct fixture *f = &fixture.native;
  const struct native_command_context_layout *p = &native_command_context;
  if (op == MPC_COMMAND_PLAY || op == MPC_COMMAND_STOP || op == MPC_COMMAND_PLAY_START) {
    assert(receiver == OBJECTS + 0x4A000);
    assert(function == IMAGE + NAV_navigationAsyncPlay);
    assert(value == (op != MPC_COMMAND_STOP));
    put_bool(f, POLICY_TIMELINE + p->timelinePlaying, op != MPC_COMMAND_STOP);
    if (op == MPC_COMMAND_PLAY_START)
      memset(f->objects + POLICY_TIMELINE - OBJECTS + NAV_timelinePulse, 0, 8);
  } else if (op == MPC_COMMAND_LOCATE) {
    assert(function == IMAGE + NAV_navigationAsyncLocate && receiver == OBJECTS + 0x4A000);
    if (marker("navigation-locate-diverged"))
      value = 960;
    memcpy(f->objects + POLICY_TIMELINE - OBJECTS + NAV_timelinePulse, &value, 8);
  } else if (op == MPC_COMMAND_SELECT_TRACK) {
    assert(function == IMAGE + NAV_navigationTrack && receiver == OBJECTS + NAV_trackProperty);
    put32(f, OBJECTS + NATIVE_SELECTED_TRACK, (uint32_t)value);
    put32(f, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, (uint32_t)value);
    put32(f, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, value == TRACK ? 0 : 1);
  } else if (op == MPC_COMMAND_SELECT_SEQUENCE) {
    assert(
      function == IMAGE + NAV_navigationSequence && receiver == OBJECTS + NAV_sequenceProperty);
    select_sequence(value == SEQUENCE2);
  } else
    assert(false);
  if (marker("navigation-pending-after-set"))
    put32(f, OBJECTS + 0x4A000 + NAV_asyncPending, 1);
  if (marker("navigation-replace-after-set"))
    select_project(&fixture, PROJECT2);
  if (marker("navigation-context-after-set"))
    put32(f, POLICY_MODE + p->modeValue, 999);
  return MPC_NATIVE_OK;
}

static int wire_read(void *data, uintptr_t address, void *output, size_t bytes) {
  if (ui && marker("manual-mute"))
    put_bool(&fixture.native, PROGRAM + native_bindings[0].requested, true);
  if (ui && address == TRACK + 0x618 && bytes == 4 && marker("manual-setup")) {
    uint32_t colour = 37;
    memcpy(output, &colour, 4);
    return 0;
  }
  if (ui && marker("manual-pan"))
    mixer_float(&fixture.native, PROGRAM + MIXER_PAN_VALUE, .25f);
  const struct native_command_context_layout *p = &native_command_context;
  if (
    ui && scope_changed_after_set && marker("scope-after-set") &&
    address == POLICY_MUTE + p->perSequenceMute) {
    memset(output, 0, bytes);
    return 0;
  }
  const struct {
    uint32_t address;
    const char *marker;
    unsigned value;
  } changes[] = {
    {POLICY_MODE + p->modeValue, "policy-mixer", p->mixerMode},
    {POLICY_MODE + p->modeValue, "policy-track-mute", p->trackMuteMode},
    {POLICY_MODE + p->modeValue, "policy-sounds", 0x20a0},
    {POLICY_MODE + p->modeValue, "policy-song", 0x2088},
    {POLICY_MODE + p->modeValue, "policy-clips", 0x209c},
    {POLICY_MODE + p->popupValue, "policy-popup", 0x3081},
    {POLICY_TIMELINE + p->timelinePlaying, "policy-playing", 1},
    {POLICY_SEQUENCER + NAV_sequencerCountIn, "navigation-count-in", 1},
    {POLICY_TIMELINE + p->timelinePlaying, "policy-invalid-playing", 2},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, "policy-recording", 1},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, "policy-overdub", 2},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, "policy-record-pending", 3},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, "policy-overdub-pending", 4},
    {POLICY_RECORD_PROPERTIES + p->recordModeValue, "policy-record-unknown", 5},
    {POLICY_SEQUENCER + p->sequencerMode, "policy-song-playback", 2},
    {POLICY_SEQUENCER + p->sequencerMode, "policy-clip-playback", 3},
    {POLICY_SEQUENCER + p->sequencerMode, "policy-playback-unknown", 99},
    {POLICY_SEQUENCER + p->sequencerSongSource, "policy-song-source", 4},
    {POLICY_RECORDER + p->recorderTimeline, "policy-recorder-mismatch", POLICY_TIMELINE + 4},
    {POLICY_RECORD_PROPERTIES, "policy-record-owner", 0},
    {POLICY_MUTE + p->perSequenceMute, "sequence-mute", 1},
    {POLICY_MUTE + p->perSequenceMute, "policy-invalid-bool", 2},
    {POLICY_RECORD_PROPERTIES + p->armModeValue, "policy-arm-mode", 2},
    {POLICY_RECORD_PROPERTIES + p->armModeValue, "policy-arm-alternate", 1},
    {POLICY_RECORDER + p->recorderProperties,
     "policy-instance-mismatch",
     POLICY_RECORD_PROPERTIES + 4},
    {POLICY_SOLO_PROPERTY + p->soloValue, "policy-solo-mode", 2},
    {PROGRAM + p->programAutomation, "policy-automation-read", 1},
    {PROGRAM + p->programAutomation, "policy-automation-write", 2},
    {POLICY_SOURCE + 0x400 + READ_clipEventList, "policy-read-source-unknown", IMAGE + 4},
    {POLICY_THREAD, "policy-broken-owner", 0},
  };
  if (ui && native_policy) {
    if (address == PROGRAM && bytes == 4 && marker("policy-unsupported")) {
      for (unsigned i = 0; i < sizeof native_program_types / sizeof *native_program_types; i++) {
        if (strcmp(native_program_types[i].name, "clip"))
          continue;
        uint32_t value = IMAGE + native_program_types[i].vtable;
        memcpy(output, &value, 4);
        return 0;
      }
      assert(!"fixture unqualified vtable missing");
    }
    for (unsigned i = 0; i < sizeof changes / sizeof *changes; i++) {
      if (address == changes[i].address && marker(changes[i].marker)) {
        uint32_t value = changes[i].value;
        memcpy(output, &value, bytes);
        return 0;
      }
    }
    if (address == POLICY_TIMELINE + p->timelinePlaying && marker("policy-race")) {
      *(unsigned char *)output = policy_reads++ % 2;
      return 0;
    }
  }
  return identity_read(data, address, output, bytes);
}

static void stop_signal(int sig) {
  (void)sig;
  stopping = 1;
}

static int exiting(void *unused) {
  (void)unused;
  return stopping;
}

static void observed(
  void *unused, bool full, enum mpc_native_error error, const struct mpc_executor_result *result) {
  (void)unused;
  assert(!ui); /* Diagnostics never serialize or perform I/O on the native UI. */
  if (!error) {
    assert(result->state == MPC_EXECUTOR_COMPLETED);
    assert(full ? result->snapshot != NULL : result->flags != NULL);
  }
}

static enum mpc_native_error start(void *unused, struct mpc_executor **out) {
  (void)unused;
  if (!access(gate, F_OK))
    return MPC_NATIVE_NOT_READY;
  struct mpc_native_io backend = io(&fixture.native);
  backend.read = wire_read;
  backend.setter = wire_setter;
  backend.float_filter = wire_float_filter;
  backend.mixer_set = wire_mixer_set;
  backend.track_edit = wire_track_edit;
  backend.navigate = wire_navigate;
  backend.on_ui_thread = on_identity_ui;
  backend.post = enqueue;
  backend.watch = watch;
  backend.unwatch = unwatch;
  backend.sequence_watch = sequence_watch;
  backend.sequence_unwatch = sequence_unwatch;
  backend.project_watch = project_watch;
  backend.project_unwatch = project_unwatch;
  return mpc_executor_open_authoritative_backend(&backend, 8, out);
}

static void cancel(void *unused) {
  (void)unused;
}

static void *owner(void *unused) {
  (void)unused;
  struct mpc_host_ops ops = {
    .discovery_root = root,
    .open = start,
    .cancel_start = cancel,
    .exiting = exiting,
    .observed = observed};
  if (commands_enabled)
    ops.command_policy = (struct mpc_command_policy){NULL, qualify};
  ops.native_command_policy = native_policy;
  int status = mpc_host_run(installation, runtime, &ops);
  atomic_store(&finished, status ? 2 : 1);
  return NULL;
}

int main(int argc, char **argv) {
  (void)validate_target;
  assert(argc == 5);
  installation = argv[1];
  runtime = argv[2];
  root = argv[3];
  snprintf(gate, sizeof gate, "%s/gate", installation);
  snprintf(pause_ui, sizeof pause_ui, "%s/pause-ui", installation);
  state_init(&fixture);
  plugin_assignment(&fixture.native, PROGRAM, "Hype", "MPC:Hype");
  navigation_enabled =
    !strcmp(argv[4], "navigation-commands") || !strcmp(argv[4], "setup-commands");
  native_policy =
    navigation_enabled || !strcmp(argv[4], "native-commands") || !strcmp(argv[4], "mixer-commands");
  if (!strcmp(argv[4], "mixer-commands"))
    mixer_init(&fixture.native, PROGRAM, OBJECTS + 0x30000);
  if (!strcmp(argv[4], "setup-commands"))
    put32(&fixture.native, PROGRAM, IMAGE + mpc_family_by_type("midi")->vtable);
  commands_enabled = native_policy || !strcmp(argv[4], "commands");
  if (native_policy) {
    policy_init(&fixture);
    put32(&fixture.native, POLICY_SEQUENCER + NAV_sequencerProject, OBJECTS);
    put32(
      &fixture.native,
      COMPONENT + NATIVE_COMPONENT_ENGINE + NAV_engineSurface,
      OBJECTS + 0x47000);
    put32(&fixture.native, OBJECTS + 0x47000 + NAV_surfaceController, OBJECTS + 0x48000);
    put32(&fixture.native, OBJECTS + 0x48000, IMAGE + NAV_controllerVtable);
    put32(&fixture.native, OBJECTS + 0x48000 + NAV_controllerSequencer, POLICY_SEQUENCER);
    put32(&fixture.native, OBJECTS + 0x48000 + NAV_controllerTime, OBJECTS + 0x49000);
    put_bool(&fixture.native, OBJECTS + 0x48000 + NAV_controllerReady, true);
    put32(&fixture.native, POLICY_SEQUENCER + NAV_sequencerTime, OBJECTS + 0x49000);
    put_bool(&fixture.native, POLICY_SEQUENCER + NAV_sequencerCountIn, false);
    put32(&fixture.native, POLICY_THREAD + NAV_audioThreadAsync, OBJECTS + 0x4A000);
    put32(&fixture.native, OBJECTS + 0x4A000, IMAGE + NAV_asyncVtable);
    put32(&fixture.native, OBJECTS + 0x4A000 + NAV_asyncSequencer, POLICY_SEQUENCER);
    put32(&fixture.native, OBJECTS + 0x4A000 + NAV_asyncQueue, OBJECTS + 0x4B000);
    for (unsigned i = 0; i < 2; i++) {
      uint32_t sequence = i ? SEQUENCE2 : SEQUENCE;
      put32(&fixture.native, sequence + NAV_sequenceTempoManager, IMAGE + 4);
      put32(
        &fixture.native,
        sequence + NAV_sequenceTempoInvoker,
        IMAGE + NAV_navigationTempoFilter);
    }
    /* This transport fixture deliberately shares one Program across its two
     * Track objects. Their synthetic sequence bindings share that Clip too. */
    put32(&fixture.native, POLICY_SEQUENCE_MAP + 0x40 + 20, POLICY_SOURCE + 0x400);
    put32(&fixture.native, POLICY_SEQUENCE_MAP + 0x40 + 24, POLICY_SOURCE + 0x900);
  }
  if (commands_enabled) {
    put_bool(&fixture.native, PROGRAM + native_bindings[1].requested, false);
    put_bool(&fixture.native, TRACK + native_bindings[2].requested, false);
  }
  if (!strcmp(argv[4], "build"))
    strcpy(fixture.native.build, "bad");
  else if (!strcmp(argv[4], "fingerprint"))
    fixture.native.corrupt = 0;
  struct sigaction action = {.sa_handler = stop_signal};
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, NULL);
  mode_t mask = umask(0022);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, owner, NULL));
  while (!atomic_load(&finished)) {
    pthread_mutex_lock(&fixture.mutex);
    bool queued = fixture.queued;
    pthread_mutex_unlock(&fixture.mutex);
    if (queued && (stopping || access(pause_ui, F_OK))) {
      if (marker("plugin-replace"))
        plugin_assignment(&fixture.native, PROGRAM, "TubeSynth", "MPC:TubeSynth");
      if (marker("plugin-vst"))
        vst_assignment(
          &fixture.native,
          PROGRAM,
          "Midimix Editor",
          "/storage/Synths/Midimix/editor.so",
          0x4d6d4564);
      if (marker("plugin-unload"))
        put32(&fixture.native, PROGRAM + PLUGIN_programInstance, 0);
      if (marker("plugin-unassigned")) {
        plugin_assignment(&fixture.native, PROGRAM, "", "");
        put32(&fixture.native, PROGRAM + PLUGIN_programInstance, 0);
      }
      if (marker("watch-replace"))
        select_project(&fixture, PROJECT2);
      if (marker("watch-rename"))
        memcpy(fixture.native.objects + 0x15000, "Renamed", 8);
      if (marker("watch-reorder")) {
        put32(&fixture.native, VECTOR, TRACK2);
        put32(&fixture.native, VECTOR + 4, TRACK);
        put32(&fixture.native, OBJECTS + NATIVE_SELECTED_TRACK_INDEX, 1);
      }
      if (marker("watch-select")) {
        put32(&fixture.native, OBJECTS + NATIVE_SELECTED_TRACK, TRACK2);
        put32(&fixture.native, OBJECTS + NATIVE_SELECTED_TRACK_MIRROR, TRACK2);
        put32(
          &fixture.native,
          OBJECTS + NATIVE_SELECTED_TRACK_INDEX,
          marker("watch-reorder") ? 0 : 1);
      }
      if (native_policy) {
        if (!navigation_enabled || marker("sequence-two"))
          select_sequence(marker("sequence-two"));
        put_bool(
          &fixture.native,
          POLICY_MUTE + native_command_context.perSequenceMute,
          marker("sequence-mute"));
      }
      pump(&fixture);
    } else {
      struct timespec delay = {0, 1000000};
      nanosleep(&delay, NULL);
    }
  }
  assert(!pthread_join(thread, NULL));
  assert(umask(mask) == 0022);
  sigaction(SIGTERM, NULL, &action);
  assert(action.sa_handler == stop_signal);
  assert(fixture.registered == fixture.removed + fixture.expired);
  if (!commands_enabled)
    assert(!fixture.native.setters);
  printf(
    "callbacks_drained=true process_settings_preserved=true setters=%u\n",
    fixture.native.setters);
  return atomic_load(&finished) == 1 ? 0 : 1;
}
