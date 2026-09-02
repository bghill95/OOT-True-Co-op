#include "CarrySync.h"
#include "Anchor.h"
#include "EnemySync.h" // PackKey, ESYNC_LOG
#include "PuppetRef.h"
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace CarrySync {

enum CarryEventKind : uint8_t {
    CARRY_EV_CARRY = 0, // picked up by the sender's player
    CARRY_EV_THROW = 1, // released: kinematics attached
    CARRY_EV_GONE = 2,  // died un-replayably on the carrier's sim
};

static constexpr float CARRY_Y_OFFSET = 38.0f;

// A carry whose puppet went away (peer disconnected, crashed, warped out) never
// gets its closing THROW: after this many frames without a live carrier the
// object falls back to local physics, otherwise a pot gating a switch would stay
// hostage for the rest of the scene.
static constexpr int32_t CARRIER_LOST_TICKS = 45;

// Scene-placed carryables worth replicating. Grow as needed.
static bool IsSyncedCarryActor(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_EN_ISHI:     // small/large liftable rocks
        case ACTOR_OBJ_TSUBO:   // pots
        case ACTOR_EN_NIW:      // cuccos — wander AI diverges between clients, but
                                // every CARRY/THROW re-converges the copies, which
                                // is what actually shows on screen
            return true;
        default:
            return false;
    }
}

struct CarryState {
    uint64_t key = 0;
    bool hadParent = false;
    bool mineLatch = false;         // we announced CARRY and still own the object
    bool remoteCarried = false;     // a peer's player is carrying this copy
    uint32_t carrierClientId = 0;
    int32_t carrierLostTicks = 0;   // frames the carrier's puppet has been gone
};

static std::unordered_map<Actor*, CarryState> sTracked;
static std::unordered_map<uint64_t, Actor*> sByKey;

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("CarrySync"), 1);
}

static void Reset() {
    sTracked.clear();
    sByKey.clear();
}

// Identity = quantized spawn position + actor id. Most carryables are NOT in
// the scene's actor list: the Death Mountain rock circles (and their kind) are
// spawned by Obj_Mure2, so they have no ActorListIndex — but the spawner lays
// them out at fixed offsets from its own position (no Rand_ anywhere), so the
// same rock sits at the same coordinates on every client. Bonus: a rock that
// breaks and respawns later gets the same key back. Computed at Init, before
// the actor ever moves; the same binary computes identical floats.
static uint64_t PosKey(const Actor* actor) {
    int16_t qx = (int16_t)lroundf(actor->world.pos.x);
    int16_t qy = (int16_t)lroundf(actor->world.pos.y);
    int16_t qz = (int16_t)lroundf(actor->world.pos.z);
    return ((uint64_t)(uint16_t)qx << 48) | ((uint64_t)(uint16_t)qy << 32) | ((uint64_t)(uint16_t)qz << 16) |
           (uint16_t)actor->id;
}

static void OnCarryableInit(Actor* actor) {
    if (!SyncEnabled() || sTracked.contains(actor)) {
        return;
    }
    CarryState st;
    st.key = PosKey(actor);
    sTracked[actor] = st;
    sByKey[st.key] = actor;
    ESYNC_LOG("[CarrySync] tracked id={} params={:#x} key={:#x}", actor->id, (uint16_t)actor->params, st.key);
}

static void SendEvent(Actor* actor, const CarryState& st, uint8_t kind) {
    nlohmann::json payload;
    payload["kind"] = kind;
    payload["key"] = st.key;
    if (kind == CARRY_EV_THROW) {
        payload["pos"] = actor->world.pos;
        payload["rotY"] = actor->world.rot.y;
        payload["vel"] = actor->velocity;
        payload["speedXZ"] = actor->speedXZ;
    }
    Anchor::Instance->SendPacket_CarryEvent(payload);
    ESYNC_LOG("[CarrySync] event kind={} key={:#x}", kind, st.key);
}

