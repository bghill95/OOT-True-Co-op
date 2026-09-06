#include "FxSync.h"
#include "Anchor.h"
#include "EnemySync.h" // ESYNC_LOG
#include "PuppetRef.h"
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <vector>
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
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
#include "src/overlays/actors/ovl_Door_Ana/z_door_ana.h"
#include "src/overlays/actors/ovl_En_Door/z_en_door.h"
#include "src/overlays/actors/ovl_Bg_Menkuri_Eye/z_bg_menkuri_eye.h"
// Lights one GTG eye target on behalf of the peer (see FX_EV_GTGEYE).
void BgMenkuriEye_LightFromPeer(Actor* thisx, PlayState* play);
// Topples one hammerable totem on behalf of the peer (see FX_EV_TOTEM).
void BgHidanDalm_BreakFromPeer(Actor* thisx, PlayState* play, u8 reverse);
// Flips one flagless Song of Time block on behalf of the peer (see FX_EV_TIMEBLOCK).
void ObjTimeblock_ToggleFromPeer(Actor* thisx, PlayState* play);
// Applies the upstairs player's cobra-solved bits (see FX_EV_BIGMIRROR).
void BgJyaBigmirror_ApplySolvedFromPeer(Actor* thisx, u8 solvedBits);
// Puts one Gerudo guard down on behalf of the peer (see FX_EV_GERUDOKO).
void EnGe2_KnockOutFromPeer(Actor* thisx, PlayState* play);
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
// z_door_ana.c: the open-grotto update routine (non-static, like BgPoEvent above).
void DoorAna_WaitOpen(DoorAna* self, PlayState* play);
void DoorAna_WaitClosed(DoorAna* self, PlayState* play);
}

