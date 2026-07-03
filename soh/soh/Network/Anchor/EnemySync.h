#ifndef NETWORK_ANCHOR_ENEMY_SYNC_H
#define NETWORK_ANCHOR_ENEMY_SYNC_H
#ifdef __cplusplus

#include <cstdint>

extern "C" {
#include "z64.h"
}

/**
 * EnemySync (Milestone 1: shared HP + synced deaths)
 *
 * Tracks every ACTORCAT_ENEMY / ACTORCAT_BOSS actor in the current scene under a
 * composite identity that is deterministic across clients (actors spawn from the
 * room setup lists in file order). When the local player damages a tracked enemy,
 * the applied hit is broadcast; remote hits are re-applied locally through the
 * enemy's own collision-damage path (synthetic AC_HIT) so hurt/death reactions,
 * knockback, and drops play out naturally on every client.
 */
namespace EnemySync {

// (roomNum, setupIndex, actorId, params) packed for map keys and packets.
// setupIndex is the occurrence index of this (actorId, params) combo within the
// room's spawn order — deterministic across clients regardless of room-visit order.
uint64_t PackKey(int16_t roomNum, uint16_t setupIndex, int16_t actorId, uint16_t params);

// Registers/refreshes all EnemySync hooks. Called from Anchor::RegisterHooks with
// the current connection state; hooks unregister when disconnected.
void RegisterHooks(bool isConnected);

// True when the user has enemy sync enabled (CVar, default on).
bool SyncEnabled();

// Exact lookup by packed key, with a fuzzy fallback (same actorId+params, nearest
// home position) that logs loudly — fuzzy hits are the desync canary.
Actor* FindActorForPacket(int16_t roomNum, uint16_t setupIndex, int16_t actorId, uint16_t params, Vec3f homePos);

// Applies a remote hit to the local copy: primes colChkInfo.damage and raises
// AC_HIT with a synthetic attacker element so the enemy's own damage code runs.
// remoteHealth is the sender's post-hit health, used to reconcile drift.
void ApplyRemoteHit(Actor* actor, uint8_t damage, uint32_t dmgFlags, Vec3s hitPos, uint8_t remoteHealth,
                    Actor* attacker);

// Handles a remote death: lethal synthetic hit so the death plays naturally, with
// a force-kill fallback if the enemy is still alive after a grace period.
void ApplyRemoteDeath(Actor* actor, Actor* attacker);

} // namespace EnemySync

#endif // __cplusplus
#endif // NETWORK_ANCHOR_ENEMY_SYNC_H
