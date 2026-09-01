#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Vm/z_en_vm.h"
}

/**
 * Beamos (EN_VM).
 *
 * The scanning head rotation lives in this->headRotY (0x25C), applied at draw
 * time — it is neither shape.rot nor a jointTable limb, so the generic stream
 * never carries it and a suppressed mirror's head freezes mid-scan while the
 * authority's keeps turning. Stream the field as an extra.
 *
 * Death needs no phase handoff: a Beamos dies to a bomb via the generic
 * ENEMY_DIED path (permanent Actor_Kill + force-kill fallback).
 */

static void EnVm_SerializeExtras(Actor* actor, nlohmann::json& x) {
    x["hry"] = ((EnVm*)actor)->headRotY;
}

static void EnVm_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        if (x.contains("hry")) {
            ((EnVm*)actor)->headRotY = x["hry"].get<int16_t>();
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnVm extras parse error: {}", ex.what());
    }
}

void RegisterEnVmAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnVm_SerializeExtras;
    adapter.DeserializeExtras = EnVm_DeserializeExtras;
    EnemySync::RegisterAdapter(ACTOR_EN_VM, adapter);
}
