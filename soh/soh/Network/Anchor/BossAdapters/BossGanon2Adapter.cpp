#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/OTRGlobals.h"
#include "soh/Enhancements/randomizer/randomizer.h"

#include <cmath>

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Ganon2/z_boss_ganon2.h"
// gGanonSkel (OBJECT_GANON2) and gGanonUncurlAndFlailAnim (OBJECT_GANON_ANIME3):
// header-only `static const char[]` OTR paths, exactly what the overlay uses at
// z_boss_ganon2.c:603 / :614.
#include "objects/object_ganon2/object_ganon2.h"
#include "objects/object_ganon_anime3/object_ganon_anime3.h"
extern PlayState* gPlayState;
// File-scope (non-static) in the overlay; not exported by its header.
void BossGanon2_SetObjectSegment(BossGanon2* self, PlayState* play, s32 objectId, u8 setRSPSegment);
// The particle pool. Slot 0 is the Master Sword once Ganon knocks it loose.
// Non-static in the overlay (z_boss_ganon2.c:65) but absent from its header.
extern BossGanon2Effect sBossGanon2Particles[100];
// The WHOLE introduction is one action function (z_boss_ganon2.c:223, a csState
// machine). Compared by ADDRESS and only ever locally — a function pointer never
// goes on the wire (ActorSyncAdapter.h:22).
void func_808FD5F4(BossGanon2* self, PlayState* play);
// z_boss_ganon2.c:1489 — the death-cutscene setup, the analogue of
// BossGanon_SetupDeathCutscene. Non-static, absent from the header. Called from
// the overlay at :1987 and :2020, both under health <= 0.
void func_80901020(BossGanon2* self, PlayState* play);
// z_boss_ganon2.c:1531 — the cutscene itself, compared by address only.
void func_8090120C(BossGanon2* self, PlayState* play);
// z_boss_ganon2.c — hands the local Link his Master Sword back, guarded by
// hasFoundMasterSword, which is a file static only the overlay can read.
void BossGanon2_CoopGiveMasterSword(PlayState* play);
// z_boss_ganon2.c:61. Non-static, so an extern declaration reaches it. Only ever
// null-tested here; the incomplete type is deliberate (no En_Zl3 header needed).
struct EnZl3;
extern struct EnZl3* sBossGanon2Zelda;
}

