#ifndef MPC_POLICY_FIXTURE_BACKEND_H
#define MPC_POLICY_FIXTURE_BACKEND_H

#include "identity_fixture_backend.h"

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
  POLICY_ARM = OBJECTS + 0x29000
};

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
  put32(n, POLICY_RECORDER + p->recorderProperties, POLICY_RECORD_PROPERTIES);
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
}

#endif
