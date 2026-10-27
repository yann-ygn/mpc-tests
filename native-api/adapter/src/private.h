#ifndef MPC_NATIVE_PRIVATE_H
#define MPC_NATIVE_PRIVATE_H

#include "mpc_native.h"
#include "mpc_executor.h"

/* Exact GNU ARM std::function<void()> layout qualified on this firmware. The
 * fixture exercises the same manager protocol without invoking native code. */
struct mpc_native_function {
  void *data[2];
  int (*manager)(void *, const void *, int);
  void (*invoke)(const void *);
};

/* Native destruction-observer ABI: the subject holds &link, and invokes virtual
 * slot 2 with the complete observer after unlinking it. It never owns/free()s it.
 * Only the atomic alive flag is changed by a destruction callback. */
struct mpc_native_lifetime;
struct mpc_native_lifetime_link {
  struct mpc_native_lifetime_link **previous;
  struct mpc_native_lifetime_link *next;
};
struct mpc_native_lifetime {
  void (*const *vtable)(struct mpc_native_lifetime *);
  struct mpc_native_lifetime_link link;
  bool alive;
};

/* SequenceListener's first two words are a vtable and its destruction subject.
 * Native Notifier<SequenceListener> owns the registration node, not this object. */
struct mpc_native_sequence_watch {
  void (*const *vtable)(struct mpc_native_sequence_watch *, uint32_t);
  struct mpc_native_lifetime_link *destruction_observers;
  uint32_t sequence;
  unsigned char used;
  bool changed, attached;
};

struct mpc_native_project_watch {
  void (*const *vtable)(struct mpc_native_project_watch *, uint32_t);
  struct mpc_native_lifetime_link *destruction_observers;
  uint32_t project;
  bool changed, loading, attached;
};

/* Backend injection is private to the isolated fixture, absent from the DSO's
 * exports. Production always supplies runtime.c's read-only memory backend. */
struct mpc_native_io {
  void *userdata;
  uintptr_t image;
  enum mpc_native_error (*identity)(void *, char[129], char[256]);
  int (*read)(void *, uintptr_t, void *, size_t);
  bool (*on_ui_thread)(void *);
  enum mpc_native_error (*setter)(void *, uintptr_t, uintptr_t, bool);
  enum mpc_native_error (*filter)(void *, uintptr_t, uintptr_t, bool *);
  enum mpc_native_error (*mixer_set)(void *, unsigned, uintptr_t, uintptr_t, unsigned, double);
  enum mpc_native_error (*float_filter)(void *, uintptr_t, uintptr_t, float *);
  bool (*post)(void *, uintptr_t, struct mpc_native_function *);
  enum mpc_native_error (*watch)(void *, uint32_t, struct mpc_native_lifetime *);
  void (*unwatch)(void *, struct mpc_native_lifetime *);
  enum mpc_native_error (*sequence_watch)(void *, uint32_t, struct mpc_native_sequence_watch *);
  enum mpc_native_error (*sequence_unwatch)(void *, struct mpc_native_sequence_watch *, bool);
  enum mpc_native_error (*project_watch)(void *, uint32_t, struct mpc_native_project_watch *);
  enum mpc_native_error (*project_unwatch)(void *, struct mpc_native_project_watch *, bool);
  void (*destroy)(void *);
};

enum mpc_ui_stage {
  MPC_UI_IDENTITY,
  MPC_UI_POLICY,
  MPC_UI_FILTER_SETTER,
  MPC_UI_READBACK,
  MPC_UI_BEFORE,
  MPC_UI_AFTER,
  MPC_UI_OTHER,
  MPC_UI_STAGES
};
struct mpc_ui_profile {
  uint64_t ns[MPC_UI_STAGES], since;
  unsigned stage;
};

struct mpc_mixer_topology {
  uint32_t program, program_vtable, rack, slots[4], formats[4];
};

struct mpc_native_adapter {
  struct mpc_native_io io;
  struct mpc_native_hooks hooks;
  int fault;
  /* UI-thread only. Distinguishes observation/no-op qualification from the
   * boundary immediately before a notifying filter or setter. */
  bool invoking_flag;
  struct mpc_ui_profile *profile;
  char instance_id[37];
};

unsigned mpc_ui_enter(struct mpc_native_adapter *, unsigned);
void mpc_ui_leave(struct mpc_native_adapter *, unsigned);

