#include "private.h"
#include "mpc_commands.h"
#include "mpc_adapter_profile.h"

#include <string.h>
#include <math.h>

bool mpc_navigation_request_valid(const struct mpc_command_request *r) {
  if ((unsigned)r->operation < MPC_COMMAND_PLAY)
    return !r->pulse && !r->bpm;
  if (
    (unsigned)r->operation > MPC_COMMAND_SEQUENCE_BPM || r->mute_scope || r->value ||
    r->has_expected || r->expected)
    return false;
  bool track = r->operation == MPC_COMMAND_SELECT_TRACK;
  if (
    (track ? !r->target.track : r->target.track != 0) ||
    (r->operation == MPC_COMMAND_LOCATE ? r->pulse < 0 || r->pulse > INT32_MAX : r->pulse != 0) ||
    (r->operation == MPC_COMMAND_SEQUENCE_BPM ? !isfinite(r->bpm) || r->bpm < 20 || r->bpm > 999
                                              : r->bpm != 0))
    return false;
  if (track)
    return !r->sequence.instance_id[0] && !r->sequence.project && !r->sequence.sequence &&
      !r->sequence.generation;
  return strnlen(r->sequence.instance_id, 37) == 36 &&
    !strncmp(r->sequence.instance_id, "run_", 4) &&
    strspn(r->sequence.instance_id + 4, "0123456789abcdef") == 32 &&
    !memcmp(r->sequence.instance_id, r->target.instance_id, 37) &&
    r->sequence.project == r->target.project && r->sequence.sequence && r->sequence.generation;
}

static bool
read_at(struct mpc_native_adapter *a, uint32_t owner, unsigned offset, void *out, size_t n) {
  return owner && !(owner % 4) && owner <= UINT32_MAX - 0x10000u &&
    !a->io.read(a->io.userdata, owner + offset, out, n);
}
static bool pointer(struct mpc_native_adapter *a, uint32_t owner, unsigned offset, uint32_t *out) {
  return read_at(a, owner, offset, out, 4) && *out && !(*out % 4);
}
static bool object(struct mpc_native_adapter *a, uint32_t owner, unsigned vtable) {
  uint32_t value;
  return read_at(a, owner, 0, &value, 4) && value == a->io.image + vtable;
}

/* Disposable UI-thread path. Neither a saved setting nor a screen label is an
 * ownership authority. Every dereferenced polymorphic owner is checked first. */
