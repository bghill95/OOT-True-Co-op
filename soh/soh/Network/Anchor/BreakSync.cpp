#include "BreakSync.h"
#include "Anchor.h"
#include "EnemySync.h" // ESYNC_LOG
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Kusa/z_en_kusa.h"
#include "src/overlays/actors/ovl_Obj_Tsubo/z_obj_tsubo.h"
#include "src/overlays/actors/ovl_Obj_Kibako/z_obj_kibako.h"
#include "src/overlays/actors/ovl_Obj_Kibako2/z_obj_kibako2.h"
#include "src/overlays/actors/ovl_En_Ishi/z_en_ishi.h"
#include "src/overlays/actors/ovl_Obj_Comb/z_obj_comb.h"
#include "src/overlays/actors/ovl_En_Wood02/z_en_wood02.h"
extern PlayState* gPlayState;
}

namespace BreakSync {

// One event kind: the receiver picks the spoof channel from the actor id.
//
// Per-actor break channels (from the full breakables scan, 2026-09-02):
//   EN_KUSA      grass    AC_HIT alone (no acHitInfo read); break also killed
//                         is detected via ACTOR_FLAG_GRASS_DESTROYED edge
//                         because a cut bush may survive as a stump (Obj_Mure
//                         regrowth) and never hit OnActorDestroy.
//   OBJ_TSUBO    pots     AC_HIT + acHitInfo->toucher.dmgFlags & 0x4FC1FFFC
//   OBJ_KIBAKO   sm crate AC_HIT alone
//   OBJ_KIBAKO2  bg crate home.rot.z != 0 (the vanilla player-bonk channel)
//   EN_ISHI      rocks    AC_HIT + dmgFlags & 0x40000048, ROCK_SMALL only
//   OBJ_COMB     beehive  AC_HIT + elements[0] acHitInfo; dmgFlags must NOT
//                         match 0x4001F866 (that mask only wobbles it)
//   EN_WOOD02    trees    home.rot.y != 0 (player-bonk drop channel)
// dmgFlags 0x8 (explosive) passes every gate above: & 0x4FC1FFFC != 0,
// & 0x40000048 != 0, & 0x4001F866 == 0.
//
// Known limits, on purpose: EN_KANBAN (cut signs, cosmetic, no clean remote
// trigger), OBJ_HANA (not breakable), Wood02 proximity bushes (local-player
// gated). Drops roll each client's own RNG — same rule as thrown pots.

static constexpr uint32_t SPOOF_DMG_FLAGS = 0x00000008; // explosive

static bool IsSyncedBreakable(int16_t id) {
    switch (id) {
        case ACTOR_EN_KUSA:
        case ACTOR_OBJ_TSUBO:
        case ACTOR_OBJ_KIBAKO:
        case ACTOR_OBJ_KIBAKO2:
        case ACTOR_EN_ISHI:
        case ACTOR_OBJ_COMB:
        case ACTOR_EN_WOOD02:
            return true;
        default:
            return false;
    }
}

struct BreakState {
    uint64_t key = 0;
    bool announced = false;      // we already sent BREAK for this actor
    bool remoteBreaking = false; // break was remote-triggered: never echo it
};

static std::unordered_map<Actor*, BreakState> sTracked;
static std::unordered_map<uint64_t, Actor*> sByKey;

// The attacker element the spoofed AC_HIT points at (Tsubo/Ishi/Comb deref it
// unconditionally — AC_HIT alone would read a stale/NULL pointer).
static ColliderInfo sSpoofHitInfo;

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("BreakSync"), 1);
}

static void Reset() {
    sTracked.clear();
    sByKey.clear();
}

static uint64_t PosKey(const Actor* actor) {
    int16_t qx = (int16_t)lroundf(actor->world.pos.x);
    int16_t qy = (int16_t)lroundf(actor->world.pos.y);
    int16_t qz = (int16_t)lroundf(actor->world.pos.z);
    return ((uint64_t)(uint16_t)qx << 48) | ((uint64_t)(uint16_t)qy << 32) | ((uint64_t)(uint16_t)qz << 16) |
           (uint16_t)actor->id;
}

static void OnBreakableInit(Actor* actor) {
    if (!SyncEnabled() || sTracked.contains(actor)) {
        return;
    }
    BreakState st;
    st.key = PosKey(actor);
    sTracked[actor] = st;
    sByKey[st.key] = actor;
}

static void SendBreak(Actor* actor, BreakState& st) {
    if (st.announced || st.remoteBreaking) {
        return;
    }
    st.announced = true;
    nlohmann::json payload;
    payload["key"] = st.key;
    Anchor::Instance->SendPacket_BreakEvent(payload);
    ESYNC_LOG("[BreakSync] break tx id={} key={:#x}", actor->id, st.key);
}

