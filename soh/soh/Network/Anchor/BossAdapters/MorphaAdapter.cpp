#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Mo/z_boss_mo.h"
extern PlayState* gPlayState;
// File-scope (non-static) in z_boss_mo.c; not exported by the header.
BossMo* BossMo_CoopCore(void);
BossMo* BossMo_CoopTent1(void);
BossMo* BossMo_CoopTent2(void);
void BossMo_CoopRegisterTent2(BossMo* tent);
void BossMo_CoopSetupDeath(BossMo* core, PlayState* play);
void BossMo_CoopMirrorCoreTick(BossMo* core, PlayState* play);
void BossMo_CoopMirrorTentTick(BossMo* tent, PlayState* play);
}

/**
 * Morpha (Water Temple): one actor id, three instances. The core (room-listed,
 * params -1 rewritten to 0 in Init, health 20 — the fight's only health) and
 * the tentacles (params 100): tentacle 1 is spawned in the core's Init
 * (occurrence key on both sides), tentacle 2 by tentacle 1's Retreat after the
 * third core hit (dynamic key, announced; the mirror wires it into the
 * overlay's statics through BossMo_CoopRegisterTent2, and the Destroy hook
 * clears the static when the despawn lands).
 *
 * The intro is the core's Update (csState machine, local on every client like
 * Gohma); the death cutscene likewise, from the DEFEATED edge: the mirror runs
 * BossMo_CoopSetupDeath (the lethal branch of the core's collision check minus
 * the damage) and releases all three copies.
 *
 * Core: streamed pose + work[] + the computed water level (the room's water
 * box, written by UpdateCore — Link swims in it, so the mirror must apply it
 * every frame), the attention / hookshot flags, the tentacle-ride position;
 * BossMo_CoopMirrorCoreTick replays the cosmetics, the effect pool and the
 * sound events. Its AC cylinder is re-submitted on the mirror, so the remote
 * player's slashes and hookshot travel as hit requests. A remote hookshot
 * cannot physically reel our copy in (the pull runs on the requester's
 * machine, under the stream), so the core's collision check launches the core
 * at the requester instead — the fake attacker's parent is their puppet.
 *
 * Tentacles: the per-segment rotation / stretch is AI-driven and cannot be
 * recomputed on the mirror, so it streams (41 x/z pairs + 41 stretch values,
 * ~600 B per tentacle per frame); width ring, scale, ripple, tip position and
 * effects are recomputed locally (BossMo_CoopMirrorTentTick). The grab targets
 * the nearest player (local Link or puppet); the grabbed client id streams and
 * that client's machine runs the hold on its own Link (grab action, position
 * lock, shake camera, damage ticks, mash escape), while the authority keeps the
 * puppet's parent link and lets go on its timer.
 */

enum MorphaPhase : uint8_t {
    MO_PHASE_PREFIGHT = 0,
    MO_PHASE_FIGHT = 1,
    MO_PHASE_DEFEATED = 2,
};

static constexpr u32 MO_STREAMED_FLAGS =
    ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOOKSHOT_PULLS_ACTOR | ACTOR_FLAG_SFX_FOR_PLAYER_BODY_HIT;

// The death-cutscene / intro enum lives in z_boss_mo.c; only the two edges matter here.
static constexpr int16_t MO_CS_BATTLE = 0;
static constexpr int16_t MO_CS_DEATH_START = 100;
static constexpr int16_t MO_TENT_STATE_DEATH_START = 200;

static bool Morpha_IsTentacle(Actor* actor) {
    return actor->params >= BOSSMO_TENTACLE;
}

static bool Morpha_CoreInBattle() {
    BossMo* core = BossMo_CoopCore();
    return core != NULL && core->actor.update != NULL && core->csState == MO_CS_BATTLE;
}

static uint8_t Morpha_GetPhase(Actor* actor) {
    BossMo* mo = (BossMo*)actor;
    if (Morpha_IsTentacle(actor)) {
        if (mo->work[MO_TENT_ACTION_STATE] >= MO_TENT_STATE_DEATH_START) {
            return MO_PHASE_DEFEATED;
        }
        BossMo* core = BossMo_CoopCore();
        if (core != NULL && core->csState != MO_CS_BATTLE && core->csState < MO_CS_DEATH_START) {
            return MO_PHASE_PREFIGHT;
        }
        return MO_PHASE_FIGHT;
    }
    if (mo->csState >= MO_CS_DEATH_START) {
        return MO_PHASE_DEFEATED;
    }
    return mo->csState != MO_CS_BATTLE ? MO_PHASE_PREFIGHT : MO_PHASE_FIGHT;
}

