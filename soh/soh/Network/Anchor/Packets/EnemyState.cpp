#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_STATE
 *
 * Batched per-game-frame snapshot of every live tracked enemy, sent by the
 * scene's authority client (lowest clientId) to same-scene peers. Receivers
 * suppress those enemies' updates and mirror the streamed state, so both
 * players see identical positions, movement, and animations.
 */

void Anchor::SendPacket_EnemyState(nlohmann::json& enemies) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_STATE;
    payload["quiet"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["enemies"] = enemies;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyState(nlohmann::json payload) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || !EnemySync::MirroringEnabled() || gPlayState == NULL) {
        return;
    }
    if (!payload.contains("sceneNum") || payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }
    // Split-brain guard: only the currently elected authority's stream is
    // applied, and a client that believes it is the authority applies nothing.
    if (EnemySync::IsLocalAuthority()) {
        return;
    }
    if (!payload.contains("clientId") || payload["clientId"].get<uint32_t>() != EnemySync::CurrentAuthorityId()) {
        return;
    }

    EnemySync::IngestEnemyState(payload);
}
