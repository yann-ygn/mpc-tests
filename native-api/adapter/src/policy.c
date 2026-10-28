#include "private.h"
#include "mpc_commands.h"
#include "mpc_adapter_profile.h"
#include "mpc_family_profile.h"
#include "mpc_read_profile.h"
#include "mpc_mixer_profile.h"

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

/* The native subscription binds this Program to the active per-track Clip.
 * Validate the member-function ABI and parameter key, not just its RTTI. No
 * event absence, HasAutomation cache, or selected-track UI state grants access.
 * A populated Clip is eligible: later native updates may supersede our value. */
static bool
clip_source(struct mpc_native_adapter *a, uint32_t program, struct mpc_read_sources *out) {
  uint32_t property = program + native_bindings[0].property;
  uint32_t node, listener, vtable;
  if (!pointer(a, property, 8, &node))
    return false;
  for (unsigned i = 0; node != property + 4 && i < READ_listenerLimit; i++) {
    uint32_t link[3];
    if (!read_value(a, node, 4, link, sizeof link))
      return false;
    listener = link[2] & ~3u;
    if (
      listener && read_value(a, listener, 0, &vtable, 4) &&
      vtable == a->io.image + READ_muteListenerVtable) {
      uint32_t capture[7];
      unsigned char applying;
      if (
        out->listener || (link[2] & 3) ||
        !read_value(a, listener, READ_listenerOwner, capture, sizeof capture) || !capture[0] ||
        capture[1] != a->io.image + READ_muteListenerCallback || capture[2] ||
        capture[3] != 0x100 || capture[4] != 0x101 || (!!capture[5] != !!capture[6]) ||
        !read_value(a, capture[0], READ_updaterApplying, &applying, 1) || applying)
        return false;
      out->listener = listener;
      out->updater = capture[0];
      out->clip = capture[5];
      out->clip_owner = capture[6];
    }
    node = link[0];
    if (!node || node % 4)
      return false;
  }
  /* A missing subscription is not an absence claim: new Tracks can already
   * have a Clip before this optional mute listener is bound. The independent
   * map lookup below establishes the canonical source in either case. */
  return node == property + 4;
}

/* Match the subscription against Sequence's real Track*-keyed shared Clip map
 * (the read-only form of native lookup 0x25b9d88). A current sequence pointer
 * and a surviving subscription alone cannot establish their association. */
static bool sequence_clip(
  struct mpc_native_adapter *a,
  const uint32_t tree[5],
  uint32_t track,
  struct mpc_read_sources *source) {
  if (tree[4] > MPC_IDENTITY_MAX_TRACKS + MPC_STATE_MAX_BUSES || (!!tree[1] != !!tree[4]))
    return false;
  uint32_t node = tree[1];
  for (unsigned i = 0; node && i < tree[4]; i++) {
    uint32_t entry[5];
    if (!read_value(a, node, 8, entry, sizeof entry))
      return false;
    if (entry[2] == track) {
      if (source->listener && (entry[3] != source->clip || entry[4] != source->clip_owner))
        return false;
      /* Global properties do not require a per-sequence mute subscription.
       * New Tracks can have a canonical Clip before that listener exists. */
      source->clip = entry[3];
      source->clip_owner = entry[4];
      return (!!source->clip == !!source->clip_owner) &&
        (!source->clip ||
         (object(a, source->clip, READ_clipVtable) &&
          object(a, source->clip + READ_clipEventList, READ_eventListVtable) &&
          read_value(
            a,
            source->clip,
            READ_eventTree,
            source->event_tree,
            sizeof source->event_tree) &&
          source->event_tree[4] <= 1000000u));
    }
    node = entry[track < entry[2] ? 0 : 1];
  }
  return !node && !source->clip && !source->clip_owner;
}

