#ifndef NETWORK_ANCHOR_COOP_WARP_H
#define NETWORK_ANCHOR_COOP_WARP_H
#ifdef __cplusplus

extern "C" {
#include "z64.h"
}

// System-initiated warps for the co-op layer. Requests are queued (latest wins;
// these are rare and single-shot) and drained on OnGameFrameUpdate only when the
// world is safe to warp: not in a cutscene, no transition in flight, and not
// paused / in the game-over menu. A queued request expires after 30 s rather than
// firing into a bad state. These bypass the user-facing teleport UI gates
// (CanTeleportTo / teleportMode) because they are system-initiated.
//
// Consumed by boss co-entry (Task 7), death/spectate scene-follow (Task 8), and
// cutscene sync (Task 9). This task only provides the two primitives + the drain.

// Plain entrance warp (models EnemySyncTestBoot.cpp). Entrance-CS triggers stay
// ARMED (respawnFlag is left untouched) so a synced cutscene can replay on load.
// cutsceneIndex >= 0 primes gSaveContext.cutsceneIndex (e.g. 0xFFF0+ scene-layer
// cutscenes); pass -1 to leave it untouched. cutsceneIndex is s32 because
// gSaveContext.cutsceneIndex is s32 and the scene-layer slots (0xFFF0..0xFFFF) do
// not fit an s16 as positive values.
void RequestEntranceWarp(s16 entranceIndex, s32 cutsceneIndex);

// Exact-position warp (models TeleportTo.cpp): RESPAWN_MODE_DOWN + respawnFlag=1,
// which SUPPRESSES entrance-CS triggers (z_demo.c:2200) — correct for spectate
// scene-follow, where re-triggering a cutscene would be wrong.
void RequestPositionWarp(s16 entranceIndex, s16 room, PosRot dest);

// Registers CoopWarp's connection-state bookkeeping. Called from Anchor::RegisterHooks
// with the current connection state (mirrors the PuppetFairy pattern); clears any pending
// request on disconnect. The queued-warp drain itself runs via CoopWarpTick.
void RegisterCoopWarpHooks(bool isConnected);

// Per-frame queued-warp drain (fires a pending warp only in a safe window). Driven by
// the Anchor per-frame dispatcher in explicit tick order, not by a self-registered
// OnGameFrameUpdate hook.
void CoopWarpTick();

#endif // __cplusplus
#endif // NETWORK_ANCHOR_COOP_WARP_H