/**
 * Ganon the beast (Boss_Ganon2, phase 2 of the final fight).
 *
 * ONE actor, ONE instance: ganon_demo room 0, params 0xFFFF, health 30
 * (BossGanon2_Init:178). Read out of the scene data rather than assumed — that
 * is the only placement of this actor in the game.
 *
 * NO params MULTIPLEXING, and this was checked rather than assumed. Every
 * `params` mention in z_boss_ganon2.c reads ANOTHER actor's params (:963-964 and
 * :1004-1005 walk the Demo_Gj pillars; :2980-2982 is envCtx.dirLight1.params).
 * `this->actor.params` is never read at all, and BossGanon2_Init branches on
 * nothing. So, unlike ACTOR_BOSS_GANON, registering this adapter cannot drag a
 * projectile variant into the stream and NO IsTrackingExcluded entry is needed.
 * The leftStream clause (`health == 0 && GetAdapter(id) == nullptr &&
 * !projectile`, EnemySync.cpp:2247-2248) has exactly one actor to act on here,
 * and it already has health 30 — the regime does not change (see THE DEFECT
 * THIS ADAPTER REMOVES FOR FREE below).
 *
 * WHAT THIS ADAPTER DOES
 *
 *   C3.1 — unk_337 AND THE SKELETON SWITCH. unk_337 (0x337, u8) selects the model
 *     in BossGanon2_Draw:2822: case 0 loads OBJECT_GANON + the Ganondorf
 *     skeleton, cases 1/2 load OBJECT_GANON2 + the beast. It is written ONLY by
 *     the introduction (:600 `= 2`, :933 `= 1`) and read by the Draw, which runs
 *     on the mirror. Frozen at the 0 that Actor_Spawn's memset left
 *     (z_actor.c:3403) it costs three things at once:
 *       1. P2 sees a HUMAN Ganondorf for the whole beast fight;
 *       2. the local skeleton stays gGanondorfSkel, so limbCount != the streamed
 *          jointTable size and ApplyPose REJECTS the pose every frame
 *          (EnemySync.cpp:1571-1577, "limb mismatch") — P2 gets an idle
 *          Ganondorf sliding along the streamed position;
 *       3. case 0 draws through BossGanon2_PostLimbDraw2:2766, which never calls
 *          func_808FD080 — so the sixteen JntSph elements of unk_424 keep their
 *          Init centres and an unset world radius, and P2 swings at a ghost box.
 *     So the field travels AND the mirror replays the SkelAnime_Free +
 *     SkelAnime_InitFlex(&gGanonSkel) of :602-603. See the guards on
 *     BossGanon2_SwitchToBeastSkeleton.
 *
 *   C3.2 — ShouldMirror false during the introduction. func_808FD5F4 is a
 *     csState machine driven by the BOSS'S OWN Update, and it is the thing that
 *     spawns Zelda (:244), moves the player, runs the cameras and performs the
 *     transformation. Suppress it on the mirror and, exactly as measured on
 *     Boss_Ganon at the bench on 2026-09-12, the machine never advances and P2
 *     is stuck inside a cutscene that cannot end. So the intro runs LOCALLY on
 *     every client, same shape as PhantomGanon_ShouldMirror:185-190 and
 *     BossGanon_ShouldMirror.
 *
 *   C3.3 — the remaining "Update writes it / Draw reads it" fields. Classified by
 *     brace-matched call-graph sweep of all 56 functions in the overlay (Update
 *     reachability including every actionFunc assignment, vs Draw reachability
 *     including the three limb callbacks), not from the plan's list.
 *
 * THE sBossGanon2Zelda CRASH TRAP, AND THE PLAN UNDERSTATES IT. The plan says the NULL
 * sBossGanon2Zelda (:61, assigned only at :244 inside the Update) is a null
 * dereference "as soon as the mirror is released". It is worse than that: the
 * two functions that dereference it are DRAW functions —
 *     func_80904FC8:2711-2712   gated on this->unk_384 > 0.0f
 *     func_80905674:2798-2800   gated on this->unk_380 > 0.0f
 * and the Draw is never suppressed. Nothing crashes today only because both
 * floats are frozen at 0. Putting them on the wire is therefore the act that
 * ARMS the trap -- so they are NOT on the wire, and the receiver forces them to 0
 * on every applied packet. A `!= NULL` guard was the first answer and review took
 * it apart: the zeroing never runs if DeserializeExtras is short-circuited (three
 * upstream returns do that permanently), and NULL is not a liveness test anyway
 * since the En_Zl3 in question calls Actor_Kill on itself without clearing the
 * pointer. See the long note in SerializeExtras.
 *
 * THE DEFECT THIS ADAPTER REMOVES FOR FREE. colChkInfo.health is a u8 and
 * func_80902524 subtracts 1, 2 or 4 (:1974, :2008), so it can land exactly on 0.
 * Without an adapter leftStream would then be true for the whole death sequence,
 * the mirror would go stale, EndSuppression("stale") would release it, and P2's
 * Ganon would run its own copy of whatever came next. Registering ANY adapter
 * for this id disarms that clause. Verified as safe to do here precisely because
 * the id is not multiplexed (see above).
 *
 * DELIBERATELY ABSENT — and each one has a line, not a silence:
 *
 *   (OnPhaseChange / the defeat handoff was in this list, on the grounds that
 *   handing the ending to the mirror's local copy would start it from whatever
 *   action function that copy happened to hold. Measured in a real fight on
 *   2026-09-13: without the handoff P2 sees no ending at all -- no beam of
 *   light, no sages -- and stands in the arena through P1's credits, because the
 *   scene change is itself inside the suppressed machine. It is now written; the
 *   "whatever action function" worry is answered by going through
 *   func_80901020, the overlay's own setup, and by refusing the handoff if
 *   sBossGanon2Zelda is null.)
 *   - The tail HIT ACCOUNTING is still out of scope. NOTE that the tail's
 *     three DRAW fields (unk_346, unk_348, unk_352) ARE here — they are
 *     "Update writes it / Draw reads it" field-list items and pure
 *     OverrideLimbDraw inputs. The tail still is NOT playable for two until
 *     per-element hit routing exists: ApplyRemoteHit sets BUMP_HIT on all
 *     sixteen elements at once, and func_80902524 distinguishes element 0 (the
 *     shield) from element 15 (the tail).
 *   - The Master Sword is now PARTLY done, in two halves: the particle
 *     travels as "ms" so P2 can see the blade at all, and the pickup proximity
 *     test in the overlay runs against Anchor_NearestPlayer so either player can
 *     reach it. What is NOT done is who ends up HOLDING it -- the csState 10
 *     sequence plays on the authority and arms its own Link. Either player can
 *     unblock the fight; handing the sword across belongs to the endgame work.
 *   - unk_394 (0x394, f32, Draw:2840). Written only at :659 (`+= 0.5f`) and
 *     :668 (`= 0.0f`), inside intro csState 19, which lasts ten frames while
 *     Ganon is off-screen falling. C3.2 makes the mirror run that itself. The
 *     plan already calls it "sans effet pratique"; agreed, and this is why.
 *   - unk_339 (0x339, s8, the ambient/lightning director). It travels NOWHERE
 *     because streaming it would do NOTHING: its only readers are
 *     BossGanon2_Update:2176 and :2236, and the mirror's Update does not run.
 *     Replaying the envCtx block it drives (:2147-2234: unk_BD/BE/D8,
 *     customSkyboxFilter, skyboxFilterColor) needs a BossGanon2_CoopMirrorFx
 *     entry point inside z_boss_ganon2.c, on the model of
 *     BossGandondrof_CoopMirrorFx — an overlay edit, outside this commit's file
 *     scope. Until then P2's arena never darkens and never flashes. The plan's
 *     "read it from a field mirror rather than from unk_339 at snapshot time"
 *     is correct advice for whoever writes that, and it stays unimplemented here
 *     rather than half-implemented.
 *   - sBossGanon2Particles[100] (file static, :65) — no Master Sword in the
 *     floor and no rubble on the mirror, apart from slot 0 (see the Master
 *     Sword note in SerializeExtras).
 *   - customColliderPos: meaningless for this actor. Both colliders are
 *     ColliderJntSph, and that flag only affects COLSHAPE_CYLINDER
 *     (EnemySync.cpp:1440-1446, :1526-1532). The JntSph world spheres are
 *     refreshed by BossGanon2_PostLimbDraw:2630-2633, which runs on the mirror —
 *     but ONLY in Draw case 1/2, which is another reason C3.1 comes first.
 *   - IsHealthless / ReplicateKill: health is 30 and the overlay contains no
 *     Actor_Kill at all (the fight ends by scene transition).
 *   - ownsSfx: there is no CoopMirrorFx path in this overlay, so the generic
 *     per-actor sfx stream is what gives the mirror Ganon's roar.
 *   - mirrorWhileDrawless: Boss_Ganon2 never nulls its own draw (no
 *     `draw = NULL` anywhere in the overlay).
 *
 * NOT ON THE WIRE ON PURPOSE — written by the limb callbacks or the Draw, which
 * run on the mirror too, so they are already correct there: unk_234[16]
 * (PostLimbDraw:2613, PostLimbDraw2:2771), unk_1B8 (:2617 / :2781), unk_1C4
 * (:2627), unk_1D0 (:2625), unk_1DC (:2623), unk_1E8 (:2621), unk_1F4 (:2619),
 * unk_200 and unk_20C (:2654-2655 / :2659-2660), unk_218 (:2652 / :2657 and
 * Draw:2835).
 *
 * NOT ON THE WIRE because the mirror's suppressed Update is their ONLY reader —
 * streaming them would move bytes nobody looks at: unk_194, unk_198, unk_1A2[5],
 * unk_1AC, unk_311, unk_313 ("look_on"), unk_314, unk_316 ("no_hit_time"),
 * unk_320, unk_332, unk_334 (the Master Sword latch), unk_335, unk_336,
 * unk_338, unk_33C, unk_344, unk_390, unk_392, unk_39E, unk_3A4, unk_3B0,
 * unk_3BC, unk_410, unk_41C, unk_420, csState, csTimer. Three of them have
 * streamed CONSEQUENCES instead of being streamed themselves: unk_318 feeds
 * unk_310 (:2078), and unk_35C/unk_36C/unk_360/unk_370 feed unk_348/unk_352
 * (:2124-2125) and unk_346 (:2089-2103).
 */

