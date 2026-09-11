#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Tw/z_boss_tw.h"
extern PlayState* gPlayState;
}

/**
 * Twinrova (Spirit Temple). One adapter for every actor of the fight: BOSS_TW
 * params TW_KOTAKE / TW_KOUME are the two sisters, TW_TWINROVA the merged form,
 * and TW_FIRE_BLAST..TW_DEATHBALL_KOUME the projectiles they spawn.
 *
 * Reported from a live session: "the two witches are very badly synced on my side,
 * invisible / frozen" -- P2 had to disconnect and let P1 finish the fight alone.
 * Both words are literal, and both come from the same thing: BOSS_TW had no
 * adapter at all, so not one private field crossed the wire while the mirror's
 * update was suppressed and its draw kept running.
 *
 * A sweep of z_boss_tw.c (87 top-level functions, 11 of them drawing) found the
 * fields a draw reads that only a non-draw function ever writes:
 *
 *   visible (0x0564)   is the visibility flag itself, written in BossTw_Init,
 *                      BossTw_SetupWait and BossTw_TwinrovaMergeCS -- hence
 *                      "invisible", exactly.
 *   actionFunc         both draw paths branch on `== BossTw_DeathCS`, and a
 *                      function pointer must never travel; the authority sends
 *                      coopInDeathCS instead (see z_boss_tw.h).
 *   work[13], workf[20] read seven and five times respectively across the draws:
 *                      crown texture scroll, tail index, fog timer, alphas.
 *                      Sent whole rather than by index -- 33 values is nothing
 *                      next to Morpha's 123, and picking indices by hand is how
 *                      you miss one.
 *   eyeTexIdx, leftEyeTexIdx, scepterAlpha, spawnPortalAlpha, spawnPortalScale,
 *   unk_5F8            the gaze, the sceptres and the summon portal.
 *
 * NOT sent, deliberately: the five fog* fields are written only in BossTw_Init,
 * so both sides already hold the same values.
 *
 * Known limits, deliberate:
 *  - No ShouldMirror. The intro, the merge and the death cutscenes take the
 *    camera and the player's inputs on each machine, which is the class of
 *    problem Bongo Bongo needed ShouldMirror for. Nothing here makes that worse
 *    than it already is, and inventing a phase machine for it without a session
 *    to test against is how a fix ends up worse than the bug.
 *  - The effect pool (TwEffType: rings, flames, shield blasts) is spawned by the
 *    authority's update and is not replicated; each side keeps its own.
 */

static void BossTw_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossTw* tw = (BossTw*)actor;

    x["vis"] = tw->visible;
    x["dcs"] = BossTw_CoopInDeathCS(actor) != 0;
    x["u"] = tw->unk_5F8;

    std::vector<int> w(tw->work, tw->work + WORK_MAX);
    x["w"] = w;
    std::vector<float> wf(tw->workf, tw->workf + FWORK_MAX);
    x["wf"] = wf;

    x["eye"] = { tw->eyeTexIdx, tw->leftEyeTexIdx };
    x["a"] = { tw->scepterAlpha, tw->spawnPortalAlpha, tw->spawnPortalScale };
}

