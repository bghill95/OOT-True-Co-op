#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Dodongo/z_boss_dodongo.h"
extern PlayState* gPlayState;
// File-scope (non-static) in z_boss_dodongo.c; not exported by the header.
void BossDodongo_SetupDeathCutscene(BossDodongo* dodongo);
}

/**
 * King Dodongo (BOSS_DODONGO).
 *
 * Intro: like Gohma, the intro cutscene IS the boss's Update (csState machine +
 * a dedicated cutscene camera), so each client plays its own intro locally.
 * FIGHT is detected as "intro finished" (csState == 4 with the cutscene camera
 * released) — never via actionFunc, which is not portable across machines.
 *
 * Extras: OverrideLimbDraw shapes the roll curl from unk_25C[50] (per-limb
 * scale morphs) and unk_1F8/unk_208; PostLimbDraw reads unk_1DA (mouth) and
 * unk_1C0 (hit flash); the damage tint uses the colorFilter fields. All are
 * update-computed, so a suppressed mirror must receive them or the curl/flash
 * freezes. Fire-breath particles stay authority-side (known SFX limitation).
 *
 * Defeat: boss->health (its own s16, not colChkInfo) hits 0 from sword hits
 * only (bomb damage floors at 1). On the FIGHT -> DEFEATED edge the mirror
 * runs the death cutscene locally via BossDodongo_SetupDeathCutscene and is
 * released for good — heart container, blue warp and clear flag are deduped by
 * stock Anchor sync.
 */

enum KingDodongoPhase : uint8_t {
    KD_PHASE_PREFIGHT = 0,
    KD_PHASE_FIGHT = 1,
    KD_PHASE_DEFEATED = 2,
};

static bool KingDodongo_IntroDone(BossDodongo* kd) {
    return kd->csState == 4 && kd->cutsceneCamera == 0;
}

static uint8_t KingDodongo_GetPhase(Actor* actor) {
    BossDodongo* kd = (BossDodongo*)actor;
    if (kd->health <= 0) {
        return KD_PHASE_DEFEATED;
    }
    return KingDodongo_IntroDone(kd) ? KD_PHASE_FIGHT : KD_PHASE_PREFIGHT;
}

static void KingDodongo_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossDodongo* kd = (BossDodongo*)actor;
    x["mouth"] = kd->unk_1DA;
    x["flash"] = kd->unk_1C0;
    x["m1f8"] = kd->unk_1F8;
    x["m208"] = kd->unk_208;
    x["cf"] = { kd->colorFilterR, kd->colorFilterG, kd->colorFilterB, kd->colorFilterMin, kd->colorFilterMax };
    std::vector<float> curl(kd->unk_25C, kd->unk_25C + 50);
    x["curl"] = curl;
}

static void KingDodongo_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossDodongo* kd = (BossDodongo*)actor;
    try {
        if (x.contains("mouth")) kd->unk_1DA = x["mouth"].get<int16_t>();
        if (x.contains("flash")) kd->unk_1C0 = x["flash"].get<int16_t>();
        if (x.contains("m1f8")) kd->unk_1F8 = x["m1f8"].get<float>();
        if (x.contains("m208")) kd->unk_208 = x["m208"].get<float>();
        if (x.contains("cf") && x["cf"].size() == 5) {
            kd->colorFilterR = x["cf"][0].get<float>();
            kd->colorFilterG = x["cf"][1].get<float>();
            kd->colorFilterB = x["cf"][2].get<float>();
            kd->colorFilterMin = x["cf"][3].get<float>();
            kd->colorFilterMax = x["cf"][4].get<float>();
        }
        if (x.contains("curl") && x["curl"].size() == 50) {
            for (int i = 0; i < 50; i++) {
                kd->unk_25C[i] = x["curl"][i].get<float>();
            }
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[KingDodongoSync] extras parse error: {}", ex.what());
    }
}

static bool KingDodongo_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    ESYNC_LOG("[KingDodongoSync] phase {}->{}", fromPhase, toPhase);
    if (toPhase == KD_PHASE_DEFEATED && fromPhase == KD_PHASE_FIGHT) {
        BossDodongo* kd = (BossDodongo*)actor;
        kd->health = 0;
        BossDodongo_SetupDeathCutscene(kd);
        Enemy_StartFinishingBlow(gPlayState, actor);
        ESYNC_LOG("[KingDodongoSync] defeat handoff (SetupDeathCutscene called locally)");
        return true;
    }
    return false;
}

static bool KingDodongo_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    // Mirror only during the fight proper, once our own intro has finished.
    return streamedPhase == KD_PHASE_FIGHT && KingDodongo_IntroDone((BossDodongo*)actor);
}

void RegisterKingDodongoAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = KingDodongo_SerializeExtras;
    adapter.DeserializeExtras = KingDodongo_DeserializeExtras;
    adapter.GetPhase = KingDodongo_GetPhase;
    adapter.OnPhaseChange = KingDodongo_OnPhaseChange;
    adapter.ShouldMirror = KingDodongo_ShouldMirror;
    EnemySync::RegisterAdapter(ACTOR_BOSS_DODONGO, adapter);
}
