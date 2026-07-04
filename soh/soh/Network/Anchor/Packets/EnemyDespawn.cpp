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
 * ENEMY_DESPAWN
 *
 * Non-death removal of a dynamically spawned enemy (e.g. an Octorok rock
 * despawning on impact). Dynamic keys only: room-listed enemies vanish on the
 * authority's room change, and replicating those kills would wrongly delete
 * live enemies for a mirror still standing in that room (stream staleness ->
 * local AI is the correct behavior there).
 */

void Anchor::SendPacket_EnemyDespawn(uint64_t enemyKey) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DESPAWN;
    payload["quiet"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = enemyKey;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
    ESYNC_LOG("[EnemySync] DESPAWN tx key={:#x}", enemyKey);
}

void Anchor::HandlePacket_EnemyDespawn(nlohmann::json payload) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || !EnemySync::MirroringEnabled() || gPlayState == NULL) {
        return;
    }
    if (payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }

    uint64_t key = payload["key"].get<uint64_t>();
    // Bug D: mirror the EnemyDied ledger. Despawns carry dynamic keys today, which
    // NoteUnresolvedRemoteKill filters out — this just guards against a future
    // static-key despawn arriving while its room is unloaded.
    int16_t keyRoom = (int16_t)(uint8_t)((key >> 48) & 0xFF);
    if (keyRoom != gPlayState->roomCtx.curRoom.num) {
        EnemySync::NoteUnresolvedRemoteKill(key);
        ESYNC_LOG("[EnemySync] DESPAWN rx unloaded-room ledger key={:#x} room={}", key, keyRoom);
    }
    EnemySync::HandleRemoteDespawn(key);
}