namespace FxSync {

enum FxEventKind : uint8_t {
    FX_EV_SPELL = 0,  // caster spawned a magic spell actor (aid says which)
    FX_EV_TORCH = 1,  // a torch's litTimer flipped (key + new value)
    FX_EV_AMMOHIT = 2,   // an EN_ARROW projectile hit something (pos, ap = arrow params)
    FX_EV_HAMMER = 3,    // hammer ground shockwave (pos)
    FX_EV_HOOKSPARK = 4, // hookshot reflected off a wall (pos)
    FX_EV_POEPAINT = 5,  // Forest Temple portrait chain decided its next painting (pt, pi, ev, nx)
    FX_EV_SHIELD = 6,    // our shield deflected a hit (pos, metal)
    FX_EV_SILVER = 7,    // we picked up a silver rupee (home pos): peers tally it and remove theirs
    FX_EV_BOTTLEDROP = 8, // we emptied a bottle: fish / blue fire / bug / fairy released (aid, pos, rot)
    FX_EV_AMYTIMER = 9,   // Amy puzzle driver: the countdown block landed, HUD timer starts (sec)
    FX_EV_BOTTLEGONE = 10, // a bottle drop we announced is gone (re-caught, swam off…): kill the copy (seq)
    FX_EV_GROTTO = 11,     // a hidden grotto (Door_Ana) was opened here (key = position)
    FX_EV_STORM = 12,      // Song of Storms: spawn the storm manager at pos on every client
    FX_EV_DOOR = 13,       // a hinged door was pushed open here (key = position, ast = swing side)
    FX_EV_ROOMTIMER = 14,  // Obj_Roomtimer started a room countdown (sec): peers adopt the same clock
    FX_EV_GTGEYE = 15,     // a Gerudo Training Ground eye target was shot (key = position)
    FX_EV_TOTEM = 16,      // a hammerable totem was toppled (key = position, rev = struck face)
    FX_EV_TIMEBLOCK = 17,  // a Song of Time block with no switch flag toggled (key = position)
    FX_EV_BIGMIRROR = 18,  // Spirit Temple: the upstairs player's cobra-solved bits (sv)
    FX_EV_GERUDOKO = 19,   // a Gerudo guard was knocked out (key = SPAWN position, they patrol)
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
    // One entry here becomes one Actor_Spawn in TickPuppetProjectiles: cap it so a
    // peer on a broken build cannot flood this scene's actor list.
    static constexpr size_t MAX_PUPPET_ARROWS = 16;
    if (payload.contains("projA")) {
        for (auto& e : payload["projA"]) {
            if (pp.arrows.size() >= MAX_PUPPET_ARROWS) {
                break;
            }
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

// --- Spin attack charge ----------------------------------------------------
// The charge glow / sparks / release blast are EN_M_THUNDER, spawned by the
// player's own action code and reading GET_PLAYER — never spawned for a
// puppet, so a peer's charged spin attack showed nothing. One replica per
// remote client, parented to its puppet: EnMThunder reads the puppet instead
// of the local player when it has a parent (z_en_m_thunder.c), and the puppet
// streams everything it needs (stateFlags1/2, unk_858 = charge level,
// bodyPartsPos, heldItemAction). It retires itself like the original does.
struct ChargeReplica {
    Actor* actor = NULL;
    bool wasCharging = false;
};
static std::unordered_map<uint32_t, ChargeReplica> sChargeReplicas;

void TickPuppetSpinCharge(uint32_t clientId, Actor* puppet) {
    if (!SyncEnabled() || gPlayState == NULL || puppet == NULL) {
        return;
    }
    ChargeReplica& cs = sChargeReplicas[clientId];
    if (cs.actor != NULL && cs.actor->update == NULL) {
        cs.actor = NULL;
    }
    Player* p = (Player*)puppet;
    bool charging = (p->stateFlags1 & PLAYER_STATE1_CHARGING_SPIN_ATTACK) != 0;
    // Rising edge only: the replica handles the release (blast) and its own
    // death; a stale CHARGING packet after the release must not spawn another.
    if (charging && !cs.wasCharging && cs.actor == NULL) {
        int16_t params = (int16_t)Player_GetMeleeWeaponHeld(p); // no magic cost byte: never drains the viewer
        cs.actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_M_THUNDER, p->bodyPartsPos[0].x,
                               p->bodyPartsPos[0].y, p->bodyPartsPos[0].z, 0, 0, 0, params, true);
        if (cs.actor != NULL) {
            cs.actor->parent = puppet;
        }
        ESYNC_LOG("[FxSync] puppet spin charge spawn client={} ok={}", clientId, cs.actor != NULL);
    }
    cs.wasCharging = charging;
}

// --- Mounts ----------------------------------------------------------------
// A rider's horse exists only on its own game: EN_HORSE is scene-placed and
// player-driven, the peers see the puppet sit on nothing. One replica per
// remote client, spawned with the co-op param bit (z_en_horse.c: skeleton
// only, no AI, no scene rules, no mount offer); the rider streams pose and
// gait while rideActor is set. After a dismount the replica stays idle where
// it was left, as the real horse does; a type change (Epona vs ranch horse)
// or the puppet's death retires it.
struct HorseReplica {
    Actor* actor = NULL;
    int8_t type = 0;
    int32_t retryTicks = 0;
};
static std::unordered_map<uint32_t, HorseReplica> sHorseReplicas;
extern "C" void EnHorse_CoopSetAnim(Actor* horseActor, int32_t animIdx, float frame, float speed);

void TickPuppetHorse(uint32_t clientId, Actor* puppet, bool active, int8_t type, Vec3f pos, Vec3s rot,
                     int8_t anim, float frame, float speed) {
    if (!SyncEnabled() || gPlayState == NULL || puppet == NULL) {
        return;
    }
    HorseReplica& hr = sHorseReplicas[clientId];
    if (hr.actor != NULL && hr.actor->update == NULL) {
        hr.actor = NULL;
    }
    if (!active) {
        if (hr.actor != NULL) {
            EnHorse_CoopSetAnim(hr.actor, 0 /* ENHORSE_ANIM_IDLE */, 0.0f, 1.0f);
        }
        return;
    }
    if (hr.actor != NULL && hr.type != type) {
        Actor_Kill(hr.actor);
        hr.actor = NULL;
    }
    if (hr.actor == NULL) {
        if (hr.retryTicks > 0) {
            hr.retryTicks--;
            return;
        }
        // 0x4000: co-op mount; 0x8000: HORSE_HNI (its object must be in the
        // scene, as it is wherever such a horse can be ridden); low byte 1 = plain.
        int16_t params = (int16_t)(0x4000 | (type == 1 ? 0x8000 : 0) | 1);
        hr.actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_HORSE, pos.x, pos.y, pos.z, rot.x, rot.y,
                               rot.z, params, true);
        hr.type = type;
        if (hr.actor == NULL || hr.actor->update == NULL) {
            hr.actor = NULL;
            hr.retryTicks = 60; // object missing here: don't hammer Actor_Spawn every frame
            ESYNC_LOG("[FxSync] puppet horse spawn failed client={} type={}", clientId, type);
            return;
        }
        ESYNC_LOG("[FxSync] puppet horse spawn client={} type={}", clientId, type);
    }
    hr.actor->world.pos = pos;
    hr.actor->shape.rot = rot;
    hr.actor->world.rot.y = rot.y;
    EnHorse_CoopSetAnim(hr.actor, anim, frame, speed);
    // Seat the puppet on OUR copy of the horse, the way Player_Action_8084BF1C
    // seats the rider on its own (saddle limb + the same -27): whatever the two
    // games disagree on about the horse, the rider sits where this horse's
    // saddle is drawn. riderPos is refreshed by EnHorse_PostDraw every frame.
    EnHorse* h = (EnHorse*)hr.actor;
    if ((((Player*)puppet)->stateFlags1 & PLAYER_STATE1_ON_HORSE) && h->riderPos.y != 0.0f) {
        puppet->world.pos.x = h->actor.world.pos.x + h->riderPos.x;
        puppet->world.pos.y = (h->actor.world.pos.y + h->riderPos.y) - 27.0f;
        puppet->world.pos.z = h->actor.world.pos.z + h->riderPos.z;
    }
}

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
        // A client that left the roster never gets ticked again: drop its
        // entry instead of keeping a dead slot forever.
        if (Anchor::Instance == nullptr || Anchor::Instance->clients.find(clientId) == Anchor::Instance->clients.end()) {
            sProj.erase(it);
        }
    }
    auto bit = sBoomReplicas.find(clientId);
    if (bit != sBoomReplicas.end()) {
        if (bit->second != NULL && bit->second->update != NULL) {
            Actor_Kill(bit->second);
        }
        sBoomReplicas.erase(bit);
    }
    auto cit = sChargeReplicas.find(clientId);
    if (cit != sChargeReplicas.end()) {
        if (cit->second.actor != NULL && cit->second.actor->update != NULL) {
            Actor_Kill(cit->second.actor); // its parent (the puppet) is going away
        }
        sChargeReplicas.erase(cit);
    }
    auto hit = sHorseReplicas.find(clientId);
    if (hit != sHorseReplicas.end()) {
        if (hit->second.actor != NULL && hit->second.actor->update != NULL) {
            Actor_Kill(hit->second.actor); // the rider left the scene / the game
        }
        sHorseReplicas.erase(hit);
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

// Every EN_ARROW impact effect -- the deku-seed / deku-nut stone burst, the nut's
// white flash, the arrow hit mark -- is spawned by the SHOOTER's own EnArrow_Fly.
// A peer only holds a pinned replica with its update suppressed, so it saw the
// projectile fly and then simply vanish. Announce the impact instead; the type
// travels with it so the receiver replays exactly what the shooter got.
extern "C" void FxSync_OnAmmoImpact(Actor* arrow) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || arrow == NULL || gPlayState == NULL) {
        return;
    }
    // A replica never runs its update, so it cannot reach this — but never echo.
    if (IsOwnProjReplica(arrow) || (arrow->parent != NULL && arrow->parent != &GET_PLAYER(gPlayState)->actor)) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_AMMOHIT;
    payload["pos"] = arrow->world.pos;
    payload["ap"] = (int16_t)arrow->params;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] ammo impact tx params={}", (int16_t)arrow->params);
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

