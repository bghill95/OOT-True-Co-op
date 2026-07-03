#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_DIED
 *
 * Hard death signal for a tracked enemy/boss, sent when the local copy's health
 * is exhausted. Backstops ENEMY_HIT: the receiver replays a lethal hit so the
 * death animation and drops happen naturally, then force-kills after a grace
 * period if the enemy is somehow still alive.
 */

void Anchor::SendPacket_EnemyDied(Actor* actor, uint64_t enemyKey) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DIED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = (int16_t)((enemyKey >> 48) & 0xFF);
    payload["setupIndex"] = (uint16_t)((enemyKey >> 32) & 0xFFFF);
    payload["actorId"] = actor->id;
    payload["params"] = (uint16_t)actor->params;
    payload["homePos"] = actor->home.pos;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyDied(nlohmann::json payload) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = EnemySync::FindActorForPacket(payload["roomNum"].get<int16_t>(),
                                                 payload["setupIndex"].get<uint16_t>(),
                                                 payload["actorId"].get<int16_t>(), payload["params"].get<uint16_t>(),
                                                 payload["homePos"].get<Vec3f>());
    if (actor == NULL) {
        return;
    }

    Actor* attacker = &GET_PLAYER(gPlayState)->actor;
    uint32_t clientId = payload["clientId"].get<uint32_t>();
    if (clients.contains(clientId) && clients[clientId].player != NULL) {
        attacker = &clients[clientId].player->actor;
    }

    EnemySync::ApplyRemoteDeath(actor, attacker);
}
