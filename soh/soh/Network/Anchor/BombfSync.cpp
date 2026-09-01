#include "BombfSync.h"
#include "Anchor.h"
#include "EnemySync.h" // PackKey, ESYNC_LOG
#include "PuppetRef.h"
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <unordered_map>

extern "C" {
#include "variables.h"
#include "src/overlays/actors/ovl_En_Bombf/z_en_bombf.h"
extern PlayState* gPlayState;
}

namespace BombfSync {

// Event kinds carried in BOMBF_EVENT packets.
enum BombfEventKind : uint8_t {
    BOMBF_EV_THROW = 0,    // body released (or spawned loose): kinematics + fuse
    BOMBF_EV_PLUCK = 1,    // scene flower emptied
    BOMBF_EV_CARRY = 2,    // body picked up (initial pluck or re-pickup)
    BOMBF_EV_DETONATE = 3, // owner's body started exploding (early fuse, fire, impact)
};

// Overhead-carry display offset for a replica riding its owner's puppet.
static constexpr float CARRY_Y_OFFSET = 38.0f;

// Owner side: one locally-simulated body.
struct BodyState {
    uint32_t bid = 0;          // per-client body id (correlates packets)
    bool announcedInit = false;
    bool hadParent = false;
    bool detonated = false;
};

// Mirror side: one cosmetic replica.
struct ReplicaState {
    bool carried = false;
    uint32_t ownerClientId = 0;
};

static std::unordered_map<uint64_t, Actor*> sFlowers;  // flower key -> scene-placed flower
static std::unordered_map<Actor*, uint64_t> sFlowerKey; // 0 = pending (resolved lazily)
static std::unordered_map<Actor*, float> sFlowerScale; // last observed flowerBombScale
static std::unordered_map<Actor*, BodyState> sBodies;  // locally-simulated bodies
static std::unordered_map<Actor*, ReplicaState> sReplicas;
static std::unordered_map<uint64_t, Actor*> sRemoteBodies; // (ownerClientId<<32)|bid -> replica
static uint32_t sNextBid = 1;
static bool sSpawningReplica = false; // guards our own Actor_Spawn

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("BombfSync"), 1);
}

static void Reset() {
    sFlowers.clear();
    sFlowerKey.clear();
    sFlowerScale.clear();
    sBodies.clear();
    sReplicas.clear();
    sRemoteBodies.clear();
    sSpawningReplica = false;
}

static uint64_t RemoteKey(uint32_t clientId, uint32_t bid) {
    return ((uint64_t)clientId << 32) | bid;
}

static void OnBombfInit(Actor* actor) {
    if (!SyncEnabled()) {
        return;
    }
    EnBombf* bombf = (EnBombf*)actor;
    if (actor->params == BOMBFLOWER_FLOWER) {
        // ActorListIndex is assigned AFTER Init; the key resolves lazily on
        // the first pre-update.
        sFlowerKey[actor] = 0;
        sFlowerScale[actor] = bombf->flowerBombScale;
    } else if (actor->params == BOMBFLOWER_BODY && !sSpawningReplica) {
        BodyState st;
        st.bid = sNextBid++;
        sBodies[actor] = st;
    }
}

static void SendBodyEvent(Actor* actor, BodyState& st, uint8_t kind) {
    EnBombf* bombf = (EnBombf*)actor;
    nlohmann::json payload;
    payload["kind"] = kind;
    payload["bid"] = st.bid;
    payload["timer"] = bombf->timer;
    if (kind == BOMBF_EV_THROW || kind == BOMBF_EV_DETONATE) {
        payload["pos"] = actor->world.pos;
        payload["rotY"] = actor->world.rot.y;
        payload["vel"] = actor->velocity;
        payload["speedXZ"] = actor->speedXZ;
    }
    Anchor::Instance->SendPacket_BombfEvent(payload);
    ESYNC_LOG("[BombfSync] event kind={} bid={} timer={}", kind, st.bid, bombf->timer);
}

static void AnnouncePluck(Actor* flower) {
    auto it = sFlowerKey.find(flower);
    if (it == sFlowerKey.end() || it->second == 0) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = BOMBF_EV_PLUCK;
    payload["key"] = it->second;
    Anchor::Instance->SendPacket_BombfEvent(payload);
}

