#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Ganondrof/z_boss_ganondrof.h"
#include "src/overlays/actors/ovl_En_fHG/z_en_fhg.h"
#include "src/overlays/actors/ovl_En_Fhg_Fire/z_en_fhg_fire.h"
extern PlayState* gPlayState;
// File-scope (non-static) in the overlays; not exported by their headers.
void BossGanondrof_Intro(BossGanondrof* boss, PlayState* play);
void BossGanondrof_Neutral(BossGanondrof* boss, PlayState* play);
void BossGanondrof_Charge(BossGanondrof* boss, PlayState* play);
void BossGanondrof_SetupDeath(BossGanondrof* boss, PlayState* play);
void BossGanondrof_SetColliderPos(Vec3f* pos, ColliderCylinder* collider);
void BossGanondrof_CoopMirrorFx(BossGanondrof* boss, PlayState* play, u8 fxMode);
void EnfHG_Intro(EnfHG* horse, PlayState* play);
void EnfHG_Done(EnfHG* horse, PlayState* play);
void EnFhgFire_CoopMirrorFx(EnFhgFire* fire, PlayState* play);
}

/**
 * Phantom Ganon (Forest Temple): three adapters for the three actors of the
 * fight.
 *
 * BOSS_GANONDROF — the rider. Params 1 = the real boss (health 30); params
 * 10+n = a decoy rider the horse spawns for painting n (health 0, killed by its
 * own horse). Phases: PREFIGHT (BossGanondrof_Intro — the intro cutscene is the
 * horse's Update, so each client plays its own, like Gohma), PAINTINGS (flyMode
 * == GND_FLY_PAINTING: the boss rides its horse and only takes arrow hits when
 * out of a painting), FIGHT (floating: energy-ball tennis, charge, stun), and
 * DEFEATED (deathState != NOT_DEAD). On the DEFEATED edge the mirror runs
 * BossGanondrof_SetupDeath locally and is released: camera, decay, warp,
 * heart container, blue warp and clear flag then play on both machines (deduped
 * by stock Anchor sync). No ENEMY_DIED races that edge: bosses only fire
 * OnBossDefeat, and the stream is snapshotted after the actor updates.
 *
 * Extras: everything the (suppressed) mirror's Draw / collision read that the
 * Update computes — flyMode, invincibility flash, leg / arm / eye draw params,
 * the body cylinder's dims (Paintings shrinks it when the fight proper starts)
 * and colType (METAL while blocking / charging), the attention + hookshot
 * flags, the shock timer and an ambient-FX mode replayed by
 * BossGanondrof_CoopMirrorFx. The two cylinders are centred on the chest limb
 * and the spear tip (PostLimbDraw, which still runs on the mirror), so the
 * adapter places them itself (customColliderPos).
 *
 * EN_FHG — the horse (ACTORCAT_BG, force-tracked). It IS the painting-phase
 * AI, so the authority runs it and the mirror streams its Skin skeleton
 * (SkelAnime_InitSkin is captured). Extras: the fog tint it and the boss draw
 * with, its turn rotation (the boss's painting-phase draw), the hit jerk
 * (shape.yOffset), the flag the emerge portal polls, and "hidden" (vanilla
 * nulls its draw once the fight leaves the paintings). Healthless: the real
 * horse dies at the end of every client's own death cutscene (never
 * replicated); a decoy's horse is despawned with its rider.
 *
 * EN_FHG_FIRE — lightning strike, spear glow and painting portals are
 * fire-and-forget projectiles (announced once, local copies); their children
 * (trails, shocks, bursts) and the death warp are untracked (spawned locally by
 * their locally-run parents). The energy ball is the one gameplay object:
 * authority-run and streamed, the mirror re-submits its AC cylinder so the
 * remote player's sword / shield hits travel as hit requests and reflect the
 * authority's ball; its impact is streamed (coopKillMode) so the mirror hides
 * its copy and spawns the damaging lightning burst locally.
 *
 * Targeting: the boss and the ball read player->actor.world.pos directly, so
 * z_boss_ganondrof.c / z_en_fhg_fire.c use Anchor_NearestPlayer (local Link or
 * puppet) where vanilla used GET_PLAYER — Phantom Ganon now hovers around,
 * throws at and charges whichever player is closest.
 */

// ---------------------------------------------------------------------------
// BOSS_GANONDROF
// ---------------------------------------------------------------------------