static void BossTw_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossTw* tw = (BossTw*)actor;

    try {
        if (x.contains("vis")) {
            tw->visible = JsonBool(x, "vis") ? 1 : 0;
        }
        tw->coopInDeathCS = JsonBool(x, "dcs") ? 1 : 0;
        if (x.contains("u")) {
            tw->unk_5F8 = (u8)x["u"].get<int>();
        }
        if (x.contains("w") && x["w"].size() == WORK_MAX) {
            for (int i = 0; i < WORK_MAX; i++) {
                tw->work[i] = (s16)x["w"][i].get<int>();
            }
        }
        if (x.contains("wf") && x["wf"].size() == FWORK_MAX) {
            for (int i = 0; i < FWORK_MAX; i++) {
                tw->workf[i] = x["wf"][i].get<float>();
            }
        }
        if (x.contains("eye") && x["eye"].size() == 2) {
            tw->eyeTexIdx = x["eye"][0].get<int16_t>();
            tw->leftEyeTexIdx = x["eye"][1].get<int16_t>();
        }
        if (x.contains("a") && x["a"].size() == 3) {
            tw->scepterAlpha = x["a"][0].get<float>();
            tw->spawnPortalAlpha = x["a"][1].get<float>();
            tw->spawnPortalScale = x["a"][2].get<float>();
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[BossTwSync] extras parse error: {}", ex.what());
        return;
    }

    // blastTailPos is the projectile trail: a circular buffer the draw walks
    // backwards from work[TAIL_IDX], ten entries at a time. Fifty Vec3f every tick
    // would be absurd on the wire, and pointless -- the mirror already receives the
    // position the authority's update writes into the slot work[TAIL_IDX] points at.
    // So rebuild it here instead, the way BongoBongo rebuilds its hand trail from
    // streamed motion. Only the projectiles have one.
    if (actor->params >= TW_FIRE_BLAST) {
        int32_t idx = tw->work[TAIL_IDX];
        if ((idx >= 0) && (idx < (int32_t)ARRAY_COUNT(tw->blastTailPos))) {
            tw->blastTailPos[idx] = tw->actor.world.pos;
            // First frame: every other slot is still zeroed from Actor_Spawn's memset
            // and the draw reads ten of them, which would fan the sprites out to the
            // world origin. Seed the whole buffer once, as the vanilla setups do
            // (z_boss_tw.c:3927, :4114). Self-arming: once seeded, no slot is zero.
            const Vec3f& probe = tw->blastTailPos[(idx + 1) % 30];
            if ((probe.x == 0.0f) && (probe.y == 0.0f) && (probe.z == 0.0f)) {
                for (size_t i = 0; i < ARRAY_COUNT(tw->blastTailPos); i++) {
                    tw->blastTailPos[i] = tw->actor.world.pos;
                }
            }
        }
    }
}

// NOT REGISTERED YET, on purpose. Review found that simply registering this makes
// the fight WORSE than having no adapter at all, and the reason is structural:
//
//   EnemySync's leftStream test is `health == 0 && GetAdapter(id) == nullptr`, and
//   Kotake and Koume carry colChkInfo.health = 0 permanently (z_boss_tw.c:454) --
//   only the merged form gets 24. So today the sisters are never streamed and never
//   suppressed: each machine runs its own pair locally. Registering an adapter for
//   ACTOR_BOSS_TW flips BOTH sisters into the suppressed-mirror regime at once, and
//   their draw path reads a whole family of fields this file does not send:
//   beamDist (which gates the beam being drawn at all), beamScale, beamPitch/Yaw/
//   Roll, the beamReflection* set, groundBlastPos2, flameAlpha, flameRotation and
//   portalRotation. The mirror would stop seeing the beam it is supposed to reflect
//   back at the sisters -- the central mechanic of the fight.
//
// Three further findings to settle before this is turned on:
//   - IsHealthless without ReplicateKill leaves mirrored blasts immortal: the
//     authority Actor_Kills them on impact and broadcasts nothing, so the mirror
//     keeps one alive for STREAM_STALE_TICKS and then hands it back to local AI with
//     an AT collider and undecremented timers -- it can hit the other player half a
//     second after it exploded, somewhere its owner never saw it.
//   - The fog* claim above is WRONG: they are rewritten every frame in
//     BossTw_Update (:2880) and, pointedly, inside BossTw_DeathCS (:1754), which is
//     the very scene coopInDeathCS exists to reproduce.
//   - visible and coopInDeathCS latch: when suppression ends (stale, authority
//     handover, death) the actor resumes local AI holding the authority's last
//     value. A sister released while the authority had visible = 0 stays invisible
//     for the rest of the fight.
//
// The serialise/deserialise pair below is sound as far as it goes (types check out,
// the trail rebuild is correct) and the clamps it needed are now in the draws
// themselves. Finish the field list and the three points above, then register.
void RegisterBossTwAdapter() {
}
