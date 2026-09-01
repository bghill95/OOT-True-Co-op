#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/BombfSync.h"
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
 * BOMBF_EVENT
 *
 * Bomb Flower events, owner -> same-scene peers (see BombfSync.h):
 *  kind 0 = throw (kinematics + fuse -> cosmetic replica on the receiver)
 *  kind 1 = pluck (flower key -> the receiver empties its copy of the flower)
 */

void Anchor::SendPacket_BombfEvent(nlohmann::json& event) {
    if (!IsSaveLoaded() || !BombfSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload = event;
    payload["type"] = BOMBF_EVENT;
    payload["sceneNum"] = gPlayState->sceneNum;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_BombfEvent(nlohmann::json payload) {
    if (!IsSaveLoaded() || !BombfSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (!payload.contains("clientId")) {
        return;
    }
    if (!payload.contains("sceneNum") || payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }

    BombfSync::Ingest(payload);
}
