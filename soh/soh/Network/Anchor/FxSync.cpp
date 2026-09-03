#include "FxSync.h"
#include "Anchor.h"
#include "EnemySync.h" // ESYNC_LOG
#include "PuppetRef.h"
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Magic_Fire/z_magic_fire.h"
#include "src/overlays/actors/ovl_Obj_Syokudai/z_obj_syokudai.h"
#include "src/overlays/actors/ovl_En_Boom/z_en_boom.h"
#include "src/overlays/actors/ovl_En_Arrow/z_en_arrow.h"
#include "src/overlays/actors/ovl_Arms_Hook/z_arms_hook.h"
#include "src/overlays/actors/ovl_Bg_Po_Event/z_bg_po_event.h"
#include "src/overlays/actors/ovl_En_G_Switch/z_en_g_switch.h"
#include "src/overlays/effects/ovl_Effect_Ss_HitMark/z_eff_ss_hitmark.h"
extern PlayState* gPlayState;
// z_en_g_switch.c: silver-rupee tally accessor + action funcs.
s16* EnGSwitch_CoopCollectedCount(void);
void EnGSwitch_SilverRupeeIdle(EnGSwitch* self, PlayState* play);
void EnGSwitch_SilverRupeeCollected(EnGSwitch* self, PlayState* play);
// z_bg_po_event.c: file-scope (non-static) puzzle state + painting action funcs.
extern u8 sBgPoEventPuzzleState;
void BgPoEvent_PaintingEmpty(BgPoEvent* self, PlayState* play);
void BgPoEvent_PaintingAppear(BgPoEvent* self, PlayState* play);
void BgPoEvent_PaintingPresent(BgPoEvent* self, PlayState* play);
void BgPoEvent_PaintingVanish(BgPoEvent* self, PlayState* play);
void BgPoEvent_PaintingBurn(BgPoEvent* self, PlayState* play);
// z_player.c internal, already called cross-file by En_Partner.
void Player_RequestQuake(PlayState* play, s32 speed, s32 y, s32 countdown);
}

namespace FxSync {

enum FxEventKind : uint8_t {
    FX_EV_SPELL = 0,  // caster spawned a magic spell actor (aid says which)
    FX_EV_TORCH = 1,  // a torch's litTimer flipped (key + new value)
    FX_EV_NUT = 2,       // deku nut impact flash (pos)
    FX_EV_HAMMER = 3,    // hammer ground shockwave (pos)
    FX_EV_HOOKSPARK = 4, // hookshot reflected off a wall (pos)
    FX_EV_POEPAINT = 5,  // Forest Temple portrait chain decided its next painting (pt, pi, ev, nx)
    FX_EV_SHIELD = 6,    // our shield deflected a hit (pos, metal)
    FX_EV_SILVER = 7,    // we picked up a silver rupee (home pos): peers tally it and remove theirs
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

// --- Puppet projectiles (arrows + hookshot) ---------------------------------
// Same model as the boomerang: the real actor exists only on its owner's game;
// PLAYER_UPDATE streams a per-frame snapshot and the peer pins neutered
// replicas. Arrow replicas get ONE grace update frame after spawn: EnArrow's
// own update hands EnArrow_Shoot to EnArrow_Fly (arming the blure trail) and
// spawns the elemental child (ArrowFire/Ice/Light, safe to leave running: it
// only follows its parent). AT flags are cleared every frame because both
// draws submit their own AT quad (func_80090480). The hookshot replica needs
// no grace: the retracted-branch offsets already draw the tip at the pinned
// position, and the chain runs from the puppet's hand via the parent
// substitution patched into ArmsHook_Draw.

struct ProjSnap {
    // Stable per-arrow identity (low bits of the OWNER's actor pointer). Actors
    // are PREPENDED to their category list, so firing a second arrow shifts the
    // first one's slot: pairing snapshot slot i with replica slot i made two
    // arrows of the same type swap identities mid-flight.
    uint32_t id = 0;
    int16_t params = 0;
    Vec3f pos = { 0, 0, 0 };
    Vec3s rot = { 0, 0, 0 };
    uint8_t timer = 0;
    uint8_t hitFlags = 0;
    bool nocked = false; // still on the bow: no Fly handoff (trail) yet
};

struct ProjReplica {
    uint32_t id = 0; // matches ProjSnap::id
    Actor* actor = NULL;
    uint8_t grace = 0; // update frames still allowed after spawn
    bool armed = false; // Fly handoff granted (release moment)
};

struct PuppetProjectiles {
    std::vector<ProjSnap> arrows;
    bool hookActive = false;
    Vec3f hookPos = { 0, 0, 0 };
    Vec3s hookRot = { 0, 0, 0 };
    std::vector<ProjReplica> arrowReplicas;
    Actor* hookReplica = NULL;
    uint8_t hookGrace = 0; // ticks to survive a flickering stream
};

static std::unordered_map<uint32_t, PuppetProjectiles> sProj;

static bool IsOwnProjReplica(Actor* a) {
    for (auto& [cid, pp] : sProj) {
        if (pp.hookReplica == a) {
            return true;
        }
        for (auto& r : pp.arrowReplicas) {
            if (r.actor == a) {
                return true;
            }
        }
    }
    return false;
}

void CollectProjectiles(nlohmann::json& payload) {
    if (gPlayState == NULL) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);