// --- Bottle contents released -------------------------------------------------
// Emptying a bottle spawns the content actor (fish, blue fire, bug, fairy) from
// the player's own action function only: the peer saw the bottle tip over and
// nothing come out. Announce the spawn; the receiver spawns an independent copy
// at the same spot (these actors are short-lived and self-contained). Bugs are
// spawned as the "from grass" kind so the copy never seeks soft soil: the soil
// -> gold skulltula chain stays the dropper's, one skulltula, no double spawn.
static bool sSpawningBottleDrop = false;
// The copies we spawned for a peer's bottle, keyed back to (owner, seq) so the
// owner's FX_EV_BOTTLEGONE can retire the right one. A fish and a bug both
// offer themselves back to a bottle every frame (Actor_OfferGetItem with
// GI_MAX), so without this each peer could pocket its own copy: one released
// fish would come back as one fish per player. The replicas are for show only.
struct BottleDropOrigin {
    uint32_t clientId = 0;
    uint32_t seq = 0;
};
static std::unordered_map<Actor*, BottleDropOrigin> sBottleDrops;
// The drops WE announced: their death (re-bottled by us, swam away, faded)
// must reach the peers, or the copy stays on their floor forever.
static std::unordered_map<Actor*, uint32_t> sLocalBottleDrops;
static uint32_t sBottleDropSeq = 0;

