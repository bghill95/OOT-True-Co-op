#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/Anchor.h"
#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "variables.h"
#include "functions.h"

extern PlayState* gPlayState;
}

namespace EnemySync {

// How long a remote-damage expectation lives before being discarded (frames).
// Covers enemies that consume the synthetic hit late (i-frames, knockback states).
static constexpr uint8_t EXPECTED_DAMAGE_LIFETIME = 60;
// Frames to wait for a lethal synthetic hit to play out before force-killing.
static constexpr uint8_t REMOTE_DEATH_GRACE = 30;

struct TrackedState {
    uint64_t key;
    Vec3f spawnPos;
    uint8_t prevHealth;
    // Damage we applied on behalf of a remote client; matching local health drops
    // are consumed instead of re-broadcast, preventing echo loops.
    uint8_t expectedRemoteDamage = 0;
    uint8_t expectedRemoteDamageTimer = 0;
    uint8_t pendingKillFrames = 0; // >0: counting down to a force-kill
    bool deathHandled = false;     // ENEMY_DIED sent or received for this actor
    Collider* acCollider = nullptr; // last AC collider this actor registered
};

static std::unordered_map<uint64_t, Actor*> keyToActor;
static std::unordered_map<Actor*, TrackedState> tracked;

// Occurrence counters for (actorId, params) within the currently loaded room.
// Occurrence order follows the room's setup list, which is identical on every
// client, and resets on room change — so indices match no matter what order each
// player visited rooms in, or whether they re-entered one.
static std::unordered_map<uint32_t, uint16_t> spawnCounts;
static int16_t countersRoomNum = -1;

// Ring of synthetic attacker elements; acHitInfo pointers handed to enemies must
// stay valid until their update consumes them a frame or two later.
static ColliderInfo syntheticToucherPool[8];
static uint8_t syntheticToucherIndex = 0;

uint64_t PackKey(int16_t roomNum, uint16_t setupIndex, int16_t actorId, uint16_t params) {
    return ((uint64_t)(uint8_t)roomNum << 48) | ((uint64_t)setupIndex << 32) | ((uint64_t)(uint16_t)actorId << 16) |
           (uint64_t)params;
}

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("EnemySync"), 1);
}

static bool IsTrackedCategory(Actor* actor) {
    return actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS;
}

template <typename F> static void ForEachColliderInfo(Collider* collider, F fn) {
    switch (collider->shape) {
        case COLSHAPE_CYLINDER:
            fn(&((ColliderCylinder*)collider)->info);
            break;
        case COLSHAPE_QUAD:
            fn(&((ColliderQuad*)collider)->info);
            break;
        case COLSHAPE_JNTSPH: {
            ColliderJntSph* jntSph = (ColliderJntSph*)collider;
            for (int32_t i = 0; i < jntSph->count; i++) {
                fn(&jntSph->elements[i].info);
            }
            break;
        }
        case COLSHAPE_TRIS: {
            ColliderTris* tris = (ColliderTris*)collider;
            for (int32_t i = 0; i < tris->count; i++) {
                fn(&tris->elements[i].info);
            }
            break;
        }
    }
}

static void Reset() {
    keyToActor.clear();
    tracked.clear();
    spawnCounts.clear();
    countersRoomNum = -1;
}

