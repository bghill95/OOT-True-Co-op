#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/OTRGlobals.h"

#include <cmath>
#include <vector>

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Ganon/z_boss_ganon.h"
// File-scope (non-static) in the overlay; not exported by its header.
void BossGanon_SetColliderPos(Vec3f* pos, ColliderCylinder* collider);
// The two action functions that make up the introduction. Compared by ADDRESS and
// only ever locally — a function pointer never goes on the wire.
void BossGanon_SetupIntroCutscene(BossGanon* self, PlayState* play);
void BossGanon_IntroCutscene(BossGanon* self, PlayState* play);
// The defeat sequence, handed back to each client's own AI by OnPhaseChange.
void BossGanon_SetupDeathCutscene(BossGanon* self, PlayState* play);
}

// ---------------------------------------------------------------------------
// Phases (C2.3)
// ---------------------------------------------------------------------------
// Measured on the bench, 2026-09-12, and it is why this is not a cosmetic commit:
// with the mirror suppressed through the introduction, P2 ENTERS the cutscene and
// never leaves it. Boss_Ganon drives that cutscene itself — BossGanon_IntroCutscene
// is a state machine on csState (z_boss_ganon.c:536, switch at :555) that ends by
// calling BossGanon_SetupWait at :1159 — so with its Update suppressed the machine
// never advances. Link is left in the pose and position the cutscene put him in,
// under the floor, and twelve injected A presses changed nothing. The window only
// unlocked when the player was warped out by hand.
//
// The fix is the one Phantom Ganon already uses: the introduction runs LOCALLY on
// every client, and mirroring starts once both sides are past it.
//
// Side effect that settles a second bench observation: BossGanon_SetupWait also
// does `sBossGanonCape->minY = 2.0f` (:2255). The cape is not a field of BossGanon
// at all, it is a separate actor (ACTOR_EN_GANON_MANT, spawned as a child at :383
// and held in the file static sBossGanonCape), configured exclusively through that
// static BY GANONDORF'S UPDATE — :546-549, :892, :1102. A suppressed Update means
// nobody ever configures it, which is why the mirror's Ganondorf had no cape.
// Running the introduction locally hands the mirror its own configured cape.
enum {
    GANON_PHASE_INTRO = 0,
    GANON_PHASE_FIGHT = 1,
    GANON_PHASE_DEFEATED = 2,
};