struct mpc_identity_registry;
enum mpc_native_error mpc_native_verify_ui(struct mpc_native_adapter *);
enum mpc_native_error
mpc_identity_create(struct mpc_native_adapter *, struct mpc_identity_registry **);
void mpc_identity_destroy(struct mpc_identity_registry *);
enum mpc_native_error mpc_identity_detach(struct mpc_identity_registry *);
enum mpc_native_error
mpc_identity_capture(struct mpc_identity_registry *, struct mpc_identity_snapshot *);
enum mpc_native_error
mpc_identity_state(struct mpc_identity_registry *, struct mpc_state_snapshot *);
enum mpc_native_error mpc_identity_resolve(
  struct mpc_identity_registry *, const struct mpc_executor_identity *, struct mpc_native_target *);
enum mpc_native_error
mpc_identity_sequence(struct mpc_identity_registry *, const struct mpc_executor_identity *);
enum mpc_native_error mpc_executor_open_authoritative_backend(
  const struct mpc_native_io *, unsigned, struct mpc_executor **);

enum mpc_native_error mpc_native_target_check(
  struct mpc_native_adapter *, const struct mpc_native_target *, enum mpc_native_family *);
enum mpc_native_error mpc_mixer_read(
  struct mpc_native_adapter *,
  uint32_t,
  unsigned,
  struct mpc_mixer_state *,
  struct mpc_mixer_topology *);
enum mpc_native_error mpc_mixer_set(
  struct mpc_native_adapter *,
  const struct mpc_native_target *,
  unsigned,
  const struct mpc_mixer_request *,
  bool,
  bool,
  bool,
  struct mpc_native_write_result *);
unsigned mpc_mixer_fields(unsigned);
unsigned mpc_mixer_field(unsigned, unsigned);
bool mpc_mixer_numeric(unsigned);
bool mpc_mixer_value(const struct mpc_mixer_state *, unsigned, unsigned, double *);
bool mpc_mixer_request_valid(unsigned, const struct mpc_mixer_request *, bool);

struct native_binding {
  uint32_t property, requested, observed, setter;
  uint32_t filter_storage, filter_manager, filter_invoker, filter_rva;
  bool owner_is_track;
};

struct native_fingerprint {
  uint32_t rva;
  unsigned char bytes[32];
};
struct native_secondary {
  uint32_t offset, vtable;
};

struct native_command_context_layout {
  uint32_t engineMode, modeVtable, modeValue, popupValue, mainMode, mixerMode, trackMuteMode,
    noPopup;
  uint32_t engineAudioThread, audioThreadVtable, audioThreadSequencer, sequencerVtable;
  uint32_t sequencerTimeline, timelineVtable, timelinePlaying;
  uint32_t sequencerRecorder, recorderVtable, recorderProperties;
  uint32_t recorderTimeline, recordPropertiesVtable, recordModeValue;
  uint32_t sequencerMode, normalSequenceMode, sequencerSongSource;
  uint32_t engineArmBehaviour, armBehaviourVtable, armBehaviourProject, armBehaviourProperties,
    armModeValue;
  uint32_t engineMuteProperties, perSequenceMute, engineSoloBehaviour, soloBehaviourVtable;
  uint32_t soloProperty, soloValue, programAutomation;
};
/* Disposable UI-thread policy sample. Never part of a public ABI or receipt. */
struct mpc_command_context {
  uint32_t target_program, target_vtable;
  uint32_t app, window, component, engine, mode, thread, sequencer, timeline, mute, solo,
    solo_property, arm, recorder, record_properties;
  uint32_t view, popup, arm_mode, solo_mode, automation, record_mode, playback_mode, song_source;
  unsigned char playing, sequence_mute;
};
enum mpc_command_operation;
enum mpc_native_error mpc_native_mute_scope(struct mpc_native_adapter *, uint32_t, uint32_t *);
enum mpc_native_error mpc_native_qualify_command(
  struct mpc_native_adapter *,
  const struct mpc_native_target *,
  enum mpc_command_operation,
  enum mpc_mute_scope,
  struct mpc_command_context *,
  bool *,
  struct mpc_native_track_state *);

enum mpc_native_error mpc_native_open_backend(
  const struct mpc_native_io *, const struct mpc_native_hooks *, struct mpc_native_adapter **);
bool mpc_native_on_ui_thread(struct mpc_native_adapter *);
enum mpc_native_error mpc_native_post(struct mpc_native_adapter *, struct mpc_native_function *);
enum mpc_native_error mpc_executor_open_backend(
  const struct mpc_native_io *, const struct mpc_executor_config *, struct mpc_executor **);

#endif
