#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
// Pull the C++ side of global.h in under proper linkage before the extern "C"
// overlay header includes it (BossRush.cpp pattern).
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Goma/z_en_goma.h"
}

/**
 * Gohma larvae (EN_GOMA, boss-room children, params 0-2).
 *
 * Movement/pose comes from generic mirroring; these extras carry the
 * update-computed draw state — without gomaType the mirror would draw a
 * larva as an egg forever (the egg->hatch switch happens in Update, which
 * is suppressed). Hatch debris and defeat limb pieces (params >= 6) are
 * excluded from tracking entirely and never reach this adapter.
 */

static void EnGoma_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnGoma* g = (EnGoma*)actor;
    x["type"] = g->gomaType;
    x["eggScale"] = g->eggScale;
    x["eggPitch"] = g->eggPitch;
    x["eggSquishAngle"] = g->eggSquishAngle;
    x["eggSquish"] = g->eggSquishAmount;
    x["eggY"] = g->eggYOffset;
    x["eyeP"] = g->eyePitch;
    x["eyeY"] = g->eyeYaw;
    x["hurt"] = g->hurtTimer;
    x["vis"] = g->visualState;
    x["slopeP"] = g->slopePitch;
    x["slopeR"] = g->slopeRoll;
    x["eenv"] = { g->eyeEnvColor[0], g->eyeEnvColor[1], g->eyeEnvColor[2] };
}

static void EnGoma_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    EnGoma* g = (EnGoma*)actor;
    try {
        if (x.contains("type")) g->gomaType = x["type"].get<int16_t>();
        if (x.contains("eggScale")) g->eggScale = x["eggScale"].get<float>();
        if (x.contains("eggPitch")) g->eggPitch = x["eggPitch"].get<float>();
        if (x.contains("eggSquishAngle")) g->eggSquishAngle = x["eggSquishAngle"].get<float>();
        if (x.contains("eggSquish")) g->eggSquishAmount = x["eggSquish"].get<float>();
        if (x.contains("eggY")) g->eggYOffset = x["eggY"].get<float>();
        if (x.contains("eyeP")) g->eyePitch = x["eyeP"].get<int16_t>();
        if (x.contains("eyeY")) g->eyeYaw = x["eyeY"].get<int16_t>();
        if (x.contains("hurt")) g->hurtTimer = x["hurt"].get<int16_t>();
        if (x.contains("vis")) g->visualState = x["vis"].get<int16_t>();
        if (x.contains("slopeP")) g->slopePitch = x["slopeP"].get<int16_t>();
        if (x.contains("slopeR")) g->slopeRoll = x["slopeR"].get<int16_t>();
        if (x.contains("eenv") && x["eenv"].size() == 3) {
            for (int i = 0; i < 3; i++) g->eyeEnvColor[i] = x["eenv"][i].get<float>();
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnGoma extras parse error: {}", ex.what());
    }
}

void RegisterEnGomaAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnGoma_SerializeExtras;
    adapter.DeserializeExtras = EnGoma_DeserializeExtras;
    EnemySync::RegisterAdapter(ACTOR_EN_GOMA, adapter);
}