extern "C" s32 FxSync_IsBottleDropReplica(Actor* actor) {
    return sBottleDrops.contains(actor) ? 1 : 0;
}

extern "C" void FxSync_OnBottleDrop(int16_t aid, int16_t params, Vec3f* pos, Vec3s* rot, Actor* dropped) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || pos == NULL || sSpawningBottleDrop) {
        return;
    }
    uint32_t seq = ++sBottleDropSeq;
    if (dropped != NULL) {
        sLocalBottleDrops[dropped] = seq;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_BOTTLEDROP;
    payload["aid"] = aid;
    payload["ap"] = params;
    payload["pos"] = *pos;
    payload["seq"] = seq;
    if (rot != NULL) {
        payload["rot"] = *rot;
    }
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] bottle drop tx aid={} params={} seq={}", aid, params, seq);
}

static void OnLocalBottleDropGone(Actor* actor) {
    auto it = sLocalBottleDrops.find(actor);
    if (it == sLocalBottleDrops.end()) {
        return;
    }
    uint32_t seq = it->second;
    sLocalBottleDrops.erase(it);
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_BOTTLEGONE;
    payload["seq"] = seq;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] bottle drop gone tx seq={}", seq);
}

// --- Amy puzzle countdown -------------------------------------------------------
// BgPoEvent_BlockFall starts the HUD timer (func_80088B34) the frame the type-1
// block lands; the mirror never runs BlockFall (FlagReplay follows the stream),
// so the driver hands it the seconds. Interface_Update counts it down locally.
void SendAmyTimer(int16_t seconds) {
    if (!SyncEnabled() || Anchor::Instance == nullptr) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_AMYTIMER;
    payload["sec"] = seconds;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] amy timer tx sec={}", seconds);
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

// killLive tells the two cases apart.
//
// A scene change already freed every actor in these tables during the teardown:
// the pointers are dangling and must only be dropped, never touched.
//
// A disconnect is the opposite. The scene keeps running and every replica we
// spawned is still alive, with nothing left to reap it: Anchor::Disable() only
// Actor_Kills the puppets, and DummyPlayer_Destroy -- which is what normally
// calls ReapClientProjectiles -- does not run until Actor_Delete next frame, by
// which time these tables are already empty. The arrows and the horse would
// linger forever, and the hookshot replica would keep `parent` pointing at the
// freed puppet, which ArmsHook_Draw dereferences on every draw frame.
static void Reset(bool killLive) {
    if (killLive && gPlayState != NULL) {
        // ReapClientProjectiles erases from all four per-client maps, so collect
        // the owners first rather than iterating a map it mutates. Duplicates are
        // harmless, it looks each table up by key.
        std::vector<uint32_t> owners;
        for (auto& [clientId, pp] : sProj) {
            owners.push_back(clientId);
        }
        for (auto& [clientId, boom] : sBoomReplicas) {
            owners.push_back(clientId);
        }
        for (auto& [clientId, charge] : sChargeReplicas) {
            owners.push_back(clientId);
        }
        for (auto& [clientId, horse] : sHorseReplicas) {
            owners.push_back(clientId);
        }
        for (uint32_t clientId : owners) {
            ReapClientProjectiles(clientId);
        }
        // Not covered by ReapClientProjectiles: spell replicas are keyed by the
        // actor, and MAGIC_FIRE carries a live damage collider.
        for (auto& [actor, spell] : sSpellReplicas) {
            if (actor != NULL && actor->update != NULL) {
                Actor_Kill(actor);
            }
        }
    }
    sSilverAnnounced.clear();
    sSpellReplicas.clear();
    sBottleDrops.clear();
    sLocalBottleDrops.clear();
    sChargeReplicas.clear();
    sHorseReplicas.clear();
    // Give unk_A73 back before the map goes: BorrowFiredCounter overwrote the
    // local player's "just fired" counter, and a scene change or disconnect in
    // the middle of a borrow would leave the flag armed and later write a stale
    // value into the NEW scene's Link.
    sA73Borrowed = false;
    sA73Saved = 0;
    sTorches.clear();
    sTorchByKey.clear();
    sBoomReplicas.clear();
    sProj.clear();
    sSpawningReplica = false;
}

