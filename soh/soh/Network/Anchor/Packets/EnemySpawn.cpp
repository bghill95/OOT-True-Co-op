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
 * ENEMY_SPAWN
 *
 * Replicates a dynamic enemy spawn (spawned by another actor's AI at runtime,
 * e.g. Stalchildren, Gohma larvae, Octorok rocks) from the authority to
 * same-scene peers, under a host-assigned dynamic key. Re-broadcast
 * periodically for living dynamics so late-joining/room-changing mirrors
 * self-heal (receivers ignore keys they already map).
 */

void Anchor::SendPacket_EnemySpawn(uint64_t enemyKey, int16_t actorId, uint16_t params, Vec3f pos, Vec3s rot,
                                   int16_t roomNum, uint64_t parentKey) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_SPAWN;
    payload["quiet"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = enemyKey;
    payload["actorId"] = actorId;
    payload["params"] = params;
    payload["pos"] = pos;
    payload["rot"] = rot;
    payload["roomNum"] = roomNum;
    payload["parentKey"] = parentKey;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
    ESYNC_LOG("[EnemySync] SPAWN tx id={} key={:#x} parent={:#x}", actorId, enemyKey, parentKey);
}

void Anchor::HandlePacket_EnemySpawn(nlohmann::json payload) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || !EnemySync::MirroringEnabled() || gPlayState == NULL) {
        return;
    }
    if (payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }

    EnemySync::HandleRemoteSpawn(payload["key"].get<uint64_t>(), payload["actorId"].get<int16_t>(),
                                 payload["params"].get<uint16_t>(), payload["pos"].get<Vec3f>(),
                                 payload["rot"].get<Vec3s>(), payload["roomNum"].get<int16_t>(),
                                 payload["parentKey"].get<uint64_t>());
}
