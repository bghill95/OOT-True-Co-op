#ifndef NETWORK_ANCHOR_COOP_LIFE_SYNC_H
#define NETWORK_ANCHOR_COOP_LIFE_SYNC_H
#ifdef __cplusplus

// Co-op death / spectate / revival (Feature A of the game-flow design).
//
// A solo death with a bottled fairy revives immediately with the world running
// (vanilla; nothing to suppress). A solo death WITHOUT a fairy parks the dead
// player in the death-anim WAIT_GROUND state (world still running — the pause
// that vanilla game-over sets is blocked by VB_ADVANCE_TO_GAME_OVER_MENU) and
// enters SPECTATE, following the surviving partner. Only when BOTH players are
// down does vanilla game-over run so both respawn together.
//
// This module owns the local life-state machine (ticked on OnGameFrameUpdate),
// the two VB hooks that keep the world alive during a solo death, the spectate
// camera, and scene-follow. It is the sole caller of SendPacket_PlayerLifeState.
//
// Registered from Anchor::RegisterHooks with the current connection state; all
// hooks unregister and the local state resets to ALIVE when disconnected.
void RegisterCoopLifeSyncHooks(bool isConnected);

// Per-frame state-machine tick. Driven by the Anchor per-frame dispatcher in explicit
// tick order (SETS myLifeState before EnemySync/BgmSync read it), not by a self-
// registered OnGameFrameUpdate hook.
void CoopLifeSyncTick();

#endif // __cplusplus
#endif // NETWORK_ANCHOR_COOP_LIFE_SYNC_H
