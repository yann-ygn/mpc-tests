#ifndef MPC_POLICY_FIXTURE_BACKEND_H
#define MPC_POLICY_FIXTURE_BACKEND_H

#include "identity_fixture_backend.h"
#include "mpc_read_profile.h"

enum {
  POLICY_MODE = OBJECTS + 0x20000,
  POLICY_THREAD = OBJECTS + 0x21000,
  POLICY_SEQUENCER = OBJECTS + 0x22000,
  POLICY_TIMELINE = OBJECTS + 0x23000,
  POLICY_MUTE = OBJECTS + 0x24000,
  POLICY_SOLO = OBJECTS + 0x25000,
  POLICY_SOLO_PROPERTY = OBJECTS + 0x26000,
  POLICY_RECORD_PROPERTIES = OBJECTS + 0x27000,
  POLICY_RECORDER = OBJECTS + 0x28000,
  POLICY_ARM = OBJECTS + 0x29000,
  POLICY_PLAYBACK = OBJECTS + 0x40000,
  POLICY_SOURCE = OBJECTS + 0x41000,
  POLICY_SOURCE2 = OBJECTS + 0x43000,
  POLICY_SEQUENCE_MAP = OBJECTS + 0x45000
};

static inline void policy_source(struct fixture *f, uint32_t program, uint32_t base) {
  uint32_t prop = program + native_bindings[0].property;
  uint32_t listener = base + 0x100, updater = base + 0x200, clip = base + 0x400;
  put32(f, prop + 8, base);
  put32(f, base + 4, prop + 4);
  put32(f, base + 12, listener);
  put32(f, listener, IMAGE + READ_muteListenerVtable);
  put32(f, listener + READ_listenerOwner, updater);
  put32(f, listener + READ_listenerCallback, IMAGE + READ_muteListenerCallback);
  put32(f, listener + READ_listenerParameterGroup, 0x100);
  put32(f, listener + READ_listenerParameterId, 0x101);
  put32(f, listener + READ_listenerClip, clip);
  put32(f, listener + READ_listenerClip + 4, clip + 0x500);
  put32(f, updater + READ_updaterProject, OBJECTS);
  put32(f, clip, IMAGE + READ_clipVtable);
  put32(f, clip + READ_clipEventList, IMAGE + READ_eventListVtable);
}

static inline void policy_init(struct identity_fixture *f) {
  struct fixture *n = &f->native;
  const struct native_command_context_layout *p = &native_command_context;
  uint32_t engine = COMPONENT + NATIVE_COMPONENT_ENGINE;
  put32(n, engine + p->engineMode, POLICY_MODE);
  put32(n, POLICY_MODE, IMAGE + p->modeVtable);
  put32(n, POLICY_MODE + p->modeValue, p->mainMode);
  put32(n, POLICY_MODE + p->popupValue, p->noPopup);
  put32(n, engine + p->engineAudioThread, POLICY_THREAD);
  put32(n, POLICY_THREAD, IMAGE + p->audioThreadVtable);
  put32(n, POLICY_THREAD + p->audioThreadSequencer, POLICY_SEQUENCER);
  put32(n, POLICY_SEQUENCER, IMAGE + p->sequencerVtable);
  put32(n, POLICY_SEQUENCER + p->sequencerTimeline, POLICY_TIMELINE);
  put32(n, POLICY_TIMELINE, IMAGE + p->timelineVtable);
  put32(n, POLICY_SEQUENCER + p->sequencerRecorder, POLICY_RECORDER);
  put32(n, POLICY_RECORDER, IMAGE + p->recorderVtable);
  put32(n, POLICY_RECORDER + p->recorderTimeline, POLICY_TIMELINE);
  put32(n, POLICY_RECORDER + p->recorderProperties, POLICY_RECORD_PROPERTIES);
  put32(n, POLICY_RECORD_PROPERTIES, IMAGE + p->recordPropertiesVtable);
  put32(n, engine + p->engineArmBehaviour, POLICY_ARM);
  put32(n, POLICY_ARM, IMAGE + p->armBehaviourVtable);
  put32(n, POLICY_ARM + p->armBehaviourProject, OBJECTS);
  put32(n, POLICY_ARM + p->armBehaviourProperties, POLICY_RECORD_PROPERTIES);
  /* Poison the old incorrect owner: a coincidental zero must never grant
   * permission or make this test pass when the ownership traversal regresses. */
  put32(n, POLICY_SEQUENCER + p->armModeValue, 0xfedcba98u);
  put32(n, engine + p->engineMuteProperties, POLICY_MUTE);
  put32(n, engine + p->engineSoloBehaviour, POLICY_SOLO);
  put32(n, POLICY_SOLO, IMAGE + p->soloBehaviourVtable);
  put32(n, POLICY_SOLO + p->soloProperty, POLICY_SOLO_PROPERTY);
  put32(n, POLICY_SEQUENCER + READ_sequencerPlaybackProperties, POLICY_PLAYBACK);
  put_bool(n, POLICY_PLAYBACK + READ_muteSoloPlayValue, true);
  put_bool(n, POLICY_PLAYBACK + READ_levelPanPlayValue, true);
  policy_source(n, PROGRAM, POLICY_SOURCE);
  policy_source(n, OBJECTS + 0x16000, POLICY_SOURCE2);
  for (unsigned i = 0; i < 2; i++) {
    uint32_t tree = (i ? SEQUENCE2 : SEQUENCE) + READ_sequenceClipTree;
    put32(n, tree + 4, POLICY_SEQUENCE_MAP);
    put32(n, tree + 8, POLICY_SEQUENCE_MAP);
    put32(n, tree + 12, POLICY_SEQUENCE_MAP + 0x40);
    put32(n, tree + 16, 2);
  }
  put32(n, POLICY_SEQUENCE_MAP + 12, POLICY_SEQUENCE_MAP + 0x40);
  put32(n, POLICY_SEQUENCE_MAP + 16, TRACK);
  put32(n, POLICY_SEQUENCE_MAP + 20, POLICY_SOURCE + 0x400);
  put32(n, POLICY_SEQUENCE_MAP + 24, POLICY_SOURCE + 0x900);
  put32(n, POLICY_SEQUENCE_MAP + 0x40 + 16, TRACK2);
  put32(n, POLICY_SEQUENCE_MAP + 0x40 + 20, POLICY_SOURCE2 + 0x400);
  put32(n, POLICY_SEQUENCE_MAP + 0x40 + 24, POLICY_SOURCE2 + 0x900);
}

#endif