enum {
    GANON2_PHASE_INTRO = 0,
    GANON2_PHASE_FIGHT = 1,
    GANON2_PHASE_DEFEATED = 2,
};

// sEyeTextures is file-static in z_boss_ganon2_data.c and has exactly three
// entries (gGanonEyeOpenTex / HalfTex / ClosedTex). BossGanon2_Draw:2832 indexes
// it with unk_310 and never bounds-checks, because locally unk_310 can only come
// from D_80907074[] (:2078), whose entries are 0..2. A value off the wire has not
// been through that table.
#define GANON2_EYE_TEX_COUNT 3

// Emit-side rounding, same reasoning as EnemySync's RoundedVec and
// BossGanonAdapter's R3: a float promoted to double prints at double precision
// (0.4f -> "0.4000000059604645"). Three decimals is below anything visible on a
// scale factor, an 0-255 alpha or a radian.
static inline double R3(float v) {
    return std::round((double)v * 1000.0) / 1000.0;
}

// ---------------------------------------------------------------------------
// C3.1 — the skeleton switch, replayed on the mirror
// ---------------------------------------------------------------------------
// z_boss_ganon2.c:601-614, minus the parts that belong to the authority (camera,
// csState, Player_SetCsActionWithHaltedActors).
//
// ONCE ONLY, and the caller enforces it on the 0 -> non-zero edge of unk_337. The
// overlay's own comment at :608-613 describes what a mismatched skeleton and
// animation produce in vanilla ("an explosion of limbs"); a DOUBLE
// SkelAnime_Free would be worse than that.
//
// Returns false and changes nothing if either object bank is missing, so the
// caller leaves unk_337 at 0 and retries next frame — the same discipline the
// overlay itself uses at :1095 and :238.
//
// Note on the cached SkelAnime pointer: TrackedState::skelAnime
// (EnemySync.cpp:113, captured by OnSkelAnimeInitCapture:736) points at
// &this->skelAnime, the struct INSIDE the actor, and ApplyPose re-reads
// ->limbCount and ->jointTable through it every frame
// (EnemySync.cpp:1571-1573). Freeing and re-initialising that same struct in
// place is therefore transparent to the stream: no dangling pointer, and the
// limb count matches from the next frame on. This runs after ApplyPose in the
// same frame (EnemySync.cpp:1772-1774), so this frame's memcpy already happened
// against the old table.
static bool BossGanon2_SwitchToBeastSkeleton(BossGanon2* g) {
    if (gPlayState == NULL) {
        return false;
    }
    s32 beastIdx = Object_GetIndex(&gPlayState->objectCtx, OBJECT_GANON2);
    s32 animIdx = Object_GetIndex(&gPlayState->objectCtx, OBJECT_GANON_ANIME3);
    // Object_GetIndex returns -1 when the bank is not in the scene's object list,
    // and BossGanon2_SetObjectSegment:114-116 would index status[-1] with it.
    if (beastIdx < 0 || animIdx < 0) {
        return false;
    }
    if (!Object_IsLoaded(&gPlayState->objectCtx, beastIdx) || !Object_IsLoaded(&gPlayState->objectCtx, animIdx)) {
        return false;
    }

    BossGanon2_SetObjectSegment(g, gPlayState, OBJECT_GANON2, false);
    SkelAnime_Free(&g->skelAnime, gPlayState);
    SkelAnime_InitFlex(gPlayState, &g->skelAnime, (FlexSkeletonHeader*)gGanonSkel, NULL, NULL, NULL, 0);
    BossGanon2_SetObjectSegment(g, gPlayState, OBJECT_GANON_ANIME3, false);
    // :614 — the beast skeleton with a Ganondorf animation reads past the end of
    // the animation data. The mirror's SkelAnime is driven by the streamed
    // jointTable, not by SkelAnime_Update, but leave it in a state that is safe
    // the moment the copy is ever released.
    Animation_PlayOnce(&g->skelAnime, (AnimationHeader*)gGanonUncurlAndFlailAnim);
    ESYNC_LOG("[BossGanon2Sync] beast skeleton installed (limbCount={})", g->skelAnime.limbCount);
    return true;
}

