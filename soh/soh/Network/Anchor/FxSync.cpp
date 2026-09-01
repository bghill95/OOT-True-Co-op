#include "FxSync.h"
#include "Anchor.h"
#include "EnemySync.h" // ESYNC_LOG
#include "PuppetRef.h"
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Magic_Fire/z_magic_fire.h"
#include "src/overlays/actors/ovl_Obj_Syokudai/z_obj_syokudai.h"
extern PlayState* gPlayState;
}

namespace FxSync {

enum FxEventKind : uint8_t {
    FX_EV_SPELL = 0, // caster spawned a magic spell actor (aid says which)
    FX_EV_TORCH = 1, // a torch's litTimer flipped (key + new value)
};

// --- Spells -----------------------------------------------------------------
// The spell actors re-center on GET_PLAYER every update, so a replica must be
// re-pinned onto the caster's puppet AFTER its update ran (OnActorUpdate).
// Magic cost is charged by the caster's Player code only — replicas are free.
// Nayru/Din screen tints run on the replica too: assumed (a nearby cast
// flashing your screen reads fine). Only MAGIC_FIRE carries a damage collider.

struct SpellReplica {
    uint32_t ownerClientId = 0;
};

static std::unordered_map<Actor*, SpellReplica> sSpellReplicas;
static bool sSpawningReplica = false;

// --- Torches ----------------------------------------------------------------
// Permanent torches latch a switch flag (already synced). Timed ones live in
// litTimer only: announce local edges, apply remote values. The flag-driven
// re-light on the receiver produces one harmless echo (values converge).

struct TorchState {
    uint64_t key = 0;
    int16_t prevLit = 0;
    bool remoteApplying = false;
};

static std::unordered_map<Actor*, TorchState> sTorches;
static std::unordered_map<uint64_t, Actor*> sTorchByKey;

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("FxSync"), 1);
}

static void Reset() {
    sSpellReplicas.clear();
    sTorches.clear();
    sTorchByKey.clear();
    sSpawningReplica = false;
}

static uint64_t PosKey(const Actor* actor) {
    int16_t qx = (int16_t)lroundf(actor->world.pos.x);
    int16_t qy = (int16_t)lroundf(actor->world.pos.y);
    int16_t qz = (int16_t)lroundf(actor->world.pos.z);
    return ((uint64_t)(uint16_t)qx << 48) | ((uint64_t)(uint16_t)qy << 32) | ((uint64_t)(uint16_t)qz << 16) |
           (uint16_t)actor->id;
}

static void OnSpellInit(Actor* actor) {
    if (!SyncEnabled() || sSpawningReplica) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_SPELL;
    payload["aid"] = actor->id;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] spell tx aid={}", actor->id);
}

static void OnSpellPostUpdate(Actor* actor) {
    auto it = sSpellReplicas.find(actor);
    if (it == sSpellReplicas.end()) {
        return;
    }
    // Undo the update's snap onto OUR local player.
    Actor* puppet = PuppetRef::Resolve(it->second.ownerClientId);
    if (puppet != NULL) {
        actor->world.pos = puppet->world.pos;
    }
    if (actor->id == ACTOR_MAGIC_FIRE) {
        MagicFire* fire = (MagicFire*)actor;
        fire->collider.base.atFlags &= ~AT_ON; // damage stays owner-side
    }
}

static void OnTorchInit(Actor* actor) {
    if (!SyncEnabled() || sTorches.contains(actor)) {
        return;
    }
    TorchState st;
    st.key = PosKey(actor);
    st.prevLit = ((ObjSyokudai*)actor)->litTimer;
    sTorches[actor] = st;
    sTorchByKey[st.key] = actor;
}