// Strip every collider each frame: a replica deals no damage, takes none, can't
// chain-trigger, can't be grabbed or targeted.
static void NeuterReplica(EnBombf* bombf) {
    bombf->bombCollider.base.atFlags &= ~AT_ON;
    bombf->bombCollider.base.acFlags &= ~AC_ON;
    bombf->bombCollider.base.ocFlags1 &= ~OC1_ON;
    bombf->explosionCollider.base.atFlags &= ~AT_ON;
    bombf->explosionCollider.base.acFlags &= ~AC_ON;
    bombf->explosionCollider.base.ocFlags1 &= ~OC1_ON;
    bombf->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
}

static void ReplicaPreUpdate(Actor* actor, ReplicaState& rs, bool* should) {
    EnBombf* bombf = (EnBombf*)actor;
    NeuterReplica(bombf);

    // Pickup does NOT go through the colliders NeuterReplica strips: EnBombf_Move
    // offers itself to Actor_OfferCarry, which only looks at player state and
    // distance. Undo the grab on its edge so a replica can't be carried locally.
    if (actor->parent != NULL) {
        PuppetRef::RevokeLocalHold(actor);
        actor->parent = NULL;
    }

    if (!rs.carried) {
        return; // free flight: local ballistics + own fuse
    }

    if (actor->params == BOMBFLOWER_EXPLOSION) {
        rs.carried = false; // fuse ran out in hand: explosion stays in place
        return;
    }
    Actor* puppet = PuppetRef::Resolve(rs.ownerClientId);
    if (puppet == NULL) {
        // Owner left mid-carry: drop the replica where it stands and let its own
        // update finish the fuse. Never park it out of the world — an actor that
        // far away is update-culled, and this hook would stop firing.
        rs.carried = false;
        return;
    }
    // Keep the update RUNNING while carried: fuse sparks, smoke, ignition SFX,
    // red flash and the timer all live in EnBombf_Move. Pin the body onto the
    // puppet instead — position rewritten every pre-update, velocity zeroed so
    // gravity never accumulates.
    actor->world.pos = puppet->world.pos;
    actor->world.pos.y += CARRY_Y_OFFSET;
    actor->shape.rot.y = puppet->shape.rot.y;
    actor->velocity = { 0, 0, 0 };
    actor->speedXZ = 0.0f;
}

// Pre-update watcher for every En_Bombf (owner bodies, replicas, flowers).
static void OnBombfPreUpdate(Actor* actor, bool* should) {
    if (!SyncEnabled()) {
        return;
    }

    auto rep = sReplicas.find(actor);
    if (rep != sReplicas.end()) {
        ReplicaPreUpdate(actor, rep->second, should);
        return;
    }

    auto body = sBodies.find(actor);
    if (body != sBodies.end()) {
        BodyState& st = body->second;
        bool hasParent = actor->parent != NULL;

        if (!st.detonated && actor->params == BOMBFLOWER_EXPLOSION) {
            // Early or on-time detonation: snap the replica to it.
            st.detonated = true;
            SendBodyEvent(actor, st, BOMBF_EV_DETONATE);
        } else if (!st.announcedInit) {
            st.announcedInit = true;
            st.hadParent = hasParent;
            SendBodyEvent(actor, st, hasParent ? BOMBF_EV_CARRY : BOMBF_EV_THROW);
        } else if (st.hadParent != hasParent) {
            st.hadParent = hasParent;
            SendBodyEvent(actor, st, hasParent ? BOMBF_EV_CARRY : BOMBF_EV_THROW);
        }
        return;
    }

    // Local FLOWER: a 1.0 -> <1.0 scale edge means the bomb was just taken.
    auto scale = sFlowerScale.find(actor);
    if (scale != sFlowerScale.end()) {
        auto fk = sFlowerKey.find(actor);
        if (fk != sFlowerKey.end() && fk->second == 0) {
            int16_t listIndex = GetActorListIndex(actor);
            if (listIndex >= 0) {
                fk->second =
                    EnemySync::PackKey(actor->room, (uint16_t)listIndex, actor->id, (uint16_t)actor->params);
                sFlowers[fk->second] = actor;
            }
        }
        float current = ((EnBombf*)actor)->flowerBombScale;
        if (scale->second >= 1.0f && current < 1.0f) {
            AnnouncePluck(actor);
        }
        scale->second = current;
    }
}

static Actor* SpawnReplica(const Vec3f& pos, int16_t rotY) {
    sSpawningReplica = true;
    Actor* replica = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_BOMBF, pos.x, pos.y, pos.z, 0, rotY, 0,
                                 BOMBFLOWER_BODY, true);
    sSpawningReplica = false;
    return replica;
}