static void BossGanon2_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossGanon2* g = (BossGanon2*)actor;

    // --- C3.1 -------------------------------------------------------------
    // unk_337 (0x337, u8). W BossGanon2_Update-side only: :600 (= 2) and :933
    // (= 1), both inside func_808FD5F4. R BossGanon2_Draw:2822 (model + object
    // bank), Update:2035 and :2110.
    x["m"] = (int32_t)g->unk_337;

    // --- C3.3, the per-frame draw inputs ----------------------------------

    // unk_19C (0x19C, s16). W Update:2043 (`unk_19C++`, unconditional).
    // R func_80905674:2794-2795 — it IS the texture scroll of Zelda's beam; at a
    // frozen 0 the beam is a static sheet. Also Update:2075/:2082.
    x["tc"] = g->unk_19C;

    // unk_1B0 (0x1B0, f32). W Update:2082 only. R BossGanon2_OverrideLimbDraw:2589
    // — the env colour of every tail limb (index >= 42). Frozen at 0 the whole
    // tail renders black.
    x["ta"] = R3(g->unk_1B0);

    // unk_1B4 (0x1B4, f32). W :552 (approach to 255) and :576 (= 0), both inside
    // the intro. R func_8090464C:2552 (the gate) and :2557 (alpha) — the Triforce
    // on Ganondorf's forehead. With C3.2 the mirror runs the intro and computes
    // this itself; sent anyway, one float, so a copy that somehow skipped the
    // intro cannot end up with a Triforce stuck on its forehead.
    x["tf"] = R3(g->unk_1B4);

    // unk_224 / unk_228 (0x224 / 0x228, f32). W :753/:904/:1109 and :671/:1108.
    // R BossGanon2_PostLimbDraw:2638 and :2644 — Matrix_Scale for limbs 7/13 (the
    // two swords) and 33/34. At the frozen 0 BOTH SWORDS ARE INVISIBLE, which is
    // the most visible single defect after the model itself.
    x["sw"] = { R3(g->unk_224), R3(g->unk_228) };

    // unk_30C (0x30C, f32). W :670/:708/:1670 and Math_ApproachZeroF at
    // Update:2039. R func_80904D88:2673 (the gate) and :2689 (Matrix_Scale) — the
    // fifteen light orbs on his body.
    x["ob"] = R3(g->unk_30C);

    // unk_310 (0x310, u8). W Update:2078, from D_80907074[unk_318].
    // R BossGanon2_Draw:2832, as the index into sEyeTextures. unk_318 itself
    // stays home: it is read only by the Update.
    x["ey"] = (int32_t)g->unk_310;

    // unk_312 (0x312, u8). R BossGanon2_Draw:2860/:2862, func_80903F38:2457 (the
    // sword trail) and PostLimbDraw:2651/:2656, which is what places the trail's
    // two endpoints AND the attack sphere of unk_444.
    //
    // SAFE TO SNAPSHOT, and this is worth stating because it is the inverse of
    // the unk_339 trap the plan warns about: unk_339 is cleared at the END of the
    // Update (:2236-2238), so a post-update snapshot always reads 0. unk_312 is
    // cleared at the START (:2042), before actionFunc runs, so a post-update
    // snapshot reads exactly what this frame's action function set.
    x["tr"] = (int32_t)g->unk_312;

    // unk_31A / unk_31C (0x31A / 0x31C, s16). W :1556 / :1551.
    // R OverrideLimbDraw:2578-2579 — limb 15 is the head, and these are its yaw
    // and roll. Without them the mirror's Ganon never looks at anything.
    x["hd"] = { g->unk_31A, g->unk_31C };

    // unk_324 (0x324, f32). W the Math_ApproachF family at :410/:1224/:1387/:1467
    // and `= 0.0f` at :1554. R func_80904108:2478 (the gate) and :2486 (alpha) —
    // the ring of fire round the arena. Its damage zone is read at :1928, inside
    // the suppressed Update, so streaming it restores the SIGHT of the ring but
    // not its hitbox; that half belongs with the per-element hit accounting work.
    x["fr"] = R3(g->unk_324);

    // The Master Sword. It is sBossGanon2Particles[0] -- a global, not an
    // actor, so nothing replicates it, and the particle update runs from
    // BossGanon2_Update which is suppressed on the mirror.
    //
    // What this fixes is a DRIFT, not an absence, and the first version of this
    // comment claimed the opposite -- that P2 "could not walk to a blade he could
    // not see". Wrong: both func_808FD210 and the unk_2E = 1 plant are inside
    // func_808FD5F4, the introduction, which C3.2 makes every client run for
    // itself. The mirror therefore forges its own blade in slot 0 and draws it.
    // What it cannot do is keep it in step afterwards, since its update stops: any
    // divergence in where the blade came to rest is frozen in. Streaming the
    // placement keeps both copies on the same spot, which is what the pickup test
    // then measures against. (The commit message got this right; the comment did
    // not.)
    //
    // Only the fields the Draw needs: `type`, where, and which way up. NOT how big
    // -- the type 1 draw hardcodes Matrix_Scale(0.03f) and never reads
    // effect->scale, which only type 2 particles use. `scale` travels anyway
    // because the receiver writes the whole record; it is three bytes of nothing.
    // Sent only while the sword exists, which is the window between Ganon knocking
    // it out of Link's hands and someone picking it up.
    //
    // Why clearing slot 0 on the mirror is safe -- and the original reasoning for
    // this was also wrong. It is NOT that nothing else claims slot 0: func_808FD27C
    // hands out the first free slot starting at 0. What makes it safe is that the
    // two windows are disjoint. DeserializeExtras is only reached past the
    // ShouldMirror gate (EnemySync.cpp:1759), and that gate keeps the mirror local
    // for the whole introduction -- which is the only time this overlay fills the
    // particle pool. When we deserialize, its update is not running; when its
    // update runs, we do not deserialize. Move DeserializeExtras ahead of that gate
    // and this breaks.
    //
    // The other half of the Master Sword work is in the overlay: the pickup
    // proximity test runs against Anchor_NearestPlayerToPos, so EITHER player
    // can reach it.
    {
        // "msg": the blade has been recovered. Not unk_334 as state -- its
        // consequence, handing each client's own Link his sword back. See
        // BossGanon2_MirrorGiveMasterSword.
        if (g->unk_334 != 0) {
            x["msg"] = true;
        }

        const BossGanon2Effect* sword = &sBossGanon2Particles[0];
        if (sword->type != 0) {
            x["ms"] = { (int32_t)sword->type,   R3(sword->position.x), R3(sword->position.y),
                        R3(sword->position.z),  (int32_t)sword->unk_2E, R3(sword->scale),
                        R3(sword->unk_38.x),    R3(sword->unk_38.y),    R3(sword->unk_38.z) };
        }
    }

    // The lightning, all four or none — the group is gated in func_80904340:2508
    // on (unk_330 != 0 || unk_328 != 0) and the whole block is skipped otherwise.
    // W Update:2157-2160 in one burst. NOTE that func_80904340 is a DRAW function
    // that also WRITES unk_330 (:2510), unk_328 (:2512-2516) and unk_32C (:2519):
    // the decay runs on both machines, and re-applying the cached extras each
    // frame simply holds the mirror at the last streamed value until the next
    // packet. Sent only while the gate is open; the receiver closes the gate
    // itself when the key is absent, so a strike cannot get stuck lit.
    if ((g->unk_330 != 0) || (g->unk_328 != 0)) {
        x["lt"] = { (int32_t)g->unk_328, (int32_t)g->unk_330, (int32_t)g->unk_340, R3(g->unk_32C) };
    }

    // unk_342 (0x342, s16). W :1971/:1997, decremented at Update:2055.
    // R OverrideLimbDraw:2586 and BossGanon2_Draw:2836 — the red flash when he is
    // hit. Combat feedback: without it P2 lands a blow and nothing acknowledges
    // it. Same field, same role as unk_1A6 on phase 1.
    x["dm"] = g->unk_342;

    // The tail's three DRAW inputs (its hit accounting is NOT here).
    // unk_346 (0x346, s16) — W the Math_ApproachS at Update:2089/:2099/:2103,
    //   R OverrideLimbDraw:2583, the tail's overall lift.
    // unk_348[5] / unk_352[5] (0x348 / 0x352, s16[5]) — W Update:2124-2125,
    //   R ONLY through the overlay's deliberate overrun of unk_2F4[42..46] and
    //   unk_2FE[42..46] at OverrideLimbDraw:2583-2584 (0x2F4 + 42*2 == 0x348 and
    //   0x2FE + 42*2 == 0x352 — confirmed against the struct). That indexing is
    //   vanilla and is NOT touched here; only the two arrays travel.
    // Frozen at 0 the tail hangs straight and dead.
    x["tl"] = g->unk_346;
    x["t1"] = { g->unk_348[0], g->unk_348[1], g->unk_348[2], g->unk_348[3], g->unk_348[4] };
    x["t2"] = { g->unk_352[0], g->unk_352[1], g->unk_352[2], g->unk_352[3], g->unk_352[4] };

    // --- The three end-of-fight effects -----------------------------------
    // All three are written only by the death cutscene func_8090120C, and the
    // first two are the sBossGanon2Zelda null-deref trap documented in the file
    // header. Gated on their own draw gates so they cost nothing during the
    // fight; the receiver zeroes them when the key is absent.

    // unk_37C / unk_380 (Zelda's beam) and unk_384 / unk_388 (the closing orb) are
    // NOT SENT, and this is a deliberate retreat from the first version of this file.
    //
    // They are the two floats that open the sBossGanon2Zelda dereference gates in the
    // Draw (func_80905674:2788 -> :2798-2800, func_80904FC8:2706 -> :2711-2712). The
    // first version sent them behind a `sBossGanon2Zelda != NULL` check in
    // DeserializeExtras. Review took that apart on two counts, both demonstrated:
    //
    //  1. The zeroing branch only runs if DeserializeExtras runs at all, and three
    //     returns upstream skip it FOR GOOD — becoming the authority
    //     (EnemySync.cpp:1683), a stale stream after 30 ticks / 1.5 s (:1714-1718),
    //     and ShouldMirror going false (:1759-1764). After any of those, the last
    //     received value stays > 0, nothing local ever writes these fields again
    //     (only the death cutscene func_8090120C does), and the Draw walks into the
    //     dereference every frame from then on.
    //  2. `!= NULL` is not a liveness test. The pointer is cleared only by
    //     BossGanon2_Reset:3161, an ActorResetFunc — but the En_Zl3 it points at is
    //     spawned with params 1 and takes the "inFinal2" branch, whose demo
    //     dispatcher calls Actor_Kill on itself (z_en_zl3.c:1359) WITHOUT clearing
    //     the pointer. The guard passes on a freed actor, which is exactly the
    //     end-of-fight case it was meant to cover.
    //
    // Both fields belong to the closing cutscene, which has to be re-measured at
    // the bench anyway (the plan's account of the death was already wrong once,
    // there). Not sending them leaves P2 without those two effects — which is where
    // he already is — and costs nothing else. Whoever finishes the endgame work must
    // close the gates in EndSuppression and test liveness, not nullness, before
    // sending them.

    // unk_38C (0x38C, f32). W the Math_ApproachF at :1660/:1662.
    // R func_8090523C:2732 (gate) and :2737 (alpha) — the Triforce flash. This one
    // reads GET_PLAYER(play)->mf_9E0 (:2744), the LOCAL Link, which always exists:
    // no guard needed, and it means each client sees the flash round its own Link.
    if (g->unk_38C > 0.0f) {
        x["tz"] = R3(g->unk_38C);
    }
}