    nlohmann::json arr = nlohmann::json::array();
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; a != NULL && arr.size() < 3;
         a = a->next) {
        // Flying arrows (parent NULL) AND the nocked one (parent = player,
        // positioned at the hand — the puppet model doesn't draw it).
        if (a->id != ACTOR_EN_ARROW || a->update == NULL || IsOwnProjReplica(a) ||
            (a->parent != NULL && a->parent != &player->actor)) {
            continue;
        }
        EnArrow* ar = (EnArrow*)a;
        nlohmann::json e;
        e["id"] = (uint32_t)((uintptr_t)a & 0xFFFFFFFFu); // stable while this arrow lives
        e["p"] = a->params;
        e["pos"] = a->world.pos;
        e["rot"] = a->shape.rot;
        e["t"] = ar->timer;
        e["hf"] = ar->hitFlags;
        e["n"] = a->parent != NULL; // nocked
        arr.push_back(e);
    }
    if (!arr.empty()) {
        payload["projA"] = arr;
    }

    // The shot hook: scan instead of trusting heldActor (which flickers during
    // the shot and made the peer's replica kill/respawn every other tick). The
    // actor only exists while a shot is out, so no distance gate is needed.
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; a != NULL; a = a->next) {
        if (a->id != ACTOR_ARMS_HOOK || a->update == NULL || IsOwnProjReplica(a)) {
            continue;
        }
        payload["hookPos"] = a->world.pos;
        payload["hookRot"] = a->shape.rot;
        break;
    }
}

void IngestPlayerProjectiles(uint32_t clientId, const nlohmann::json& payload) {
    PuppetProjectiles& pp = sProj[clientId];
    pp.arrows.clear();
    if (payload.contains("projA")) {
        for (auto& e : payload["projA"]) {
            ProjSnap s;
            s.id = e.contains("id") ? e["id"].get<uint32_t>() : 0;
            s.params = e["p"].get<int16_t>();
            s.pos = e["pos"].get<Vec3f>();
            s.rot = e["rot"].get<Vec3s>();
            s.timer = e.contains("t") ? e["t"].get<uint8_t>() : 0;
            s.hitFlags = e.contains("hf") ? e["hf"].get<uint8_t>() : 0;
            s.nocked = e.contains("n") && e["n"].get<bool>();
            pp.arrows.push_back(s);
        }
    }
    pp.hookActive = payload.contains("hookPos");
    if (pp.hookActive) {
        pp.hookPos = payload["hookPos"].get<Vec3f>();
        pp.hookRot = payload["hookRot"].get<Vec3s>();
    }
}

// One boomerang replica per remote client (defined here so ReapClientProjectiles
// below can drop it together with that client's arrows and hookshot).
static std::unordered_map<uint32_t, Actor*> sBoomReplicas;

