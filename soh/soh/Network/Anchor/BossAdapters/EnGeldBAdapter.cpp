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
    } catch (const std::exception& ex) { SPDLOG_WARN("[EnemySync] EnGeldB extras parse error: {}", ex.what()); }
}

// The small key, when the force-kill has to step in -- a last resort, and only
// that. On every path where the copy reaches its own death, the key is already
// handled without this: the lethal synthetic hit lands on the non-stun branch,
// z_en_geldb.c:1397 runs, and EnGeldB_SetupDefeated fires the defeat hook
// (:1340), which clears pendingKillFrames so the force-kill never happens. What
// is left for this callback is the copy that never gets there at all -- frozen
// and still airborne when the stall budget runs out, its exit gated on
// `bgCheckFlags & 1` (:1044) -- where the alternative is a Gerudo who dies with
// nothing.
//
// Every other enemy here drops from its own death function, so letting the copy
// die natively is enough. This one does not: the key is dropped by
// EnGeldB_CollisionCheck (z_en_geldb.c:1396-1405), on the lethal NON-stun
// branch, and the chain that follows -- SetupDefeated, Defeated, SetupFlee,
// Flee, Actor_Kill -- drops nothing at all. It is the only Item_Drop in the
// whole overlay. So a mirror that reaches its death by any other route, the
// freeze exit included, arrives empty-handed; and the generic roll cannot stand
// in, because sItemDropIds (z_en_item00.c:79-118) holds no ITEM00_SMALL_KEY at
// any index. Nothing rolls a key.
//
// This matters at every single instance: all four Gerudo placed in the game
// (gerudoway rooms 1, 2, 4 and 5) carry params 0x0A00, 0x0C00, 0x0E00 and
// 0x0F00, so keyFlag = params & 0xFF00 (z_en_geldb.c:243) is non-zero for every
// one of them, and they are the four that free the carpenters.
//
// Dropping it on both screens is safe and is the model this project already
// uses: dungeonKeys is a shared count synced as a delta
// (Packets/UpdateDungeonItems.cpp), and the copy nobody picks up is reaped by
// the collectible-flag replay in FlagReplay.cpp:408-425 as soon as the other
// player takes theirs. One key reaches the team either way. The reaper's "leave
// flag 0 alone" guard does not apply, precisely because keyFlag is never 0 here.
static void EnGeldB_OnForcedKill(Actor* actor) {
    EnGeldB* geldB = (EnGeldB*)actor;
    if (geldB->keyFlag == 0 || gPlayState == NULL) {
        return;
    }
    EnItem00* key = Item_DropCollectible(gPlayState, &actor->world.pos, geldB->keyFlag | ITEM00_SMALL_KEY);
    if (key != NULL) {
        // Same toss as vanilla (z_en_geldb.c:1399-1400): back towards the spot
        // she was posted at, so it does not land somewhere unreachable.
        key->actor.world.rot.y = Math_Vec3f_Yaw(&key->actor.world.pos, &actor->home.pos);
        key->actor.speedXZ = 6.0f;
        Audio_PlaySoundGeneral(NA_SE_SY_TRE_BOX_APPEAR, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                               &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
    }
}

void RegisterEnGeldBAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = EnGeldB_SerializeExtras;
    adapter.DeserializeExtras = EnGeldB_DeserializeExtras;
    adapter.OnForcedKill = EnGeldB_OnForcedKill;
    // The key above replaces the generic roll, which could never produce it.
    adapter.ownsDeathDrop = true;
    EnemySync::RegisterAdapter(ACTOR_EN_GELDB, adapter);
}
