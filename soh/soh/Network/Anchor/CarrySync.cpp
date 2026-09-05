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
        case ACTOR_EN_KUSA:     // grass tufts: cut is BreakSync's, but they are
                                // liftable and throwable like a pot (Actor_OfferCarry
                                // in EnKusa_Main), and that half was unsynced
        case ACTOR_EN_RU1:      // Princess Ruto (Jabu-Jabu): carried and thrown like a
                                // pot (Actor_OfferCarry), lands by her own physics and
                                // presses the blue button herself; keyed by id (PosKey)
        case ACTOR_OBJ_KIBAKO:  // small wooden crate (Jabu-Jabu elevator room…): same
                                // Idle/Held/Thrown trio as a pot; its break stays
                                // BreakSync's (AC_HIT) like the pot's
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
    int32_t carriedUpdates = 0;     // actor updates run while parented to the puppet
    bool hasPendingThrow = false;   // THROW received before the copy ran its held update
    nlohmann::json pendingThrow;
    bool cullForced = false; // we set ACTOR_FLAG_UPDATE_CULLING_DISABLED on this copy
    int32_t uncullTicks = 0; // frames left before handing its culling setting back
};

// Frames a released copy keeps updating off-camera: long enough for the whole
// throw arc and its break, short enough that a scene's worth of thrown pots
// does not stay permanently uncullable.
static constexpr int32_t UNCULL_RELEASE_TICKS = 200;

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
    // Princess Ruto: the scene places one copy per room (woken/killed by the
    // infTable flags, the newcomer kills any older one), so the live instance
    // sits at a different spawn spot on each client once she was carried
    // through a door. There is only ever one of her: key her by id alone.
    if (actor->id == ACTOR_EN_RU1) {
        return (uint16_t)actor->id;
    }
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

// Hands a copy its own culling setting back once it no longer needs to run
// off-camera. Only ever clears the flag WE forced on at CARRY time: an actor
// that had it from its own lift path keeps it.
static void ReleaseCull(Actor* actor, CarryState& st) {
    if (st.cullForced) {
        actor->flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
        st.cullForced = false;
    }
    st.uncullTicks = 0;
}

// Clearing the parent + applying the streamed kinematics makes the actor's own
// release logic run: real local physics, real break-on-impact.
static void ApplyThrow(Actor* actor, CarryState& st, const nlohmann::json& payload) {
    st.remoteCarried = false;
    // Keep updating through the flight and the break, then stop: a thrown copy
    // that stayed uncullable forever kept running its AI (a cucco's gravity,
    // bgcheck and wander) for the rest of the scene, once per object ever
    // lifted by a peer.
    st.uncullTicks = UNCULL_RELEASE_TICKS;
    st.hasPendingThrow = false;
    st.pendingThrow = nullptr;
    actor->parent = NULL; // its own release logic takes over
    st.hadParent = false; // no false CARRY/THROW edge on our side
    actor->world.pos = payload.value("pos", actor->world.pos);
    actor->world.rot.y = payload.value("rotY", (int16_t)0);
    actor->shape.rot.y = actor->world.rot.y;
    actor->velocity = payload.value("vel", actor->velocity);
    actor->speedXZ = payload.value("speedXZ", actor->speedXZ);
}

