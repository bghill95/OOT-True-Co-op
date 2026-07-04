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
 * ENEMY_HIT_REQUEST
 *
 * Sent by a mirroring client when its player lands a hit on a suppressed
 * (stream-driven) enemy, targeted at the authority only. Unlike ENEMY_HIT
 * ("I applied this damage, replay it"), this is "please apply this damage":
 * the sender applied nothing locally — the authority validates the hit through
 * the enemy's own damage code and the outcome streams back via ENEMY_STATE.
 */

void Anchor::SendPacket_EnemyHitRequest(Actor* actor, uint64_t enemyKey, uint8_t damage, uint32_t dmgFlags,
                                        Vec3s hitPos) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    uint32_t authorityId = EnemySync::CurrentAuthorityId();
    if (authorityId == UINT32_MAX || authorityId == ownClientId) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_HIT_REQUEST;
    payload["quiet"] = true;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = enemyKey;
    payload["actorId"] = actor->id;
    payload["homePos"] = actor->home.pos;
    payload["damage"] = damage;
    payload["dmgFlags"] = dmgFlags;
    payload["hitPos"] = hitPos;
    payload["targetClientId"] = authorityId;

    SendJsonToRemote(payload);
    ESYNC_LOG("[EnemySync] HITREQ tx key={:#x} dmg={} flags={:#x}", enemyKey, damage, dmgFlags);
}

void Anchor::HandlePacket_EnemyHitRequest(nlohmann::json payload) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = EnemySync::FindActorForPacket(payload["key"].get<uint64_t>(), payload["actorId"].get<int16_t>(),
                                                 payload["homePos"].get<Vec3f>());
    if (actor == NULL) {
        return;
    }
    // Only apply if we actually own this enemy's AI right now.
    if (EnemySync::IsSuppressed(actor)) {
        // We're no longer this enemy's authority — the requester should retarget
        // the current authority. Log it (with who we think owns it) so a stuck
        // hand-off shows up instead of silently eating the mirror's hit.
        ESYNC_LOG("[EnemySync] HITREQ rx dropped (suppressed) key={:#x} auth={}", payload["key"].get<uint64_t>(),
                  EnemySync::CurrentAuthorityId());
        return;
    }

    // Prefer the requester's puppet so knockback pushes away from the right player
    Actor* attacker = &GET_PLAYER(gPlayState)->actor;
    uint32_t clientId = payload["clientId"].get<uint32_t>();
    if (clients.contains(clientId) && clients[clientId].player != NULL) {
        attacker = &clients[clientId].player->actor;
    }

    uint8_t damage = payload["damage"].get<uint8_t>();
    ESYNC_LOG("[EnemySync] HITREQ rx id={} dmg={}", actor->id, damage);

    // Passing our current health as remoteHealth turns the drift clamp into a
    // no-op — correct for a request: the authority's pool is the only pool.
    EnemySync::ApplyRemoteHit(actor, damage, payload["dmgFlags"].get<uint32_t>(), payload["hitPos"].get<Vec3s>(),
                              actor->colChkInfo.health, attacker);
}