/**
 * Ganondorf (Boss_Ganon, phase 1 of the final fight).
 *
 * ONE actorId multiplexes SIX actors. BossGanon_Init (z_boss_ganon.c:347, :390)
 * rewrites update/draw from params:
 *
 *   (s16)params <  0x64   Ganondorf himself           health 40
 *   0x64 - 0xC7           the tennis light ball       health 0
 *   0xC8 - 0xF9           reconverging shards         health 0
 *   0xFA - 0x103          the big-magic charge balls  health 0
 *   >= 0x104              the big magic, thrown       health 0
 *   0x12C / 0x190         terminal shards             health 0
 *
 * SIGNEDNESS MATTERS. The two placed instances are ganon_boss room 0 with
 * params 0xFFFF and ganon_final room 0 with params 0x0001, read out of the
 * scene data. 0xFFFF is -1 as the s16 the overlay compares,
 * so the arena Ganondorf passes `params < 0x64` only under a SIGNED read. Every
 * params test in this file therefore goes through GanondorfParams(), which
 * reads actor->params as s16 exactly like the overlay does. Reading it as
 * uint16_t (as TrackedState::spawnParams does) turns the real boss into a
 * projectile variant.
 *
 * What this adapter does today:
 *
 *   - GetPhase / ShouldMirror (C2.3) and OnPhaseChange (C2.4). See the Phases
 *     block at the top of the file and the handoff just above RegisterBossGanonAdapter;
 *     both are there because of what the bench measured on 2026-09-12, not because
 *     the plan asked for them in that order — it put them AFTER the draw fields, and
 *     the measurement says they come first: without ShouldMirror(INTRO) P2 cannot
 *     play the fight at all.
 *
 *   - customColliderPos. BossGanon_Update:2938 puts the body cylinder on the
 *     TORSO LIMB (BossGanon_SetColliderPos(&unk_1FC, &collider)), not on
 *     world.pos. Without this flag the generic mirror runs
 *     Collider_UpdateCylinder (EnemySync.cpp:1367) and the cylinder sits a
 *     torso-height off. unk_1FC needs nothing on the wire: PostLimbDraw:3328
 *     writes it from limb 2 and the Draw runs on every client.
 *
 *   - the "Update writes it / Draw reads it" fields — the four groups sent by
 *     BossGanon_SerializeExtras below.
 *     On a mirror the tracked actor's Update is suppressed and its Draw is not,
 *     so each of these is frozen at the 0 that Actor_Spawn's memset left
 *     (z_actor.c:3403).
 *
 * DELIBERATELY ABSENT, and not an oversight:
 *
 *   - fwork[], triforceType and the rest of the cutscene fields. fwork is only
 *     safe AFTER ShouldMirror(INTRO) is false — which is now the case (see the
 *     Phases block above), so it becomes eligible in the next commit rather than
 *     being ruled out; the sBossGanonZelda guard below still has to survive it.
 *   - legRot (0x050C, Vec3f). Frozen at 0 on the mirror, so the six
 *     additions in BossGanon_OverrideLimbDraw:3270-3298 all contribute nothing
 *     and his legs do not sway. Purely cosmetic, and it is a Vec3f every tick
 *     for a gait: left off until the cheaper fields have been seen in play.
 *     Listed here because every one of the 24 fields deserves a line, and this
 *     was the only one with none.
 *   - mirrorWhileDrawless: Ganondorf never nulls his own draw.
 *   - ownsSfx: there is no BossGanon_CoopMirrorFx path in the overlay yet, so
 *     the generic per-actor sfx stream is what gives the mirror his voice.
 *   - IsHealthless: Ganondorf carries health 40 and the five healthless
 *     variants are kept out of tracking altogether (EnemySync.cpp
 *     IsTrackingExcluded), so a callback here would be dead weight. NOTE that
 *     IsHealthless could not have done that job anyway: it feeds only the spawn
 *     announce (EnemySync.cpp:2259) and OnEnemyActorKill (:2796), NEVER the
 *     leftStream clause (:2226) that decides the regime.
 *
 * NOT ON THE WIRE ON PURPOSE — written by BossGanon_PostLimbDraw / BossGanon_Draw,
 * which run on the mirror too, so they are already right there: unk_1FC (:3328),
 * unk_208 (:3334), unk_214, unk_220, unk_22C (:3342), unk_238 (:3340),
 * unk_260 (:3345, refreshed every frame because unk_25C is zeroed at :3348 and
 * only BossGanon_PlayTennis ever sets it), unk_2EC[] (:3324 and Draw:3874),
 * triforcePos (:3351) and focus.pos (:3330).
 */

// ---------------------------------------------------------------------------
// Crash trap 2: actor->parent
// ---------------------------------------------------------------------------
// BossGanon_LightBall_Update:3946/3950, func_808E1EB4:4262 and
// func_808E2544:4382/4388 all dereference this->actor.parent on their FIRST
// update, unguarded — the En_Bb shape documented at EnemySync.cpp:493-499, and
// mirror-side parent resolution is best-effort (EnemySync.cpp:2451-2459).
// The five variants are therefore kept out of tracking entirely
// (EnemySync.cpp IsTrackingExcluded), which is also what keeps them out of the
// stream now that GetAdapter(ACTOR_BOSS_GANON) is non-null: the leftStream
// clause is `health == 0 && GetAdapter(id) == nullptr && !projectile`
// (EnemySync.cpp:2226), so merely registering this adapter would otherwise
// have flipped all five from "never streamed" to "streamed and suppressed" in
// one step — the exact regression that keeps RegisterBossTwAdapter empty
// (BossTwAdapter.cpp:130-145).
// Every callback below re-checks params anyway, so that lifting the tracking
// exclusion later (lot 2.7 wants the tennis ball) cannot silently feed a
// variant through Ganondorf's field set.

static inline int16_t GanondorfParams(Actor* actor) {
    return (int16_t)actor->params;
}

static inline bool BossGanon_IsDorf(Actor* actor) {
    return GanondorfParams(actor) < 0x64;
}

// Emit-side rounding, same reasoning as EnemySync's RoundedVec: a float
// promoted to double prints at double precision (0.4f -> "0.4000000059604645",
// 18 bytes for one scale). Three decimals is below anything visible on a scale
// factor, a radian or an 0-255 alpha, and it is emit-side only.
static inline double R3(float v) {
    return std::round((double)v * 1000.0) / 1000.0;
}

