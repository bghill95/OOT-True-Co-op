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
#include "functions.h"
#include "src/overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "src/overlays/actors/ovl_En_Bom_Chu/z_en_bom_chu.h"
extern PlayState* gPlayState;
// Not declared in the overlay header; both are non-static in z_en_bom_chu.c.
void EnBomChu_UpdateFloorPoly(EnBomChu* chu, CollisionPoly* floorPoly, PlayState* play);
void EnBomChu_Move(EnBomChu* chu, PlayState* play);
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
    bool chuLaunched = false; // chu only: THROW already sent (from Move entry)
};

// Mirror side: one cosmetic replica.
struct ReplicaState {
    bool carried = false;
    bool launched = false; // chu only: crawl initialized from a THROW packet
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
static uint32_t sAbsorbFrame = 0; // only valid the frame it was armed (the chu's own update)

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
    if (sAbsorbNextBomb && actor->id == ACTOR_EN_BOM && gPlayState != NULL &&
        gPlayState->gameplayFrames == sAbsorbFrame) {
        // A chu replica's own crash/fuse just spawned this bomb: keep it as a
        // mute replica so it neither deals damage nor echoes back to the owner.
        sAbsorbNextBomb = false;
        ReplicaState mute;
        mute.ownerClientId = sAbsorbOwnerId;
        sReplicas[actor] = mute;
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
        // NEVER let a replica run EnBomChu_WaitForRelease: it has no parent, so
        // that state would snap it onto OUR local player and deref floorPoly
        // with no NULL check (a documented vanilla //!@bug — real 0xc0000005 in
        // playtest). Until the THROW packet initializes the crawl (launched),
        // hold the update and ride the owner's puppet while carried.
        if (!rs.launched) {
            *should = false;
            EnBomChu* chu = (EnBomChu*)actor;
            if (chu->timer > 1) {
                chu->timer--;
            }
            Actor* puppet = rs.carried ? PuppetRef::Resolve(rs.ownerClientId) : NULL;
            if (puppet != NULL) {
                actor->world.pos = PuppetRef::CarryAnchor(puppet);
                actor->shape.rot.y = puppet->shape.rot.y;
            }
            return;
        }
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
        sAbsorbFrame = gPlayState->gameplayFrames;
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
    actor->world.pos = PuppetRef::CarryAnchor(puppet);
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
    auto body = sBodies.find(actor);
    if (body == sBodies.end()) {
        return;
    }
    BodyState& st = body->second;
    bool hasParent = actor->parent != NULL;

    if (actor->id == ACTOR_EN_BOM_CHU) {
        // The release snaps the chu to the floor and computes its axes DURING
        // its own update (WaitForRelease -> Move), one step after the parent
        // clears — announcing on the parent edge would ship the still-carried
        // in-air position and the peer's crawl init would find no floor and
        // blow up instantly. Announce THROW only once Move is live.
        if (!st.announcedInit) {
            st.announcedInit = true;
            st.hadParent = hasParent;
            if (hasParent) {
                SendBodyEvent(actor, st, BOMB_EV_CARRY);
            }
        } else if (!st.chuLaunched && ((EnBomChu*)actor)->actionFunc == EnBomChu_Move) {
            st.chuLaunched = true;
            SendBodyEvent(actor, st, BOMB_EV_THROW); // pos/rotY now floor-snapped
        }
        return;
    }

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

        // Killed (it blew up, or the chu bailed on a missing floor) but still in
        // the table until its Actor_Delete next frame. Writing pos/velocity, or
        // worse re-arming EnBomChu_Move, drives a corpse; drop the stale entry
        // and let the spawn path below build a live body for this event.
        if (replica != NULL && replica->update == NULL) {
            sReplicas.erase(replica);
            sRemoteBodies.erase(known);
            replica = NULL;
        }

        if (replica == NULL) {
            Vec3f pos;
            int16_t rotY = 0;
            if (payload.contains("pos")) {
                pos = payload["pos"].get<Vec3f>();
                rotY = payload.value("rotY", (int16_t)0);
            } else if (Actor* puppet = PuppetRef::Resolve(ownerId)) {
                pos = PuppetRef::CarryAnchor(puppet);
            } else {
                return; // carry event with no puppet to hang it on; the next
                        // throw/detonate carries a position and spawns it then
            }
            replica = SpawnReplica(actorId, pos, rotY);
            if (replica == NULL) {
                return;
            }
            sRemoteBodies[rkey] = replica;
            ReplicaState fresh;
            fresh.ownerClientId = ownerId;
            sReplicas[replica] = fresh;
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
                if (replica->id == ACTOR_EN_BOM_CHU) {
                    // Hand-roll what EnBomChu_WaitForRelease does on release,
                    // from the packet's data instead of the local player, WITH
                    // the missing floor-poly NULL check.
                    EnBomChu* chu = (EnBomChu*)replica;
                    int16_t yaw = replica->shape.rot.y;
                    chu->axisForwards = { Math_SinS(yaw), 0.0f, Math_CosS(yaw) };
                    chu->axisUp = { 0.0f, 1.0f, 0.0f };
                    chu->axisLeft = { Math_SinS((int16_t)(yaw + 0x4000)), 0.0f, Math_CosS((int16_t)(yaw + 0x4000)) };
                    Actor_UpdateBgCheckInfo(gPlayState, replica, 0.0f, 0.0f, 0.0f, 4);
                    if (replica->floorPoly == NULL) {
                        Actor_Kill(replica); // no ground under the packet's pos: bail out safely
                        break;
                    }
                    EnBomChu_UpdateFloorPoly(chu, replica->floorPoly, gPlayState);
                    chu->actionFunc = EnBomChu_Move;
                    rs.launched = true;
                }
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
