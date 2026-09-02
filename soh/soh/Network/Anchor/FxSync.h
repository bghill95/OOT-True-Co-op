#ifndef NETWORK_ANCHOR_FX_SYNC_H
#define NETWORK_ANCHOR_FX_SYNC_H
#ifdef __cplusplus

#include <nlohmann/json.hpp>

extern "C" {
#include "z64math.h"
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

} // namespace FxSync

#endif
#endif
