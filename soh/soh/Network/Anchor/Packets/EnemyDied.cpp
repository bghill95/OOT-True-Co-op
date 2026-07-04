#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
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
 * ENEMY_DIED
 *
 * Hard death signal for a tracked enemy/boss, sent when the local copy's health
 * is exhausted. Backstops ENEMY_HIT: the receiver replays a lethal hit so the
 * death animation and drops happen naturally, then force-kills after a grace
 * period if the enemy is somehow still alive.
 */

void Anchor::SendPacket_EnemyDied(Actor* actor, uint64_t enemyKey, bool permanent) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DIED;
    payload["sceneNum"] = gPlayState->sceneNum;
    // The raw key (dynamic keys don't decompose into room/setup/params fields);
    // actorId + homePos ride along for the receiver's fuzzy fallback and logs.
    payload["key"] = enemyKey;
    payload["actorId"] = actor->id;
    payload["homePos"] = actor->home.pos;
    payload["permanent"] = permanent; // Actor_Kill death vs defeat-hook death (regrowers)

    int sent = 0;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
            sent++;
        }
    }
    ESYNC_LOG("[EnemySync] DIED tx id={} n={}", actor->id, sent);
}

void Anchor::HandlePacket_EnemyDied(nlohmann::json payload) {
    if (!IsSaveLoaded() || !EnemySync::SyncEnabled() || gPlayState == NULL) {
        return;
    }
    if (payload["sceneNum"].get<int16_t>() != gPlayState->sceneNum) {
        return;
    }
    ESYNC_LOG("[EnemySync] DIED rx id={}", payload["actorId"].get<int16_t>());

    uint64_t key = payload["key"].get<uint64_t>();
    Actor* actor = EnemySync::FindActorForPacket(key, payload["actorId"].get<int16_t>(),
                                                 payload["homePos"].get<Vec3f>());
    if (actor == NULL) {
        // Bug D: the enemy lives in a room we don't have loaded (the authority can
        // still re-enter it later). Record the kill so its copy dies on room load
        // instead of the death being silently dropped here.
        int16_t keyRoom = (int16_t)(uint8_t)((key >> 48) & 0xFF);
        if (keyRoom != gPlayState->roomCtx.curRoom.num) {
            EnemySync::NoteUnresolvedRemoteKill(key);
            ESYNC_LOG("[EnemySync] DIED rx unloaded-room ledger key={:#x} room={}", key, keyRoom);
        }
        return; // FindActorForPacket already logged the canary if it mattered
    }
    // Adapter-managed bosses hand their defeat to the local simulation on the
    // streamed phase edge; the authority's eventual Actor_Kill must not inject
    // a lethal synthetic hit mid-defeat-cutscene (it would force-kill early and
    // skip the heart container / blue warp / clear flag).
    if (EnemySync::IsDying(actor) && EnemySync::GetAdapter(((Actor*)actor)->id) != nullptr) {
        return;
    }

    Actor* attacker = &GET_PLAYER(gPlayState)->actor;
    uint32_t clientId = payload["clientId"].get<uint32_t>();
    if (clients.contains(clientId) && clients[clientId].player != NULL) {
        attacker = &clients[clientId].player->actor;
    }

    // Death handoff: ApplyRemoteDeath releases suppression for good (after its
    // echo/duplicate guards) so the local simulation plays the death naturally.
    bool permanent = !payload.contains("permanent") || payload["permanent"].get<bool>();
    EnemySync::ApplyRemoteDeath(actor, attacker, permanent);
}