// The arena's ring of fire, replayed on the mirror.
//
// Bench-observed 2026-09-13: P2 walked straight out through the flames. Review had
// predicted exactly this — streaming unk_324 restores the ring's LOOK (func_80904108
// draws it from the Draw, which runs everywhere) but not its BITE, because the hurt
// test lives at the tail of func_80902348 (z_boss_ganon2.c:1928-1941), reachable only
// from the Update, which is suppressed on the mirror. The ring was scenery for P2.
//
// Re-run of the vanilla test, value for value: centre (-200, -200), radius 784, set
// the body flame timers, light the player and knock him back along the outward angle
// with the same magnitude and the same 0x10 flag.
//
// One deliberate omission: vanilla also does `sBossGanon2Zelda->unk_3C8 = 8` here, so
// that Zelda reacts. That is the pointer this file refuses to touch on a mirror — see
// the long note in SerializeExtras about why `!= NULL` is not a liveness test. The
// effect is Zelda flinching; the cost of getting it wrong is a crash. Left out.
//
// Runs from DeserializeExtras, i.e. once per applied packet, which is the authority's
// own Update cadence. When the mirror is NOT suppressed (the whole introduction, per
// ShouldMirror) its own Update runs this test itself, so there is no double burn.
static void BossGanon2_MirrorFireRing(BossGanon2* g) {
    if (g->unk_324 <= 0.0f || gPlayState == NULL) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL) {
        return;
    }
    f32 dx = -200.0f - player->actor.world.pos.x;
    f32 dz = -200.0f - player->actor.world.pos.z;
    if (sqrtf(SQ(dx) + SQ(dz)) <= 784.0f) {
        return;
    }
    for (size_t j = 0; j < ARRAY_COUNT(player->bodyFlameTimers); j++) {
        // bodyFlameTimers is u8[]; Rand_S16Offset(0, 200) yields 0..199, so the
        // narrowing is exact. Cast to say so rather than let C4244 imply a bug.
        player->bodyFlameTimers[j] = (u8)Rand_S16Offset(0, 200);
    }
    player->bodyIsBurning = true;
    func_8002F6D4(gPlayState, &g->actor, 10.0f, Math_Atan2S(dz, dx), 0.0f, 0x10);
}