// Key by SPAWN position instead of current position, for actors that move on
// their own and therefore stand somewhere different on each client. home.pos
// comes from the scene's actor list, so it is identical everywhere and stable
// for the actor's whole life.
static uint64_t HomeKey(const Actor* actor) {
    int16_t qx = (int16_t)lroundf(actor->home.pos.x);
    int16_t qy = (int16_t)lroundf(actor->home.pos.y);
    int16_t qz = (int16_t)lroundf(actor->home.pos.z);
    return ((uint64_t)(uint16_t)qx << 48) | ((uint64_t)(uint16_t)qy << 32) | ((uint64_t)(uint16_t)qz << 16) |
           (uint16_t)actor->id;
}

static uint64_t PosKey(const Actor* actor) {
    int16_t qx = (int16_t)lroundf(actor->world.pos.x);
    int16_t qy = (int16_t)lroundf(actor->world.pos.y);
    int16_t qz = (int16_t)lroundf(actor->world.pos.z);
    return ((uint64_t)(uint16_t)qx << 48) | ((uint64_t)(uint16_t)qy << 32) | ((uint64_t)(uint16_t)qz << 16) |
           (uint16_t)actor->id;
}

// Hidden grottos (Door_Ana). Vanilla opens one when its AC cylinder is hit by a
// bomb (or on the Song of Storms), and the opened state lives ONLY in the actor
// params bits 0x300 -- no switch flag, nothing saved. A peer therefore cannot
// learn about it two ways at once: BombSync neuters every replica collider (no
// double damage, no chain detonation), so the peer's bomb never hits anything,
// and there is no flag for the flag sync to carry. Its copy stayed shut for good.
// Announce it by position; scene-placed, so the key matches on every client.
// Song of Storms. The whole effect hangs off EN_OKARINA_EFFECT, which Oceff_Storm
// spawns for the player who played the song -- so it exists on that client only.
// It owns env flag 5 for its 20-second life, and four actors read that flag:
// the windmill (Bg_Relay_Objects), the Colossus oasis (Bg_Spot11_Oasis), the
// storm-openable grottos (Door_Ana) and the bean sprout watering (Obj_Bean).
// None of them fired on the peer. Replicate the manager itself rather than any
// one consequence, and all four follow, along with the rain, sky and lightning.
// Hinged doors. EnDoor_Idle only swings when playerIsOpening is set, and the only
// thing that sets it is the LOCAL Link's own door code (z_player.c). So a peer saw
// the puppet walk through a door that never moved. EnDoor_Open touches nothing but
// the door -- animation, sfx, bubbles -- so replaying it on the peer is free.
// animStyle travels because it encodes which side the opener pushed from and their
// age; picking it locally would swing the wrong leaf.
extern "C" void FxSync_OnDoorOpen(Actor* doorActor, uint8_t animStyle) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || doorActor == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_DOOR;
    payload["key"] = PosKey(doorActor);
    payload["ast"] = animStyle;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] door open tx key={:#x} style={}", PosKey(doorActor), animStyle);
}

extern "C" void FxSync_OnStormSong(Actor* okarinaEffect) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || okarinaEffect == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_STORM;
    payload["pos"] = okarinaEffect->world.pos;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] storm song tx");
}

// Obj_Roomtimer (Gerudo Training Ground, Shadow Temple): each client runs its
// own copy, so each starts its own countdown the frame ITS player walks in.
// Two players entering seconds apart get two different clocks, and the loser's
// Play_TriggerVoidOut only ejects whoever ran out first — the other keeps
// playing a challenge that, on his screen, is still running. Announce the
// countdown so whoever enters second adopts the clock already ticking instead
// of restarting it. The overlay side refuses to re-arm a live timer, so this
// cannot ping-pong.
extern "C" void FxSync_OnRoomTimerStart(int16_t seconds) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_ROOMTIMER;
    payload["sec"] = seconds;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] room timer tx sec={}", seconds);
}

// Gerudo Training Ground eye targets: the lit tally is a file-scope counter in
// the overlay, fed only by the local Link's arrows. Split the four eyes between
// two players and neither side ever reaches four. Announce our hits so the peer
// lights the matching eye and counts it too.
extern "C" void FxSync_OnGtgEyeLit(Actor* eye) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || eye == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_GTGEYE;
    payload["key"] = PosKey(eye);
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] gtg eye tx key={:#x}", PosKey(eye));
}