// Same, at centimetre precision, for a world position — EnemySync::RoundedVec
// is file-static so this mirrors it rather than exporting it.
static inline nlohmann::json R2Vec(const Vec3f& v) {
    nlohmann::json j;
    j["x"] = std::round((double)v.x * 100.0) / 100.0;
    j["y"] = std::round((double)v.y * 100.0) / 100.0;
    j["z"] = std::round((double)v.z * 100.0) / 100.0;
    return j;
}

static void BossGanon_SerializeExtras(Actor* actor, nlohmann::json& x) {
    if (!BossGanon_IsDorf(actor)) {
        return;
    }
    BossGanon* gd = (BossGanon*)actor;

    // unk_1A2 (0x1A2, s16). Written ONLY by BossGanon_Update:2898
    // (`this->unk_1A2++`), read by six draws: DrawShock:3452,
    // DrawHandLightBall:3479/3494, DrawBigMagicCharge:3521/3536/3542/3555/3561,
    // DrawDarkVortex:3650. One s16 unfreezes six texture scrolls.
    x["a2"] = gd->unk_1A2;

    // unk_1A6 (0x1A6, s16) — the damage flash. Draw:3866 tests `(unk_1A6 & 2) != 0`
    // and pushes Gfx_SetFog(255, 50, 0, ...), the red wash over Ganondorf when he is
    // hit. Armed to 15 by Update:2493 / :2767 / :2815 and decremented at :2916-2917,
    // so on a mirror it sits at 0 forever: today P2 lands a hit and Ganondorf does
    // not flinch visually at all. The plan filed this with the
    // cutscene fields; it does not belong there -- it is the only one of that group
    // that plays during the fight, and it is combat feedback, not decoration.
    //
    // Safe despite the field being multiplexed: in the five projectile variants
    // unk_1A6 is a ring index into unk_2EC[] / unk_3C4[] (:4275, :4400), an entirely
    // different meaning. Both ends of this adapter are behind BossGanon_IsDorf, and
    // the variants are excluded from tracking in EnemySync's IsTrackingExcluded, so
    // no variant ever reaches this code.
    x["df"] = gd->unk_1A6;

    // The ball he charges in his hand. handLightBallScale (0x254)
    // GATES the whole draw (DrawHandLightBall:3475), so at 0 the mirror sees an
    // empty hand wind up and throw nothing. unk_258 (0x258) is its spin
    // (Matrix_RotateZ, :3490). Written by PoundFloor:1971/1935,
    // PlayTennis:2391, SetupBlock:2438, ChargeLightBall:2340.
    x["hb"] = { R3(gd->handLightBallScale), R3(gd->unk_258) };

    // The big magic, sent as one block (the plan is right that
    // splitting it gives an effect at the world origin: unk_278 IS its centre).
    // unk_284 (0x284) gates DrawBigMagicCharge:3514 outright; unk_288 (0x288)
    // and unk_290 (0x290) are its two alphas (:3534/:3540, :3518); unk_28C
    // (0x28C) the light-fleck scale (:3525); unk_2D0 (0x2D0) the ball scale
    // (:3554); unk_278 (0x278, Vec3f) the centre every Matrix_Translate in the
    // function uses (:3523, :3530, :3552, :3560). Written by
    // BossGanon_ChargeBigMagic:2090-2099/2129-2137/2194-2209 and cleared by
    // BossGanon_SetupVulnerable:2573-2576.
    x["bm"] = { R3(gd->unk_284), R3(gd->unk_288), R3(gd->unk_28C), R3(gd->unk_290), R3(gd->unk_2D0) };
    x["bp"] = R2Vec(gd->unk_278);
    // unk_1AA (0x1AA) seeds BossGanon_InitRand at :3559, so without it the rays
    // land in a different arrangement on each machine; unk_1AC (0x1AC) is the
    // ray COUNT and bounds the loop at :3566. Both written by
    // SetupChargeBigMagic:2040-2041, unk_1AC incremented (and clamped to
    // ARRAY_COUNT(unk_294)) at ChargeBigMagic:2119-2120.
    x["bi"] = { gd->unk_1AA, gd->unk_1AC };
    // unk_294[15] (0x294, f32) are the per-ray alphas (:3571). Only the first
    // unk_1AC entries are ever read, and unk_1AC rides in "bi" on the same
    // frame, so a stale tail can never be drawn: send exactly what is read.
    if (gd->unk_1AC > 0) {
        std::vector<double> rays;
        int32_t n = gd->unk_1AC;
        if (n > (int32_t)ARRAY_COUNT(gd->unk_294)) {
            n = (int32_t)ARRAY_COUNT(gd->unk_294);
        }
        rays.reserve(n);
        for (int32_t i = 0; i < n; i++) {
            rays.push_back(R3(gd->unk_294[i]));
        }
        x["br"] = rays;
    }

    // The shocks, all four together or none. unk_2E6 (0x2E6) and
    // unk_2E8 (0x2E8) are the two timers that gate DrawShock:3413 (and pick
    // which of its two branches runs, :3419); both are decremented by
    // BossGanon_Update:2924-2930, so on a mirror they never expire either.
    // shockGlow (0x2E4, u8) picks glow-vs-square (:3443/:3449) and is written
    // by Update:2896 / Vulnerable:2603 / Damaged:2741.
    x["sk"] = { gd->unk_2E6, gd->unk_2E8, (int32_t)gd->shockGlow };
    // unk_49C[18] (0x49C, f32) is the per-sprite scale (Matrix_Scale at :3428
    // and :3441): at 0 the sprites exist but are infinitely small, i.e. the
    // shock is invisible. Written by Update:2981/2983 under exactly the same
    // unk_2E6/unk_2E8 gate, and read by nothing else, so sending it only while
    // that gate is open cannot leave a stale value on screen.
    //
    // Send only what the running branch reads, and it is not the whole array.
    // DrawShock has two: the unk_2E8 branch walks [0..17] against the player's
    // body parts (:3418-3428), the unk_2E6 branch -- the one live almost all the
    // time -- is `for (i = 1; i < 15; i++)` (:3436-3441). Index 0 is never even
    // written by the Update, whose own loop starts at 1 (:2978). And the window is
    // not short: z_boss_ganon.c:1490 sets unk_2E6 = 20000, which at 20 ticks/s is
    // up to a thousand seconds of open gate, so the difference is ~2.6 KB/s of
    // steady traffic for values nothing draws.
    if ((gd->unk_2E6 != 0) || (gd->unk_2E8 != 0)) {
        // 18 entries only while the body-part branch can run; 1..14 otherwise, and
        // the receiver copies back at the same offset so index 0 keeps its local 0.
        size_t first = (gd->unk_2E8 != 0) ? 0 : 1;
        size_t last = (gd->unk_2E8 != 0) ? ARRAY_COUNT(gd->unk_49C) : 15;
        std::vector<double> sparks;
        sparks.reserve(last - first);
        for (size_t i = first; i < last; i++) {
            sparks.push_back(R3(gd->unk_49C[i]));
        }
        x["sv"] = sparks;
        x["sf"] = (int32_t)first;
    }
}