enum PhantomGanonPhase : uint8_t {
    PG_PHASE_PREFIGHT = 0,
    PG_PHASE_PAINTINGS = 1,
    PG_PHASE_FIGHT = 2,
    PG_PHASE_DEFEATED = 3,
};

static bool PhantomGanon_IsDecoy(Actor* actor) {
    return actor->params >= GND_FAKE_BOSS;
}

static uint8_t PhantomGanon_GetPhase(Actor* actor) {
    BossGanondrof* pg = (BossGanondrof*)actor;
    if (pg->deathState != NOT_DEAD) {
        return PG_PHASE_DEFEATED;
    }
    if (pg->actionFunc == BossGanondrof_Intro) {
        return PG_PHASE_PREFIGHT;
    }
    return pg->flyMode == GND_FLY_PAINTING ? PG_PHASE_PAINTINGS : PG_PHASE_FIGHT;
}

static void PhantomGanon_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossGanondrof* pg = (BossGanondrof*)actor;
    uint8_t fxMode = 0;
    if (pg->actionFunc == BossGanondrof_Neutral) {
        fxMode = 1;
    } else if (pg->actionFunc == BossGanondrof_Charge) {
        fxMode = 2;
    }
    x["fly"] = pg->flyMode;
    x["inv"] = pg->work[GND_INVINC_TIMER];
    x["var"] = pg->work[GND_VARIANCE_TIMER];
    x["shk"] = pg->shockTimer;
    x["fx"] = fxMode;
    x["leg"] = { pg->legRotY, pg->legRotZ, pg->legSplitY };
    x["arm"] = { pg->armRotY, pg->armRotZ };
    x["eye"] = { pg->fwork[GND_EYE_BRIGHTNESS], pg->fwork[GND_EYE_ALPHA] };
    x["col"] = { pg->colliderBody.dim.radius, pg->colliderBody.dim.height, pg->colliderBody.dim.yShift,
                 (int)pg->colliderBody.base.colType };
    x["nav"] = actor->naviEnemyId;
}

static void PhantomGanon_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossGanondrof* pg = (BossGanondrof*)actor;
    try {
        if (x.contains("fly")) pg->flyMode = x["fly"].get<uint8_t>();
        if (x.contains("inv")) pg->work[GND_INVINC_TIMER] = x["inv"].get<int16_t>();
        if (x.contains("var")) pg->work[GND_VARIANCE_TIMER] = x["var"].get<int16_t>();
        if (x.contains("shk")) pg->shockTimer = x["shk"].get<uint8_t>();
        if (x.contains("leg") && x["leg"].size() == 3) {
            pg->legRotY = x["leg"][0].get<float>();
            pg->legRotZ = x["leg"][1].get<float>();
            pg->legSplitY = x["leg"][2].get<float>();
        }
        if (x.contains("arm") && x["arm"].size() == 2) {
            pg->armRotY = x["arm"][0].get<float>();
            pg->armRotZ = x["arm"][1].get<float>();
        }
        if (x.contains("eye") && x["eye"].size() == 2) {
            pg->fwork[GND_EYE_BRIGHTNESS] = x["eye"][0].get<float>();
            pg->fwork[GND_EYE_ALPHA] = x["eye"][1].get<float>();
        }
        if (x.contains("col") && x["col"].size() == 4) {
            pg->colliderBody.dim.radius = x["col"][0].get<int16_t>();
            pg->colliderBody.dim.height = x["col"][1].get<int16_t>();
            pg->colliderBody.dim.yShift = x["col"][2].get<int16_t>();
            pg->colliderBody.base.colType = (uint8_t)x["col"][3].get<int>();
        }
        if (x.contains("nav")) actor->naviEnemyId = x["nav"].get<int8_t>();

        // Cylinders ride the chest limb and the spear tip (Update does this right
        // after the action function; PostLimbDraw refreshed both last frame).
        BossGanondrof_SetColliderPos(&pg->targetPos, &pg->colliderBody);
        BossGanondrof_SetColliderPos(&pg->spearTip, &pg->colliderSpear);
        // The real boss's point light follows the spear tip (Update).
        if (actor->params == GND_REAL_BOSS) {
            Lights_PointNoGlowSetInfo(&pg->lightInfo, pg->spearTip.x, pg->spearTip.y, pg->spearTip.z, 255, 255, 255,
                                      200);
        }
        // Ambient particles / hum: once per streamed frame (the cached extras are
        // re-applied every game frame — see KingDodongoAdapter).
        if (gPlayState != NULL && pg->coopMirrorVar != pg->work[GND_VARIANCE_TIMER]) {
            pg->coopMirrorVar = pg->work[GND_VARIANCE_TIMER];
            BossGanondrof_CoopMirrorFx(pg, gPlayState, x.contains("fx") ? x["fx"].get<uint8_t>() : 0);
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[PhantomGanonSync] extras parse error: {}", ex.what());
    }
}