// Hammerable totems (Fire Temple, Gerudo Training Ground). Their switch flag is
// (params >> 8) & 0xFF and the GTG placements all carry 0xFF, outside the valid
// 0x00-0x3F switch range, so nothing replicable ever leaves the actor. A totem
// smashed by one player therefore stays standing AND SOLID on the other screen,
// blocking the way. Announce the break, carrying which face was struck so the
// copy topples the same way.
extern "C" void FxSync_OnHammerTotemBreak(Actor* totem, u8 reverse) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || totem == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_TOTEM;
    payload["key"] = PosKey(totem);
    payload["rev"] = reverse;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] hammer totem tx key={:#x} rev={}", PosKey(totem), reverse);
}

// Song of Time blocks whose unk_177 is 0 (every Gerudo Training Ground one)
// flip with `params ^= 0x8000` and no switch flag at all, and the song is read
// off the singer's own msgCtx — so only the singer's copy moves. The other
// player then walks into a block that is not there, or through one that is.
extern "C" void FxSync_OnTimeBlockToggle(Actor* block) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || block == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_TIMEBLOCK;
    payload["key"] = PosKey(block);
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] time block tx key={:#x}", PosKey(block));
}

// Spirit Temple big mirror. Its two cobra-solved bits are computed only inside
// the branch that needs the cobras to exist, and they exist only for a player
// whose own room is one of the two top ones. The player standing in the statue
// room below — the other half of a puzzle explicitly built for two vantage
// points — had both bits stuck at zero and never saw the light arrive.
// Gerudo guards. ACTORCAT_NPC, so outside EnemySync: every client walks its own
// copy against its own Link. Right for detection — each player must be spotted
// from where they actually stand — but the knockout is a fact about the world,
// and without this one player clubs a guard, thinks the way is clear, and the
// other is seized by a guard who never went down on his screen. Keyed by SPAWN
// position: the guards patrol, so their current position differs per client.
extern "C" void FxSync_OnGerudoGuardKO(Actor* guard) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || guard == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_GERUDOKO;
    payload["key"] = HomeKey(guard);
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] gerudo guard KO tx key={:#x}", HomeKey(guard));
}

extern "C" void FxSync_OnBigMirrorSolved(Actor* mirror, u8 solvedBits) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || mirror == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_BIGMIRROR;
    payload["sv"] = solvedBits;
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] big mirror tx solved={:#x}", solvedBits);
}