static void BossGanon_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    if (!BossGanon_IsDorf(actor)) {
        return;
    }
    BossGanon* gd = (BossGanon*)actor;
    try {
        if (x.contains("a2"))
            gd->unk_1A2 = x["a2"].get<int16_t>();
        if (x.contains("df"))
            gd->unk_1A6 = x["df"].get<int16_t>();
        if (x.contains("hb") && x["hb"].size() == 2) {
            gd->handLightBallScale = x["hb"][0].get<float>();
            gd->unk_258 = x["hb"][1].get<float>();
        }
        if (x.contains("bm") && x["bm"].size() == 5) {
            gd->unk_284 = x["bm"][0].get<float>();
            gd->unk_288 = x["bm"][1].get<float>();
            gd->unk_28C = x["bm"][2].get<float>();
            gd->unk_290 = x["bm"][3].get<float>();
            gd->unk_2D0 = x["bm"][4].get<float>();
        }
        if (x.contains("bp"))
            gd->unk_278 = x["bp"].get<Vec3f>();
        if (x.contains("bi") && x["bi"].size() == 2) {
            gd->unk_1AA = x["bi"][0].get<int16_t>();
            int32_t rayCount = x["bi"][1].get<int32_t>();
            // Hard clamp: unk_1AC is the loop bound of DrawBigMagicCharge:3566
            // over unk_294[15]. The authority clamps it at :2119, but a value
            // that reaches us off the wire has not been through that code path,
            // and an over-long loop here reads past the struct field.
            if (rayCount < 0) {
                rayCount = 0;
            } else if (rayCount > (int32_t)ARRAY_COUNT(gd->unk_294)) {
                rayCount = (int32_t)ARRAY_COUNT(gd->unk_294);
            }
            gd->unk_1AC = (int16_t)rayCount;
        }
        if (x.contains("br") && x["br"].is_array()) {
            size_t n = x["br"].size();
            if (n > ARRAY_COUNT(gd->unk_294)) {
                n = ARRAY_COUNT(gd->unk_294);
            }
            for (size_t i = 0; i < n; i++) {
                gd->unk_294[i] = x["br"][i].get<float>();
            }
        }
        if (x.contains("sk") && x["sk"].size() == 3) {
            gd->unk_2E6 = x["sk"][0].get<int16_t>();
            gd->unk_2E8 = x["sk"][1].get<int16_t>();
            gd->shockGlow = (uint8_t)x["sk"][2].get<int32_t>();
        }
        if (x.contains("sv") && x["sv"].is_array()) {
            // "sf" is the index the sender started at (0 or 1 — see SerializeExtras);
            // absent means an older sender that always started at 0. Clamp both the
            // offset and the count: a wire value has not been through our own bounds.
            size_t first = x.contains("sf") ? (size_t)x["sf"].get<int32_t>() : 0;
            if (first > ARRAY_COUNT(gd->unk_49C)) {
                first = ARRAY_COUNT(gd->unk_49C);
            }
            size_t n = x["sv"].size();
            if (n > ARRAY_COUNT(gd->unk_49C) - first) {
                n = ARRAY_COUNT(gd->unk_49C) - first;
            }
            for (size_t i = 0; i < n; i++) {
                gd->unk_49C[first + i] = x["sv"][i].get<float>();
            }
        }
    } catch (const std::exception& ex) { SPDLOG_WARN("[BossGanonSync] extras parse error: {}", ex.what()); }

    // customColliderPos: the body cylinder rides the torso limb, not world.pos
    // (BossGanon_Update:2938). unk_1FC was refreshed by our OWN PostLimbDraw
    // (:3328) during last frame's Draw, which the mirror still runs.
    BossGanon_SetColliderPos(&gd->unk_1FC, &gd->collider);

    // -----------------------------------------------------------------------
    // Crash trap 1: sBossGanonZelda is NULL on a mirror.
    // -----------------------------------------------------------------------
    // BossGanon_DrawTriforce:3612 does
    //     this->triforcePos = sBossGanonZelda->unk_31C;
    // with no null check, and it is reached whenever
    //     fwork[GDF_TRIFORCE_PRIM_A] > 0  &&  triforceType == GDF_TRIFORCE_ZELDA
    // (the gate at :3593 and the branch at :3611). sBossGanonZelda is only ever
    // assigned while the intro cutscene runs locally, and BossGanon_Reset:5100
    // clears it — so on a mirror, whose Update (and therefore whose
    // BossGanon_IntroCutscene) never runs, the pointer stays NULL for the whole
    // fight. Nothing crashes TODAY only because PRIM_A is frozen at 0.
    //
    // This adapter sends neither fwork nor triforceType, so the pair cannot be
    // set from the wire. Enforce that as an invariant rather than leaving it
    // incidental: while this copy is mirrored it must never take the Zelda
    // branch. Correct as semantics, not just as defence — the Zelda triforce
    // only ever draws during the intro, and from C2.3 on the intro is exactly
    // the phase in which ShouldMirror is false and the copy runs locally (with
    // its own sBossGanonZelda spawned at z_boss_ganon.c:594).
    //
    // THIS GUARD MUST SURVIVE the commit that adds fwork to the wire; it is
    // what makes that commit safe to write at all.
    if ((gd->triforceType == GDF_TRIFORCE_ZELDA) && (gd->fwork[GDF_TRIFORCE_PRIM_A] > 0.0f)) {
        gd->fwork[GDF_TRIFORCE_PRIM_A] = 0.0f;
    }
}