static void Morpha_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossMo* mo = (BossMo*)actor;
    std::vector<int> w(mo->work, mo->work + MO_SHORT_MAX);
    x["w"] = w;
    x["t0"] = mo->timers[0];
    x["flg"] = (uint32_t)(actor->flags & MO_STREAMED_FLAGS);
    x["sfx"] = { mo->coopSfxSeq, mo->coopSfxId };
    if (Morpha_IsTentacle(actor)) {
        std::vector<int> rot;
        std::vector<int> str;
        rot.reserve(82);
        str.reserve(41);
        for (int i = 0; i < 41; i++) {
            rot.push_back(mo->tentRot[i].x);
            rot.push_back(mo->tentRot[i].z);
            str.push_back((int)(mo->tentStretch[i].y * 100.0f));
        }
        x["rot"] = rot;
        x["str"] = str;
        x["ring"] = { mo->widthIndex, mo->pulsePhase };
        x["cut"] = { mo->cutIndex, mo->meltIndex, mo->noBubbles, mo->linkToLeft };
        x["f"] = { mo->tentPulse, mo->cutScale, mo->baseAlpha, mo->tentRippleSize, mo->fwork[MO_TENT_MAX_STRETCH],
                   mo->waterTexAlpha };
        x["bbt"] = mo->baseBubblesTimer;
        x["draw"] = mo->drawActor;
        x["grab"] = mo->coopGrabClientId;
    } else {
        x["wl"] = (int)gPlayState->colCtx.colHeader->waterBoxes[0].ySurface;
        x["wlv"] = mo->waterLevel;
        x["cf"] = { mo->baseAlpha, mo->waterTex1x, mo->waterTex1y, mo->waterTex2y };
        x["draw"] = mo->drawActor;
        x["hc"] = mo->hitCount;
    }
}