enum mpc_native_error mpc_navigation_capture(
  struct mpc_native_adapter *a,
  uint32_t project,
  struct mpc_navigation_context *out,
  struct mpc_navigation_state *state) {
  memset(out, 0, sizeof *out);
  memset(state, 0, sizeof *state);
  enum mpc_native_error error = mpc_native_verify_ui(a);
  if (error)
    return error;
  const struct native_command_context_layout *p = &native_command_context;
  uint32_t app, window, component, thread, mode, mirror;
  if (
    !pointer(a, (uint32_t)a->io.image, NATIVE_APPLICATION_SLOT, &app) ||
    !object(a, app, NATIVE_APPLICATION_VTABLE) ||
    !pointer(a, app, NATIVE_APPLICATION_WINDOW, &window) ||
    !object(a, window, NATIVE_WINDOW_VTABLE) ||
    !pointer(a, window, NATIVE_WINDOW_COMPONENT, &component) ||
    !object(a, component, NATIVE_COMPONENT_VTABLE))
    return MPC_NATIVE_NOT_READY;
  out->engine = component + NATIVE_COMPONENT_ENGINE;
  if (
    !object(a, out->engine, NATIVE_ENGINE_VTABLE) ||
    !pointer(a, out->engine, NATIVE_ENGINE_PROJECT, &out->project) || out->project != project ||
    !object(a, project, NATIVE_PROJECT_VTABLE) ||
    !pointer(a, out->engine, p->engineAudioThread, &thread) ||
    !object(a, thread, p->audioThreadVtable) ||
    !pointer(a, thread, p->audioThreadSequencer, &out->sequencer) ||
    !object(a, out->sequencer, p->sequencerVtable) ||
    !pointer(a, out->sequencer, NAV_sequencerProject, &mirror) || mirror != project ||
    !pointer(a, out->sequencer, p->sequencerTimeline, &out->timeline) ||
    !object(a, out->timeline, p->timelineVtable) ||
    !pointer(a, out->sequencer, p->sequencerRecorder, &out->recorder) ||
    !object(a, out->recorder, p->recorderVtable) ||
    !pointer(a, out->recorder, p->recorderTimeline, &mirror) || mirror != out->timeline ||
    !pointer(a, out->recorder, p->recorderProperties, &out->properties) ||
    !object(a, out->properties, p->recordPropertiesVtable) ||
    !pointer(a, project, NATIVE_SELECTED_SEQUENCE, &out->sequence) ||
    !object(a, out->sequence, NATIVE_SEQUENCE_VTABLE) ||
    !pointer(a, project, NATIVE_SELECTED_SEQUENCE_MIRROR, &mirror) || mirror != out->sequence ||
    !pointer(a, out->engine, p->engineMode, &mode) || !object(a, mode, p->modeVtable) ||
    !read_at(a, mode, p->modeValue, &out->mode, 4) ||
    !read_at(a, mode, p->popupValue, &out->popup, 4) ||
    !read_at(a, out->timeline, p->timelinePlaying, &out->playing, 1) || out->playing > 1 ||
    !read_at(a, out->sequencer, NAV_sequencerCountIn, &out->count_in, 1) || out->count_in > 1 ||
    !read_at(a, out->properties, p->recordModeValue, &out->record_mode, 4) ||
    out->record_mode > 4 || !read_at(a, out->sequencer, p->sequencerMode, &out->playback_mode, 4) ||
    !read_at(a, out->sequencer, p->sequencerSongSource, &out->song_source, 4) ||
    !read_at(a, out->timeline, NAV_timelinePulse, &state->pulse, sizeof state->pulse))
    return MPC_NATIVE_NOT_READY;
  if (
    !read_at(a, out->sequence, NATIVE_SEQUENCE_USED, &out->sequence_used, 1) ||
    out->sequence_used > 1 ||
    !read_at(a, out->sequence, NATIVE_SEQUENCE_BPM, &state->sequence_bpm, 4) ||
    !isfinite(state->sequence_bpm) || state->sequence_bpm < 20 || state->sequence_bpm > 999 ||
    !read_at(
      a,
      out->sequence,
      NAV_sequenceTempoStorage,
      out->tempo_filter,
      sizeof out->tempo_filter))
    return MPC_NATIVE_NOT_READY;
  uint32_t master;
  unsigned char enabled;
  if (
    pointer(a, out->sequencer, NAV_masterOwner, &master) &&
    read_at(a, master, NAV_masterEnabled, &enabled, 1) && enabled <= 1 &&
    read_at(a, master, NAV_masterBpm, &state->master_bpm, 4) && isfinite(state->master_bpm) &&
    state->master_bpm >= 20 && state->master_bpm <= 999) {
    state->master_known = 1;
    state->master_enabled = enabled;
  }
  uint32_t surface, controller, time;
  if (
    pointer(a, out->engine, NAV_engineSurface, &surface) &&
    pointer(a, surface, NAV_surfaceController, &controller) &&
    object(a, controller, NAV_controllerVtable) &&
    pointer(a, controller, NAV_controllerSequencer, &mirror) && mirror == out->sequencer &&
    pointer(a, out->sequencer, NAV_sequencerTime, &time) &&
    pointer(a, controller, NAV_controllerTime, &mirror) && mirror == time &&
    read_at(a, time, NAV_timeSource, &out->time_source, 4) &&
    read_at(a, controller, NAV_controllerReady, &out->transport_ready, 1) &&
    out->transport_ready <= 1)
    out->controller = controller;
  uint32_t native;
  if (
    pointer(a, thread, NAV_audioThreadAsync, &out->async) &&
    object(a, out->async, NAV_asyncVtable) && pointer(a, out->async, NAV_asyncSequencer, &native) &&
    native == out->sequencer && pointer(a, out->async, NAV_asyncQueue, &out->async_queue)) {
    /* Native queue owns its captures; our DSO never runs an audio callback. */
  } else
    out->async = out->async_queue = 0;
  state->known = 1;
  state->playing = out->playing;
  state->record_mode = out->record_mode;
  return MPC_NATIVE_OK;
}

bool mpc_navigation_idle(struct mpc_native_adapter *a, const struct mpc_navigation_context *c) {
  uint32_t pending;
  return c->async && read_at(a, c->async, NAV_asyncPending, &pending, sizeof pending) && !pending;
}