void Ingest(const nlohmann::json& payload) {
    try {
        uint8_t kind = payload.at("kind").get<uint8_t>();

        if (kind == BOMBF_EV_PLUCK) {
            uint64_t key = payload.at("key").get<uint64_t>();
            auto it = sFlowers.find(key);
            if (it == sFlowers.end()) {
                return;
            }
            EnBombf* flower = (EnBombf*)it->second;
            flower->flowerBombScale = 0.0f;
            flower->timer = 180; // regrow, mirrors EnBombf_GrowBomb's reset
            flower->actor.flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
            sFlowerScale[it->second] = 0.0f; // no echo edge
            return;
        }

        uint32_t ownerId = payload.at("clientId").get<uint32_t>();
        uint32_t bid = payload.at("bid").get<uint32_t>();
        uint64_t rkey = RemoteKey(ownerId, bid);
        Actor* replica = NULL;
        auto known = sRemoteBodies.find(rkey);
        if (known != sRemoteBodies.end()) {
            replica = known->second;
        }

        if (replica == NULL) {
            // First sighting of this body: spawn it where the packet says (throw/
            // detonate carry a position; carry rides the puppet from frame one).
            Vec3f pos;
            int16_t rotY = 0;
            if (payload.contains("pos")) {
                pos = payload["pos"].get<Vec3f>();
                rotY = payload.value("rotY", (int16_t)0);
            } else if (Actor* puppet = PuppetRef::Resolve(ownerId)) {
                pos = puppet->world.pos;
                pos.y += CARRY_Y_OFFSET;
            } else {
                return; // carry event with no puppet to hang it on; the throw
                        // that follows carries a position and spawns it then.
            }
            replica = SpawnReplica(pos, rotY);
            if (replica == NULL) {
                return;
            }
            sRemoteBodies[rkey] = replica;
            sReplicas[replica] = ReplicaState{ /*carried=*/false, ownerId };
            NeuterReplica((EnBombf*)replica);
        }

        ReplicaState& rs = sReplicas[replica];
        EnBombf* bombf = (EnBombf*)replica;
        bombf->timer = payload.value("timer", bombf->timer);

        switch (kind) {
            case BOMBF_EV_CARRY:
                rs.carried = true;
                replica->velocity = { 0, 0, 0 };
                replica->speedXZ = 0.0f;
                break;
            case BOMBF_EV_THROW:
                rs.carried = false;
                replica->world.pos = payload.at("pos").get<Vec3f>();
                replica->world.rot.y = payload.value("rotY", (int16_t)0);
                replica->velocity = payload.at("vel").get<Vec3f>();
                replica->speedXZ = payload.at("speedXZ").get<float>();
                break;
            case BOMBF_EV_DETONATE:
                rs.carried = false;
                replica->world.pos = payload.at("pos").get<Vec3f>();
                replica->velocity = { 0, 0, 0 };
                replica->speedXZ = 0.0f;
                bombf->timer = 1; // its own update explodes it next frame, in place
                break;
            default:
                break;
        }
        ESYNC_LOG("[BombfSync] replica event kind={} bid={} timer={}", kind, bid, bombf->timer);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[BombfSync] malformed BOMBF_EVENT: {}", e.what());
    }
}

void RegisterHooks(bool isConnected) {
    // Same stale-pointer discipline as EnemySync/DynaSync.
    if (!isConnected) {
        Reset();
    }

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_ID_HOOK(OnActorInit, ACTOR_EN_BOMBF, isConnected, [](void* actor) { OnBombfInit((Actor*)actor); });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_BOMBF, isConnected,
                 [](void* actor, bool* should) { OnBombfPreUpdate((Actor*)actor, should); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actor) {
        Actor* a = (Actor*)actor;
        sBodies.erase(a);
        if (sReplicas.erase(a)) {
            for (auto it = sRemoteBodies.begin(); it != sRemoteBodies.end();) {
                it = (it->second == a) ? sRemoteBodies.erase(it) : std::next(it);
            }
        }
        if (sFlowerScale.erase(a)) {
            sFlowerKey.erase(a);
            for (auto it = sFlowers.begin(); it != sFlowers.end();) {
                it = (it->second == a) ? sFlowers.erase(it) : std::next(it);
            }
        }
    });
}

} // namespace BombfSync
