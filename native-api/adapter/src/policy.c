#include "private.h"
#include "mpc_commands.h"
#include "mpc_adapter_profile.h"

#include <string.h>

static bool
read_value(struct mpc_native_adapter *a, uint32_t owner, uint32_t offset, void *out, size_t bytes) {
  return owner && !(owner % 4) && offset <= UINT32_MAX - owner &&
    !a->io.read(a->io.userdata, owner + offset, out, bytes);
}

static bool pointer(struct mpc_native_adapter *a, uint32_t owner, uint32_t offset, uint32_t *out) {
  return read_value(a, owner, offset, out, 4) && *out && !(*out % 4);
}

static bool object(struct mpc_native_adapter *a, uint32_t address, uint32_t rva) {
  uint32_t vtable;
  return read_value(a, address, 0, &vtable, 4) && vtable == a->io.image + rva;
}

/* Read the live preference through the same verified owners as command policy.
 * This deliberately does not claim an independent inactive-sequence value. */
enum mpc_native_error
mpc_native_mute_scope(struct mpc_native_adapter *a, uint32_t project, uint32_t *scope) {
  uint32_t app, window, component, engine, current, mute;
  unsigned char enabled;
  if (
    !pointer(a, (uint32_t)a->io.image, NATIVE_APPLICATION_SLOT, &app) ||
    !object(a, app, NATIVE_APPLICATION_VTABLE) ||
    !pointer(a, app, NATIVE_APPLICATION_WINDOW, &window) ||
    !object(a, window, NATIVE_WINDOW_VTABLE) ||
    !pointer(a, window, NATIVE_WINDOW_COMPONENT, &component) ||
    !object(a, component, NATIVE_COMPONENT_VTABLE) ||
    (engine = component + NATIVE_COMPONENT_ENGINE) < component ||
    !object(a, engine, NATIVE_ENGINE_VTABLE) ||
    !pointer(a, engine, NATIVE_ENGINE_PROJECT, &current) || current != project ||
    !pointer(a, engine, native_command_context.engineMuteProperties, &mute) ||
    !read_value(a, mute, native_command_context.perSequenceMute, &enabled, 1) || enabled > 1)
    return MPC_NATIVE_READ_FAILED;
  *scope = enabled ? MPC_MUTE_SEQUENCE : MPC_MUTE_GLOBAL;
  return MPC_NATIVE_OK;
}

static bool capture(
  struct mpc_native_adapter *a,
  const struct mpc_native_target *target,
  struct mpc_command_context *out) {
  memset(out, 0, sizeof *out);
  const struct native_command_context_layout *p = &native_command_context;
  uint32_t project, arm_project, arm_properties;
  return pointer(a, (uint32_t)a->io.image, NATIVE_APPLICATION_SLOT, &out->app) &&
    object(a, out->app, NATIVE_APPLICATION_VTABLE) &&
    pointer(a, out->app, NATIVE_APPLICATION_WINDOW, &out->window) &&
    object(a, out->window, NATIVE_WINDOW_VTABLE) &&
    pointer(a, out->window, NATIVE_WINDOW_COMPONENT, &out->component) &&
    object(a, out->component, NATIVE_COMPONENT_VTABLE) &&
    (out->engine = out->component + NATIVE_COMPONENT_ENGINE) >= out->component &&
    object(a, out->engine, NATIVE_ENGINE_VTABLE) &&
    pointer(a, out->engine, NATIVE_ENGINE_PROJECT, &project) && project == target->project &&
    pointer(a, out->engine, p->engineMode, &out->mode) && object(a, out->mode, p->modeVtable) &&
    read_value(a, out->mode, p->modeValue, &out->view, 4) &&
    read_value(a, out->mode, p->popupValue, &out->popup, 4) &&
    pointer(a, out->engine, p->engineAudioThread, &out->thread) &&
    object(a, out->thread, p->audioThreadVtable) &&
    pointer(a, out->thread, p->audioThreadSequencer, &out->sequencer) &&
    object(a, out->sequencer, p->sequencerVtable) &&
    pointer(a, out->sequencer, p->sequencerTimeline, &out->timeline) &&
    object(a, out->timeline, p->timelineVtable) &&
    read_value(a, out->timeline, p->timelinePlaying, &out->playing, 1) &&
    pointer(a, out->sequencer, p->sequencerRecorder, &out->recorder) &&
    object(a, out->recorder, p->recorderVtable) &&
    pointer(a, out->recorder, p->recorderProperties, &out->record_properties) &&
    pointer(a, out->engine, p->engineArmBehaviour, &out->arm) &&
    object(a, out->arm, p->armBehaviourVtable) &&
    pointer(a, out->arm, p->armBehaviourProject, &arm_project) && arm_project == target->project &&
    pointer(a, out->arm, p->armBehaviourProperties, &arm_properties) &&
    arm_properties == out->record_properties &&
    read_value(a, out->record_properties, p->armModeValue, &out->arm_mode, 4) &&
    pointer(a, out->engine, p->engineMuteProperties, &out->mute) &&
    read_value(a, out->mute, p->perSequenceMute, &out->sequence_mute, 1) &&
    pointer(a, out->engine, p->engineSoloBehaviour, &out->solo) &&
    object(a, out->solo, p->soloBehaviourVtable) &&
    pointer(a, out->solo, p->soloProperty, &out->solo_property) &&
    read_value(a, out->solo_property, p->soloValue, &out->solo_mode, 4) &&
    read_value(a, target->program, p->programAutomation, &out->automation, 4);
}

enum mpc_native_error mpc_native_qualify_command(
  struct mpc_native_adapter *a,
  const struct mpc_native_target *target,
  enum mpc_command_operation operation,
  enum mpc_mute_scope scope,
  struct mpc_command_context *baseline,
  bool *known) {
  if ((unsigned)operation > MPC_COMMAND_RECORD_ARMED || (unsigned)scope > MPC_MUTE_SEQUENCE)
    return MPC_NATIVE_INVALID_ARGUMENT;
  /* Verifies firmware, UI ownership, family and live target before using the
   * family's automation layout. Both captures are disposable native reads. */
  struct mpc_native_track_state state;
  enum mpc_native_error error = mpc_native_read_track(a, target, &state);
  if (error)
    return error;
  struct mpc_command_context before, after;
  if (!capture(a, target, &before) || !capture(a, target, &after))
    return MPC_NATIVE_READ_FAILED;
  if (memcmp(&before, &after, sizeof before))
    return MPC_NATIVE_STATE_UNSTABLE;
  const struct native_command_context_layout *p = &native_command_context;
  if (
    (before.view != p->mainMode && before.view != p->mixerMode &&
     before.view != p->trackMuteMode) ||
    before.popup != p->noPopup || before.playing || before.automation || before.arm_mode > 1 ||
    before.solo_mode > 1 || before.sequence_mute > 1)
    return MPC_NATIVE_NOT_READY;
  if (operation == MPC_COMMAND_MUTE && before.sequence_mute != (scope == MPC_MUTE_SEQUENCE))
    return MPC_NATIVE_MUTE_SCOPE_MISMATCH;
  if (*known && memcmp(baseline, &before, sizeof before))
    return MPC_NATIVE_STATE_UNSTABLE;
  *baseline = before;
  *known = true;
  return MPC_NATIVE_OK;
}