enum mpc_native_error mpc_navigation_qualify(const struct mpc_navigation_context *c, unsigned op) {
  if (!c->controller || !c->transport_ready || !c->async || !c->async_queue)
    return MPC_NATIVE_NOT_READY;
  if (c->time_source)
    return MPC_NATIVE_PLAYBACK_CONTEXT_UNQUALIFIED;
  if (op == MPC_COMMAND_SEQUENCE_BPM && !c->sequence_used)
    return MPC_NATIVE_NOT_READY;
  if (c->record_mode)
    return MPC_NATIVE_RECORDING_UNQUALIFIED;
  if (c->playback_mode || c->song_source)
    return MPC_NATIVE_PLAYBACK_CONTEXT_UNQUALIFIED;
  if (c->popup != native_command_context.noPopup && c->popup != NAV_menuPopup)
    return MPC_NATIVE_NOT_READY;
  /* Initial scope never silently queues a next sequence or moves a running
   * playhead. These require a distinct completion model and qualification. */
  if (
    c->playing &&
    (op == MPC_COMMAND_SELECT_SEQUENCE || op == MPC_COMMAND_LOCATE ||
     op == MPC_COMMAND_SEQUENCE_BPM))
    return MPC_NATIVE_PLAYBACK_CONTEXT_UNQUALIFIED;
  return MPC_NATIVE_OK;
}

enum mpc_native_error mpc_navigation_invoke(
  struct mpc_native_adapter *a,
  const struct mpc_navigation_context *c,
  unsigned op,
  uint32_t target,
  int64_t pulse,
  double bpm,
  bool *invoked) {
  if (op == MPC_COMMAND_SEQUENCE_BPM) {
    if (
      !a->io.float_filter || !a->io.mixer_set || !c->tempo_filter[2] ||
      c->tempo_filter[3] != a->io.image + NAV_navigationTempoFilter)
      return MPC_NATIVE_FILTER_MISSING;
    float value = (float)bpm;
    *invoked = true; /* The notifying native filter is invocation too. */
    enum mpc_native_error error = a->io.float_filter(
      a->io.userdata,
      c->sequence + NAV_sequenceTempoStorage,
      c->tempo_filter[3],
      &value);
    if (error)
      return error;
    struct mpc_navigation_context checked;
    struct mpc_navigation_state state;
    error = mpc_navigation_capture(a, c->project, &checked, &state);
    if (error || memcmp(c, &checked, sizeof checked))
      return error ? error : MPC_NATIVE_STATE_UNSTABLE;
    if (!isfinite(value) || value != (float)bpm)
      return MPC_NATIVE_VALUE_REJECTED;
    /* Same native notifying Property<float> setter as the mixer; only its
     * receiver differs. Never write model memory or bypass the native filter. */
    return a->io.mixer_set(
      a->io.userdata,
      MPC_COMMAND_PAN,
      a->io.image + NAV_navigationTempoSetter,
      c->sequence + NAV_sequenceTempoProperty,
      0,
      value);
  }
  if (!a->io.navigate)
    return MPC_NATIVE_NOT_READY;
  uintptr_t function, receiver;
  switch (op) {
  case MPC_COMMAND_PLAY:
  case MPC_COMMAND_STOP:
    function = NAV_navigationAsyncPlay;
    receiver = c->async;
    pulse = op == MPC_COMMAND_PLAY;
    break;
  case MPC_COMMAND_PLAY_START:
    /* Only executor.c's fenced composition may implement this operation. */
    return MPC_NATIVE_INVALID_ARGUMENT;
  case MPC_COMMAND_LOCATE:
    function = NAV_navigationAsyncLocate;
    receiver = c->async;
    break;
  case MPC_COMMAND_SELECT_TRACK:
    function = NAV_navigationTrack;
    receiver = c->project + NAV_trackProperty;
    pulse = target;
    break;
  case MPC_COMMAND_SELECT_SEQUENCE:
    function = NAV_navigationSequence;
    receiver = c->project + NAV_sequenceProperty;
    pulse = target;
    break;
  default:
    return MPC_NATIVE_INVALID_ARGUMENT;
  }
  *invoked = true;
  return a->io.navigate(a->io.userdata, op, a->io.image + function, receiver, pulse);
}
