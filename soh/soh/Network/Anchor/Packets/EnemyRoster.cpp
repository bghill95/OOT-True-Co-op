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
 * ENEMY_ROSTER
 *
 * Ghost-enemy repair for late room entry. A mirror entering a room requests
 * the authority's roster for it; the authority (if it has that room loaded)
 * replies with {key, alive, hp} for every tracked enemy there. The mirror
 * kills local enemies the authority no longer has (deaths that happened
 * before the mirror was listening) and reconciles health. One packet type:
 * "request": true = query, absent/false = data.
 */

void Anchor::SendPacket_EnemyRosterRequest(int16_t roomNum) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    uint32_t authorityId = EnemySync::CurrentAuthorityId();
    if (authorityId == UINT32_MAX || authorityId == ownClientId) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_ROSTER;
    payload["quiet"] = true;
    payload["request"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = roomNum;
    payload["targetClientId"] = authorityId;

    SendJsonToRemote(payload);
    ESYNC_LOG("[EnemySync] ROSTER req tx room={}", roomNum);
}

void Anchor::HandlePacket_EnemyRoster(nlohmann::json payload) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || !EnemySync::MirroringEnabled() || gPlayState == NULL) {
        return;
    }
    if (payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }
    int16_t roomNum = payload["roomNum"].get<int16_t>();

    if (payload.contains("request") && payload["request"].get<bool>()) {
        // Only answer for a room we actually have loaded, as its authority.
        if (!EnemySync::IsLocalAuthority() || gPlayState->roomCtx.curRoom.num != roomNum) {
            return;
        }
        nlohmann::json reply;
        reply["type"] = ENEMY_ROSTER;
        reply["quiet"] = true;
        reply["sceneNum"] = gPlayState->sceneNum;
        reply["roomNum"] = roomNum;
        reply["entries"] = EnemySync::BuildRoster(roomNum);
        reply["targetClientId"] = payload["clientId"].get<uint32_t>();
        SendJsonToRemote(reply);
        ESYNC_LOG("[EnemySync] ROSTER reply tx room={} n={}", roomNum, reply["entries"].size());
        return;
    }

    // Data: reconcile only against the client we currently elect as authority.
    if (EnemySync::IsLocalAuthority() ||
        payload["clientId"].get<uint32_t>() != EnemySync::CurrentAuthorityId()) {
        return;
    }
    EnemySync::ReconcileRoster(roomNum, payload["entries"]);
}
