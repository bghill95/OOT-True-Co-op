#include "BombSync.h"
#include "Anchor.h"
#include "EnemySync.h" // ESYNC_LOG
#include "PuppetRef.h"
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <unordered_map>

extern "C" {
#include "variables.h"
#include "src/overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "src/overlays/actors/ovl_En_Bom_Chu/z_en_bom_chu.h"
extern PlayState* gPlayState;
}

namespace BombSync {

// Covers EN_BOM (placed bombs) and EN_BOM_CHU (bombchus). A replica runs its
// own vanilla update — fuse sparks/smoke/hiss/flash come from `timer` alone,
// and a chu's surface-crawling is deterministic, so replicas self-simulate
// between events. A chu explodes by spawning an EN_BOM with timer=0 on the
// OWNER's side, which flows through this module like any bomb; the chu replica
// is killed just before its own local fuse would blow (and any bomb its crash
// spawns is absorbed as a mute replica) so the blast only shows where the
// owner's real one happened.

enum BombEventKind : uint8_t {
    BOMB_EV_THROW = 0,    // released/placed/launched: kinematics + fuse
    BOMB_EV_CARRY = 1,    // in the owner's hands (spawn or re-pickup)
    BOMB_EV_DETONATE = 2, // owner's bomb started exploding
    BOMB_EV_GONE = 3,     // owner's body died without exploding (eaten, chu resolved)
};

static constexpr float CARRY_Y_OFFSET = 38.0f;

// Owner side: one locally-simulated body.
struct BodyState {
    uint32_t bid = 0;
    bool announcedInit = false;
    bool hadParent = false;
    bool detonated = false;
};

// Mirror side: one cosmetic replica.
struct ReplicaState {
    bool carried = false;
    uint32_t ownerClientId = 0;
};

static std::unordered_map<Actor*, BodyState> sBodies;
static std::unordered_map<Actor*, ReplicaState> sReplicas;
static std::unordered_map<uint64_t, Actor*> sRemoteBodies; // (ownerClientId<<32)|bid -> replica
static uint32_t sNextBid = 1;
static bool sSpawningReplica = false;
// Armed while a chu replica is about to explode: the EN_BOM its vanilla code
// spawns must become a mute replica, not a tracked local body (echo!).
static bool sAbsorbNextBomb = false;
static uint32_t sAbsorbOwnerId = 0;

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("BombSync"), 1);
}

static void Reset() {
    sBodies.clear();
    sReplicas.clear();
    sRemoteBodies.clear();
    sSpawningReplica = false;
    sAbsorbNextBomb = false;
}

static uint64_t RemoteKey(uint32_t clientId, uint32_t bid) {
    return ((uint64_t)clientId << 32) | bid;
}

static int16_t GetFuse(Actor* actor) {
    return actor->id == ACTOR_EN_BOM_CHU ? ((EnBomChu*)actor)->timer : ((EnBom*)actor)->timer;
}

static void SetFuse(Actor* actor, int16_t t) {
    if (actor->id == ACTOR_EN_BOM_CHU) {
        ((EnBomChu*)actor)->timer = t;
    } else {
        ((EnBom*)actor)->timer = t;
    }
}

// Strip every collider each frame: no double damage, no chain-detonation (the
// chain check derefs collider.base.oc), no theft, no targeting.
static void NeuterReplica(Actor* actor) {
    if (actor->id == ACTOR_EN_BOM_CHU) {
        EnBomChu* chu = (EnBomChu*)actor;
        chu->collider.base.atFlags &= ~AT_ON;
        chu->collider.base.acFlags &= ~AC_ON;
        chu->collider.base.ocFlags1 &= ~OC1_ON;
    } else {
        EnBom* bomb = (EnBom*)actor;
        bomb->bombCollider.base.atFlags &= ~AT_ON;
        bomb->bombCollider.base.acFlags &= ~AC_ON;
        bomb->bombCollider.base.ocFlags1 &= ~OC1_ON;
        bomb->explosionCollider.base.atFlags &= ~AT_ON;
        bomb->explosionCollider.base.acFlags &= ~AC_ON;
        bomb->explosionCollider.base.ocFlags1 &= ~OC1_ON;
    }
    actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
}

static void OnBodyInit(Actor* actor) {
    if (!SyncEnabled() || sSpawningReplica) {
        return;
    }
    if (actor->id == ACTOR_EN_BOM && actor->params != BOMB_BODY) {
        return;
    }
    if (sAbsorbNextBomb && actor->id == ACTOR_EN_BOM) {
        // A chu replica's own crash/fuse just spawned this bomb: keep it as a
        // mute replica so it neither deals damage nor echoes back to the owner.
        sAbsorbNextBomb = false;
        sReplicas[actor] = ReplicaState{ /*carried=*/false, sAbsorbOwnerId };
        NeuterReplica(actor);
        return;
    }
    BodyState st;
    st.bid = sNextBid++;
    sBodies[actor] = st;
}

