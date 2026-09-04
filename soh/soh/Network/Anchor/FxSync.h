#ifndef NETWORK_ANCHOR_FX_SYNC_H
#define NETWORK_ANCHOR_FX_SYNC_H
#ifdef __cplusplus

#include <nlohmann/json.hpp>

extern "C" {
#include "z64math.h"
struct Actor;
}

// Visual-effects sync: magic spells (Din's Fire / Farore's Wind / Nayru's
// Love) replicated on the caster's puppet, and temporary torch lighting
// (Obj_Syokudai litTimer — permanent torches already sync via switch flags).
// Gameplay damage/stun needs nothing here: local hits on mirrored enemies are
// already forwarded with their original dmgFlags (ENEMY_HIT_REQUEST).
namespace FxSync {

bool SyncEnabled();
void Ingest(uint32_t senderClientId, const nlohmann::json& payload);
void RegisterHooks(bool isConnected);

// Puppet boomerang: a neutered EN_BOOM replica pinned each frame to the
// thrower's streamed flight position (PLAYER_UPDATE boomPos/boomYaw). Its
// update is suppressed, so its AT quad is never submitted — it can't land
// false local hits. Called from DummyPlayer's per-frame client apply.
void TickPuppetBoomerang(uint32_t clientId, bool active, Vec3f pos, int16_t yaw, int16_t pitch);

// Puppet projectiles (arrows incl. elemental/nut/seed, hookshot): same model.
// CollectProjectiles adds the local player's in-flight snapshot to the
// PLAYER_UPDATE payload; IngestPlayerProjectiles stores the peer's snapshot;
// TickPuppetProjectiles (DummyPlayer, per frame) reconciles pinned replicas.
void CollectProjectiles(nlohmann::json& payload);
void IngestPlayerProjectiles(uint32_t clientId, const nlohmann::json& payload);
void TickPuppetProjectiles(uint32_t clientId, struct Actor* puppet);
// Kills one client's projectile replicas (its puppet died / it went offline).
void ReapClientProjectiles(uint32_t clientId);

// Amy puzzle (Forest Temple): the driver announces the countdown its timer
// block started on landing; the mirror shows the same HUD timer.
void SendAmyTimer(int16_t seconds);

} // namespace FxSync

#endif
#endif