// The half of the Master Sword work the plan left open: who ends up HOLDING it.
//
// Ganon knocks the blade out of Link's hands during the introduction, and the
// introduction runs LOCALLY on every client (C3.2) -- so both Links really are
// disarmed, symmetrically. Getting it back is not symmetric: that happens in the
// pickup sequence (:1411-1441), driven by BossGanon2_Update, which the mirror
// does not run. So P2 was left permanently swordless, and since the finishing
// blow demands PLAYER_IA_SWORD_MASTER in hand (:1732), he could never end the
// fight even once the endgame handoff gave him the right to.
//
// unk_334 itself stays off the wire, for the reason the file header gives: the
// mirror's only reader of it is a suppressed Update. What travels is its
// CONSEQUENCE, which is the shape this file already uses for unk_318 and the
// lightning group.
//
// The restitution goes through the game's own two calls, the ones behind the
// "this is when link picks up the sword in the Ganon fight" comment in
// z_player.c:func_8085283C -- Item_Give then func_80846720(play, player, 0).
//
// The randomizer guard below is NOT redundant, and the first version of this
// function got that badly wrong: its comment claimed func_80846720 handled the
// shuffled-Master-Sword case. It does not. Read in full (z_player.c:10705-10721)
// it contains no randomizer test at all -- it reads D_808546F0[linkAge] and
// equips unconditionally. Item_Give does not filter either (z_parameter.c:1956
// and :1979 set both the owned flag and buttonItems[0]; only sticks and nuts are
// gated). The real vanilla guard is hasFoundMasterSword, a per-client file static
// computed once in BossGanon2_Init (:196-200), which makes the pickup and the
// draw skip out entirely on a shuffled seed -- and this path does not go through
// either of them.
//
// Without the guard: a seed where P1 found the Master Sword and P2 did not would
// hand P2 a blade the seed refuses him, duplicating the check instead of moving
// it, and leaving INFTABLE_SWORDLESS set beside an equipped sword.
// The work itself is in the overlay, BossGanon2_CoopGiveMasterSword, and it has
// to be: the only correct guard is hasFoundMasterSword, a file static of
// z_boss_ganon2.c computed before anyone is disarmed. This function tried to
// carry its own guard first and got it wrong -- CHECK_OWNED_EQUIP read here is
// false for EVERY client, because the introduction's Inventory_DeleteEquipment
// has already XORed the owned bit away, so a shuffled seed left both Links
// permanently swordless in front of a finishing blow neither could land.
static void BossGanon2_MirrorGiveMasterSword(BossGanon2* g) {
    if (gPlayState == NULL) {
        return;
    }
    // unk_334 is the mirror's own latch, and nothing local writes it while its
    // update is suppressed, so it doubles as the "already done" marker: no extra
    // per-actor state to carry, and the restitution runs exactly once. Set before
    // the call so a withheld blade consumes the event too.
    g->unk_334 = 1;
    BossGanon2_CoopGiveMasterSword(gPlayState);
    ESYNC_LOG("[BossGanon2Sync] Master Sword handback played for the local Link");
}

