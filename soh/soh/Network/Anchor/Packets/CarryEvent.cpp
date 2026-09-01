#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/CarrySync.h"
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
 * CARRY_EVENT
 *
 * Scene-placed carryable prop transitions (rocks, pots), carrier -> same-scene
 * peers (see CarrySync.h): kind 0 carry / 1 throw / 2 gone.
 */

void Anchor::SendPacket_CarryEvent(nlohmann::json& event) {
    if (!IsSaveLoaded() || !CarrySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload = event;
    payload["type"] = CARRY_EVENT;
    payload["sceneNum"] = gPlayState->sceneNum;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_CarryEvent(nlohmann::json payload) {
    if (!IsSaveLoaded() || !CarrySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (!payload.contains("sceneNum") || payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }
    if (!payload.contains("clientId")) {
        return;
    }

    CarrySync::Ingest(payload["clientId"].get<uint32_t>(), payload);
}