// unk_A73 is the local player's "just fired" counter. A replica arrow needs it
// non-zero for its single graced vanilla update or EnArrow_Shoot kills itself
// (z_en_arrow.c:211), but bosses read the SAME field to react to OUR shots
// (Ganondorf/Twinrova deflect, Gohma's stun window). So borrow it and give it
// back as soon as no replica still needs it, instead of leaving it set.
static bool sA73Borrowed = false;
static uint8_t sA73Saved = 0;

static void BorrowFiredCounter() {
    Player* lp = GET_PLAYER(gPlayState);
    if (!sA73Borrowed) {
        sA73Saved = lp->unk_A73;
        sA73Borrowed = true;
    }
    lp->unk_A73 = 4;
}

static void ReleaseFiredCounterIfIdle() {
    if (!sA73Borrowed || gPlayState == NULL) {
        return;
    }
    for (auto& [cid, other] : sProj) {
        for (auto& r : other.arrowReplicas) {
            if (r.actor != NULL && r.grace > 0) {
                return; // still mid-handoff
            }
        }
    }
    Player* lp = GET_PLAYER(gPlayState);
    if (lp->unk_A73 <= 4) { // untouched since we set it: nothing of the player's to lose
        lp->unk_A73 = sA73Saved;
    }
    sA73Borrowed = false;
}

// Kills every replica owned by one client and drops the pointers.
//
// Replicas are only ticked from DummyPlayer_Update, but Anchor::RefreshClientActors
// Actor_Kills EVERY dummy Player on any roster change (join, leave, disconnect).
// Without this the replicas linger with their update suppressed, are never
// reaped, and the hookshot replica keeps `parent` pointing at the freed arena
// block -- which ArmsHook_Draw dereferences on every draw frame.
void ReapClientProjectiles(uint32_t clientId) {
    auto it = sProj.find(clientId);
    if (it != sProj.end()) {
        PuppetProjectiles& pp = it->second;
        for (auto& r : pp.arrowReplicas) {
            if (r.actor != NULL) {
                if (r.actor->update != NULL) {
                    Actor_Kill(r.actor);
                }
                r.actor = NULL;
            }
        }
        pp.arrowReplicas.clear();
        pp.arrows.clear();
        if (pp.hookReplica != NULL) {
            pp.hookReplica->parent = NULL; // never leave a dangling parent for the draw
            if (pp.hookReplica->update != NULL) {
                Actor_Kill(pp.hookReplica);
            }
            pp.hookReplica = NULL;
        }
        pp.hookActive = false;
        pp.hookGrace = 0;
    }
    auto bit = sBoomReplicas.find(clientId);
    if (bit != sBoomReplicas.end()) {
        if (bit->second != NULL && bit->second->update != NULL) {
            Actor_Kill(bit->second);
        }
        sBoomReplicas.erase(bit);
    }
}