// Grass never reliably dies on cut (stump + regrowth), so watch the flag edge.
static void OnKusaPreUpdate(Actor* actor) {
    auto it = sTracked.find(actor);
    if (it == sTracked.end() || !SyncEnabled()) {
        return;
    }
    if (actor->flags & ACTOR_FLAG_GRASS_DESTROYED) {
        SendBreak(actor, it->second);
    }
}

// Trees announce on the bonk channel (they drop but never die either).
static void OnWoodPreUpdate(Actor* actor) {
    auto it = sTracked.find(actor);
    if (it == sTracked.end() || !SyncEnabled()) {
        return;
    }
    if (actor->home.rot.y != 0) {
        SendBreak(actor, it->second);
    }
}

void Ingest(const nlohmann::json& payload) {
    try {
        uint64_t key = payload.at("key").get<uint64_t>();
        auto it = sByKey.find(key);
        if (it == sByKey.end() || it->second->update == NULL) {
            return; // unknown or already dead (e.g. broke by its own physics)
        }
        Actor* actor = it->second;
        auto st = sTracked.find(actor);
        if (st == sTracked.end() || st->second.announced || st->second.remoteBreaking) {
            return;
        }
        st->second.remoteBreaking = true;

        sSpoofHitInfo.toucher.dmgFlags = SPOOF_DMG_FLAGS;

        switch (actor->id) {
            case ACTOR_EN_KUSA:
                ((EnKusa*)actor)->collider.base.acFlags |= AC_HIT;
                break;
            case ACTOR_OBJ_TSUBO: {
                ObjTsubo* tsubo = (ObjTsubo*)actor;
                tsubo->collider.info.acHitInfo = &sSpoofHitInfo;
                tsubo->collider.base.acFlags |= AC_HIT;
                break;
            }
            case ACTOR_OBJ_KIBAKO:
                ((ObjKibako*)actor)->collider.base.acFlags |= AC_HIT;
                break;
            case ACTOR_OBJ_KIBAKO2:
                // The vanilla player-bonk channel; no collider spoof needed.
                actor->home.rot.z = 1;
                break;
            case ACTOR_EN_ISHI: {
                if (actor->params & 1) {
                    break; // ROCK_LARGE can only be lifted, never hit-broken
                }
                EnIshi* ishi = (EnIshi*)actor;
                ishi->collider.info.acHitInfo = &sSpoofHitInfo;
                ishi->collider.base.acFlags |= AC_HIT;
                break;
            }
            case ACTOR_OBJ_COMB: {
                ObjComb* comb = (ObjComb*)actor;
                comb->colliderItems[0].info.acHitInfo = &sSpoofHitInfo;
                comb->collider.base.acFlags |= AC_HIT;
                break;
            }
            case ACTOR_EN_WOOD02:
                actor->home.rot.y = 1; // bonk-drop channel
                break;
            default:
                break;
        }
        ESYNC_LOG("[BreakSync] break rx id={} key={:#x}", actor->id, key);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[BreakSync] malformed BREAK_EVENT: {}", e.what());
    }
}

void RegisterHooks(bool isConnected) {
    if (!isConnected) {
        Reset();
    }

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_ID_HOOK(OnActorInit, ACTOR_EN_KUSA, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_TSUBO, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_KIBAKO, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_KIBAKO2, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_ISHI, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_COMB, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_WOOD02, isConnected, [](void* actor) { OnBreakableInit((Actor*)actor); });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_KUSA, isConnected,
                 [](void* actor, bool* should) { OnKusaPreUpdate((Actor*)actor); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_WOOD02, isConnected,
                 [](void* actor, bool* should) { OnWoodPreUpdate((Actor*)actor); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actorRef) {
        Actor* a = (Actor*)actorRef;
        auto it = sTracked.find(a);
        if (it == sTracked.end()) {
            return;
        }
        // A death while connected = it broke (these actors have no other way
        // to die mid-scene). Killed-by-own-physics after a synced THROW breaks
        // on both sides already; the receiver's copy is then dead and Ingest
        // drops the echo.
        if (SyncEnabled() && (a->id == ACTOR_OBJ_TSUBO || a->id == ACTOR_OBJ_KIBAKO || a->id == ACTOR_OBJ_KIBAKO2 ||
                              a->id == ACTOR_EN_ISHI || a->id == ACTOR_OBJ_COMB)) {
            SendBreak(a, it->second);
        }
        auto bk = sByKey.find(it->second.key);
        if (bk != sByKey.end() && bk->second == a) {
            sByKey.erase(bk);
        }
        sTracked.erase(it);
    });
}

} // namespace BreakSync
