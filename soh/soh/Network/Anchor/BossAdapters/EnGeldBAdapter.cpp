#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_GeldB/z_en_geldb.h"
}

/**
 * Gerudo fighter (EN_GELDB) — the mini-boss guarding each jailed carpenter.
 *
 * EnGeldB_Draw renders nothing at all while `action == GELDB_WAIT && invisible`
 * (z_en_geldb.c). Both fields are set by EnGeldB_SetupWait, which runs from Init
 * on EVERY client, and only the AI ever clears them: EnGeldB_Wait drops
 * `invisible` when a player comes within 300 units, and SetupReady moves `action`
 * on. A suppressed mirror never runs either, so its copy stays in the initial
 * WAIT/invisible pair for the whole fight — correctly positioned by the stream,
 * hitting with a submitted sword quad, and completely undrawn. Neither field is
 * part of the generic stream (they are not pos/rot/joints/collider bits), so
 * carry them as extras.
 *
 * `action` is also read by the draw path to place the parry Tris and by Update to
 * gate AC submission; replaying it on the mirror is safe because Update does not
 * run there, and the Tris are deliberately not submitted by SubmitColliders.
 *
 * Deliberately NOT streamed:
 * - spinAttackState: EnGeldB_Draw turns `>= 2` into the capture sequence
 *   (nextEntranceIndex + transitionTrigger), and Draw DOES run on the mirror.
 *   Replicating it would jail whichever player did not get caught.
 * - iceTimer / blinkState: cosmetic, and iceTimer drives colorFilterTimer from
 *   inside Draw, so replaying it would fight the stream every frame.
 */

static void EnGeldB_SerializeExtras(Actor* actor, nlohmann::json& x) {
    EnGeldB* geldB = (EnGeldB*)actor;
    x["inv"] = geldB->invisible;
    x["act"] = geldB->action;
}

static void EnGeldB_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    try {
        EnGeldB* geldB = (EnGeldB*)actor;
        if (x.contains("inv")) {
            geldB->invisible = x["inv"].get<int16_t>();
        }
        if (x.contains("act")) {
            geldB->action = x["act"].get<int32_t>();
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] EnGeldB extras parse error: {}", ex.what());
    }
}

void RegisterEnGeldBAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnGeldB_SerializeExtras;
    adapter.DeserializeExtras = EnGeldB_DeserializeExtras;
    EnemySync::RegisterAdapter(ACTOR_EN_GELDB, adapter);
}