// The pose a remote-carried copy takes on its carrier's puppet: the carrier's
// streamed position + facing when we have them (Player_PostLimbDraw's output on
// their side), else the head estimate and the puppet's own yaw.
static void PinToPuppet(Actor* actor, Actor* puppet) {
    Vec3s rot;
    actor->world.pos = PuppetRef::CarryAnchor(puppet);
    if (PuppetRef::CarryRotation(puppet, &rot)) {
        actor->shape.rot = rot;
        actor->world.rot.y = rot.y;
    } else {
        actor->shape.rot.y = puppet->shape.rot.y;
    }
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

    if (!st.remoteCarried && st.cullForced && st.uncullTicks > 0 && --st.uncullTicks == 0) {
        ReleaseCull(actor, st);
    }

    if (st.remoteCarried) {
        // Keep the actor parented to the carrier's puppet so its own held
        // state machine stays put, and ride the puppet overhead ourselves
        // (the puppet's Player update never runs the real carry positioning).
        Actor* puppet = PuppetRef::Resolve(st.carrierClientId);
        if (puppet != NULL) {
            // The copy's own state machine only advances inside its update. A
            // THROW applied before one carried update ran (CARRY+THROW drained
            // from the same packet batch, or a copy that sat culled off-screen
            // when CARRY came in) leaves it in its IDLE state at the release
            // point: no physics, floating, until the peer's BREAK pops it in
            // mid-air -- the impact effect then never shows on this side. Hold
            // the throw until the held update has run once.
            if (st.hasPendingThrow && st.carriedUpdates >= 1) {
                ApplyThrow(actor, st, st.pendingThrow);
                return;
            }
            st.carriedUpdates++;
            st.carrierLostTicks = 0;
            actor->parent = puppet;
            PinToPuppet(actor, puppet);
            st.hadParent = true;
            return;
        }
        // Carrier gone: hold the object still (never park it out of the world,
        // or update culling would stop this hook from ever firing again), then
        // hand it back to local physics so it drops where it was last seen.
        if (st.hasPendingThrow) {
            ApplyThrow(actor, st, st.pendingThrow); // never hold a throw hostage to a dead puppet
            return;
        }
        actor->parent = NULL;
        if (++st.carrierLostTicks >= CARRIER_LOST_TICKS) {
            ESYNC_LOG("[CarrySync] carrier {} lost, releasing key={:#x}", st.carrierClientId, st.key);
            st.remoteCarried = false;
            st.carrierLostTicks = 0;
            st.uncullTicks = UNCULL_RELEASE_TICKS; // drop where it stands, then cull normally
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

// Re-pin a remote-carried copy AFTER its own update. On the carrier the held
// actor's position is written by Player_PostLimbDraw -- during the DRAW pass,
// after every actor update -- so an actor whose held state still runs physics
// (a cucco: gravity + bgcheck every EnNiw_Update, rocks and pots do nothing
// while parented) drops from the anchor inside its update and is lifted back
// by the draw. The puppet never draws it back up, so the pre-update anchor alone
// left the cucco hanging at the puppet's waist. Mirror the carrier's frame:
// anchor before the update, anchor again after it.
static void OnCarryablePostUpdate(Actor* actor) {
    if (!SyncEnabled()) {
        return;
    }
    auto it = sTracked.find(actor);
    if (it == sTracked.end() || !it->second.remoteCarried) {
        return;
    }
    Actor* puppet = PuppetRef::Resolve(it->second.carrierClientId);
    if (puppet != NULL && actor->parent == puppet) {
        PinToPuppet(actor, puppet);
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
        // sByKey is only re-pointed on Actor_Destroy, so a pot smashed this
        // frame still answers to its key until Actor_Delete. Pinning a corpse to
        // a puppet, or handing it back an actionFunc, revives it for a frame and
        // (worse) leaves sTracked an entry for an actor about to be freed. Drop
        // the event: the destroy hook hands the key to a live sibling right after.
        if (actor->update == NULL) {
            ESYNC_LOG("[CarrySync] event kind={} for dying key={:#x}", kind, key);
            return;
        }
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
                st.carriedUpdates = 0;
                st.hasPendingThrow = false;
                // Actors only update inside the camera's culling volume (or with
                // this flag): a copy the local camera cannot see would neither
                // advance to its held state nor ride the puppet, and the pre-update
                // hook above never fires for it. Keep every remote-carried copy
                // updating; the actors' own lift path sets the same flag anyway.
                // Remember whether the flag was ours to clear (ReleaseCull).
                if (!st.cullForced && !(actor->flags & ACTOR_FLAG_UPDATE_CULLING_DISABLED)) {
                    st.cullForced = true;
                }
                st.uncullTicks = 0;
                actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
                break;
            case CARRY_EV_THROW:
                if (st.remoteCarried && st.carriedUpdates < 1) {
                    // Held update not run yet: see OnCarryablePreUpdate.
                    st.hasPendingThrow = true;
                    st.pendingThrow = payload;
                } else {
                    ApplyThrow(actor, st, payload);
                }
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
    COND_ID_HOOK(OnActorInit, ACTOR_EN_KUSA, isConnected, [](void* actor) { OnCarryableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_RU1, isConnected, [](void* actor) { OnCarryableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_KIBAKO, isConnected, [](void* actor) { OnCarryableInit((Actor*)actor); });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_ISHI, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_TSUBO, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_NIW, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_KUSA, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_RU1, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_KIBAKO, isConnected,
                 [](void* actor, bool* should) { OnCarryablePreUpdate((Actor*)actor, should); });

    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_ISHI, isConnected, [](void* actor) { OnCarryablePostUpdate((Actor*)actor); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_OBJ_TSUBO, isConnected,
                 [](void* actor) { OnCarryablePostUpdate((Actor*)actor); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_NIW, isConnected, [](void* actor) { OnCarryablePostUpdate((Actor*)actor); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_KUSA, isConnected, [](void* actor) { OnCarryablePostUpdate((Actor*)actor); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_RU1, isConnected, [](void* actor) { OnCarryablePostUpdate((Actor*)actor); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_OBJ_KIBAKO, isConnected,
                 [](void* actor) { OnCarryablePostUpdate((Actor*)actor); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actorRef) {
        Actor* a = (Actor*)actorRef;
        auto it = sTracked.find(a);
        if (it == sTracked.end()) {
            return;
        }
        // Died while we still owned it and a peer shows it carried/parked:
        // tell them to drop theirs too. A normal thrown break needs no event
        // (each copy breaks by its own physics). Actor_Delete WITHOUT a prior
        // Actor_Kill (update still set) is a room unload or the scene teardown,
        // not a death: the peer's copy stays, its carrier-lost fallback covers a
        // carrier who warped out mid-carry.
        if (a->update == NULL && it->second.mineLatch && it->second.hadParent) {
            SendEvent(a, it->second, CARRY_EV_GONE);
        }
        // A respawned instance (same key, Obj_Mure2 cycle) may already have
        // re-claimed the slot; only erase the mapping if it is still ours, and
        // hand the key to another live instance sharing it (Ruto: the scene
        // spawns one copy per room and the newcomer kills the older one, so
        // the survivor must keep receiving the events).
        uint64_t key = it->second.key;
        sTracked.erase(it);
        auto bk = sByKey.find(key);
        if (bk != sByKey.end() && bk->second == a) {
            sByKey.erase(bk);
            for (auto& [other, st] : sTracked) {
                if (st.key == key && other->update != NULL) {
                    sByKey[key] = other;
                    break;
                }
            }
        }
    });
}

} // namespace CarrySync