extern "C" void FxSync_OnGrottoOpen(Actor* actor) {
    if (!FxSync::SyncEnabled() || Anchor::Instance == nullptr || actor == NULL || gPlayState == NULL) {
        return;
    }
    nlohmann::json payload;
    payload["kind"] = FX_EV_GROTTO;
    payload["key"] = PosKey(actor);
    Anchor::Instance->SendPacket_FxEvent(payload);
    ESYNC_LOG("[FxSync] grotto open tx key={:#x}", PosKey(actor));
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

        if (kind == FX_EV_AMMOHIT) {
            Vec3f pos = payload.at("pos").get<Vec3f>();
            int16_t ap = payload.value("ap", (int16_t)ARROW_NUT);
            if (ap >= ARROW_SEED) {
                // Slingshot seed and deku nut: the same stone burst, different sfx.
                if (ap == ARROW_NUT &&
                    // The white flash fills the whole screen — only take it when
                    // the burst is actually near us, like the thrower gets it.
                    Math_Vec3f_DistXYZ(&GET_PLAYER(gPlayState)->actor.world.pos, &pos) < 600.0f) {
                    iREG(50) = -1; // full-screen white flash (TransitionFade, z_fbdemo_fade.c)
                }
                EffectSsStone1_Spawn(gPlayState, &pos, 0);
                SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 20,
                                                   (ap == ARROW_NUT) ? NA_SE_IT_DEKU : NA_SE_IT_SLING_REFLECT);
            } else {
                EffectSsHitMark_SpawnCustomScale(gPlayState, 0, 150, &pos);
            }
            ESYNC_LOG("[FxSync] ammo impact rx params={}", ap);
            return;
        }

        if (kind == FX_EV_BOTTLEDROP) {
            int16_t aid = payload.at("aid").get<int16_t>();
            int16_t ap = payload.value("ap", (int16_t)0);
            Vec3f pos = payload.at("pos").get<Vec3f>();
            Vec3s rot = payload.value("rot", Vec3s{ 0, 0, 0 });
            if (aid == ACTOR_EN_INSECT) {
                ap = 1; // INSECT_TYPE_SPAWNED: wanders and fades, never hunts soft soil
            } else if (aid != ACTOR_EN_FISH && aid != ACTOR_EN_ICE_HONO && aid != ACTOR_EN_ELF) {
                return;
            }
            uint32_t seq = payload.value("seq", (uint32_t)0);
            sSpawningBottleDrop = true;
            Actor* copy = Actor_Spawn(&gPlayState->actorCtx, gPlayState, aid, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z,
                                      ap, true);
            sSpawningBottleDrop = false;
            if (copy != NULL && aid != ACTOR_EN_ELF) {
                sBottleDrops[copy] = { senderClientId, seq }; // never re-bottleable: it is the peer's item
            }
            ESYNC_LOG("[FxSync] bottle drop rx aid={} params={} seq={} ok={}", aid, ap, seq, copy != NULL);
            return;
        }

        if (kind == FX_EV_BOTTLEGONE) {
            // The owner's drop is gone (it re-bottled it, it swam off / faded):
            // retire our copy, otherwise it stays on the floor for good.
            uint32_t seq = payload.value("seq", (uint32_t)0);
            for (auto it = sBottleDrops.begin(); it != sBottleDrops.end(); ++it) {
                if (it->second.clientId == senderClientId && it->second.seq == seq) {
                    if (it->first->update != NULL) {
                        Actor_Kill(it->first);
                    }
                    sBottleDrops.erase(it);
                    break;
                }
            }
            ESYNC_LOG("[FxSync] bottle drop gone rx client={} seq={}", senderClientId, seq);
            return;
        }

        if (kind == FX_EV_AMYTIMER) {
            int16_t sec = payload.value("sec", (int16_t)0);
            if (sec > 0) {
                func_80088B34(sec); // same HUD timer the driver's BlockFall started
            } else {
                gSaveContext.timerState = 0xA;
            }
            ESYNC_LOG("[FxSync] amy timer rx sec={}", sec);
            return;
        }

        if (kind == FX_EV_ROOMTIMER) {
            int16_t sec = payload.value("sec", (int16_t)0);
            // Only adopt the peer's clock if we have none running: our own copy
            // of the timer actor may not have reached its start yet, and a
            // second announcement must never reset a countdown in progress.
            if (sec > 0 && gSaveContext.timerSeconds <= 0) {
                func_80088B34(sec);
            }
            ESYNC_LOG("[FxSync] room timer rx sec={} adopted={}", sec, gSaveContext.timerSeconds <= 0);
            return;
        }

        if (kind == FX_EV_GERUDOKO) {
            uint64_t key = payload.at("key").get<uint64_t>();
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_EN_GE2 || a->update == NULL || HomeKey(a) != key) {
                    continue;
                }
                EnGe2_KnockOutFromPeer(a, gPlayState);
                break;
            }
            ESYNC_LOG("[FxSync] gerudo guard KO rx key={:#x}", key);
            return;
        }

        if (kind == FX_EV_BIGMIRROR) {
            uint8_t solved = payload.value("sv", (uint8_t)0);
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_BG_JYA_BIGMIRROR || a->update == NULL) {
                    continue;
                }
                BgJyaBigmirror_ApplySolvedFromPeer(a, solved);
                break;
            }
            ESYNC_LOG("[FxSync] big mirror rx solved={:#x}", solved);
            return;
        }

        if (kind == FX_EV_TIMEBLOCK) {
            uint64_t key = payload.at("key").get<uint64_t>();
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_OBJ_TIMEBLOCK || a->update == NULL || PosKey(a) != key) {
                    continue;
                }
                ObjTimeblock_ToggleFromPeer(a, gPlayState);
                break;
            }
            ESYNC_LOG("[FxSync] time block rx key={:#x}", key);
            return;
        }

        if (kind == FX_EV_TOTEM) {
            uint64_t key = payload.at("key").get<uint64_t>();
            uint8_t rev = payload.value("rev", (uint8_t)0);
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_BG_HIDAN_DALM || a->update == NULL || PosKey(a) != key) {
                    continue;
                }
                BgHidanDalm_BreakFromPeer(a, gPlayState, rev);
                break;
            }
            ESYNC_LOG("[FxSync] hammer totem rx key={:#x}", key);
            return;
        }

        if (kind == FX_EV_GTGEYE) {
            uint64_t key = payload.at("key").get<uint64_t>();
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_BG_MENKURI_EYE || a->update == NULL || PosKey(a) != key) {
                    continue;
                }
                BgMenkuriEye_LightFromPeer(a, gPlayState);
                break;
            }
            ESYNC_LOG("[FxSync] gtg eye rx key={:#x}", key);
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

        if (kind == FX_EV_DOOR) {
            uint64_t key = payload.at("key").get<uint64_t>();
            uint8_t style = payload.value("ast", (uint8_t)0);
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_DOOR].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_EN_DOOR || a->update == NULL || PosKey(a) != key) {
                    continue;
                }
                EnDoor* door = (EnDoor*)a;
                if (door->playerIsOpening == 0) {
                    // Straight into the vanilla trigger: its own EnDoor_Idle picks this
                    // up next frame and runs the real open sequence. Never routed through
                    // the Player door code, so this cannot echo back.
                    door->animStyle = style;
                    door->playerIsOpening = 1;
                }
                ESYNC_LOG("[FxSync] door open rx key={:#x}", key);
                break;
            }
            return;
        }

        if (kind == FX_EV_STORM) {
            Vec3f pos = payload.at("pos").get<Vec3f>();
            // Spawned straight, not through Oceff_Storm: the peer gets the storm and
            // its env flag without the caster-only swirl or the magic cost. Its own
            // Init kills it if a storm is already running here, so no doubling.
            Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_OKARINA_EFFECT, pos.x, pos.y, pos.z, 0, 0, 0, 1,
                        true);
            ESYNC_LOG("[FxSync] storm song rx");
            return;
        }

        if (kind == FX_EV_GROTTO) {
            uint64_t key = payload.at("key").get<uint64_t>();
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; a != NULL; a = a->next) {
                if (a->id != ACTOR_DOOR_ANA || a->update == NULL || PosKey(a) != key) {
                    continue;
                }
                DoorAna* grotto = (DoorAna*)a;
                if (grotto->actionFunc != DoorAna_WaitClosed) {
                    break; // already open here (both players bombed it)
                }
                // Same order as DoorAna_WaitClosed: free the cylinder BEFORE clearing
                // the params, or Destroy frees it a second time (it gates on 0x200).
                if (a->params & 0x200) {
                    Collider_DestroyCylinder(gPlayState, &grotto->collider);
                } else {
                    a->flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED; // storm-opened path
                }
                a->params &= ~0x0300;
                grotto->actionFunc = DoorAna_WaitOpen;
                Audio_PlaySoundGeneral(NA_SE_SY_CORRECT_CHIME, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                       &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
                ESYNC_LOG("[FxSync] grotto open rx key={:#x}", key);
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
        Reset(/*killLive=*/true); // the scene keeps running: our replicas are still in it
    }

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(/*killLive=*/false); });

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
        sBottleDrops.erase(a);
        OnLocalBottleDropGone(a); // our own released fish / bug / fire: tell the peers
        for (auto& [cid, cs] : sChargeReplicas) {
            if (cs.actor == a) {
                cs.actor = NULL;
            }
        }
        for (auto& [cid, hr] : sHorseReplicas) {
            if (hr.actor == a) {
                hr.actor = NULL;
            }
        }
        sSilverAnnounced.erase(a);
        // A dying puppet takes its projectile replicas with it: that reap lives
        // in DummyPlayer_Destroy, keyed by the puppet's own client id (matching
        // `client.player == a` here never fired, see there).
        for (auto it = sBoomReplicas.begin(); it != sBoomReplicas.end();) {
            it = (it->second == a) ? sBoomReplicas.erase(it) : std::next(it);
        }
        // Drop the pointers to a dying replica. Impact effects are NOT announced
        // from here: EnArrow_Fly announces them itself (FxSync_OnAmmoImpact),
        // where the impact point and the projectile type are both known and a
        // real hit is distinguishable from a room unload.
        if (a->id == ACTOR_EN_ARROW || a->id == ACTOR_ARMS_HOOK) {
            for (auto& [cid, pp] : sProj) {
                if (pp.hookReplica == a) {
                    pp.hookReplica = NULL;
                }
                for (auto& r : pp.arrowReplicas) {
                    if (r.actor == a) {
                        r.actor = NULL;
                    }
                }
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