// Authority side. Reads actionFunc, which is allowed here and nowhere else: the
// pointer is compared locally and only the resulting uint8_t is streamed —
// PhantomGanon_GetPhase does exactly the same (PhantomGanonAdapter.cpp:95).
static uint8_t BossGanon_GetPhase(Actor* actor) {
    BossGanon* gd = (BossGanon*)actor;
    if (actor->colChkInfo.health == 0) {
        return GANON_PHASE_DEFEATED;
    }
    if (gd->actionFunc == BossGanon_SetupIntroCutscene || gd->actionFunc == BossGanon_IntroCutscene) {
        return GANON_PHASE_INTRO;
    }
    return GANON_PHASE_FIGHT;
}

// Mirror side, evaluated locally every frame. Both halves are load-bearing:
//   - the streamed phase keeps us local while the AUTHORITY is still in the
//     introduction, so we do not start mirroring a Ganondorf who has not begun;
//   - our own actionFunc keeps us local while WE are still in it, because the two
//     clients do not enter the room on the same frame and ours may lag.
// Mirroring resumes only once both are past it, which is the same shape as
// PhantomGanon_ShouldMirror (:185-190).
static bool BossGanon_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossGanon* gd = (BossGanon*)actor;
    if (streamedPhase == GANON_PHASE_INTRO) {
        return false;
    }
    return gd->actionFunc != BossGanon_SetupIntroCutscene && gd->actionFunc != BossGanon_IntroCutscene;
}

