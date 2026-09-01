#include "DynaSync.h"
#include "Anchor.h"
#include "EnemySync.h" // PackKey, authority election, ESYNC_LOG
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
}

namespace DynaSync {

// Authority resends every tracked actor at this cadence even when idle, so
// mirrors stay suppressed while an authority is present (a mirror falling back
// to local AI on an idle-but-diverged platform would drift, e.g. a sunk
// Ydan_Hasi plank floating back up). Must be < STALE_TICKS.
static constexpr uint64_t KEYFRAME_TICKS = 20;

// A mirrored actor whose stream went quiet this long reverts to local AI
// (authority left/disconnected — same discipline as EnemySync's stale path).
static constexpr uint64_t STALE_TICKS = 45;

// Positions are floats driven by AI math; ignore sub-visible jitter.
static constexpr float POS_EPSILON = 0.01f;

// A local push (dyna.unk_150, written only by OUR player) claims a block; the
// claim stays fresh while pushing and while the block still slides, plus this
// grace window. Streamed with "own":1 so peers (the scene authority included)
// mirror the claimant instead of running their own copy.
static constexpr uint64_t CLAIM_TICKS = 45;

// Whitelist: scene-placed DynaPoly actors whose MOTION should mirror.
// Grow this table dungeon by dungeon; never add flag-driven state here
// (SetFlag.cpp's exclusions own water level / forest elevator / collapse).
static bool IsSyncedDynaActor(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_BG_YDAN_HASI:      // Deku Tree: water plank + seesaw
        case ACTOR_BG_HIDAN_SYOKU:    // Fire Temple: elevator pillar
        case ACTOR_BG_DDAN_JD:        // Dodongo's Cavern: cycling pillars/platform
        case ACTOR_BG_BDAN_OBJECTS:   // Jabu-Jabu: elevator platform & co
        case ACTOR_BG_GND_FIREMEIRO:  // Ganon's Castle fire trial: sinking platforms
        case ACTOR_BG_HAKA_TRAP:      // Shadow Temple: guillotines / spike traps (cyclic)
        case ACTOR_BG_HAKA_TUBO:      // Shadow Temple: spinning pot
        case ACTOR_BG_HAKA_MEGANEBG:  // Shadow Temple: moving invisible platforms
        case ACTOR_BG_HIDAN_FSLIFT:   // Fire Temple: flame elevator
        case ACTOR_BG_HIDAN_SIMA:     // Fire Temple: rising fire platform (timer)
        case ACTOR_BG_HIDAN_RSEKIZOU: // Fire Temple: rotating spiked statues
        case ACTOR_BG_JYA_1FLIFT:     // Spirit Temple: 1F elevator
        case ACTOR_BG_JYA_LIFT:       // Spirit Temple: chain lift
        case ACTOR_BG_JYA_ZURERUKABE: // Spirit Temple: sliding walls (cyclic)
        case ACTOR_BG_MENKURI_KAITEN: // GTG: rotating ring platform
        case ACTOR_BG_RELAY_OBJECTS:  // Windmill: rotating center structure
        case ACTOR_BG_SPOT18_BASKET:  // Goron City: spinning vase
            // NOT one-shot switch-flag movers (Bg_Ddan_Kd stairs, Hamsteps, gates,
            // shutters...): Anchor's flag sync already replays those on both
            // clients. NOT the SetFlag.cpp exclusions (water level, forest
            // elevator, tower collapse). NOT pushblocks (phase B, needs
            // per-actor claim). NOT Obj_Lift collapsing platforms (per-player
            // stand trigger). NOT boss-room platforms (adapter territory).
            return true;
        default:
            return false;
    }
}

// Claimable: movable by WHOEVER touches it (push blocks). Motion is streamed
// by the claimant instead of the scene authority while a claim is fresh.
static bool IsClaimableDynaActor(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_OBJ_OSHIHIKI:  // push blocks (every dungeon)
        case ACTOR_BG_HIDAN_ROCK: // Fire Temple push stone
            return true;
        default:
            return false;
    }
}

struct TrackedDyna {
    uint64_t key = 0;
    Vec3f lastSentPos{};
    Vec3s lastSentRot{};
    bool everSent = false;
    bool suppressed = false;
    bool claimable = false;
    bool hadUncullFlag = false; // actor's own culling setting, restored on release
    uint64_t localClaimTick = 0; // last tick our player pushed/moved it
    Vec3f prevPos{};             // motion detection for claim upkeep
};

// Latest streamed state per key (game-thread only: written by the queued
// DYNA_STATE handler, read by the ShouldActorUpdate mirror step).
struct RemoteDynaState {
    uint64_t rxTick = 0;
    Vec3f pos{};
    Vec3s rot{};
};

