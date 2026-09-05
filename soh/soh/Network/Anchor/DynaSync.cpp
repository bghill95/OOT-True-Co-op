#include "DynaSync.h"
#include "Anchor.h"
#include "EnemySync.h" // PackKey, authority election, ESYNC_LOG
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Bg_Po_Event/z_bg_po_event.h"
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
        case ACTOR_BG_SPOT01_FUSYA:   // Kakariko: windmill sails seen from outside —
                                      // cosmetic, NO DynaPoly, so it is fed to
                                      // OnDynaRegistered by an OnActorInit hook instead
        case ACTOR_BG_SPOT18_BASKET:  // Goron City: spinning vase
        case ACTOR_BG_MIZU_UZU:       // Water Temple: free-phase spinner (rot.y += 0x1C0/frame)
        case ACTOR_BG_MIZU_MOVEBG:    // Water Temple: moving platforms (flag + path)
        case ACTOR_BG_HAKA_SHIP:      // Shadow Temple: boat (flag then scripted sail — flag
                                      // replay times differ, the hull must not)
        case ACTOR_EN_SIOFUKI:        // Jabu: water-spout platforms (flag + timer + oscillation)
        case ACTOR_EN_BROB:           // Jabu: wobbling blob platform (timers + proximity)
            // NOT one-shot switch-flag movers (Bg_Ddan_Kd stairs, Hamsteps, gates,
            // shutters...): Anchor's flag sync already replays those on both
            // clients. NOT the SetFlag.cpp exclusions (water level, forest
            // elevator, tower collapse). NOT pushblocks (phase B, needs
            // per-actor claim). NOT Obj_Lift collapsing platforms (per-player
            // stand trigger). NOT boss-room platforms (adapter territory).
            return true;

        case ACTOR_BG_MORI_HASHIRA4:
            // Forest Temple, the four rotating pillars (params 0 after Init masks
            // it): BgMoriHashira4_PillarsRotate does
            //     shape.rot.y = world.rot.y += 0x96
            // every frame, unconditionally, from the moment the room loads. No
            // flag, no trigger, no resync point — so the phase is decided by WHEN
            // each client loaded the room and the offset never converges. Same
            // family as BG_MIZU_UZU above.
            // The other params are the gates: one-shot switch-flag movers the flag
            // sync already replays on both clients, so they stay out.
            return actor->params == 0;

        default:
            return false;
    }
}

static bool IsRideClaimable(const Actor* actor);

// Claimable: movable by WHOEVER touches it (push blocks). Motion is streamed
// by the claimant instead of the scene authority while a claim is fresh.
static bool IsClaimableDynaActor(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_BG_PO_EVENT: // Forest Temple: Amy's picture blocks (the paintings share the id, type >= 2)
            return ((const BgPoEvent*)actor)->type < 2;
        case ACTOR_OBJ_OSHIHIKI:       // push blocks (every dungeon)
        case ACTOR_BG_HIDAN_ROCK:      // Fire Temple push stone
        case ACTOR_BG_MORI_KAITENKABE: // Forest Temple: push-rotated basement wall
        case ACTOR_BG_JYA_COBRA:       // Spirit Temple: push-rotated cobra mirror
        case ACTOR_BG_ICE_OBJECTS:     // Ice Cavern: sliding ice blocks
        case ACTOR_BG_GND_ICEBLOCK:    // Ganon's Castle: sliding ice blocks
        case ACTOR_BG_PUSHBOX:         // generic push crate
        case ACTOR_BG_SPOT15_RRBOX:    // Lon Lon Ranch: push crate
        case ACTOR_BG_HAKA:            // Graveyard/tombs: pushable gravestones
        case ACTOR_BG_SPOT18_OBJ:      // Goron City: push statue (latches a flag once placed)
            return true;
        default:
            return IsRideClaimable(actor);
    }
}

// Ride-claimables: platforms driven by whoever STANDS on them. The trigger is
// DYNA_INTERACT_PLAYER_ON_TOP, written by our own Player's bgcheck (a puppet
// never sets it), never dyna.unk_150. Unlike push blocks their update rewrites
// world.pos from private state every frame (Obj_Bean replays its path points,
// Mori_Elevator picks a target from OUR room and flag), so a mirror must be
// suppressed outright: the pin cannot fight the update. Our rider still claims
// it — interactFlags are read in the hook, before the update would run.
static bool IsRideClaimable(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_OBJ_BEAN: // magic bean leaf platform (adult); the child soil never moves
            return LINK_AGE_IN_YEARS == YEARS_ADULT;
        case ACTOR_BG_MORI_ELEVATOR: // Forest Temple elevator (flag 0x1B stays per-client by design)
        case ACTOR_OBJ_ELEVATOR:     // stone elevator (Gerudo Training Ground), toggles on each rider
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
    bool ride = false;          // claimed by standing on it, mirrored with update suppressed
    bool hadUncullFlag = false; // actor's own culling setting, restored on release
    uint64_t localClaimTick = 0; // last tick our player pushed/moved it
    uint64_t softClaimTick = 0;  // last tick a module asked us to keep driving it (HoldSoftClaim)
    Vec3f prevPos{};             // motion detection for claim upkeep
    Vec3s prevRot{};             // idem — push-rotated walls/mirrors move rot only
};

