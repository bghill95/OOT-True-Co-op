#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/BombSync.h"
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
 * BOMB_EVENT
 *
 * Player-placed bomb (EN_BOM) events, owner -> same-scene peers (see
 * BombSync.h): kind 0 throw / 1 carry / 2 detonate, correlated by per-client
 * body id.
 */

void Anchor::SendPacket_BombEvent(nlohmann::json& event) {
    if (!IsSaveLoaded() || !BombSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload = event;
    payload["type"] = BOMB_EVENT;
    payload["sceneNum"] = gPlayState->sceneNum;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_BombEvent(nlohmann::json payload) {
    if (!IsSaveLoaded() || !BombSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (!payload.contains("sceneNum") || payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }
    if (!payload.contains("clientId")) {
        return;
    }

    BombSync::Ingest(payload);
}