static std::unordered_map<Actor*, TrackedDyna> sTracked;
static std::unordered_map<uint64_t, RemoteDynaState> sRemote;
static std::unordered_map<uint64_t, uint64_t> sRemoteClaims; // key -> last claimed-entry rx tick
static uint64_t sTick = 0;

static bool Fresh(uint64_t tick, uint64_t window) {
    return tick != 0 && (sTick - tick) <= window;
}

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("DynaSync"), 1);
}

// Give an actor its own culling setting back. Whitelisted platforms are noisy
// (Shadow Temple guillotines, the windmill): left uncullable they keep running
// and playing SFX from across the map once syncing stops.
static void RestoreCull(Actor* actor, const TrackedDyna& st) {
    if (!st.hadUncullFlag) {
        actor->flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
    }
}

// Safe on both paths that call it: OnActorDestroy has already dropped every
// actor of the outgoing scene, so what is left in sTracked is still alive.
static void Reset() {
    for (auto& [actor, st] : sTracked) {
        RestoreCull(actor, st);
    }
    sTracked.clear();
    sRemote.clear();
    sRemoteClaims.clear();
}

// OnDynaPolySetBgActor: fires from DynaPoly_SetBgActor (z_bgcheck.c) during the
// actor's own Init, so ActorListIndex is already assigned for scene-placed
// actors (set in the setup spawn loop, before Init runs via Actor_SpawnEntry).
static void OnDynaRegistered(Actor* actor) {
    if (!SyncEnabled() || sTracked.contains(actor) || (!IsSyncedDynaActor(actor) && !IsClaimableDynaActor(actor))) {
        return;
    }

    // ActorListIndex is assigned AFTER Init returns (z_actor.c spawn loop) and
    // this hook fires from within Init — the key is resolved lazily on the
    // first tick/pre-update (key == 0 = pending).
    TrackedDyna st;
    st.claimable = IsClaimableDynaActor(actor);
    st.prevPos = actor->world.pos;
    st.hadUncullFlag = (actor->flags & ACTOR_FLAG_UPDATE_CULLING_DISABLED) != 0;
    sTracked[actor] = st;

    // The update-culling gate in Actor_UpdateAll sits BEFORE the
    // ShouldActorUpdate hook: the authority needs the AI running while its
    // player is far, and the mirror needs the hook firing to apply the stream.
    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
}

// Returns false when the actor turns out to be dynamically spawned (no
// deterministic identity): the entry is erased, `st` must not be touched.
static bool EnsureKey(Actor* actor, TrackedDyna& st) {
    if (st.key != 0) {
        return true;
    }
    int16_t listIndex = GetActorListIndex(actor);
    if (listIndex < 0) {
        sTracked.erase(actor);
        return false;
    }
    st.key = EnemySync::PackKey(actor->room, (uint16_t)listIndex, actor->id, (uint16_t)actor->params);
    ESYNC_LOG("[DynaSync] tracked id={} params={:#x} room={} listIndex={} key={:#x}", actor->id,
              (uint16_t)actor->params, actor->room, listIndex, st.key);
    return true;
}

// Mirror step, in the actor's own update slot. Fresh stream => apply + suppress;
// stale/no stream => local AI (never suppress into silence).
static void OnShouldDynaUpdate(Actor* actor, bool* should) {
    auto it = sTracked.find(actor);
    if (it == sTracked.end() || gPlayState == NULL) {
        return;
    }
    TrackedDyna& st = it->second;
    if (!SyncEnabled()) {
        // Kill switch flipped mid-scene: hand culling back, keep the entry so
        // flipping it on again re-arms on the next update the actor gets.
        RestoreCull(actor, st);
        return;
    }
    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
    if (!EnsureKey(actor, st)) {
        return; // dynamic spawn: entry erased
    }

    if (st.claimable) {
        // Claim upkeep: a nonzero push force means OUR player is moving it;
        // keep the claim while it still slides after release.
        bool moved = fabsf(actor->world.pos.x - st.prevPos.x) > POS_EPSILON ||
                     fabsf(actor->world.pos.y - st.prevPos.y) > POS_EPSILON ||
                     fabsf(actor->world.pos.z - st.prevPos.z) > POS_EPSILON;
        st.prevPos = actor->world.pos;
        if (fabsf(((DynaPolyActor*)actor)->unk_150) > 0.001f ||
            (Fresh(st.localClaimTick, CLAIM_TICKS) && moved && !st.suppressed)) {
            st.localClaimTick = sTick;
        }
        if (Fresh(st.localClaimTick, CLAIM_TICKS)) {
            st.suppressed = false;
            return; // we own it: local simulation, PerFrameTick streams it
        }
        // Unclaimed by us: mirror a fresh remote stream below — the scene
        // authority included (a claimant elsewhere outranks the authority).
    } else if (EnemySync::IsLocalAuthority()) {
        st.suppressed = false;
        return;
    }

    auto rs = sRemote.find(st.key);
    if (rs == sRemote.end() || (sTick - rs->second.rxTick) > STALE_TICKS) {
        if (st.suppressed) {
            st.suppressed = false;
            ESYNC_LOG("[DynaSync] stale key={:#x} -> local AI", st.key);
        }
        return;
    }

    actor->world.pos = rs->second.pos;
    actor->shape.rot = rs->second.rot;
    st.suppressed = true;
    *should = false;
    // DynaPoly_UpdateBgActorTransforms (end of Actor_UpdateAll) re-reads
    // world.pos/shape.rot afterwards, so the collision mesh follows.
}