static void SendBodyEvent(Actor* actor, BodyState& st, uint8_t kind) {
    nlohmann::json payload;
    payload["kind"] = kind;
    payload["bid"] = st.bid;
    payload["aid"] = actor->id;
    payload["timer"] = GetFuse(actor);
    if (kind == BOMB_EV_THROW || kind == BOMB_EV_DETONATE) {
        payload["pos"] = actor->world.pos;
        payload["rotY"] = actor->world.rot.y;
        payload["vel"] = actor->velocity;
        payload["speedXZ"] = actor->speedXZ;
    }
    Anchor::Instance->SendPacket_BombEvent(payload);
    ESYNC_LOG("[BombSync] event kind={} bid={} aid={} timer={}", kind, st.bid, actor->id, GetFuse(actor));
}

static void ReplicaPreUpdate(Actor* actor, ReplicaState& rs, bool* should) {
    NeuterReplica(actor);

    // Pickup goes through Actor_OfferCarry, which ignores every collider flag
    // NeuterReplica strips: undo a local grab on its edge.
    if (actor->parent != NULL) {
        PuppetRef::RevokeLocalHold(actor);
        actor->parent = NULL;
    }

    if (actor->id == ACTOR_EN_BOM_CHU) {
        // The blast is replicated through the owner's EN_BOM (spawned by the
        // real chu at detonation). Kill this crawler just before its own local
        // fuse blows so there is exactly one visible explosion; a crash-spawned
        // bomb in the meantime is absorbed by the init hook above.
        if (GetFuse(actor) <= 1) {
            Actor_Kill(actor);
            *should = false;
            return;
        }
        sAbsorbNextBomb = true; // the chu may crash-explode inside this very update
        sAbsorbOwnerId = rs.ownerClientId;
        return;
    }

    if (!rs.carried) {
        return; // free flight/grounded: local ballistics + own fuse
    }

    if (actor->params == BOMB_EXPLOSION) {
        rs.carried = false; // fuse ran out in hand: explosion stays in place
        return;
    }
    Actor* puppet = PuppetRef::Resolve(rs.ownerClientId);
    if (puppet == NULL) {
        rs.carried = false; // owner left mid-carry: drop where it stands
        return;
    }
    // Keep the update RUNNING while carried (sparks, smoke, hiss, flash,
    // natural fuse) — just pin the body onto the puppet, velocity zeroed so
    // gravity never accumulates.
    actor->world.pos = puppet->world.pos;
    actor->world.pos.y += CARRY_Y_OFFSET;
    actor->shape.rot.y = puppet->shape.rot.y;
    actor->velocity = { 0, 0, 0 };
    actor->speedXZ = 0.0f;
}

static void OnBodyPreUpdate(Actor* actor, bool* should) {
    if (!SyncEnabled()) {
        return;
    }

    auto rep = sReplicas.find(actor);
    if (rep != sReplicas.end()) {
        ReplicaPreUpdate(actor, rep->second, should);
        return;
    }
    // A chu replica did not crash this frame after all: disarm the absorber so
    // it cannot swallow a legitimate local bomb later.
    if (sAbsorbNextBomb && actor->id == ACTOR_EN_BOM_CHU) {
        sAbsorbNextBomb = false;
    }

    auto body = sBodies.find(actor);
    if (body == sBodies.end()) {
        return;
    }
    BodyState& st = body->second;
    bool hasParent = actor->parent != NULL;

    if (!st.detonated && actor->id == ACTOR_EN_BOM && actor->params == BOMB_EXPLOSION) {
        // Early (fire, chain, chu-spawned timer=0) or on-time detonation.
        st.detonated = true;
        SendBodyEvent(actor, st, BOMB_EV_DETONATE);
    } else if (!st.announcedInit) {
        // Actor_SpawnAsChild links the parent AFTER Init returns, so the held
        // state is only readable from the first pre-update on.
        st.announcedInit = true;
        st.hadParent = hasParent;
        SendBodyEvent(actor, st, hasParent ? BOMB_EV_CARRY : BOMB_EV_THROW);
    } else if (st.hadParent != hasParent) {
        st.hadParent = hasParent;
        SendBodyEvent(actor, st, hasParent ? BOMB_EV_CARRY : BOMB_EV_THROW);
    }
}