Actor* FindActorForPacket(int16_t roomNum, uint16_t setupIndex, int16_t actorId, uint16_t params, Vec3f homePos) {
    uint64_t key = PackKey(roomNum, setupIndex, actorId, params);
    auto it = keyToActor.find(key);
    if (it != keyToActor.end()) {
        return it->second;
    }

    // Fuzzy fallback: same actor type and params, nearest spawn position. A hit
    // here means spawn order diverged between clients — log it as a canary.
    Actor* best = nullptr;
    float bestDistSq = 250.0f * 250.0f; // beyond this it's likely a different enemy
    for (auto& [actor, state] : tracked) {
        if (actor->id != actorId || (uint16_t)actor->params != params) {
            continue;
        }
        float dx = state.spawnPos.x - homePos.x;
        float dy = state.spawnPos.y - homePos.y;
        float dz = state.spawnPos.z - homePos.z;
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = actor;
        }
    }

    if (best != nullptr) {
        SPDLOG_WARN("[EnemySync] Fuzzy-matched enemy actorId={} params={} (exact key miss — spawn order desync?)",
                    actorId, params);
    } else if (gPlayState != NULL && gPlayState->roomCtx.curRoom.num == roomNum) {
        // Only a desync signal when we actually have that room loaded; enemies in
        // other rooms legitimately don't exist locally.
        SPDLOG_WARN("[EnemySync] No match for enemy actorId={} params={} setupIndex={} room={}", actorId, params,
                    setupIndex, roomNum);
    }
    return best;
}

void ApplyRemoteHit(Actor* actor, uint8_t damage, uint32_t dmgFlags, Vec3s hitPos, uint8_t remoteHealth,
                    Actor* attacker) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;

    // Reconcile drift: local health should never exceed the remote pre-hit pool
    uint8_t remotePreHit = (uint8_t)std::min<int>(remoteHealth + damage, 255);
    if (actor->colChkInfo.health > remotePreHit) {
        actor->colChkInfo.health = remotePreHit;
        state.prevHealth = remotePreHit;
    }

    ColliderInfo* synthetic = &syntheticToucherPool[syntheticToucherIndex];
    syntheticToucherIndex = (syntheticToucherIndex + 1) % 8;
    memset(synthetic, 0, sizeof(ColliderInfo));
    synthetic->toucher.dmgFlags = dmgFlags != 0 ? dmgFlags : DMG_SLASH;
    synthetic->toucher.damage = damage;
    synthetic->toucherFlags = TOUCH_ON | TOUCH_HIT;

    // Prime the same state a real collision-check hit would have left behind,
    // then let the enemy's own update consume it (hurt anim, knockback, death).
    // Same trick Anchor already uses for destructible walls in HookHandlers.cpp.
    actor->colChkInfo.damage = damage;

    if (state.acCollider != nullptr) {
        Collider* col = state.acCollider;
        col->acFlags |= AC_HIT;
        col->ac = attacker;
        ForEachColliderInfo(col, [&](ColliderInfo* info) {
            info->bumperFlags |= BUMP_HIT;
            info->acHitInfo = synthetic;
            info->bumper.hitPos = hitPos;
        });
    } else {
        // No collider seen yet (e.g. custom boss damage paths): apply directly
        actor->colChkInfo.health = (actor->colChkInfo.health > damage) ? actor->colChkInfo.health - damage : 0;
        state.prevHealth = actor->colChkInfo.health;
        return;
    }

    state.expectedRemoteDamage = (uint8_t)std::min<int>(state.expectedRemoteDamage + damage, 255);
    state.expectedRemoteDamageTimer = EXPECTED_DAMAGE_LIFETIME;
}

void ApplyRemoteDeath(Actor* actor, Actor* attacker) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;
    if (state.deathHandled) {
        return;
    }
    state.deathHandled = true; // local kill must not re-broadcast

    if (actor->update == NULL) {
        return; // already dead locally
    }

    uint8_t health = actor->colChkInfo.health;
    Vec3s hitPos = { (int16_t)actor->world.pos.x, (int16_t)actor->world.pos.y, (int16_t)actor->world.pos.z };
    ApplyRemoteHit(actor, health > 0 ? health : 1, 0, hitPos, 0, attacker);
    state.pendingKillFrames = REMOTE_DEATH_GRACE;
}

