#include "mixer_fixture_backend.h"
#include "host.h"

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
static bool native_policy;
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

static int wire_read(void *data, uintptr_t address, void *output, size_t bytes) {
  if (ui && marker("manual-mute"))
    put_bool(&fixture.native, PROGRAM + native_bindings[0].requested, true);
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

static enum mpc_native_error start(void *unused, struct mpc_executor **out) {
  (void)unused;
  if (!access(gate, F_OK))
    return MPC_NATIVE_NOT_READY;
  struct mpc_native_io backend = io(&fixture.native);
  backend.read = wire_read;
  backend.setter = wire_setter;
  backend.float_filter = fixture_float_filter;
  backend.mixer_set = fixture_mixer_set;
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
  struct mpc_host_ops ops =
    {.discovery_root = root, .open = start, .cancel_start = cancel, .exiting = exiting};
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
  native_policy = !strcmp(argv[4], "native-commands") || !strcmp(argv[4], "mixer-commands");
  if (!strcmp(argv[4], "mixer-commands"))
    mixer_init(&fixture.native, PROGRAM, OBJECTS + 0x30000);
  commands_enabled = native_policy || !strcmp(argv[4], "commands");
  if (native_policy)
    policy_init(&fixture);
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
      if (native_policy) {
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