static void OnCarryablePreUpdate(Actor* actor, bool* should) {
    if (!SyncEnabled()) {
        return;
    }
    auto it = sTracked.find(actor);
    if (it == sTracked.end()) {
        return;
    }
    CarryState& st = it->second;

    if (st.remoteCarried) {
        // Keep the actor parented to the carrier's puppet so its own held
        // state machine stays put, and ride the puppet overhead ourselves
        // (the puppet's Player update never runs the real carry positioning).
        Actor* puppet = PuppetRef::Resolve(st.carrierClientId);
        if (puppet != NULL) {
            st.carrierLostTicks = 0;
            actor->parent = puppet;
            actor->world.pos = PuppetRef::CarryAnchor(puppet);
            actor->shape.rot.y = puppet->shape.rot.y;
            st.hadParent = true;
            return;
        }
        // Carrier gone: hold the object still (never park it out of the world,
        // or update culling would stop this hook from ever firing again), then
        // hand it back to local physics so it drops where it was last seen.
        actor->parent = NULL;
        if (++st.carrierLostTicks >= CARRIER_LOST_TICKS) {
            ESYNC_LOG("[CarrySync] carrier {} lost, releasing key={:#x}", st.carrierClientId, st.key);
            st.remoteCarried = false;
            st.carrierLostTicks = 0;
        }
        st.hadParent = false;
        return;
    }

    bool hasParent = actor->parent != NULL;
    if (hasParent != st.hadParent) {
        st.hadParent = hasParent;
        if (hasParent) {
            // Only our own player can parent a non-remote-carried copy.
            st.mineLatch = true;
            SendEvent(actor, st, CARRY_EV_CARRY);
        } else if (st.mineLatch) {
            SendEvent(actor, st, CARRY_EV_THROW);
        }
    }
}

void Ingest(uint32_t senderClientId, const nlohmann::json& payload) {
    try {
        uint8_t kind = payload.at("kind").get<uint8_t>();
        uint64_t key = payload.at("key").get<uint64_t>();
        auto it = sByKey.find(key);
        if (it == sByKey.end()) {
            ESYNC_LOG("[CarrySync] event kind={} for unknown key={:#x}", kind, key);
            return;
        }
        Actor* actor = it->second;
        CarryState& st = sTracked[actor];

        switch (kind) {
            case CARRY_EV_CARRY:
                // Both players can grab the same pot inside one round trip; the
                // peer wins, so take it out of our own hands as a whole (clearing
                // only actor->parent would leave Link stuck in the carry pose).
                PuppetRef::RevokeLocalHold(actor);
                st.remoteCarried = true;
                st.carrierClientId = senderClientId;
                st.carrierLostTicks = 0;
                st.mineLatch = false;
                break;
            case CARRY_EV_THROW:
                st.remoteCarried = false;
                actor->parent = NULL; // its own release logic takes over
                st.hadParent = false; // no false CARRY/THROW edge on our side
                actor->world.pos = payload.at("pos").get<Vec3f>();
                actor->world.rot.y = payload.value("rotY", (int16_t)0);
                actor->shape.rot.y = actor->world.rot.y;
                actor->velocity = payload.at("vel").get<Vec3f>();
                actor->speedXZ = payload.at("speedXZ").get<float>();
                break;
            case CARRY_EV_GONE:
                Actor_Kill(actor);
                break;
            default:
                break;
        }
        ESYNC_LOG("[CarrySync] applied kind={} key={:#x}", kind, key);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[CarrySync] malformed CARRY_EVENT: {}", e.what());
    }
}

void RegisterHooks(bool isConnected) {
    if (!isConnected) {
        Reset();
    }

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_ID_HOOK(OnActorInit, ACTOR_EN_ISHI, isConnected, [](void* actor) { OnCarryableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_TSUBO, isConnected, [](void* actor) { OnCarryableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_NIW, isConnected, [](void* actor) { OnCarryableInit((Actor*)actor); });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_ISHI, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_TSUBO, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_NIW, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actorRef) {
        Actor* a = (Actor*)actorRef;
        auto it = sTracked.find(a);
        if (it == sTracked.end()) {
            return;
        }
        // Died while we still owned it and a peer shows it carried/parked:
        // tell them to drop theirs too. A normal thrown break needs no event
        // (each copy breaks by its own physics).
        if (it->second.mineLatch && it->second.hadParent) {
            SendEvent(a, it->second, CARRY_EV_GONE);
        }
        // A respawned instance (same key, Obj_Mure2 cycle) may already have
        // re-claimed the slot; only erase the mapping if it is still ours.
        auto bk = sByKey.find(it->second.key);
        if (bk != sByKey.end() && bk->second == a) {
            sByKey.erase(bk);
        }
        sTracked.erase(it);
    });
}

} // namespace CarrySync
