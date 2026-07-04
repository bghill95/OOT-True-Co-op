#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
// Pull the C++ side of global.h in under proper linkage before the extern "C"
// overlay header includes it (BossRush.cpp pattern).
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"

void BossGoma_SetupDefeated(BossGoma* thisx, PlayState* play);
extern PlayState* gPlayState;
}

/**
 * Gohma (M3 vertical slice).
 *
 * State-gated suppression: the intro cutscene IS Gohma's Update, so both
 * clients run their own intro locally (harmless — while disableGameplayLogic
 * is set, Gohma submits no colliders and applies no gameplay). Mirroring
 * engages only while the streamed phase is FIGHT and the local intro has
 * finished. On the FIGHT -> DEFEATED edge the mirror calls
 * BossGoma_SetupDefeated directly (plain un-suppression would leave a stale
 * actionFunc) and hands the defeat cutscene to the local simulation — both
 * clients get the cutscene, heart container, blue warp, and room-clear flag
 * natively (all deduped by stock Anchor sync).
 */

enum GohmaPhase : uint8_t {
    GOHMA_PHASE_PREFIGHT = 0,
    GOHMA_PHASE_FIGHT = 1,
    GOHMA_PHASE_DEFEATED = 2,
};

static uint8_t Gohma_GetPhase(Actor* actor) {
    BossGoma* goma = (BossGoma*)actor;
    if (goma->disableGameplayLogic) {
        return ((int8_t)actor->colChkInfo.health <= 0) ? GOHMA_PHASE_DEFEATED : GOHMA_PHASE_PREFIGHT;
    }
    return GOHMA_PHASE_FIGHT;
}

static void Gohma_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossGoma* g = (BossGoma*)actor;
    x["eye"] = g->eyeState;
    x["irx"] = g->eyeIrisRotX;
    x["iry"] = g->eyeIrisRotY;
    x["lidB"] = g->eyeLidBottomRotX;
    x["lidT"] = g->eyeLidTopRotX;
    x["isx"] = g->eyeIrisScaleX;
    x["isy"] = g->eyeIrisScaleY;
    x["vis"] = g->visualState;
    x["inv"] = g->invincibilityFrames;
    x["menv"] = { g->mainEnvColor[0], g->mainEnvColor[1], g->mainEnvColor[2] };
    x["eenv"] = { g->eyeEnvColor[0], g->eyeEnvColor[1], g->eyeEnvColor[2] };
    x["tail"] = { g->tailLimbsScale[0], g->tailLimbsScale[1], g->tailLimbsScale[2], g->tailLimbsScale[3] };
    // deadLimbsState / decayingProgress / noBackfaceCulling deliberately not
    // streamed: the defeat sequence runs locally on both clients, and streaming
    // deadLimbsState would double-spawn the PostLimbDraw limb pieces.
}

static void Gohma_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossGoma* g = (BossGoma*)actor;
    try {
        if (x.contains("eye")) g->eyeState = x["eye"].get<int16_t>();
        if (x.contains("irx")) g->eyeIrisRotX = x["irx"].get<int16_t>();
        if (x.contains("iry")) g->eyeIrisRotY = x["iry"].get<int16_t>();
        if (x.contains("lidB")) g->eyeLidBottomRotX = x["lidB"].get<int16_t>();
        if (x.contains("lidT")) g->eyeLidTopRotX = x["lidT"].get<int16_t>();
        if (x.contains("isx")) g->eyeIrisScaleX = x["isx"].get<float>();
        if (x.contains("isy")) g->eyeIrisScaleY = x["isy"].get<float>();
        if (x.contains("vis")) g->visualState = x["vis"].get<int16_t>();
        if (x.contains("inv")) g->invincibilityFrames = x["inv"].get<int16_t>();
        if (x.contains("menv") && x["menv"].size() == 3) {
            for (int i = 0; i < 3; i++) g->mainEnvColor[i] = x["menv"][i].get<float>();
        }
        if (x.contains("eenv") && x["eenv"].size() == 3) {
            for (int i = 0; i < 3; i++) g->eyeEnvColor[i] = x["eenv"][i].get<float>();
        }
        if (x.contains("tail") && x["tail"].size() == 4) {
            for (int i = 0; i < 4; i++) g->tailLimbsScale[i] = x["tail"][i].get<float>();
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[GohmaSync] extras parse error: {}", ex.what());
    }
}

static bool Gohma_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    ESYNC_LOG("[GohmaSync] phase {}->{}", fromPhase, toPhase);
    if (toPhase == GOHMA_PHASE_DEFEATED && fromPhase == GOHMA_PHASE_FIGHT) {
        // Final streamed pose is already applied; run the defeat locally from
        // here (cutscene, decay, heart container, blue warp, Flags_SetClear).
        actor->colChkInfo.health = 0;
        BossGoma_SetupDefeated((BossGoma*)actor, gPlayState);
        ESYNC_LOG("[GohmaSync] defeat handoff (SetupDefeated called locally)");
        return true;
    }
    return false;
}

static bool Gohma_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossGoma* g = (BossGoma*)actor;
    // Mirror only during the fight proper, and only once our own intro is done
    // (the intro cutscene must run locally on every client).
    return streamedPhase == GOHMA_PHASE_FIGHT && !g->disableGameplayLogic;
}

void RegisterGohmaAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = Gohma_SerializeExtras;
    adapter.DeserializeExtras = Gohma_DeserializeExtras;
    adapter.GetPhase = Gohma_GetPhase;
    adapter.OnPhaseChange = Gohma_OnPhaseChange;
    adapter.ShouldMirror = Gohma_ShouldMirror;
    EnemySync::RegisterAdapter(ACTOR_BOSS_GOMA, adapter);
}
