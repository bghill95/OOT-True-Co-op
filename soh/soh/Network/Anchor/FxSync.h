#ifndef NETWORK_ANCHOR_FX_SYNC_H
#define NETWORK_ANCHOR_FX_SYNC_H
#ifdef __cplusplus

#include <nlohmann/json.hpp>

// Visual-effects sync: magic spells (Din's Fire / Farore's Wind / Nayru's
// Love) replicated on the caster's puppet, and temporary torch lighting
// (Obj_Syokudai litTimer — permanent torches already sync via switch flags).
// Gameplay damage/stun needs nothing here: local hits on mirrored enemies are
// already forwarded with their original dmgFlags (ENEMY_HIT_REQUEST).
namespace FxSync {

bool SyncEnabled();
void Ingest(uint32_t senderClientId, const nlohmann::json& payload);
void RegisterHooks(bool isConnected);

} // namespace FxSync

#endif
#endif
