#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Po_Field/z_en_po_field.h"
}

/**
 * Big Poe / Field Poe (EN_PO_FIELD).
 *
 * Same visibility scheme as EN_POH: the ghost fades through lightColor.a,
 * advanced only by its action funcs — a suppressed mirror stays at alpha 0
 * and renders as nothing (scale comes from the generic stream already).
 */

static void EnPoField_SerializeExtras(Actor* actor, nlohmann::json& x) {
    x["la"] = ((EnPoField*)actor)->lightColor.a;
}

static void EnPoField_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        if (x.contains("la")) {
            EnPoField* po = (EnPoField*)actor;
            po->lightColor.a = x["la"].get<uint8_t>();
            actor->shape.shadowAlpha = po->lightColor.a;
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnPoField extras parse error: {}", ex.what());
    }
}

void RegisterEnPoFieldAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnPoField_SerializeExtras;
    adapter.DeserializeExtras = EnPoField_DeserializeExtras;
    EnemySync::RegisterAdapter(ACTOR_EN_PO_FIELD, adapter);
}