// Latest streamed state per key (game-thread only: written by the queued
// DYNA_STATE handler, read by the ShouldActorUpdate mirror step).
struct RemoteDynaState {
    uint64_t rxTick = 0;
    Vec3f pos{};
    Vec3s rot{};
};

// A soft claim ("own":2): the sender is running a scripted sequence on the
// actor (the Amy puzzle driver) and wants everyone to follow, but yields to a
// real push claim, and between two soft claimants the lower client id wins.
struct RemoteSoftClaim {
    uint64_t rxTick = 0;
    uint32_t senderId = 0;
};

static std::unordered_map<Actor*, TrackedDyna> sTracked;
static std::unordered_map<uint64_t, RemoteDynaState> sRemote;
static std::unordered_map<uint64_t, uint64_t> sRemoteClaims; // key -> last claimed-entry rx tick
static std::unordered_map<uint64_t, RemoteSoftClaim> sRemoteSoft; // key -> last soft-claimed entry
static uint64_t sTick = 0;

static bool Fresh(uint64_t tick, uint64_t window) {
    return tick != 0 && (sTick - tick) <= window;
}

// True when a fresh remote soft claim outranks our own (any remote soft claim
// when we hold none; the lower client id when both hold one).
static bool RemoteSoftWins(const TrackedDyna& st) {
    auto it = sRemoteSoft.find(st.key);
    if (it == sRemoteSoft.end() || !Fresh(it->second.rxTick, CLAIM_TICKS)) {
        return false;
    }
    if (!Fresh(st.softClaimTick, CLAIM_TICKS)) {
        return true;
    }
    return Anchor::Instance == nullptr || it->second.senderId < Anchor::Instance->ownClientId;
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
    sRemoteSoft.clear();
}

// Identity = quantized spawn position + actor id, computed at Init before the
// actor ever moves. NOT ActorListIndex: the same scene loads different actor
// lists per setup layer (age, time of day), so the same platform can sit at a
// different index on each client (seen live: Kakariko windmill sails, index 6
// on one side, 5 on the other). Spawn positions are identical everywhere.
static uint64_t PosKey(const Actor* actor) {
    int16_t qx = (int16_t)lroundf(actor->world.pos.x);
    int16_t qy = (int16_t)lroundf(actor->world.pos.y);
    int16_t qz = (int16_t)lroundf(actor->world.pos.z);
    return ((uint64_t)(uint16_t)qx << 48) | ((uint64_t)(uint16_t)qy << 32) | ((uint64_t)(uint16_t)qz << 16) |
           (uint16_t)actor->id;
}

