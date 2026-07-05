#ifndef NETWORK_ANCHOR_BGM_SYNC_H
#define NETWORK_ANCHOR_BGM_SYNC_H
#ifdef __cplusplus

// BGM rendezvous-seek sync (Task 11 / M4 W3 step 2).
//
// Each player hears the music for the area THEY are in; when both players are in the
// same area the MAIN-BGM track is position-locked so there is no flam/echo. The only
// offset is entry-time skew (each instance starts the area track when its own player
// loads the scene). The fix is a RENDEZVOUS-SEEK: the ARRIVING client seeks its MAIN
// BGM to the RESIDENT peer's scriptCounter (via Audio_StartSeqSkipTicks), so the
// resident's music is never interrupted.
//
// Flow (all gated by the local CVar gRemote.Anchor.BgmSync, default on):
//   * On scene load the arriver sends BGM_POS_REQUEST to same-scene peers; a peer
//     replies BGM_POS{seqId, scriptCounter}; the arriver seeks to it (guards below).
//   * A quiet ~5s BGM_STATE heartbeat measures drift; if it exceeds the CVar
//     threshold only the HIGHER clientId re-seeks (deterministic, no reseek fight).
//   * BGM_RESTART is the restart-mode / long-track fallback (both restart from 0).
// Seek guards: payload seqId == our active MAIN seqId (& 0xFF), not NA_BGM_DISABLED,
// not mid-ocarina, and scene unchanged.
//
// Also hosts the cross-workstream spectate fix: while the local player is DOWNED and
// spectating a living partner (Task 8 myLifeState == LIFE_STATE_DOWNED), the vanilla
// no-fairy-death game-over music is replaced by the live scene BGM.
//
// Called from Anchor::RegisterHooks with the current connection state (mirrors the
// CoopLifeSync / CutsceneSync pattern). Implementation in Packets/BgmSync.cpp.
void RegisterBgmSyncHooks(bool isConnected);

// Per-frame BGM driver (spectate restore, rendezvous request, drift heartbeat). Driven by
// the Anchor per-frame dispatcher in explicit tick order (runs after CoopLifeSync sets
// myLifeState), not by a self-registered OnGameFrameUpdate hook.
void BgmSyncTick();

#endif // __cplusplus
#endif // NETWORK_ANCHOR_BGM_SYNC_H
