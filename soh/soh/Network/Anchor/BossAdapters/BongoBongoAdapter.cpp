#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Sst/z_boss_sst.h"
extern PlayState* gPlayState;
}

/**
 * Bongo Bongo (Shadow Temple). One adapter for the three actors of the fight:
 * BOSS_SST params BONGO_HEAD is the head, params BONGO_LEFT_HAND /
 * BONGO_RIGHT_HAND the two hands. They share a single state machine through the
 * overlay's file statics, so the phase always comes from the head
 * (BossSst_CoopGetPhase).
 *
 * What this fixes, and why the boss needed an adapter at all:
 *
 * 1. THE MIRROR'S PLAYER LOST HIS CONTROLLER FOR THE WHOLE FIGHT.
 *    BossSst_HeadSetupLurk nulls the draw of all three actors during the ambush
 *    (z_boss_sst.c:372-374). With no adapter, EnemySync's draw-less heuristic
 *    (EnemySync.cpp:1479) handed the mirror back to its local AI -- correct so
 *    far, the intro is a per-client cutscene -- and that copy ran
 *    BossSst_HeadSetupIntro, which teleports the local Link to the room centre,
 *    sets PLAYER_STATE1_INPUT_DISABLED and parks MAIN_CAM in CAM_STAT_WAIT. The
 *    intro then makes the boss drawable again mid-cutscene (:484, :501) -- and on
 *    THAT frame the heuristic stopped applying, suppression kicked in, and the
 *    local intro died a frame before its only line that gives the inputs back
 *    (:441). ShouldMirror below replaces the heuristic with the real question:
 *    has OUR copy finished its own intro? Until it has, it runs locally, to the
 *    end.
 *
 * 2. THE BOSS WAS NEVER KILLABLE ON THE MIRROR. Vanilla only reaches
 *    BossSst_HeadSetupDeath from inside BossSst_HeadVulnerable
 *    (z_boss_sst.c:2620); a mirrored head frozen on any other action sends the
 *    generic lethal hit to BossSst_HeadSetupStunned instead. The defeat now
 *    travels as a phase edge and BossSst_CoopMirrorSetupDeath runs the cutscene
 *    locally -- camera, thrash, melt, blue warp, heart container, clear flag --
 *    exactly like Phantom Ganon's defeat handoff.
 *
 * 3. THE AUTHORITY'S LENS OF TRUTH DECIDED WHO COULD SEE HIM.
 *    ACTOR_FLAG_REACT_TO_LENS is in EnemySync's STREAMED_ACTOR_FLAGS while
 *    actorCtx.lensActive is strictly local, so the mirrored head was invisible
 *    to a player holding his own lens and visible to one who was not.
 *    BossSst_CoopMirrorTick re-derives the flag from the local lens, after
 *    ApplyPose has written the authority's bit over ours.
 *
 * Extras: the head streams only vVanish (its Draw takes everything else from
 * jointTable and the PostLimbDraw, both of which still run on a mirror). A hand
 * streams the two limb-1 offsets its Draw applies (handZPosMod / handYRotMod, in
 * BossSst_OverrideHandDraw) and the shockwave cylinder, which is placed once where
 * the hand slammed and then grows in place (:1552, :2654) -- neither snapping it
 * to world.pos nor dragging it with the hand would be right, so the adapter
 * places it itself (customColliderPos; the head's own cylinder is placed by
 * BossSst_PostHeadDraw, which runs on the mirror too). The hand trail is rebuilt
 * locally from the streamed motion.
 *
 * Known limit, deliberate: the effect arrays (shockwave rings, ice crystals, the
 * growing shadow) are spawned by locally-run helpers the suppressed update never
 * calls, so a mirrored hand slams without its ring and a mirrored freeze shows no
 * crystals. Cosmetic, and replaying them would double the sounds the generic sfx
 * stream already carries.
 */

// ActorSyncAdapter::GetPhase says "never actionFunc". The rule guards against
// putting a function POINTER on the wire, where two clients need not agree; here
// the comparison is purely local -- each side classifies its own head and sends
// the small enum that comes out, exactly as Phantom Ganon's GetPhase does with
// BossGanondrof_Intro. There is no other way to tell this fight's states apart:
// the head keeps no phase field of its own.
static uint8_t BongoBongo_GetPhase(Actor* actor) {
    return BossSst_CoopGetPhase(actor);
}

