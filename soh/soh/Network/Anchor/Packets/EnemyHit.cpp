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
 * ENEMY_HIT
 *
 * Sent when the local player damages a tracked enemy/boss, targeted at clients in
 * the same scene. Carries the applied damage so the remote copy replays the hit
 * through its own damage code, plus post-hit health for drift reconciliation.
 */

void Anchor::SendPacket_EnemyHit(Actor* actor, uint64_t enemyKey, uint8_t damage, uint32_t dmgFlags, Vec3s hitPos,
                                 uint8_t health) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_HIT;
    payload["quiet"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = (int16_t)((enemyKey >> 48) & 0xFF);
    payload["setupIndex"] = (uint16_t)((enemyKey >> 32) & 0xFFFF);
    payload["actorId"] = actor->id;
    payload["params"] = (uint16_t)actor->params;
    payload["homePos"] = actor->home.pos;
    payload["damage"] = damage;
    payload["dmgFlags"] = dmgFlags;
    payload["hitPos"] = hitPos;
    payload["health"] = health;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyHit(nlohmann::json payload) {
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

    // Prefer the attacker's puppet so knockback pushes away from the right player
    Actor* attacker = &GET_PLAYER(gPlayState)->actor;
    uint32_t clientId = payload["clientId"].get<uint32_t>();
    if (clients.contains(clientId) && clients[clientId].player != NULL) {
        attacker = &clients[clientId].player->actor;
    }

    EnemySync::ApplyRemoteHit(actor, payload["damage"].get<uint8_t>(), payload["dmgFlags"].get<uint32_t>(),
                              payload["hitPos"].get<Vec3s>(), payload["health"].get<uint8_t>(), attacker);
}