static void OnEnemyActorInit(Actor* actor) {
    if (!IsTrackedCategory(actor) || gPlayState == NULL) {
        return;
    }

    int16_t roomNum = gPlayState->roomCtx.curRoom.num;
    if (roomNum != countersRoomNum) {
        spawnCounts.clear();
        countersRoomNum = roomNum;
    }
    uint32_t comboKey = ((uint32_t)(uint16_t)actor->id << 16) | (uint16_t)actor->params;
    uint16_t occurrence = spawnCounts[comboKey]++;

    TrackedState state;
    state.key = PackKey(roomNum, occurrence, actor->id, (uint16_t)actor->params);
    state.spawnPos = actor->world.pos;
    state.prevHealth = actor->colChkInfo.health;

    keyToActor[state.key] = actor;
    tracked[actor] = state;
}

static void OnEnemyActorUpdate(Actor* actor) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;

    if (state.pendingKillFrames > 0) {
        state.pendingKillFrames--;
        if (state.pendingKillFrames == 0 && actor->update != NULL) {
            // The lethal synthetic hit didn't take (custom damage path) — force it
            actor->colChkInfo.health = 0;
            Actor_Kill(actor);
            return;
        }
    }

    uint8_t health = actor->colChkInfo.health;

    if (health < state.prevHealth) {
        uint8_t delta = state.prevHealth - health;
        // Consume drops we caused on behalf of remote clients instead of echoing
        uint8_t remoteShare = std::min(delta, state.expectedRemoteDamage);
        state.expectedRemoteDamage -= remoteShare;
        uint8_t localShare = delta - remoteShare;

        if (localShare > 0) {
            uint32_t dmgFlags = 0;
            Vec3s hitPos = { (int16_t)actor->world.pos.x, (int16_t)actor->world.pos.y,
                             (int16_t)actor->world.pos.z };
            if (state.acCollider != nullptr) {
                ForEachColliderInfo(state.acCollider, [&](ColliderInfo* info) {
                    if (dmgFlags == 0 && info->acHitInfo != NULL) {
                        dmgFlags = info->acHitInfo->toucher.dmgFlags;
                        hitPos = info->bumper.hitPos;
                    }
                });
            }
            Anchor::Instance->SendPacket_EnemyHit(actor, state.key, localShare, dmgFlags, hitPos, health);
        }
    }
    state.prevHealth = health;

    if (state.expectedRemoteDamage > 0 && state.expectedRemoteDamageTimer > 0) {
        state.expectedRemoteDamageTimer--;
        if (state.expectedRemoteDamageTimer == 0) {
            state.expectedRemoteDamage = 0;
        }
    }
}

static void OnEnemyActorKill(Actor* actor) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;

    // Only broadcast real deaths (health exhausted), not despawns/cleanup kills
    if (!state.deathHandled && actor->colChkInfo.health == 0) {
        state.deathHandled = true;
        Anchor::Instance->SendPacket_EnemyDied(actor, state.key);
    }
}

static void OnEnemyActorDestroy(Actor* actor) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    keyToActor.erase(it->second.key);
    tracked.erase(it);
}

static void OnColliderSetAC(Actor* actor, Collider* collider) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    it->second.acCollider = collider;
}

void RegisterHooks(bool isConnected) {
    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_HOOK(OnActorInit, isConnected, [](void* actor) {
        if (SyncEnabled()) {
            OnEnemyActorInit((Actor*)actor);
        }
    });

    COND_HOOK(OnActorUpdate, isConnected, [](void* actor) {
        if (SyncEnabled()) {
            OnEnemyActorUpdate((Actor*)actor);
        }
    });

    COND_HOOK(OnActorKill, isConnected, [](void* actor) {
        if (SyncEnabled()) {
            OnEnemyActorKill((Actor*)actor);
        }
    });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actor) { OnEnemyActorDestroy((Actor*)actor); });

    COND_HOOK(OnCollisionCheckSetAC, isConnected, [](void* actor, void* collider) {
        if (actor != NULL) {
            OnColliderSetAC((Actor*)actor, (Collider*)collider);
        }
    });
}

} // namespace EnemySync
