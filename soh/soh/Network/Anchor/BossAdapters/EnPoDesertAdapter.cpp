#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Po_Desert/z_en_po_desert.h"
}

/**
 * Desert Poe guide (EN_PO_DESERT) — same lightColor.a fade as its cousins.
 */

static void EnPoDesert_SerializeExtras(Actor* actor, nlohmann::json& x) {
    x["la"] = ((EnPoDesert*)actor)->lightColor.a;
}

static void EnPoDesert_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        if (x.contains("la")) {
            EnPoDesert* po = (EnPoDesert*)actor;
            po->lightColor.a = x["la"].get<uint8_t>();
            actor->shape.shadowAlpha = po->lightColor.a;
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnPoDesert extras parse error: {}", ex.what());
    }
}

void RegisterEnPoDesertAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnPoDesert_SerializeExtras;
    adapter.DeserializeExtras = EnPoDesert_DeserializeExtras;
    EnemySync::RegisterAdapter(ACTOR_EN_PO_DESERT, adapter);
}