// Fires from DynaPoly_SetBgActor (z_bgcheck.c) during the actor's own Init
// (or from OnActorInit for whitelisted spinners with no DynaPoly).
static void OnDynaRegistered(Actor* actor) {
    if (!SyncEnabled() || sTracked.contains(actor) || (!IsSyncedDynaActor(actor) && !IsClaimableDynaActor(actor))) {
        return;
    }

    TrackedDyna st;
    st.key = PosKey(actor);
    st.claimable = IsClaimableDynaActor(actor);
    st.ride = IsRideClaimable(actor);
    st.prevPos = actor->world.pos;
    st.hadUncullFlag = (actor->flags & ACTOR_FLAG_UPDATE_CULLING_DISABLED) != 0;
    sTracked[actor] = st;
    ESYNC_LOG("[DynaSync] tracked id={} params={:#x} key={:#x}", actor->id, (uint16_t)actor->params, st.key);

    // The update-culling gate in Actor_UpdateAll sits BEFORE the
    // ShouldActorUpdate hook: the authority needs the AI running while its
    // player is far, and the mirror needs the hook firing to apply the stream.
    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
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

    if (st.claimable) {
        // Claim upkeep: a nonzero push force means OUR player is moving it;
        // keep the claim while it still slides after release.
        bool moved = fabsf(actor->world.pos.x - st.prevPos.x) > POS_EPSILON ||
                     fabsf(actor->world.pos.y - st.prevPos.y) > POS_EPSILON ||
                     fabsf(actor->world.pos.z - st.prevPos.z) > POS_EPSILON ||
                     actor->shape.rot.x != st.prevRot.x || actor->shape.rot.y != st.prevRot.y ||
                     actor->shape.rot.z != st.prevRot.z;
        st.prevPos = actor->world.pos;
        st.prevRot = actor->shape.rot;
        bool localSignal = st.ride ? (((DynaPolyActor*)actor)->interactFlags & DYNA_INTERACT_PLAYER_ON_TOP) != 0
                                   : fabsf(((DynaPolyActor*)actor)->unk_150) > 0.001f;
        if (localSignal || (Fresh(st.localClaimTick, CLAIM_TICKS) && moved && !st.suppressed)) {
            st.localClaimTick = sTick;
        }
        if (Fresh(st.localClaimTick, CLAIM_TICKS)) {
            st.suppressed = false;
            return; // we own it: local simulation, PerFrameTick streams it
        }
        // A remote push claim outranks everything else; otherwise a soft claim
        // we hold keeps our copy live unless a better-ranked peer holds one.
        auto rc = sRemoteClaims.find(st.key);
        bool remoteHard = rc != sRemoteClaims.end() && Fresh(rc->second, CLAIM_TICKS);
        if (!remoteHard && Fresh(st.softClaimTick, CLAIM_TICKS) && !RemoteSoftWins(st)) {
            st.suppressed = false;
            return;
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
    // Bg_Haka repurposes minVelocityY as its pull distance: the draw offsets the
    // dirt patch back by it (else the dirt slides along with the stone) and a
    // claim takeover recomputes pos from it. Reconstruct it from the streamed
    // displacement projected on the facing axis.
    if (actor->id == ACTOR_BG_HAKA) {
        // The pull axis is shape.rot.y FLIPPED: Bg_Haka sets world.rot.y =
        // shape.rot.y + 0x8000 when a pull starts — which never happens on the
        // mirror, so world.rot.y is not usable here. Set it too, so a claim
        // takeover by our player continues the pull instead of teleporting.
        int16_t pullYaw = (int16_t)(actor->shape.rot.y + 0x8000);
        actor->world.rot.y = pullYaw;
        actor->minVelocityY = (actor->world.pos.x - actor->home.pos.x) * Math_SinS(pullYaw) +
                              (actor->world.pos.z - actor->home.pos.z) * Math_CosS(pullYaw);
    }
    st.suppressed = true;
    // Claimables keep their update RUNNING while mirroring: the block's own
    // update is what offers the grab (A) to our player, so a suppressed block
    // could never be claimed by anyone but the authority. Its idle AI can't
    // fight the stream — the position is overwritten every frame above, and the
    // first real push (dyna.unk_150) flips it to a local claim before this.
    // Ride-claimables are the exception: their update WOULD fight the pin (see
    // IsRideClaimable), and standing on them needs no update to be detected.
    if (!st.claimable || st.ride) {
        *should = false;
    }
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
            if (e.value("own", 0) == 2) {
                sRemoteSoft[key] = RemoteSoftClaim{ sTick, senderClientId };
            } else if (owned) {
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
        bool localClaim = st.claimable && Fresh(st.localClaimTick, CLAIM_TICKS);
        bool remoteClaim = false;
        bool softClaim = false;
        if (st.claimable) {
            auto rc = sRemoteClaims.find(st.key);
            remoteClaim = rc != sRemoteClaims.end() && Fresh(rc->second, CLAIM_TICKS);
            auto rs = sRemoteSoft.find(st.key);
            bool remoteSoft = rs != sRemoteSoft.end() && Fresh(rs->second.rxTick, CLAIM_TICKS);
            softClaim = !localClaim && !remoteClaim && Fresh(st.softClaimTick, CLAIM_TICKS) && !RemoteSoftWins(st);
            remoteClaim = remoteClaim || (remoteSoft && !softClaim);
        }
        // The claimant streams its blocks; the authority streams the rest.
        if (!(localClaim || softClaim || (isAuthority && !remoteClaim))) {
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
        } else if (softClaim) {
            e["own"] = 2;
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

// True while this copy is a mirror of a fresh remote stream (authority or a
// remote claimant): its scripted motion is not ours to run.
bool IsRemoteDriven(const Actor* actor) {
    auto it = sTracked.find(const_cast<Actor*>(actor));
    return it != sTracked.end() && it->second.suppressed;
}

// Keep driving a claimable actor from this client for CLAIM_TICKS more ticks:
// our copy stays live and streams with "own":2 unless someone is pushing it or
// a lower client id holds its own soft claim. Called every frame by the module
// running the scripted sequence (the Amy puzzle), so a peer entering the room
// later mirrors the puzzle in progress instead of overriding it as authority.
void HoldSoftClaim(Actor* actor) {
    auto it = sTracked.find(actor);
    if (it != sTracked.end() && it->second.claimable) {
        it->second.softClaimTick = sTick;
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

    // Whitelisted spinners with no DynaPoly collision never reach the hook
    // above; track them from their plain actor init.
    COND_ID_HOOK(OnActorInit, ACTOR_BG_SPOT01_FUSYA, isConnected,
                 [](void* actor) { OnDynaRegistered((Actor*)actor); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actor) { sTracked.erase((Actor*)actor); });

    COND_HOOK(ShouldActorUpdate, isConnected,
              [](void* actor, bool* should) { OnShouldDynaUpdate((Actor*)actor, should); });
}

} // namespace DynaSync