static bool PhantomGanon_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    ESYNC_LOG("[PhantomGanonSync] phase {}->{} params={}", fromPhase, toPhase, actor->params);
    if (toPhase == PG_PHASE_DEFEATED && !PhantomGanon_IsDecoy(actor)) {
        // Final streamed pose applied; the death cutscene now runs locally
        // (camera, decay, death warp, heart container, blue warp, clear flag).
        actor->colChkInfo.health = 0;
        actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
        BossGanondrof_SetupDeath((BossGanondrof*)actor, gPlayState);
        Enemy_StartFinishingBlow(gPlayState, actor);
        ESYNC_LOG("[PhantomGanonSync] defeat handoff (SetupDeath called locally)");
        return true;
    }
    return false;
}

static bool PhantomGanon_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossGanondrof* pg = (BossGanondrof*)actor;
    // The intro cutscene (driven by the horse) runs locally on every client;
    // mirror once both the authority and we are past it.
    return streamedPhase != PG_PHASE_PREFIGHT && pg->actionFunc != BossGanondrof_Intro && pg->deathState == NOT_DEAD;
}

static bool PhantomGanon_ReplicateKill(Actor* actor) {
    return PhantomGanon_IsDecoy(actor); // the real boss's final Actor_Kill ends every client's own cutscene
}

// ---------------------------------------------------------------------------
// EN_FHG (the horse)
// ---------------------------------------------------------------------------

enum PhantomHorsePhase : uint8_t {
    FHG_PHASE_INTRO = 0,
    FHG_PHASE_FIGHT = 1,
};

static bool PhantomHorse_Always(Actor* actor) {
    return true;
}

static uint8_t PhantomHorse_GetPhase(Actor* actor) {
    EnfHG* horse = (EnfHG*)actor;
    return horse->actionFunc == EnfHG_Intro ? FHG_PHASE_INTRO : FHG_PHASE_FIGHT;
}

static void PhantomHorse_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnfHG* horse = (EnfHG*)actor;
    x["wc"] = { horse->warpColorFilterR, horse->warpColorFilterG, horse->warpColorFilterB, horse->warpColorFilterUnk1,
                horse->warpColorFilterUnk2 };
    x["turn"] = horse->turnRot;
    x["kw"] = horse->fhgFireKillWarp;
    x["inp"] = horse->bossGndInPainting;
    x["hid"] = actor->draw == NULL;
}

static void PhantomHorse_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    EnfHG* horse = (EnfHG*)actor;
    try {
        if (x.contains("wc") && x["wc"].size() == 5) {
            horse->warpColorFilterR = x["wc"][0].get<float>();
            horse->warpColorFilterG = x["wc"][1].get<float>();
            horse->warpColorFilterB = x["wc"][2].get<float>();
            horse->warpColorFilterUnk1 = x["wc"][3].get<float>();
            horse->warpColorFilterUnk2 = x["wc"][4].get<float>();
        }
        if (x.contains("turn")) horse->turnRot = x["turn"].get<int16_t>();
        if (x.contains("kw")) horse->fhgFireKillWarp = x["kw"].get<uint8_t>();
        if (x.contains("inp")) horse->bossGndInPainting = x["inp"].get<uint8_t>();
        if (JsonBool(x, "hid") && actor->draw != NULL) {
            // Vanilla EnfHG_Retreat: once the fight leaves the paintings the horse
            // is done for good (draw = NULL, EnfHG_Done). The mirror step lets a
            // draw-less actor run locally, so park the action too.
            actor->draw = NULL;
            horse->actionFunc = EnfHG_Done;
            ESYNC_LOG("[PhantomGanonSync] horse hidden (fight left the paintings)");
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[PhantomGanonSync] horse extras parse error: {}", ex.what());
    }
}

static bool PhantomHorse_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    EnfHG* horse = (EnfHG*)actor;
    // Local intro first; and once our own boss's death cutscene asked the horse
    // to go, let the local update honour it.
    return streamedPhase == FHG_PHASE_FIGHT && horse->actionFunc != EnfHG_Intro && !horse->killActor;
}

static bool PhantomHorse_ReplicateKill(Actor* actor) {
    return actor->params != GND_REAL_BOSS; // decoy horses die with their decoy
}

