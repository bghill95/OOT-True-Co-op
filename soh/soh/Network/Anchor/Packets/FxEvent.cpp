#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/FxSync.h"
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
 * FX_EVENT
 *
 * Visual-effects events, sender -> same-scene peers (see FxSync.h):
 * kind 0 = magic spell cast (replica on the caster's puppet),
 * kind 1 = torch litTimer edge (position key).
 */

void Anchor::SendPacket_FxEvent(nlohmann::json& event) {
    if (!IsSaveLoaded() || !FxSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload = event;
    payload["type"] = FX_EVENT;
    payload["sceneNum"] = gPlayState->sceneNum;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_FxEvent(nlohmann::json payload) {
    if (!IsSaveLoaded() || !FxSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (!payload.contains("sceneNum") || payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }
    if (!payload.contains("clientId")) {
        return;
    }

    FxSync::Ingest(payload["clientId"].get<uint32_t>(), payload);
}