void TickPuppetProjectiles(uint32_t clientId, Actor* puppet) {
    if (gPlayState == NULL || !SyncEnabled()) {
        return;
    }
    if (puppet == NULL || puppet->update == NULL) {
        ReapClientProjectiles(clientId); // owner gone: nothing left to pin them to
        return;
    }
    PuppetProjectiles& pp = sProj[clientId];

    // Arrows are matched by the owner's stable arrow id, never by slot.
    std::vector<ProjReplica> kept;
    kept.reserve(pp.arrows.size());
    for (auto& s : pp.arrows) {
        ProjReplica found;
        for (auto& r : pp.arrowReplicas) {
            if (r.actor != NULL && r.id == s.id) {
                found = r;
                r.actor = NULL; // claimed; whatever is left over is reaped below
                break;
            }
        }
        found.id = s.id;
        kept.push_back(found);
    }
    for (auto& r : pp.arrowReplicas) { // snapshots that disappeared
        if (r.actor != NULL && r.actor->update != NULL) {
            Actor_Kill(r.actor); // the elemental child self-kills on parent->update == NULL
        }
    }
    pp.arrowReplicas.swap(kept);

    for (size_t i = 0; i < pp.arrows.size(); i++) {
        ProjReplica& r = pp.arrowReplicas[i];
        ProjSnap& s = pp.arrows[i];
        if (r.actor != NULL && (r.actor->update == NULL || r.actor->params != s.params)) {
            if (r.actor->update != NULL) {
                Actor_Kill(r.actor);
            }
            r.actor = NULL;
        }
        if (r.actor == NULL) {
            BorrowFiredCounter();
            r.actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_ARROW, s.pos.x, s.pos.y, s.pos.z,
                                  s.rot.x, s.rot.y, 0, s.params, false);
            r.grace = 0;
            r.armed = false;
            if (r.actor != NULL) {
                ((EnArrow*)r.actor)->collider.base.atFlags &= ~AT_ON;
            }
            ESYNC_LOG("[FxSync] puppet arrow spawn client={} p={} ok={}", clientId, s.params, r.actor != NULL);
        }
        // Fly handoff (trail + elemental child) only once released — a nocked
        // replica armed too early streaked a white trail off the arrowhead.
        if (!s.nocked && !r.armed) {
            r.armed = true;
            r.grace = 1;
            BorrowFiredCounter(); // the graced update runs EnArrow_Shoot
        }
        if (r.actor != NULL) {
            EnArrow* ar = (EnArrow*)r.actor;
            r.actor->world.pos = s.pos;
            r.actor->shape.rot = s.rot;
            r.actor->speedXZ = 1.0f; // gates the nut/seed sparkle rendering
            ar->timer = s.timer;
            ar->hitFlags = s.hitFlags;
            ar->collider.base.atFlags &= ~AT_ON; // the draw submits the AT quad itself
            // Elemental flame/frost/light child: vanilla spawns it from EnArrow's
            // update (suppressed here), so do it ourselves — this also lights the
            // NOCKED arrow, which the update wouldn't reach until release.
            if (r.actor->child == NULL && s.params >= ARROW_FIRE && s.params <= ARROW_LIGHT) {
                int16_t childId = (s.params == ARROW_FIRE)  ? ACTOR_ARROW_FIRE
                                  : (s.params == ARROW_ICE) ? ACTOR_ARROW_ICE
                                                            : ACTOR_ARROW_LIGHT;
                Actor_SpawnAsChild(&gPlayState->actorCtx, r.actor, gPlayState, childId, s.pos.x, s.pos.y, s.pos.z, 0,
                                   0, 0, 0);
            }
            if (r.actor->child != NULL) {
                r.actor->child->destroy = NULL; // ArrowFire/Light_Destroy calls Magic_Reset (local magic!)
            }
        }
    }
    ReleaseFiredCounterIfIdle();

    // Hookshot: fully suppressed, pinned, chained to the puppet via parent.
    // Hysteresis: survive short stream gaps instead of kill/respawn flicker.
    if (pp.hookReplica != NULL && pp.hookReplica->update == NULL) {
        pp.hookReplica = NULL;
    }
    if (pp.hookActive) {
        pp.hookGrace = 6;
    } else if (pp.hookGrace > 0) {
        pp.hookGrace--;
    }
    if (pp.hookActive || pp.hookGrace > 0) {
        if (pp.hookReplica == NULL) {
            pp.hookReplica = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_ARMS_HOOK, pp.hookPos.x,
                                         pp.hookPos.y, pp.hookPos.z, 0, pp.hookRot.y, 0, 0, false);
            ESYNC_LOG("[FxSync] puppet hook spawn client={} ok={}", clientId, pp.hookReplica != NULL);
        }
        if (pp.hookReplica != NULL) {
            pp.hookReplica->world.pos = pp.hookPos;
            pp.hookReplica->shape.rot = pp.hookRot;
            pp.hookReplica->parent = puppet; // ArmsHook_Draw chains from this player's hand
            ((ArmsHook*)pp.hookReplica)->collider.base.atFlags &= ~AT_ON;
        }
    } else if (pp.hookReplica != NULL) {
        Actor_Kill(pp.hookReplica);
        pp.hookReplica = NULL;
    }
}