// ---------------------------------------------------------------------------
// EN_FHG_FIRE (the energy ball; everything else is fire-and-forget or untracked)
// ---------------------------------------------------------------------------

static bool PhantomFire_Always(Actor* actor) {
    return true;
}

static void PhantomFire_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnFhgFire* fire = (EnFhgFire*)actor;
    x["fm"] = fire->work[FHGFIRE_FIRE_MODE];
    x["km"] = fire->coopKillMode;
    x["var"] = fire->work[FHGFIRE_VARIANCE_TIMER];
    x["al"] = fire->fwork[FHGFIRE_ALPHA];
}

static void PhantomFire_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    EnFhgFire* fire = (EnFhgFire*)actor;
    try {
        if (x.contains("fm")) fire->work[FHGFIRE_FIRE_MODE] = x["fm"].get<int16_t>();
        if (x.contains("km")) fire->coopKillMode = x["km"].get<uint8_t>();
        if (x.contains("var")) fire->work[FHGFIRE_VARIANCE_TIMER] = x["var"].get<int16_t>();
        if (x.contains("al")) fire->fwork[FHGFIRE_ALPHA] = x["al"].get<float>();
        if (fire->coopKillMode != 0) {
            // Impacted on the authority (draw nulled there, copy killed 30 frames
            // later): hide ours without touching draw, which would hand the copy
            // back to its local update.
            Actor_SetScale(actor, 0.0f);
        }
        if (gPlayState != NULL &&
            (fire->coopMirrorVar != fire->work[FHGFIRE_VARIANCE_TIMER] || fire->coopMirrorKillMode != fire->coopKillMode)) {
            fire->coopMirrorVar = fire->work[FHGFIRE_VARIANCE_TIMER];
            EnFhgFire_CoopMirrorFx(fire, gPlayState);
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[PhantomGanonSync] fire extras parse error: {}", ex.what());
    }
}

static bool PhantomFire_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    return actor->params == FHGFIRE_ENERGY_BALL;
}

void RegisterPhantomGanonAdapters() {
    {
        ActorSyncAdapter adapter;
        adapter.SerializeExtras = PhantomGanon_SerializeExtras;
        adapter.DeserializeExtras = PhantomGanon_DeserializeExtras;
        adapter.GetPhase = PhantomGanon_GetPhase;
        adapter.OnPhaseChange = PhantomGanon_OnPhaseChange;
        adapter.ShouldMirror = PhantomGanon_ShouldMirror;
        adapter.customColliderPos = true;
        adapter.IsHealthless = PhantomGanon_IsDecoy;
        adapter.ReplicateKill = PhantomGanon_ReplicateKill;
        adapter.ownsSfx = true; // its mirror tick replays this actor's sounds itself
        // The rider nulls its own draw for the whole painting phase; without this
        // the generic draw-less bail hands the mirror back to its local AI right
        // then, and that copy runs the fight a second time.
        adapter.mirrorWhileDrawless = true;
        EnemySync::RegisterAdapter(ACTOR_BOSS_GANONDROF, adapter);
    }
    {
        ActorSyncAdapter adapter;
        adapter.SerializeExtras = PhantomHorse_SerializeExtras;
        adapter.DeserializeExtras = PhantomHorse_DeserializeExtras;
        adapter.GetPhase = PhantomHorse_GetPhase;
        adapter.ShouldMirror = PhantomHorse_ShouldMirror;
        adapter.IsHealthless = PhantomHorse_Always;
        adapter.ReplicateKill = PhantomHorse_ReplicateKill;
        adapter.ownsSfx = true; // its mirror tick replays this actor's sounds itself
        // Same for the horse -- and it is the horse AI that spawns the decoy
        // rider/horse pairs, so a released mirror doubled every one of them.
        adapter.mirrorWhileDrawless = true;
        EnemySync::RegisterAdapter(ACTOR_EN_FHG, adapter);
    }
    {
        ActorSyncAdapter adapter;
        adapter.SerializeExtras = PhantomFire_SerializeExtras;
        adapter.DeserializeExtras = PhantomFire_DeserializeExtras;
        adapter.ShouldMirror = PhantomFire_ShouldMirror;
        adapter.IsHealthless = PhantomFire_Always;
        adapter.ReplicateKill = PhantomFire_Always;
        adapter.ownsSfx = true; // its mirror tick replays this actor's sounds itself
        EnemySync::RegisterAdapter(ACTOR_EN_FHG_FIRE, adapter);
    }
}