static void OnTorchPreUpdate(Actor* actor) {
    auto it = sTorches.find(actor);
    if (it == sTorches.end() || !SyncEnabled()) {
        return;
    }
    TorchState& st = it->second;
    int16_t lit = ((ObjSyokudai*)actor)->litTimer;
    bool wasLit = st.prevLit != 0;
    bool isLit = lit != 0;
    if (wasLit != isLit) {
        if (st.remoteApplying) {
            st.remoteApplying = false; // this edge IS the remote apply: no echo
        } else {
            nlohmann::json payload;
            payload["kind"] = FX_EV_TORCH;
            payload["key"] = st.key;
            payload["lit"] = lit;
            Anchor::Instance->SendPacket_FxEvent(payload);
            ESYNC_LOG("[FxSync] torch tx key={:#x} lit={}", st.key, lit);
        }
    }
    st.prevLit = lit;
}

void Ingest(uint32_t senderClientId, const nlohmann::json& payload) {
    try {
        uint8_t kind = payload.at("kind").get<uint8_t>();

        if (kind == FX_EV_SPELL) {
            int16_t aid = payload.at("aid").get<int16_t>();
            if (aid != ACTOR_MAGIC_FIRE && aid != ACTOR_MAGIC_WIND && aid != ACTOR_MAGIC_DARK) {
                return;
            }
            Actor* puppet = PuppetRef::Resolve(senderClientId);
            if (puppet == NULL) {
                return; // no puppet to attach the spell to
            }
            sSpawningReplica = true;
            Actor* replica = Actor_Spawn(&gPlayState->actorCtx, gPlayState, aid, puppet->world.pos.x,
                                         puppet->world.pos.y, puppet->world.pos.z, 0, 0, 0, 0, true);
            sSpawningReplica = false;
            if (replica != NULL) {
                sSpellReplicas[replica] = SpellReplica{ senderClientId };
            }
            ESYNC_LOG("[FxSync] spell rx aid={} owner={}", aid, senderClientId);
            return;
        }

        if (kind == FX_EV_TORCH) {
            uint64_t key = payload.at("key").get<uint64_t>();
            auto it = sTorchByKey.find(key);
            if (it == sTorchByKey.end() || it->second->update == NULL) {
                return;
            }
            ObjSyokudai* torch = (ObjSyokudai*)it->second;
            auto st = sTorches.find(it->second);
            if (st != sTorches.end()) {
                st->second.remoteApplying = true;
            }
            torch->litTimer = payload.at("lit").get<int16_t>();
            ESYNC_LOG("[FxSync] torch rx key={:#x} lit={}", key, torch->litTimer);
            return;
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[FxSync] malformed FX_EVENT: {}", e.what());
    }
}

void RegisterHooks(bool isConnected) {
    if (!isConnected) {
        Reset();
    }

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_ID_HOOK(OnActorInit, ACTOR_MAGIC_FIRE, isConnected, [](void* actor) { OnSpellInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_MAGIC_WIND, isConnected, [](void* actor) { OnSpellInit((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_MAGIC_DARK, isConnected, [](void* actor) { OnSpellInit((Actor*)actor); });

    COND_ID_HOOK(OnActorUpdate, ACTOR_MAGIC_FIRE, isConnected, [](void* actor) { OnSpellPostUpdate((Actor*)actor); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_MAGIC_WIND, isConnected, [](void* actor) { OnSpellPostUpdate((Actor*)actor); });
    COND_ID_HOOK(OnActorUpdate, ACTOR_MAGIC_DARK, isConnected, [](void* actor) { OnSpellPostUpdate((Actor*)actor); });

    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_SYOKUDAI, isConnected, [](void* actor) { OnTorchInit((Actor*)actor); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_SYOKUDAI, isConnected,
                 [](void* actor, bool* should) { OnTorchPreUpdate((Actor*)actor); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actorRef) {
        Actor* a = (Actor*)actorRef;
        sSpellReplicas.erase(a);
        auto it = sTorches.find(a);
        if (it != sTorches.end()) {
            auto bk = sTorchByKey.find(it->second.key);
            if (bk != sTorchByKey.end() && bk->second == a) {
                sTorchByKey.erase(bk);
            }
            sTorches.erase(it);
        }
    });
}

} // namespace FxSync