void Ingest(uint32_t senderClientId, const nlohmann::json& payload) {
    try {
        if (!payload.contains("dynas")) {
            return;
        }
        bool fromAuthority = senderClientId == EnemySync::CurrentAuthorityId();
        for (const auto& e : payload["dynas"]) {
            if (!e.contains("key") || !e.contains("pos") || !e.contains("rot")) {
                continue;
            }
            bool owned = e.value("own", 0) != 0;
            // Accept the scene authority's stream, plus any claimant's own
            // entries. A stale non-authority sender with no claim is dropped.
            if (!owned && !fromAuthority) {
                continue;
            }
            uint64_t key = e["key"].get<uint64_t>();
            if (owned) {
                sRemoteClaims[key] = sTick;
            }
            RemoteDynaState& r = sRemote[key];
            r.rxTick = sTick;
            r.pos = e["pos"].get<Vec3f>();
            r.rot = e["rot"].get<Vec3s>();
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[DynaSync] malformed DYNA_STATE: {}", e.what());
    }
}

static bool MovedSinceLastSend(const Actor* actor, const TrackedDyna& st) {
    return fabsf(actor->world.pos.x - st.lastSentPos.x) > POS_EPSILON ||
           fabsf(actor->world.pos.y - st.lastSentPos.y) > POS_EPSILON ||
           fabsf(actor->world.pos.z - st.lastSentPos.z) > POS_EPSILON ||
           actor->shape.rot.x != st.lastSentRot.x || actor->shape.rot.y != st.lastSentRot.y ||
           actor->shape.rot.z != st.lastSentRot.z;
}

void PerFrameTick() {
    sTick++;

    if (!SyncEnabled() || gPlayState == NULL || !Anchor::Instance->IsSaveLoaded()) {
        return;
    }

    bool isAuthority = EnemySync::IsLocalAuthority();
    bool keyframe = (sTick % KEYFRAME_TICKS) == 0;
    nlohmann::json entries = nlohmann::json::array();

    for (auto& [actor, st] : sTracked) {
        if (st.key == 0) {
            continue; // resolved by the pre-update hook; skip until then
        }
        bool localClaim = st.claimable && Fresh(st.localClaimTick, CLAIM_TICKS);
        bool remoteClaim = false;
        if (st.claimable) {
            auto rc = sRemoteClaims.find(st.key);
            remoteClaim = rc != sRemoteClaims.end() && Fresh(rc->second, CLAIM_TICKS);
        }
        // The claimant streams its blocks; the authority streams the rest.
        if (!(localClaim || (isAuthority && !remoteClaim))) {
            continue;
        }
        if (!keyframe && st.everSent && !MovedSinceLastSend(actor, st)) {
            continue;
        }
        nlohmann::json e;
        e["key"] = st.key;
        e["pos"] = actor->world.pos;
        e["rot"] = actor->shape.rot;
        if (localClaim) {
            e["own"] = 1;
        }
        entries.push_back(e);
        st.lastSentPos = actor->world.pos;
        st.lastSentRot = actor->shape.rot;
        st.everSent = true;
    }

    if (!entries.empty()) {
        Anchor::Instance->SendPacket_DynaState(entries);
    }
}

void RegisterHooks(bool isConnected) {
    // While disconnected the OnActorDestroy/OnSceneInit hooks are unregistered,
    // so the Actor* map would go stale; drop everything (EnemySync's HIGH-fix
    // discipline). Tracking rebuilds on the next scene load.
    if (!isConnected) {
        Reset();
    }

    // NOTE: PerFrameTick is NOT registered here; the Anchor per-frame
    // dispatcher (HookHandlers.cpp) drives it in explicit order.

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_HOOK(OnDynaPolySetBgActor, isConnected, [](void* actor, int32_t bgId) { OnDynaRegistered((Actor*)actor); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actor) { sTracked.erase((Actor*)actor); });

    COND_HOOK(ShouldActorUpdate, isConnected,
              [](void* actor, bool* should) { OnShouldDynaUpdate((Actor*)actor, should); });
}

} // namespace DynaSync