static enum mpc_native_error capture_sources(
  struct mpc_native_adapter *a,
  const struct mpc_native_target *target,
  unsigned operation,
  struct mpc_command_context *context) {
  struct mpc_read_sources *s = &context->sources;
  uint32_t mirror;
  if (
    !pointer(a, target->project, NATIVE_SELECTED_SEQUENCE, &s->sequence) ||
    !read_value(a, target->project, NATIVE_SELECTED_SEQUENCE_MIRROR, &mirror, 4) ||
    mirror != s->sequence || !object(a, s->sequence, NATIVE_SEQUENCE_VTABLE) ||
    !read_value(a, s->sequence, READ_sequenceClipTree, s->sequence_tree, sizeof s->sequence_tree) ||
    !clip_source(a, target->program, s) || !sequence_clip(a, s->sequence_tree, target->track, s) ||
    (s->updater &&
     (!read_value(a, s->updater, READ_updaterProject, &mirror, 4) || mirror != target->project)) ||
    !pointer(a, context->sequencer, READ_sequencerPlaybackProperties, &s->playback_properties) ||
    !read_value(a, s->playback_properties, READ_muteSoloPlayValue, &s->mute_solo_play, 1) ||
    !read_value(a, s->playback_properties, READ_levelPanPlayValue, &s->level_pan_play, 1) ||
    s->mute_solo_play > 1 || s->level_pan_play > 1)
    return MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED;
  /* Pan's native parameter 0x205 reaches Mixable +0x394 (Program
   * +0x3d0), through the same filtered/notifying property path as manual
   * control. Fence its live filter capture as well as the canonical Clip;
   * volume ownership alone is insufficient. Capture contains the clamp
   * limits, not the current pan value, so native playback remains eligible. */
  if (
    operation == MPC_COMMAND_PAN &&
    (!read_value(a, target->program, MIXER_PAN_STORAGE, s->pan_filter, sizeof s->pan_filter) ||
     !s->pan_filter[2] || s->pan_filter[3] != a->io.image + MIXER_PAN_FILTER))
    return MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED;
  /* Single solo invokes native peer setters. Every possible user peer must
   * have a qualified Program/source and Off/Read policy too; a Write peer must
   * not inherit the target's Read permission. Multi only changes its target. */
  if (operation != MPC_COMMAND_SOLO || context->solo_mode)
    return MPC_NATIVE_OK;
  uint32_t pool, begin, end, tracks[MPC_IDENTITY_MAX_TRACKS];
  if (
    !pointer(a, target->project, NATIVE_POOL_OFFSET, &pool) ||
    !read_value(a, pool, NATIVE_USER_BEGIN, &begin, 4) ||
    !read_value(a, pool, NATIVE_USER_END, &end, 4) || !begin || begin % 4 || end < begin ||
    (end - begin) % 4 || (end - begin) / 4 > MPC_IDENTITY_MAX_TRACKS ||
    !read_value(a, begin, 0, tracks, end - begin))
    return MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED;
  s->peers = UINT64_C(14695981039346656037);
  for (unsigned i = 0; i < (end - begin) / 4; i++) {
    uint32_t program, vtable, mode;
    if (
      !object(a, tracks[i], NATIVE_TRACK_VTABLE) ||
      !pointer(a, tracks[i], NATIVE_PROGRAM_OFFSET, &program) ||
      !read_value(a, program, 0, &vtable, 4) ||
      !read_value(a, program, native_command_context.programAutomation, &mode, 4))
      return MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED;
    unsigned family = 0;
    for (unsigned j = 1; j <= MPC_NATIVE_AUDIO; j++)
      if (vtable == a->io.image + mpc_family_by_id(j)->vtable)
        family = j;
    if (!family || mode > 1)
      return MPC_NATIVE_AUTOMATION_PEER_UNQUALIFIED;
    struct mpc_read_sources peer = {0};
    if (
      !clip_source(a, program, &peer) || !sequence_clip(a, s->sequence_tree, tracks[i], &peer) ||
      (peer.updater &&
       (!read_value(a, peer.updater, READ_updaterProject, &mirror, 4) ||
        mirror != target->project)))
      return MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED;
    uint32_t words[] =
      {tracks[i], program, vtable, mode, peer.listener, peer.updater, peer.clip, peer.clip_owner};
    for (unsigned j = 0; j < sizeof words / sizeof *words; j++)
      s->peers = (s->peers ^ words[j]) * UINT64_C(1099511628211);
    for (unsigned j = 0; j < READ_eventTreeWords; j++)
      s->peers = (s->peers ^ peer.event_tree[j]) * UINT64_C(1099511628211);
  }
  return MPC_NATIVE_OK;
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
  out->target_program = target->program;
  const struct native_command_context_layout *p = &native_command_context;
  uint32_t project, arm_project, arm_properties, recorder_timeline;
  return read_value(a, target->program, 0, &out->target_vtable, 4) &&
    pointer(a, (uint32_t)a->io.image, NATIVE_APPLICATION_SLOT, &out->app) &&
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
    read_value(a, out->sequencer, p->sequencerMode, &out->playback_mode, 4) &&
    read_value(a, out->sequencer, p->sequencerSongSource, &out->song_source, 4) &&
    pointer(a, out->sequencer, p->sequencerTimeline, &out->timeline) &&
    object(a, out->timeline, p->timelineVtable) &&
    read_value(a, out->timeline, p->timelinePlaying, &out->playing, 1) &&
    pointer(a, out->sequencer, p->sequencerRecorder, &out->recorder) &&
    object(a, out->recorder, p->recorderVtable) &&
    pointer(a, out->recorder, p->recorderTimeline, &recorder_timeline) &&
    recorder_timeline == out->timeline &&
    pointer(a, out->recorder, p->recorderProperties, &out->record_properties) &&
    object(a, out->record_properties, p->recordPropertiesVtable) &&
    read_value(a, out->record_properties, p->recordModeValue, &out->record_mode, 4) &&
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
  bool *known,
  struct mpc_native_track_state *readback) {
  if ((unsigned)operation > MPC_COMMAND_INSERT || (unsigned)scope > MPC_MUTE_SEQUENCE)
    return MPC_NATIVE_INVALID_ARGUMENT;
  /* Verifies firmware, UI ownership, family and live target before using the
   * family's automation layout. Both captures are disposable native reads. */
  struct mpc_native_track_state state;
  enum mpc_native_error error = mpc_native_read_track(a, target, &state);
  if (error)
    return error;
  if (
    operation >= MPC_COMMAND_VOLUME
      ? !mpc_mixer_fields(state.family)
      : !(mpc_family_by_id(state.family)->operations & (1u << operation)))
    return MPC_NATIVE_UNSUPPORTED_TRACK_TYPE;
  struct mpc_command_context before, after;
  if (!capture(a, target, &before) || !capture(a, target, &after))
    return MPC_NATIVE_READ_FAILED;
  bool read_write = before.automation == 1 &&
    (operation == MPC_COMMAND_MUTE || operation == MPC_COMMAND_SOLO ||
     operation == MPC_COMMAND_VOLUME || operation == MPC_COMMAND_PAN);
  if (read_write) {
    before.source_error = capture_sources(a, target, operation, &before);
    after.source_error = capture_sources(a, target, operation, &after);
    /* Without a Clip the native property has no per-sequence updater. Do not
     * label a global Program change as a sequence write or create source data
     * implicitly. Matching no-ops retain their existing observation behavior. */
    if (operation == MPC_COMMAND_MUTE && scope == MPC_MUTE_SEQUENCE) {
      if (!before.source_error && (!before.sources.clip || !before.sources.listener))
        before.source_error = MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED;
      if (!after.source_error && (!after.sources.clip || !after.sources.listener))
        after.source_error = MPC_NATIVE_AUTOMATION_SOURCE_UNQUALIFIED;
    }
  }
  if (memcmp(&before, &after, sizeof before))
    return MPC_NATIVE_STATE_UNSTABLE;
  const struct native_command_context_layout *p = &native_command_context;
  if (
    (before.view != p->mainMode && before.view != p->mixerMode &&
     before.view != p->trackMuteMode) ||
    before.popup != p->noPopup || before.playing > 1 || before.arm_mode > 1 ||
    before.solo_mode > 1 || before.sequence_mute > 1)
    return MPC_NATIVE_NOT_READY;
  /* The recorder enum covers pending as well as live Record/Overdub. Neither
   * stopped transport nor a false IsRecordingActive establishes recording Off.
   * The visible screen alone cannot establish an ordinary sequence source.
   * The separate Song source and unknown enum values also fail closed. */
  if (before.record_mode)
    return MPC_NATIVE_RECORDING_UNQUALIFIED;
  if (before.playback_mode != p->normalSequenceMode || before.song_source)
    return MPC_NATIVE_PLAYBACK_CONTEXT_UNQUALIFIED;
  if (
    operation >= MPC_COMMAND_VOLUME && before.automation &&
    !((operation == MPC_COMMAND_VOLUME || operation == MPC_COMMAND_PAN) && before.automation == 1))
    return MPC_NATIVE_MIXER_AUTOMATION_UNQUALIFIED;
  if (before.automation > 1)
    return MPC_NATIVE_AUTOMATION_UNQUALIFIED;
  if (operation == MPC_COMMAND_MUTE && before.sequence_mute != (scope == MPC_MUTE_SEQUENCE))
    return MPC_NATIVE_MUTE_SCOPE_MISMATCH;
  if (
    *known && baseline->automation == before.automation &&
    (baseline->source_error != before.source_error ||
     memcmp(&baseline->sources, &before.sources, sizeof before.sources)))
    return MPC_NATIVE_AUTOMATION_SOURCE_CHANGED;
  if (*known && memcmp(baseline, &before, sizeof before))
    return MPC_NATIVE_STATE_UNSTABLE;
  /* Expected/no-op checks still precede invocation. Source-qualified Read
   * writes use the original native setter once; verification never writes. */
  if (read_write && a->invoking_flag && before.source_error)
    return before.source_error;
  *baseline = before;
  *known = true;
  *readback = state;
  return MPC_NATIVE_OK;
}