static void BossGanon2_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossGanon2* g = (BossGanon2*)actor;
    try {
        // --- C3.1, first: everything else is drawn through the model it picks ---
        if (x.contains("m")) {
            int32_t model = x["m"].get<int32_t>();
            // BossGanon2_Draw:2822 switches on 0/1/2 with no default; a wire value
            // outside that draws nothing at all. Clamp to the case set.
            if (model < 0 || model > 2) {
                model = 0;
            }
            // The 0 -> non-zero edge, ONCE. The local field is only ever written
            // here while this copy is mirrored, so it is a reliable latch. Any
            // non-zero value takes the edge, not just the 2 of :600: a client
            // whose own introduction was skipped receives 1 directly.
            if (model != 0 && g->unk_337 == 0) {
                if (BossGanon2_SwitchToBeastSkeleton(g)) {
                    g->unk_337 = (uint8_t)model;
                }
                // else: banks not resident yet — leave unk_337 at 0 so the next
                // frame takes the edge again. Never assign ahead of the skeleton.
            } else {
                g->unk_337 = (uint8_t)model;
            }
        }

        if (x.contains("tc")) g->unk_19C = x["tc"].get<int16_t>();
        if (x.contains("ta")) g->unk_1B0 = x["ta"].get<float>();
        if (x.contains("tf")) g->unk_1B4 = x["tf"].get<float>();
        if (x.contains("sw") && x["sw"].size() == 2) {
            g->unk_224 = x["sw"][0].get<float>();
            g->unk_228 = x["sw"][1].get<float>();
        }
        if (x.contains("ob")) g->unk_30C = x["ob"].get<float>();
        if (x.contains("ey")) {
            // CRASH GUARD: BossGanon2_Draw:2832 does
            //     gSPSegment(..., SEGMENTED_TO_VIRTUAL(sEyeTextures[this->unk_310]))
            // with no bounds check. sEyeTextures has three entries.
            int32_t eye = x["ey"].get<int32_t>();
            if (eye < 0 || eye >= GANON2_EYE_TEX_COUNT) {
                eye = 0;
            }
            g->unk_310 = (uint8_t)eye;
        }
        if (x.contains("tr")) {
            // PostLimbDraw:2651/:2656 dispatch on == 1 and == 2 and index
            // unk_444.elements[0] / [1] accordingly; Draw:2860 only tests != 0.
            // Clamp so a wire value can never light the trail without also
            // selecting one of the two sword limbs it belongs to.
            int32_t trail = x["tr"].get<int32_t>();
            if (trail < 0 || trail > 2) {
                trail = 0;
            }
            g->unk_312 = (uint8_t)trail;
        }
        if (x.contains("hd") && x["hd"].size() == 2) {
            g->unk_31A = x["hd"][0].get<int16_t>();
            g->unk_31C = x["hd"][1].get<int16_t>();
        }
        if (x.contains("fr")) {
            g->unk_324 = x["fr"].get<float>();
            BossGanon2_MirrorFireRing(g);
        }

        // The blade has been recovered on the authority -- by EITHER player, since
        // the overlay's pickup test now accepts the nearest of them. Hand this
        // client's Link his sword back, once.
        if (x.value("msg", false) && g->unk_334 == 0) {
            BossGanon2_MirrorGiveMasterSword(g);
        }

        // The Master Sword particle. Absent means "not lying on the ground
        // right now", so clear it rather than leave a ghost blade planted forever --
        // the mirror's own update never runs and would never clear it by itself.
        if (x.contains("ms") && x["ms"].size() == 9) {
            BossGanon2Effect* sword = &sBossGanon2Particles[0];
            int32_t type = x["ms"][0].get<int32_t>();
            // Only type 1 is the sword; anything else off the wire would put an
            // unrelated particle behaviour on slot 0, which the draw reads blind.
            sword->type = (type == 1) ? 1 : 0;
            sword->position.x = x["ms"][1].get<float>();
            sword->position.y = x["ms"][2].get<float>();
            sword->position.z = x["ms"][3].get<float>();
            sword->unk_2E = (s16)x["ms"][4].get<int32_t>();
            sword->scale = x["ms"][5].get<float>();
            sword->unk_38.x = x["ms"][6].get<float>();
            sword->unk_38.y = x["ms"][7].get<float>();
            sword->unk_38.z = x["ms"][8].get<float>();
        } else {
            sBossGanon2Particles[0].type = 0;
        }

        if (x.contains("lt") && x["lt"].size() == 4) {
            g->unk_328 = x["lt"][0].get<int16_t>();
            g->unk_330 = x["lt"][1].get<int16_t>();
            g->unk_340 = x["lt"][2].get<int16_t>();
            g->unk_32C = x["lt"][3].get<float>();
        } else {
            // Key absent = the authority's strike is over. Close the gate of
            // func_80904340:2508 ourselves; otherwise the last streamed values
            // would be re-applied every frame and the bolts would never go out.
            g->unk_328 = 0;
            g->unk_330 = 0;
        }

        if (x.contains("dm")) g->unk_342 = x["dm"].get<int16_t>();
        if (x.contains("tl")) g->unk_346 = x["tl"].get<int16_t>();
        if (x.contains("t1") && x["t1"].size() == ARRAY_COUNT(g->unk_348)) {
            for (size_t i = 0; i < ARRAY_COUNT(g->unk_348); i++) {
                g->unk_348[i] = x["t1"][i].get<int16_t>();
            }
        }
        if (x.contains("t2") && x["t2"].size() == ARRAY_COUNT(g->unk_352)) {
            for (size_t i = 0; i < ARRAY_COUNT(g->unk_352); i++) {
                g->unk_352[i] = x["t2"][i].get<int16_t>();
            }
        }

        // -------------------------------------------------------------------
        // The crash trap: sBossGanon2Zelda is dereferenced by TWO DRAW paths.
        // -------------------------------------------------------------------
        //     func_80905674:2798-2800   reached when unk_380 > 0.0f
        //     func_80904FC8:2711-2712   reached when unk_384 > 0.0f
        // Neither has a null check, and the Draw runs on every client whether or
        // not the actor is mirrored. sBossGanon2Zelda is assigned only at :244,
        // inside the introduction, and BossGanon2_Reset:3161 clears it.
        //
        // C3.2 is what normally guarantees the pointer: the introduction runs
        // locally on every client, so every client spawns its own En_Zl3 (which
        // is ACTORCAT_NPC and therefore never tracked — checked). This guard does
        // not rely on that: these two floats are the ONLY way a value off the
        // wire can open those gates, so they are the right place to stop it.
        // "zb" and "zo" are no longer sent — see the long note in SerializeExtras.
        // These fields are still forced to 0 on every applied packet, because that is
        // what keeps the two Draw gates shut for a mirror that never runs the death
        // cutscene locally. It is cheap and it is the invariant, not a leftover.
        g->unk_37C = 0.0f;
        g->unk_380 = 0.0f;
        g->unk_384 = 0.0f;
        if (x.contains("tz")) {
            g->unk_38C = x["tz"].get<float>();
        } else {
            g->unk_38C = 0.0f;
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[BossGanon2Sync] extras parse error: {}", ex.what());
    }
}

// Authority side. Reads actionFunc, which is allowed here and nowhere else: the
// pointer is compared locally and only the resulting uint8_t is streamed — the
// same thing PhantomGanon_GetPhase (PhantomGanonAdapter.cpp:95) and
// BossGanon_GetPhase do.
//
// func_808FD5F4 is the introduction and ONLY the introduction: the single
// assignment `actionFunc = func_808FD5F4` is at :218, in func_808FD5C4, which is
// called once from BossGanon2_Init:185. (func_808FD5C4 itself is never an
// actionFunc value, so there is nothing else to compare against.)
static uint8_t BossGanon2_GetPhase(Actor* actor) {
    BossGanon2* g = (BossGanon2*)actor;
    // DEFEATED is "the ending has actually started", not "health reached 0", and
    // the difference is load-bearing in two ways.
    //
    // Too early: health hits 0 before the ending in co-op, because the debt
    // fallback (EnemySync.cpp:2787) writes it directly. The edge is consumed once
    // and only once (EnemySync.cpp:1745), so a DEFEATED sent while the authority
    // is still fighting releases the mirror for good -- st.dying, remoteStates
    // erased -- and leaves two Ganons diverging for the rest of the fight.
    //
    // Never: colChkInfo.health is a u8 while the fight compares it signed
    // (:2048, `temp_v0_4 <= 0`). Two damage off a health of 1 leaves 255, which
    // the overlay treats as dead and `== 0` does not see at all.
    //
    // func_8090120C is assigned in exactly one place, func_80901020:1492, which is
    // the only way into the ending -- so this is precisely "the ending is running",
    // and it stays true for the rest of the fight. Reading actionFunc here is
    // allowed: it is compared locally and only the resulting uint8_t is streamed.
    if (g->actionFunc == func_8090120C) {
        return GANON2_PHASE_DEFEATED;
    }
    if (g->actionFunc == func_808FD5F4) {
        return GANON2_PHASE_INTRO;
    }
    return GANON2_PHASE_FIGHT;
}