// Mirror side, on the streamed edge into DEFEATED (C2.4).
//
// Bench-measured 2026-09-12, and the plan's description of the symptom is WRONG:
// it predicts the released mirror replays the INTRODUCTION cutscene. It does not.
// P2 sees nothing at all — Ganondorf simply stops being drawn while he sits at 0 HP
// in P2's actor list, with the room, the organ and P1's puppet rendering normally
// around him. The defect is an absence, not a wrong cutscene.
//
// Hand the defeat back to the local AI, the way Phantom Ganon does: the streamed
// pose has already been applied by the caller (EnemySync.cpp:1746), so the local
// sequence starts from the right place and then owns the camera, the animation, the
// window shattering and the tower cutscene.
//
// The awkward part is that BossGanon_SetupDeathCutscene (z_boss_ganon.c:1191) is
// conditional all the way through — if OBJECT_GANON_ANIME2 is not resident it does
// NOTHING, with no retry and no fallback. And there is no second chance here:
// EnemySync consumes the phase edge (`st.phase = r.phase`, :1744) BEFORE calling us,
// so returning false does not get us called again for this transition. So we check
// whether the setup actually took, by comparing actionFunc across the call, and only
// release the mirror if it did. If it did not, we stay mirrored — which is exactly
// today's behaviour, no better but no worse, and the log says so.
//
// In practice the object is resident: BossGanon_SetupIntroCutscene waits on that
// same OBJECT_GANON_ANIME2 before the fight can even begin (:485-499).
static bool BossGanon_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    if (!BossGanon_IsDorf(actor) || toPhase != GANON_PHASE_DEFEATED) {
        return false;
    }
    BossGanon* gd = (BossGanon*)actor;
    BossGanonActionFunc before = gd->actionFunc;

    actor->colChkInfo.health = 0;
    actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
    BossGanon_SetupDeathCutscene(gd, gPlayState);

    if (gd->actionFunc == before) {
        ESYNC_LOG("[BossGanonSync] defeat handoff REFUSED: OBJECT_GANON_ANIME2 not loaded, staying mirrored");
        return false;
    }
    // Deliberately NOT calling Enemy_StartFinishingBlow: Phantom Ganon's overlay uses
    // it, ovl_Boss_Ganon never does (zero occurrences), and adding it here would give
    // the mirror a finishing-blow freeze the authority does not have.
    ESYNC_LOG("[BossGanonSync] defeat handoff (SetupDeathCutscene ran locally)");
    return true;
}

void RegisterBossGanonAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = BossGanon_SerializeExtras;
    adapter.DeserializeExtras = BossGanon_DeserializeExtras;
    adapter.GetPhase = BossGanon_GetPhase;
    adapter.ShouldMirror = BossGanon_ShouldMirror;
    adapter.OnPhaseChange = BossGanon_OnPhaseChange;
    // The body cylinder is placed on the torso limb by the adapter, not by
    // ApplyPose / SubmitColliders. See BossGanon_DeserializeExtras.
    adapter.customColliderPos = true;
    // No Item_Drop anywhere in ovl_Boss_Ganon: a generic roll would invent
    // treasure the fight does not give.
    adapter.ownsDeathDrop = true;
    // NOT set, and each for a stated reason (see the file header):
    // OnPhaseChange (C2.4 — the defeat handoff), IsHealthless, ReplicateKill,
    // ownsSfx, mirrorWhileDrawless.
    EnemySync::RegisterAdapter(ACTOR_BOSS_GANON, adapter);
}
