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
    // The raw key (dynamic keys don't decompose into room/setup/params fields);
    // actorId + homePos ride along for the receiver's fuzzy fallback and logs.
    payload["key"] = enemyKey;
    payload["actorId"] = actor->id;
    payload["homePos"] = actor->home.pos;
    payload["damage"] = damage;
    payload["dmgFlags"] = dmgFlags;
    payload["hitPos"] = hitPos;
    payload["health"] = health;

    int sent = 0;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
            sent++;
        }
    }
    ESYNC_LOG("[EnemySync] HIT tx id={} dmg={} n={}", actor->id, damage, sent);
}

void Anchor::HandlePacket_EnemyHit(nlohmann::json payload) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }
    ESYNC_LOG("[EnemySync] HIT rx id={} dmg={}", payload["actorId"].get<int16_t>(), payload["damage"].get<uint8_t>());

    Actor* actor = EnemySync::FindActorForPacket(payload["key"].get<uint64_t>(), payload["actorId"].get<int16_t>(),
                                                 payload["homePos"].get<Vec3f>());
    if (actor == NULL) {
        return; // FindActorForPacket already logged the canary if it mattered
    }
    // While mirrored, health arrives via ENEMY_STATE — a synthetic hit here would
    // prime collider state the suppressed update never consumes and rot the
    // expectedRemoteDamage accounting (its decay runs in OnActorUpdate).
    if (EnemySync::IsSuppressed(actor)) {
        ESYNC_LOG("[EnemySync] HIT rx dropped (suppressed) key={:#x}", payload["key"].get<uint64_t>());
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