extern "C" void FxSync_OnHookSpark(Vec3f* pos) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || pos == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_HOOKSPARK;
    payload["pos"] = *pos;
    Anchor::Instance->SendPacket_FxEvent(payload);
}

extern "C" void FxSync_OnHammerShockwave(Vec3f* pos) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || pos == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_HAMMER;
    payload["pos"] = *pos;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] hammer shockwave tx");
}

// Forest Temple Poe paintings (Joelle/Beth): the portrait hops between the
// three paintings of a chain, the next one picked at RANDOM (BgPoEvent_
// NextPainting), and only the copy that got shot/approached knows the pick.
// Broadcast (type, index, event, chosen next) so the peer applies the same
// pick instead of rolling its own; the Poe itself is an announced dynamic spawn.
extern "C" void FxSync_OnPoePainting(Actor* painting, s32 ev) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || painting == NULL) {
        return;
    }
    BgPoEvent* p = (BgPoEvent*)painting;
    nlohmann::json payload;
    payload["kind"] = FX_EV_POEPAINT;
    payload["pt"] = p->type;
    payload["pi"] = p->index;
    payload["ev"] = ev;
    payload["nx"] = sBgPoEventPuzzleState;
    Anchor::Instance->SendPacket_FxEvent(payload);
}

// Shield deflection sparks spawn from the blocker's own collision pass only.
extern "C" void FxSync_OnShieldHit(Vec3f* pos, s32 metal) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || pos == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_SHIELD;
    payload["pos"] = *pos;
    payload["metal"] = metal != 0;
    Anchor::Instance->SendPacket_FxEvent(payload);
}

// --- Silver rupees ----------------------------------------------------------
// En_G_Switch tallies pickups in a file static, per client: split 3/2 between
// two players, nobody reaches 5 and the door never opens. Announce our pickups
// (the Idle -> Collected edge); the peer bumps its tally and removes its copy.
static std::unordered_set<Actor*> sSilverAnnounced;

static void OnSilverRupeePreUpdate(Actor* a) {
    EnGSwitch* g = (EnGSwitch*)a;
    if (g->type != ENGSWITCH_SILVER_RUPEE || g->actionFunc != EnGSwitch_SilverRupeeCollected ||
        sSilverAnnounced.contains(a)) {
        return;
    }
    sSilverAnnounced.insert(a);
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_SILVER;
    payload["pos"] = a->home.pos;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] silver rupee tx");
}

// --- Puppet boomerang -------------------------------------------------------
// One replica per remote client, spawned while their stream says "in flight"
// and killed when it stops (catch, drop, scene change). The pin happens here;
// suppression happens in the ShouldActorUpdate hook below.


void TickPuppetBoomerang(uint32_t clientId, bool active, Vec3f pos, int16_t yaw, int16_t pitch) {
    if (gPlayState == NULL || !SyncEnabled()) {
        return;
    }
    Actor*& boom = sBoomReplicas[clientId];
    if (boom != NULL && boom->update == NULL) {
        boom = NULL; // reaped elsewhere (scene teardown)
    }
    if (active) {
        if (boom == NULL) {
            boom = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_BOOM, pos.x, pos.y, pos.z, 0, yaw, 0, 0,
                               false);
            ESYNC_LOG("[FxSync] puppet boomerang spawn client={} ok={}", clientId, boom != NULL);
        }
        if (boom != NULL) {
            // EnBoom_Draw orients on world.rot and spins from activeTimer —
            // both update-fed on the thrower, so drive them here.
            boom->world.pos = pos;
            boom->world.rot.y = yaw;
            boom->world.rot.x = pitch;
            ((EnBoom*)boom)->activeTimer++;
        }
    } else if (boom != NULL) {
        Actor_Kill(boom);
        boom = NULL;
    }
}

