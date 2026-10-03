#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/DynaSync.h"
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
 * DYNA_STATE
 *
 * Batched snapshot of whitelisted moving DynaPoly actors (platforms,
 * elevators), sent by the scene's authority client to same-scene peers.
 * Entries are included only when an actor moved since the last send, plus a
 * periodic keyframe of everything tracked (see DynaSync::KEYFRAME_TICKS), so
 * an idle room costs a few bytes every third of a second.
 *
 * Receivers suppress those actors' updates and mirror the streamed transform;
 * DynaPoly collision follows automatically (see DynaSync.h).
 */

void Anchor::SendPacket_DynaState(nlohmann::json& dynas) {
    if (!IsSaveLoaded() || !DynaSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = DYNA_STATE;
    payload["quiet"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["dynas"] = dynas;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_DynaState(nlohmann::json payload) {
    if (!IsSaveLoaded() || !DynaSync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (!payload.contains("sceneNum") || payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }
    if (!payload.contains("clientId")) {
        return;
    }

    // Per-entry acceptance lives in DynaSync::Ingest: the authority's entries
    // always apply, and claimed ("own") push-block entries apply from anyone —
    // including on the authority itself, which mirrors a claimant's blocks.
    DynaSync::Ingest(payload["clientId"].get<uint32_t>(), payload);
}