// C3.2. Mirror side, evaluated locally every frame. Both halves are load-bearing,
// exactly as on phase 1:
//   - the streamed phase keeps us local while the AUTHORITY is still in the
//     introduction, so we never start mirroring a Ganon who has not transformed;
//   - our own actionFunc keeps us local while WE are still in it, because the two
//     clients do not reach the room on the same frame and ours may lag — or may
//     be entering long after the authority, in which case it plays the whole
//     introduction to itself.
//
// This is not cosmetic. func_808FD5F4 is a csState machine driven by this actor's
// own Update; suppress it and the machine stops, which on Boss_Ganon was measured
// at the bench to leave P2 inside a cutscene he could not leave. It is also what
// spawns this client's En_Zl3 (:244) and what performs its own skeleton switch
// (:602-603), so the C3.1 recovery path above is the exception rather than the
// rule.
static bool BossGanon2_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossGanon2* g = (BossGanon2*)actor;
    if (streamedPhase == GANON2_PHASE_INTRO) {
        return false;
    }
    // The authority's ending is running. Normally the defeat handoff released us
    // long ago and we are never asked again -- it does remoteStates.erase and
    // st.dying, and EnemySync.cpp:1697 returns early from then on. So reaching
    // here means the handoff REFUSED, which happens for exactly one reason: our
    // own sBossGanon2Zelda was still null, because our introduction had not
    // spawned her yet. That is the late joiner, someone who spawns Ganon2 during
    // the ending; the phase edge was consumed before OnPhaseChange was even called
    // (EnemySync.cpp:1745), so no second edge will ever come.
    //
    // This is the retry the refusal needs, and it is here because ShouldMirror is
    // the one thing re-evaluated every frame with this information to hand.
    //
    // The first version of this block just returned false -- release the mirror,
    // let the local AI run -- with a comment claiming that was "the same answer
    // the handoff gives everyone else". It was not. The handoff STARTS the ending
    // (func_80901020); releasing alone un-suppresses an actor whose actionFunc is
    // frozen wherever the suppression caught it, mid-fight, so the late joiner
    // would fight a whole Ganon nobody else is fighting while everyone else is in
    // the Chamber of the Sages. It also silently overruled OnPhaseChange's own
    // "staying mirrored" refusal, whose log line then said the opposite of what
    // happened.
    if (streamedPhase == GANON2_PHASE_DEFEATED) {
        if (g->actionFunc == func_8090120C) {
            return false; // ending already running here, keep running it
        }
        if ((sBossGanon2Zelda != NULL) && (g->actionFunc != func_808FD5F4) && (gPlayState != NULL)) {
            actor->colChkInfo.health = 0;
            actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
            func_80901020(g, gPlayState);
            ESYNC_LOG("[BossGanon2Sync] defeat handoff retried (missed the phase edge)");
            return false;
        }
        // Zelda still absent, or we are mid-introduction: keep mirroring, which is
        // precisely what OnPhaseChange chose when it refused. The ending
        // dereferences her from end to end; a frozen pose beats a crash.
        return true;
    }
    return g->actionFunc != func_808FD5F4;
}

// The endgame. Measured in a real fight on 2026-09-13, and it is the same
// defect as Ganondorf's death was: once Ganon is struck down, P2 saw NOTHING --
// not Zelda's beam of light, not the seven sages -- and stayed standing in the
// arena while the credits rolled on P1's screen.
//
// The cause is not a missing packet. The entire ending is func_8090120C, a
// csState machine reached only through func_80901020, and BOTH are driven by
// BossGanon2_Update, which is suppressed on the mirror. The scene change out of
// the arena is inside it (z_boss_ganon2.c:1872, nextEntranceIndex =
// ENTR_CHAMBER_OF_THE_SAGES_0), so the mirror never even leaves the room. No
// amount of streaming fixes that: the ending has to be handed back to each
// client's own AI, exactly as C2.4 did for phase 1.
//
// The null guard is the load-bearing line. sBossGanon2Zelda is dereferenced
// throughout the cutscene, and the crash trap's whole lesson was that this pointer is the
// trap of this file. It is spawned by the introduction (:251), which C3.2 makes
// every client run for itself, so it should be set here -- but "should" is what
// the plan said about several things that were measured false. If it is null we
// keep mirroring: P2 then sees the streamed pose and no ending, which is today's
// behaviour, rather than a crash.
//
// There is no second chance -- EnemySync consumes the phase edge before calling
// us (EnemySync.cpp:1744) -- so everything this needs must be true on the frame
// the authority's health reaches 0. Returning true erases the remote state and
// ends suppression for good (:1748-1754); ShouldMirror is not consulted again.
static bool BossGanon2_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    if (toPhase != GANON2_PHASE_DEFEATED) {
        return false;
    }
    BossGanon2* g = (BossGanon2*)actor;

    if (sBossGanon2Zelda == NULL) {
        ESYNC_LOG("[BossGanon2Sync] defeat handoff REFUSED: no local En_Zl3, staying mirrored");
        return false;
    }
    if (g->actionFunc == func_8090120C) {
        // Already in the ending (we were never suppressed, or ran the whole
        // fight locally). Release, but do not restart the animation.
        ESYNC_LOG("[BossGanon2Sync] defeat handoff: already in the ending");
        return true;
    }

    actor->colChkInfo.health = 0;
    actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    func_80901020(g, gPlayState);

    // No "did the setup take?" check here, unlike phase 1's handoff. There it was
    // load-bearing: BossGanon_SetupDeathCutscene is conditional on
    // OBJECT_GANON_ANIME2 and quietly does nothing otherwise. func_80901020 has no
    // such condition -- it assigns actionFunc = func_8090120C unconditionally
    // (z_boss_ganon2.c:1492) -- so the equivalent test could never fire, and a
    // guard that can never fire reads like protection that is not there.
    ESYNC_LOG("[BossGanon2Sync] defeat handoff (func_80901020 ran locally)");
    return true;
}

void RegisterBossGanon2Adapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = BossGanon2_SerializeExtras;
    adapter.DeserializeExtras = BossGanon2_DeserializeExtras;
    adapter.GetPhase = BossGanon2_GetPhase;
    adapter.ShouldMirror = BossGanon2_ShouldMirror;
    // ovl_Boss_Ganon2 contains no Item_Drop: a generic roll would invent treasure
    // the fight does not give.
    adapter.ownsDeathDrop = true;
    adapter.OnPhaseChange = BossGanon2_OnPhaseChange;
    // NOT set, each for a stated reason (see the file header): customColliderPos
    // (JntSph, not cylinders), IsHealthless, ReplicateKill, ownsSfx,
    // mirrorWhileDrawless.
    EnemySync::RegisterAdapter(ACTOR_BOSS_GANON2, adapter);
}