static void Reset() {
    sSilverAnnounced.clear();
    sSpellReplicas.clear();
    sTorches.clear();
    sTorchByKey.clear();
    sBoomReplicas.clear();
    sProj.clear();
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

        if (kind == FX_EV_NUT) {
            Vec3f pos = payload.at("pos").get<Vec3f>();
            // The white flash fills the whole screen — only take it when the
            // burst is actually near us, like the thrower experiences it.
            if (Math_Vec3f_DistXYZ(&GET_PLAYER(gPlayState)->actor.world.pos, &pos) < 600.0f) {
                iREG(50) = -1; // full-screen white flash (TransitionFade, z_fbdemo_fade.c)
            }
            EffectSsStone1_Spawn(gPlayState, &pos, 0);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 20, NA_SE_IT_DEKU);
            ESYNC_LOG("[FxSync] nut flash rx");
            return;
        }

        if (kind == FX_EV_HOOKSPARK) {
            Vec3f pos = payload.at("pos").get<Vec3f>();
            CollisionCheck_SpawnShieldParticlesMetal(gPlayState, &pos);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 4, NA_SE_IT_HOOKSHOT_REFLECT);
            return;
        }

        if (kind == FX_EV_HAMMER) {
            static Vec3f zeroVec = { 0.0f, 0.0f, 0.0f };
            Vec3f pos = payload.at("pos").get<Vec3f>();
            Player_RequestQuake(gPlayState, 27767, 7, 20);
            gPlayState->actorCtx.unk_02 = 4; // scene-wide actor shake, as vanilla
            EffectSsBlast_SpawnWhiteShockwave(gPlayState, &pos, &zeroVec, &zeroVec);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 20, NA_SE_IT_HAMMER_HIT);
            ESYNC_LOG("[FxSync] hammer shockwave rx");
            return;
        }

        if (kind == FX_EV_POEPAINT) {
            uint8_t pt = payload.at("pt").get<uint8_t>();
            uint8_t pi = payload.at("pi").get<uint8_t>();
            uint8_t ev = payload.at("ev").get<uint8_t>();
            uint8_t nx = payload.at("nx").get<uint8_t>();
            if (ev != 2) {
                sBgPoEventPuzzleState = nx; // the Empty painting with this index appears on its own
            }
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_BG_PO_EVENT) {
                    continue;
                }
                BgPoEvent* p = (BgPoEvent*)a;
                if (p->type != pt || p->type < 2) {
                    continue;
                }
                bool showing = p->actionFunc == BgPoEvent_PaintingPresent ||
                               p->actionFunc == BgPoEvent_PaintingAppear || p->actionFunc == BgPoEvent_PaintingVanish;
                if (p->index == pi) {
                    if (ev == 0) {
                        if (showing) { // portrait left this painting: blank it (no fade, the sender already faded)
                            p->timer = 255;
                            p->actionFunc = BgPoEvent_PaintingEmpty;
                        }
                    } else if (p->actionFunc != BgPoEvent_PaintingBurn) {
                        // shot: same unlink + burn as vanilla, minus the shooter's spawn/cutscene
                        if (a->parent != NULL) {
                            a->parent->child = NULL;
                            a->parent = NULL;
                        }
                        if (a->child != NULL) {
                            a->child->parent = NULL;
                            a->child = NULL;
                        }
                        p->timer = 20;
                        p->actionFunc = BgPoEvent_PaintingBurn;
                        if (ev == 1) {
                            Audio_PlayActorSound2(a, NA_SE_EN_PO_LAUGH2);
                        }
                    }
                } else if (ev != 2 && p->index != nx && showing) {
                    p->timer = 255; // a stale local portrait elsewhere in the chain
                    p->actionFunc = BgPoEvent_PaintingEmpty;
                }
            }
            ESYNC_LOG("[FxSync] poe painting rx type={} idx={} ev={} next={}", pt, pi, ev, nx);
            return;
        }

        if (kind == FX_EV_SHIELD) {
            Vec3f pos = payload.at("pos").get<Vec3f>();
            if (payload.value("metal", true)) {
                EffectSsHitMark_SpawnFixedScale(gPlayState, EFFECT_HITMARK_METAL, &pos);
                CollisionCheck_SpawnShieldParticlesMetal(gPlayState, &pos);
                SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 4, NA_SE_IT_SHIELD_REFLECT_SW);
            } else {
                CollisionCheck_SpawnShieldParticles(gPlayState, &pos);
                SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 4, NA_SE_IT_REFLECTION_WOOD);
            }
            return;
        }

        if (kind == FX_EV_SILVER) {
            Vec3f pos = payload.at("pos").get<Vec3f>();
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_EN_G_SWITCH) {
                    continue;
                }
                EnGSwitch* g = (EnGSwitch*)a;
                if (g->type != ENGSWITCH_SILVER_RUPEE || g->actionFunc != EnGSwitch_SilverRupeeIdle ||
                    Math_Vec3f_DistXYZ(&a->home.pos, &pos) > 40.0f) {
                    continue;
                }
                (*EnGSwitch_CoopCollectedCount())++; // the tracker plays the note / sets the flag from this
                sSilverAnnounced.insert(a);           // a remote pickup is never re-announced
                Actor_Kill(a);
                ESYNC_LOG("[FxSync] silver rupee rx -> tally {}", *EnGSwitch_CoopCollectedCount());
                break;
            }
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

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_ARROW, isConnected, [](void* actor, bool* should) {
        for (auto& [cid, pp] : sProj) {
            for (auto& r : pp.arrowReplicas) {
                if (r.actor == (Actor*)actor) {
                    if (r.grace > 0) {
                        r.grace--; // one vanilla update: Fly handoff + elemental child
                    } else {
                        *should = false;
                    }
                    return;
                }
            }
        }
    });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_ARMS_HOOK, isConnected, [](void* actor, bool* should) {
        if (IsOwnProjReplica((Actor*)actor)) {
            *should = false;
        }
    });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_BOOM, isConnected, [](void* actor, bool* should) {
        for (auto& [cid, boom] : sBoomReplicas) {
            if (boom == (Actor*)actor) {
                *should = false; // pinned from the stream; never fly or collide
                return;
            }
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_G_SWITCH, isConnected,
                 [](void* actor, bool* should) { OnSilverRupeePreUpdate((Actor*)actor); });

    COND_ID_HOOK(OnActorInit, ACTOR_OBJ_SYOKUDAI, isConnected, [](void* actor) { OnTorchInit((Actor*)actor); });
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_SYOKUDAI, isConnected,
                 [](void* actor, bool* should) { OnTorchPreUpdate((Actor*)actor); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actorRef) {
        Actor* a = (Actor*)actorRef;
        sSpellReplicas.erase(a);
        sSilverAnnounced.erase(a);
        // A dying puppet takes its projectile replicas with it: they are only
        // ticked from DummyPlayer_Update, so nothing else would ever reap them.
        if (Anchor::Instance != nullptr) {
            for (auto& [cid, client] : Anchor::Instance->clients) {
                if ((Actor*)client.player == a) {
                    ReapClientProjectiles(cid);
                    break;
                }
            }
        }
        for (auto it = sBoomReplicas.begin(); it != sBoomReplicas.end();) {
            it = (it->second == a) ? sBoomReplicas.erase(it) : std::next(it);
        }
        if (a->id == ACTOR_EN_ARROW || a->id == ACTOR_ARMS_HOOK) {
            bool wasReplica = false;
            for (auto& [cid, pp] : sProj) {
                if (pp.hookReplica == a) {
                    pp.hookReplica = NULL;
                    wasReplica = true;
                }
                for (auto& r : pp.arrowReplicas) {
                    if (r.actor == a) {
                        r.actor = NULL;
                        wasReplica = true;
                    }
                }
            }
            // Deku nut impact: the white flash + burst are spawned by the
            // owner's EnArrow update at death — replay them on peers.
            if (!wasReplica && a->id == ACTOR_EN_ARROW && a->params == ARROW_NUT && a->parent == NULL &&
                SyncEnabled() && gPlayState != NULL) {
                nlohmann::json payload;
                payload["kind"] = FX_EV_NUT;
                payload["pos"] = a->world.pos;
                Anchor::Instance->SendPacket_FxEvent(payload);
                ESYNC_LOG("[FxSync] nut flash tx");
            }
        }
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
