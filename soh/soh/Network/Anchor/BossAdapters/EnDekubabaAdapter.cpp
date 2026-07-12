#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
// Pull the C++ side of global.h in under proper linkage before the extern "C"
// overlay header includes it (BossRush.cpp pattern).
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Dekubaba/z_en_dekubaba.h"
}

/**
 * Deku Baba (EN_DEKUBABA).
 *
 * Generic mirroring streams the head skeleton pose + world position, and the
 * base stream now carries actor.scale (fixes the "minimized" head). But the Baba
 * draws its STEM procedurally: EnDekubaba_Draw picks retracted-stub vs
 * extended-stalk by comparing actionFunc, and shapes the stalk from
 * stemSectionAngle[3] — none of which the suppressed mirror recomputes (its
 * Update, and thus actionFunc, is frozen at the Init "Wait" state, so it always
 * draws the tiny buried stub). These extras carry the stem draw-state.
 *
 * Death is likewise gated behind the suppressed Update (EnDekubaba_UpdateDamage
 * only starts a death on a fresh AC_HIT), so a killed Baba would freeze as a
 * health-0 husk. GetPhase/OnPhaseChange hand the death off to local simulation.
 */

static void EnDekubaba_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnDekubaba* b = (EnDekubaba*)actor;
    x["stem"] = EnDekubaba_MirrorGetStemMode(b);
    x["sa0"] = b->stemSectionAngle[0];
    x["sa1"] = b->stemSectionAngle[1];
    x["sa2"] = b->stemSectionAngle[2];
    x["timer"] = b->timer;
}

static void EnDekubaba_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    EnDekubaba* b = (EnDekubaba*)actor;
    try {
        uint8_t stem = x.contains("stem") ? x["stem"].get<uint8_t>() : 1;
        int16_t sa0 = x.contains("sa0") ? x["sa0"].get<int16_t>() : b->stemSectionAngle[0];
        int16_t sa1 = x.contains("sa1") ? x["sa1"].get<int16_t>() : b->stemSectionAngle[1];
        int16_t sa2 = x.contains("sa2") ? x["sa2"].get<int16_t>() : b->stemSectionAngle[2];
        int16_t timer = x.contains("timer") ? x["timer"].get<int16_t>() : b->timer;
        EnDekubaba_MirrorApplyDraw(b, stem, sa0, sa1, sa2, timer);
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnDekubaba extras parse error: {}", ex.what());
    }
}

// health==0 is the observable "dying" edge (the adapter contract asks GetPhase to
// avoid reading actionFunc). A Baba's HP only reaches 0 when it is killed.
static uint8_t EnDekubaba_GetPhase(Actor* actor) {
    return (actor->colChkInfo.health == 0) ? 1 : 0;
}

static bool EnDekubaba_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    if (fromPhase == 0 && toPhase == 1) {
        // The authority's Baba just died. Play the death locally (shrink +
        // Actor_Kill via EnDekubaba_ShrinkDie) instead of freezing at health-0 as
        // a husk, then release to local simulation (return true).
        EnDekubaba_MirrorSetupDeath((EnDekubaba*)actor);
        ESYNC_LOG("[EnemySync] EnDekubaba death handoff (local ShrinkDie)");
        return true;
    }
    return false;
}

void RegisterEnDekubabaAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnDekubaba_SerializeExtras;
    adapter.DeserializeExtras = EnDekubaba_DeserializeExtras;
    adapter.GetPhase = EnDekubaba_GetPhase;
    adapter.OnPhaseChange = EnDekubaba_OnPhaseChange;
    EnemySync::RegisterAdapter(ACTOR_EN_DEKUBABA, adapter);
}
