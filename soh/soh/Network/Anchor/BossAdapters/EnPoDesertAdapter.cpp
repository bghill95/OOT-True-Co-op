#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
}

/**
 * Desert Poe guide (EN_PO_DESERT) — intentionally NOT adapted.
 *
 * There used to be an adapter here streaming lightColor.a, copied from the other
 * Poes. It never ran: En_Po_Desert is ACTORCAT_BG (z_en_po_desert.c:26), and
 * EnemySync only tracks ACTORCAT_ENEMY, ACTORCAT_BOSS and the two ids in
 * IsForcedTracked. Registering an adapter for an actor that never enters the
 * tracking table is dead code that reads like a feature.
 *
 * Leaving it untracked is also the RIGHT behaviour, which is why this is a
 * deletion and not a fix. Each client runs its own guide against its own Link:
 * it lights its torch, waits, and leads that player across the Haunted Wasteland
 * at their pace. Two players crossing the desert each get a guide that waits for
 * THEM — where a single shared guide would drag one of them along at the other's
 * speed, or strand whoever fell behind. The guide is a piece of guidance, not a
 * piece of world state.
 *
 * If it is ever made shared, the work is: add ACTOR_EN_PO_DESERT to
 * IsForcedTracked, restore an adapter for lightColor.a, and decide what its
 * xzDistToPlayer waiting logic (z_en_po_desert.c:138) should key off — none of
 * which is worth doing for a behaviour that is already better as it stands.
 */

void RegisterEnPoDesertAdapter() {
    // Intentionally empty: see above.
}
