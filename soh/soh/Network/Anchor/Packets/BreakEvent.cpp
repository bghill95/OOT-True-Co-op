#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/BreakSync.h"
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
 * BREAK_EVENT
 *
 * Breakable-scenery destruction, breaker -> same-scene peers (see BreakSync.h):
 * one kind, keyed by quantized spawn position + actor id; the receiver picks
 * the per-actor spoof channel from the id.
 */

void Anchor::SendPacket_BreakEvent(nlohmann::json& event) {
    if (!IsSaveLoaded() || !BreakSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload = event;
    payload["type"] = BREAK_EVENT;
    payload["sceneNum"] = gPlayState->sceneNum;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_BreakEvent(nlohmann::json payload) {
    if (!IsSaveLoaded() || !BreakSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (!payload.contains("sceneNum") || payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }

    BreakSync::Ingest(payload);
}
