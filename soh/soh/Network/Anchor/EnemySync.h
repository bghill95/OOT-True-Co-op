#ifndef NETWORK_ANCHOR_ENEMY_SYNC_H
#define NETWORK_ANCHOR_ENEMY_SYNC_H
#ifdef __cplusplus

#include <cstdint>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
}

// Verbose sync tracing for the two-instance test rig: INFO-level, flushed
// immediately (log-grep assertions read files while the game runs). Desync
// canaries stay unconditional SPDLOG_WARN — never route those through this.
#define ESYNC_LOG(...)                    \
    do {                                  \
        if (EnemySync::VerboseSync()) {   \
            SPDLOG_INFO(__VA_ARGS__);     \
        }                                 \
    } while (0)

/**
 * EnemySync
 *
 * M1 (shared HP + synced deaths): tracks every ACTORCAT_ENEMY / ACTORCAT_BOSS
 * actor under a composite identity that is deterministic across clients. Local
 * hits are broadcast; remote hits are re-applied through the enemy's own
 * collision-damage path (synthetic AC_HIT) so reactions and drops play out
 * naturally on every client.
 *
 * M2 (host-authority mirroring): the same-scene client with the lowest clientId
 * runs enemy AI and streams pos/rot/focus/health/jointTable every game frame
 * (ENEMY_STATE). Other clients suppress tracked enemies' updates via the
 * ShouldActorUpdate hook, apply the streamed state in the actor's own update
 * slot, and re-submit its colliders so weapon hits, contact damage, and
 * Z-targeting keep working. Mirror-side hits travel to the authority as
 * ENEMY_HIT_REQUEST. Deaths hand control back to the local simulation
 * (M1 path) so death animations and item drops stay local and natural.
 * Enemies with no fresh stream (authority elsewhere / disconnected) fall back
 * to local AI — plain M1 behavior, per actor.
 */
namespace EnemySync {

// (roomNum, setupIndex, actorId, params) packed for map keys and packets.
// setupIndex is the occurrence index of this (actorId, params) combo within the
// room's spawn order — deterministic across clients regardless of room-visit order.
uint64_t PackKey(int16_t roomNum, uint16_t setupIndex, int16_t actorId, uint16_t params);

// Registers/refreshes all EnemySync hooks. Called from Anchor::RegisterHooks with
// the current connection state; hooks unregister when disconnected.
void RegisterHooks(bool isConnected);

// Per-frame bookkeeping (election, streaming, culling, aging). Driven by the Anchor
// per-frame dispatcher in explicit tick order (runs after CoopLifeSync sets myLifeState),
// not by a self-registered OnGameFrameUpdate hook.
void PerFrameTick();

// True when the user has enemy sync enabled (CVar, default on).
bool SyncEnabled();

// True when verbose sync tracing is enabled (CVar, default off; test rigs set it).
bool VerboseSync();

// True when M2 enemy mirroring is enabled (CVar, default on). Off = pure M1.
bool MirroringEnabled();

// The clientId that owns enemy AI for the local scene (lowest clientId among
// same-scene, save-loaded clients, self included). UINT32_MAX when unknown.
uint32_t CurrentAuthorityId();
bool IsLocalAuthority();

// True while the given actor's update is suppressed and driven by the stream.
bool IsSuppressed(Actor* actor);

// True once a death handoff latched this actor (it will never be re-mirrored).
bool IsDying(Actor* actor);

// True when the actor is a tracked enemy/boss (used by DummyPlayer's PvP path).
bool IsTrackedEnemy(Actor* actor);

// Death handoff: stop mirroring this actor for good and let the local
// simulation play the death (called on ENEMY_DIED receipt, before ApplyRemoteDeath).
void ReleaseForDeath(Actor* actor);

// Game-thread ingest of an ENEMY_STATE payload into the stream cache.
void IngestEnemyState(const nlohmann::json& payload);

// Dynamic-spawn replication and roster repair (called by packet handlers).
void HandleRemoteSpawn(uint64_t key, int16_t actorId, uint16_t params, Vec3f pos, Vec3s rot, int16_t roomNum,
                       uint64_t parentKey);
void HandleRemoteDespawn(uint64_t key);
nlohmann::json BuildRoster(int16_t roomNum);
void ReconcileRoster(int16_t roomNum, const nlohmann::json& entries);

// Records a remote kill (ENEMY_DIED/ENEMY_DESPAWN) whose enemy lives in a room we
// haven't loaded yet, so we apply it on that room's next OnActorInit rather than
// dropping it. Static keys only (dynamic spawns don't exist until their spawn
// packet is processed). Scene-scoped; cleared on scene teardown.
void NoteUnresolvedRemoteKill(uint64_t key);

// Exact lookup by packed key. Static keys fall back to a fuzzy match (same
// actorId+params, nearest home position) that logs loudly — fuzzy hits are the
// desync canary; dynamic keys never fuzzy-match. Keys of recently destroyed
// actors never resolve (prevents mis-kills from stale packets).
Actor* FindActorForPacket(uint64_t key, int16_t actorId, Vec3f homePos);

// Applies a remote hit to the local copy: primes colChkInfo.damage and raises
// AC_HIT with a synthetic attacker element so the enemy's own damage code runs.
// remoteHealth is the sender's post-hit health, used to reconcile drift.
void ApplyRemoteHit(Actor* actor, uint8_t damage, uint32_t dmgFlags, Vec3s hitPos, uint8_t remoteHealth,
                    Actor* attacker);

// Handles a remote death: lethal synthetic hit so the death plays naturally.
// permanent (Actor_Kill deaths) adds a force-kill fallback after a grace period;
// defeat-hook deaths (regrowers) skip it — a copy that can't consume the hit
// must not be despawned one-sidedly. Engages the dying latch itself, only for
// deaths it actually applies.
void ApplyRemoteDeath(Actor* actor, Actor* attacker, bool permanent);

} // namespace EnemySync

#endif // __cplusplus
#endif // NETWORK_ANCHOR_ENEMY_SYNC_H
