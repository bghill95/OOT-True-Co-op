#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Poh/z_en_poh.h"
}

/**
 * Poe (EN_POH — graveyard/field Poes, incl. the one a pulled grave spawns).
 *
 * Visibility is not a flag: the whole ghost fades through this->lightColor.a,
 * and its scale is even derived from it during appear/death. Both are advanced
 * only by the action funcs, so a suppressed mirror keeps alpha 0 forever — the
 * replica exists, chases, can hit, but renders as nothing. Stream the color,
 * the scale, and the bob counters so the mirror looks alive.
 */

static void EnPoh_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnPoh* poh = (EnPoh*)actor;
    x["la"] = poh->lightColor.a;
    x["sc"] = actor->scale.x;
    x["b5"] = poh->unk_195; // bob phase
    x["b8"] = poh->unk_198; // flame/visual timer
}

static void EnPoh_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnPoh* poh = (EnPoh*)actor;
        if (x.contains("la")) {
            poh->lightColor.a = x["la"].get<uint8_t>();
        }
        if (x.contains("sc")) {
            Actor_SetScale(actor, x["sc"].get<float>());
        }
        if (x.contains("b5")) {
            poh->unk_195 = x["b5"].get<uint8_t>();
        }
        if (x.contains("b8")) {
            poh->unk_198 = x["b8"].get<int16_t>();
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnPoh extras parse error: {}", ex.what());
    }
}

void RegisterEnPohAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnPoh_SerializeExtras;
    adapter.DeserializeExtras = EnPoh_DeserializeExtras;
    // EnPoh_Death drops nothing. The overlay's only drop is in EnPoh_Init, for
    // the EN_POH_RUPEE variant (z_en_poh.c:218) -- and all three Poes placed in
    // the game (spot02, params 0x0000) are EN_POH_NORMAL, so no instance can
    // reach it. A generic roll here would hand the mirrored player a rupee the
    // authority's screen never showed.
    adapter.ownsDeathDrop = true;
    EnemySync::RegisterAdapter(ACTOR_EN_POH, adapter);
}
