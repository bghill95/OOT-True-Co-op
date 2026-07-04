#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * PLAYER_LIFE_STATE
 *
 * Edge-triggered life-state broadcast: { state, sceneNum, targetTeamId }.
 *
 * state is a LifeState (0 ALIVE, 1 REVIVING, 2 DOWNED, 3 GAME_OVER). Sent once
 * per transition (Task 8 owns the state machine and the send calls); THIS task
 * only provides the plumbing, so SendPacket_PlayerLifeState is defined but not
 * invoked from any gameplay trigger.
 *
 * Cross-version safe: a legacy peer that never learns this packet type routes it
 * to the dispatch ladder's no-op default (Anchor.cpp), and the life state also
 * rides UPDATE_CLIENT_STATE / ALL_CLIENT_STATE under a contains guard so a
 * life-state-less client-state blob still parses.
 */

void Anchor::SendPacket_PlayerLifeState(u8 state) {
    if (!IsSaveLoaded()) {
        return;
    }

    // Cache locally and mirror into our own client entry so the value stays
    // consistent with what PrepClientState() ships to late joiners.
    myLifeState = state;
    if (clients.contains(ownClientId)) {
        clients[ownClientId].lifeState = state;
    }

    nlohmann::json payload;
    payload["type"] = PLAYER_LIFE_STATE;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["state"] = state;
    payload["sceneNum"] = gPlayState->sceneNum;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_PlayerLifeState(nlohmann::json payload) {
    uint32_t clientId = payload["clientId"].get<uint32_t>();
    u8 state = payload["state"].get<u8>();

    if (!clients.contains(clientId)) {
        return;
    }

    clients[clientId].lifeState = state;

    SPDLOG_INFO("[LifeSync] rx client={} state={}", clientId, state);
}