static Actor* SpawnReplica(int16_t actorId, const Vec3f& pos, int16_t rotY) {
    sSpawningReplica = true;
    Actor* replica = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId, pos.x, pos.y, pos.z, 0, rotY, 0,
                                 actorId == ACTOR_EN_BOM ? BOMB_BODY : 0, true);
    sSpawningReplica = false;
    return replica;
}

void Ingest(const nlohmann::json& payload) {
    try {
        uint8_t kind = payload.at("kind").get<uint8_t>();
        uint32_t ownerId = payload.at("clientId").get<uint32_t>();
        uint32_t bid = payload.at("bid").get<uint32_t>();
        int16_t actorId = payload.value("aid", (int16_t)ACTOR_EN_BOM);
        uint64_t rkey = RemoteKey(ownerId, bid);
        Actor* replica = NULL;
        auto known = sRemoteBodies.find(rkey);
        if (known != sRemoteBodies.end()) {
            replica = known->second;
        }

        if (kind == BOMB_EV_GONE) {
            if (replica != NULL && replica->update != NULL) {
                Actor_Kill(replica);
            }
            return;
        }

        if (replica == NULL) {
            Vec3f pos;
            int16_t rotY = 0;
            if (payload.contains("pos")) {
                pos = payload["pos"].get<Vec3f>();
                rotY = payload.value("rotY", (int16_t)0);
            } else if (Actor* puppet = PuppetRef::Resolve(ownerId)) {
                pos = puppet->world.pos;
                pos.y += CARRY_Y_OFFSET;
            } else {
                return; // carry event with no puppet to hang it on; the next
                        // throw/detonate carries a position and spawns it then
            }
            replica = SpawnReplica(actorId, pos, rotY);
            if (replica == NULL) {
                return;
            }
            sRemoteBodies[rkey] = replica;
            sReplicas[replica] = ReplicaState{ /*carried=*/false, ownerId };
            NeuterReplica(replica);
        }

        ReplicaState& rs = sReplicas[replica];
        SetFuse(replica, payload.value("timer", GetFuse(replica)));

        switch (kind) {
            case BOMB_EV_CARRY:
                rs.carried = true;
                replica->velocity = { 0, 0, 0 };
                replica->speedXZ = 0.0f;
                break;
            case BOMB_EV_THROW:
                rs.carried = false;
                replica->world.pos = payload.at("pos").get<Vec3f>();
                replica->world.rot.y = payload.value("rotY", (int16_t)0);
                replica->shape.rot.y = replica->world.rot.y;
                replica->velocity = payload.at("vel").get<Vec3f>();
                replica->speedXZ = payload.at("speedXZ").get<float>();
                break;
            case BOMB_EV_DETONATE:
                rs.carried = false;
                replica->world.pos = payload.at("pos").get<Vec3f>();
                replica->velocity = { 0, 0, 0 };
                replica->speedXZ = 0.0f;
                if (replica->id == ACTOR_EN_BOM && replica->params == BOMB_BODY) {
                    SetFuse(replica, 1); // its own update explodes it next frame, in place
                }
                break;
            default:
                break;
        }
        ESYNC_LOG("[BombSync] replica event kind={} bid={} aid={}", kind, bid, actorId);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[BombSync] malformed BOMB_EVENT: {}", e.what());
    }
}

void RegisterHooks(bool isConnected) {
    if (!isConnected) {
        Reset();
    }

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_ID_HOOK(OnActorInit, ACTOR_EN_BOM, isConnected, [](void* actor) { OnBodyInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_BOM_CHU, isConnected, [](void* actor) { OnBodyInit((Actor*)actor); });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_BOM, isConnected,
                 [](void* actor, bool* should) { OnBodyPreUpdate((Actor*)actor, should); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_BOM_CHU, isConnected,
                 [](void* actor, bool* should) { OnBodyPreUpdate((Actor*)actor, should); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actor) {
        Actor* a = (Actor*)actor;
        auto body = sBodies.find(a);
        if (body != sBodies.end()) {
            // Died without exploding (eaten by King Dodongo, chu handed its
            // blast to a fresh EN_BOM): reap the replica or it rolls forever.
            if (SyncEnabled() && !body->second.detonated) {
                SendBodyEvent(a, body->second, BOMB_EV_GONE);
            }
            sBodies.erase(body);
        }
        if (sReplicas.erase(a)) {
            for (auto it = sRemoteBodies.begin(); it != sRemoteBodies.end();) {
                it = (it->second == a) ? sRemoteBodies.erase(it) : std::next(it);
            }
        }
    });
}

} // namespace BombSync