static void Morpha_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossMo* mo = (BossMo*)actor;
    try {
        if (x.contains("w") && x["w"].size() == MO_SHORT_MAX) {
            for (int i = 0; i < MO_SHORT_MAX; i++) {
                mo->work[i] = (int16_t)x["w"][i].get<int>();
            }
        }
        if (x.contains("t0")) mo->timers[0] = x["t0"].get<int16_t>();
        if (x.contains("flg")) {
            actor->flags = (actor->flags & ~MO_STREAMED_FLAGS) | (x["flg"].get<uint32_t>() & MO_STREAMED_FLAGS);
        }
        if (x.contains("sfx") && x["sfx"].size() == 2) {
            mo->coopSfxSeq = x["sfx"][0].get<uint8_t>();
            mo->coopSfxId = x["sfx"][1].get<uint16_t>();
        }
        if (x.contains("draw")) mo->drawActor = x["draw"].get<bool>() ? 1 : 0;

        if (Morpha_IsTentacle(actor)) {
            if (x.contains("rot") && x["rot"].size() == 82 && x.contains("str") && x["str"].size() == 41) {
                for (int i = 0; i < 41; i++) {
                    mo->tentRot[i].x = (int16_t)x["rot"][i * 2].get<int>();
                    mo->tentRot[i].z = (int16_t)x["rot"][i * 2 + 1].get<int>();
                    mo->tentStretch[i].y = x["str"][i].get<int>() / 100.0f;
                }
            }
            if (x.contains("ring") && x["ring"].size() == 2) {
                mo->widthIndex = x["ring"][0].get<int16_t>();
                mo->pulsePhase = x["ring"][1].get<int16_t>();
            }
            if (x.contains("cut") && x["cut"].size() == 4) {
                mo->cutIndex = x["cut"][0].get<int16_t>();
                mo->meltIndex = x["cut"][1].get<int16_t>();
                mo->noBubbles = x["cut"][2].get<int16_t>();
                mo->linkToLeft = x["cut"][3].get<int16_t>();
            }
            if (x.contains("f") && x["f"].size() == 6) {
                mo->tentPulse = x["f"][0].get<float>();
                mo->cutScale = x["f"][1].get<float>();
                mo->baseAlpha = x["f"][2].get<float>();
                mo->tentRippleSize = x["f"][3].get<float>();
                mo->fwork[MO_TENT_MAX_STRETCH] = x["f"][4].get<float>();
                mo->waterTexAlpha = x["f"][5].get<float>();
            }
            if (x.contains("bbt")) mo->baseBubblesTimer = x["bbt"].get<int16_t>();
            if (x.contains("grab")) mo->coopGrabClientId = x["grab"].get<uint32_t>();
            // A second tentacle materialised from ENEMY_SPAWN is nobody's child:
            // give it the overlay's static slot and the otherTent links.
            if (actor->parent == NULL && BossMo_CoopTent1() != mo && BossMo_CoopTent2() == NULL) {
                BossMo_CoopRegisterTent2(mo);
                ESYNC_LOG("[MorphaSync] second tentacle registered");
            }
            if (gPlayState != NULL && mo->coopMirrorVar != mo->work[MO_TENT_VAR_TIMER]) {
                mo->coopMirrorVar = mo->work[MO_TENT_VAR_TIMER];
                BossMo_CoopMirrorTentTick(mo, gPlayState);
            }
        } else {
            if (x.contains("wl")) mo->coopWaterLevel = (int16_t)x["wl"].get<int>();
            if (x.contains("wlv")) mo->waterLevel = x["wlv"].get<float>();
            if (x.contains("cf") && x["cf"].size() == 4) {
                mo->baseAlpha = x["cf"][0].get<float>();
                mo->waterTex1x = x["cf"][1].get<float>();
                mo->waterTex1y = x["cf"][2].get<float>();
                mo->waterTex2y = x["cf"][3].get<float>();
            }
            if (x.contains("hc")) mo->hitCount = x["hc"].get<uint8_t>();
            if (gPlayState != NULL && mo->coopMirrorVar != mo->work[MO_TENT_VAR_TIMER]) {
                mo->coopMirrorVar = mo->work[MO_TENT_VAR_TIMER];
                BossMo_CoopMirrorCoreTick(mo, gPlayState);
            }
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[MorphaSync] extras parse error: {}", ex.what());
    }
}

static bool Morpha_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    BossMo* mo = (BossMo*)actor;
    ESYNC_LOG("[MorphaSync] phase {}->{} params={}", fromPhase, toPhase, actor->params);
    if (toPhase != MO_PHASE_DEFEATED) {
        return false;
    }
    if (!Morpha_IsTentacle(actor)) {
        // Final streamed pose applied; the death cutscene (which also drives the
        // tentacles' death states) now runs locally on the core.
        actor->colChkInfo.health = 0;
        Enemy_StartFinishingBlow(gPlayState, actor);
        BossMo_CoopSetupDeath(mo, gPlayState);
        ESYNC_LOG("[MorphaSync] core defeat handoff (SetupDeath called locally)");
    }
    // Tentacles: released for the local death sequence the core drives (a
    // second tentacle is despawned by the authority's packet).
    return true;
}

static bool Morpha_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    if (streamedPhase != MO_PHASE_FIGHT) {
        return false;
    }
    if (Morpha_IsTentacle(actor)) {
        return Morpha_CoreInBattle(); // our own intro (the core's Update) first
    }
    BossMo* mo = (BossMo*)actor;
    return mo->csState == MO_CS_BATTLE;
}

static bool Morpha_ReplicateKill(Actor* actor) {
    return Morpha_IsTentacle(actor); // the second tentacle is killed by the fight; the core never is
}

void RegisterMorphaAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = Morpha_SerializeExtras;
    adapter.DeserializeExtras = Morpha_DeserializeExtras;
    adapter.GetPhase = Morpha_GetPhase;
    adapter.OnPhaseChange = Morpha_OnPhaseChange;
    adapter.ShouldMirror = Morpha_ShouldMirror;
    adapter.IsHealthless = Morpha_IsTentacle; // the core's health is the fight's health
    adapter.ReplicateKill = Morpha_ReplicateKill;
    EnemySync::RegisterAdapter(ACTOR_BOSS_MO, adapter);
}
