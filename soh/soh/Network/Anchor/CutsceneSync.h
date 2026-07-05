#ifndef NETWORK_ANCHOR_CUTSCENE_SYNC_H
#define NETWORK_ANCHOR_CUTSCENE_SYNC_H
#ifdef __cplusplus

// Cutscene sync (Task 9): when one player triggers a story cutscene, the partner
// is pulled to the same spot and the cutscene is replayed LOCALLY on the partner
// (the same "replay locally" discipline as boss co-entry). Two kinds:
//   kind 0 (entrance CS)     - detected by the VB_PLAY_ENTRANCE_CS hook.
//   kind 1 (scene-layer CS)  - detected at OnSceneInit (cutsceneIndex >= 0xFFF0).
// Both broadcast CUTSCENE_SYNC. The receiver (Anchor::HandlePacket_CutsceneSync)
// and the sender detectors + echo latches live in Packets/CutsceneSync.cpp.
//
// Called from Anchor::RegisterHooks with the current connection state (mirrors the
// CoopWarp / BossEntry pattern).
void RegisterCutsceneSyncHooks(bool isConnected);

// Per-frame reconcile of the in-flight pull replay (converges the cleared guard flag and
// ties the echo-latch lifetime to the warp). Driven by the Anchor per-frame dispatcher in
// explicit tick order, not by a self-registered OnGameFrameUpdate hook.
void CutsceneSyncTick();

#endif // __cplusplus
#endif // NETWORK_ANCHOR_CUTSCENE_SYNC_H