static void BongoBongo_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossSst* boss = (BossSst*)actor;

    if (actor->params == BONGO_HEAD) {
        x["van"] = boss->actionVar; // vVanish
    } else {
        x["hnd"] = { boss->handZPosMod, boss->handYRotMod };
        // Centre only: the radius it grows to travels on the generic per-collider
        // channel (RemoteEnemyState::colRadius).
        x["cyl"] = { boss->colliderCyl.dim.pos.x, boss->colliderCyl.dim.pos.y, boss->colliderCyl.dim.pos.z };
    }
}

static void BongoBongo_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossSst* boss = (BossSst*)actor;
    int32_t vanish = 0;

    try {
        if (actor->params == BONGO_HEAD) {
            if (x.contains("van")) {
                vanish = x["van"].get<int32_t>();
            }
        } else {
            if (x.contains("hnd") && x["hnd"].size() == 2) {
                boss->handZPosMod = x["hnd"][0].get<int16_t>();
                boss->handYRotMod = x["hnd"][1].get<int16_t>();
            }
            if (x.contains("cyl") && x["cyl"].size() == 3) {
                boss->colliderCyl.dim.pos.x = x["cyl"][0].get<int16_t>();
                boss->colliderCyl.dim.pos.y = x["cyl"][1].get<int16_t>();
                boss->colliderCyl.dim.pos.z = x["cyl"][2].get<int16_t>();
            }
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[BongoBongoSync] extras parse error: {}", ex.what());
        return;
    }

    // Runs every game frame (the cached extras are re-applied each one), which is
    // exactly the cadence the trail and the lens check want.
    if (gPlayState != NULL) {
        BossSst_CoopMirrorTick(actor, gPlayState, vanish);
    }
}

static bool BongoBongo_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    uint8_t local = BossSst_CoopGetPhase(actor);

    // The ambush and the 611-frame intro are a per-client cutscene: each machine
    // teleports its own Link, takes its own camera and gives its own inputs back.
    // Mirror only once BOTH sides are in the fight proper -- our own phase is the
    // half that matters, since the two players trip the ambush at their own pace.
    return local == BONGO_COOP_FIGHT && streamedPhase == BONGO_COOP_FIGHT;
}

static bool BongoBongo_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    ESYNC_LOG("[BongoBongoSync] phase {}->{} params={}", fromPhase, toPhase, actor->params);
    if (toPhase != BONGO_COOP_DEATH) {
        return false;
    }
    // The head runs the defeat cutscene locally and drives both hands into their
    // thrash from inside BossSst_HeadDeath, 60 frames later; the hands only need to
    // be let go, or the stream would fight the sequence their own head is playing.
    // BossSst_CoopMirrorSetupDeath puts them in their damage pose for those 60
    // frames, as the vanilla killing blow does -- released on a frozen attack action
    // they were free to hit a player the cutscene had just disarmed.
    if (actor->params == BONGO_HEAD && gPlayState != NULL) {
        BossSst_CoopMirrorSetupDeath(actor, gPlayState);
        ESYNC_LOG("[BongoBongoSync] defeat handoff (SetupDeath called locally)");
    }
    return true;
}

void RegisterBongoBongoAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = BongoBongo_SerializeExtras;
    adapter.DeserializeExtras = BongoBongo_DeserializeExtras;
    adapter.GetPhase = BongoBongo_GetPhase;
    adapter.OnPhaseChange = BongoBongo_OnPhaseChange;
    adapter.ShouldMirror = BongoBongo_ShouldMirror;
    // The hand's shockwave cylinder stays where the slam landed while the hand
    // lifts away; DeserializeExtras places it, and the head's is placed by
    // BossSst_PostHeadDraw.
    adapter.customColliderPos = true;
    // All three actors null their own draw for the whole ambush, so the generic
    // draw-less bail must not be the one deciding when the mirror runs locally --
    // ShouldMirror is.
    adapter.mirrorWhileDrawless = true;
    // Defeat handed back by OnPhaseChange. The arrows and magic this fight gives
    // are dropped per HIT during the fight (z_boss_sst.c:1776, :1890, :2610), not
    // at death, so there is nothing for a death roll to stand in for.
    adapter.ownsDeathDrop = true;
    EnemySync::RegisterAdapter(ACTOR_BOSS_SST, adapter);
}
