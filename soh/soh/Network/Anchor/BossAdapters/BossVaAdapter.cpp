#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
// Pull the C++ side of global.h in under proper linkage before the extern "C"
// overlay header includes it (BossRush.cpp pattern).
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Va/z_boss_va.h"
extern PlayState* gPlayState;
// File-scope (non-static) in z_boss_va.c; not exported by the header.
void BossVa_SetupBodyDeath(BossVa* boss, PlayState* play);
void BossVa_SetupSupportCut(BossVa* boss, PlayState* play);
void BossVa_KillBari(BossVa* boss, PlayState* play);
void BossVa_UpdateEffects(PlayState* play);
// Co-op accessors added at the bottom of z_boss_va.c.
s8* BossVa_CoopCsState(void);
u8* BossVa_CoopFightPhase(void);
s8* BossVa_CoopPhase4HP(void);
u8* BossVa_CoopBodyState(void);
u8* BossVa_CoopKillBari(void);
u8* BossVa_CoopBodyBari(void); // [10]
}

/**
 * Barinade (BOSS_VA) — one actor id, ~15 instances telling themselves apart by
 * params (body, 3 supports, 3 zappers, 10 Bari, 3 stumps, door).
 *
 * The fight is driven by file-scope statics (sCsState / sFightPhase /
 * sPhase4HP / sBodyState / sKillBari / sBodyBari), and the death cutscene is
 * DISTRIBUTED across body + zappers + supports (each advances sCsState in its
 * own actionFunc). The model here:
 *  - Intro: local on every client (like Gohma/Dodongo) — sCsState < BATTLE.
 *  - Battle: parts are mirrored (pose streamed); the BODY's extras carry the
 *    combat statics, adopted by the mirror so door/stumps/draw code (which all
 *    read the statics) stay coherent. Stumps and door are NEVER mirrored.
 *  - Part deaths are phase edges: a Bari dying (isDead) or a support being cut
 *    (onCeiling dropping) replays the same vanilla entry point locally on the
 *    mirror (KillBari / SetupSupportCut — the latter also spawns the stump,
 *    which is why EnemySync drops remote BossVa spawns with params >= 16),
 *    then releases the actor to the local simulation for good.
 *  - Defeat: on the FIGHT -> DONE edge of the BODY the mirror runs
 *    BossVa_SetupBodyDeath locally; that sets the local sCsState to
 *    DEATH_START, which flips every part's ShouldMirror to false — the whole
 *    distributed death cutscene then plays natively on both clients.
 *
 * Boomerang: Barinade only reacts to hits whose collider->ac wears
 * ACTOR_EN_BOOM. The mirror's hits arrive through ENEMY_HIT_REQUEST, which now
 * streams the attacker's actor id and replays through EnemySync::FakeAttacker
 * — without that, the mirror player could never kill a Bari.
 *
 * Known limits: sVaEffects sparks/tumors are spawned by actionFuncs, so the
 * mirror shows fewer particles during the battle (same class of limitation as
 * Dodongo's fire breath); the pool still decays via the BossVa_UpdateEffects
 * call in the body's DeserializeExtras.
 */

namespace {

// BossVaCutscene lives in z_boss_va.c — mirror the two thresholds we gate on.
constexpr int8_t kBattle = 13;     // BOSSVA_BATTLE
constexpr int8_t kDeathStart = 14; // DEATH_START
constexpr uint8_t kPhaseDeath = 18; // PHASE_DEATH

enum VaPhase : uint8_t {
    VA_PREFIGHT = 0, // intro (or death) running locally
    VA_FIGHT = 1,    // battle proper: mirror me
    VA_DONE = 2,     // body: defeated / support: cut / bari: dead
};

bool IsSupport(int16_t params) {
    return params >= BOSSVA_SUPPORT_1 && params <= BOSSVA_SUPPORT_3;
}
bool IsZapper(int16_t params) {
    return params >= BOSSVA_ZAPPER_1 && params <= BOSSVA_ZAPPER_3;
}
bool IsBari(int16_t params) {
    return params >= BOSSVA_BARI_UPPER_1 && params <= BOSSVA_BARI_LOWER_5;
}
bool IsLocalOnly(int16_t params) {
    return params >= BOSSVA_STUMP_1; // stumps + door: statics-driven, always local
}

} // namespace

static uint8_t BossVaSync_GetPhase(Actor* actor) {
    BossVa* va = (BossVa*)actor;
    int16_t params = va->actor.params;
    int8_t cs = *BossVa_CoopCsState();

    if (IsLocalOnly(params)) {
        return VA_PREFIGHT;
    }
    if (params == BOSSVA_BODY) {
        if (cs >= kDeathStart) {
            return VA_DONE;
        }
        return cs == kBattle ? VA_FIGHT : VA_PREFIGHT;
    }
    if (cs != kBattle) {
        return VA_PREFIGHT;
    }
    if (IsSupport(params)) {
        return va->onCeiling ? VA_FIGHT : VA_DONE;
    }
    if (IsBari(params)) {
        return va->isDead ? VA_DONE : VA_FIGHT;
    }
    return VA_FIGHT; // zappers die inside the death cutscene, after release
}

static void BossVaSync_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossVa* va = (BossVa*)actor;
    int16_t params = va->actor.params;

    if (params == BOSSVA_BODY) {
        x["cs"] = *BossVa_CoopCsState();
        x["fp"] = *BossVa_CoopFightPhase();
        x["hp4"] = *BossVa_CoopPhase4HP();
        x["bs"] = *BossVa_CoopBodyState();
        x["kb"] = *BossVa_CoopKillBari();
        u8* bari = BossVa_CoopBodyBari();
        x["bari"] = std::vector<uint8_t>(bari, bari + 10);
        x["glow"] = va->bodyGlow;
        x["p0"] = va->unk_1A0;
        x["p1"] = va->unk_1A4;
        x["yo"] = va->actor.shape.yOffset;
        x["cr"] = va->colliderBody.dim.radius;
        x["ch"] = va->colliderBody.dim.height;
        x["cdf"] = va->colliderBody.info.bumper.dmgFlags;
    } else if (IsSupport(params)) {
        x["oc"] = va->onCeiling;
        x["hrx"] = va->headRot.x;
        x["t"] = va->timer;
        x["t2"] = va->timer2;
    } else if (IsZapper(params)) {
        x["r4y"] = va->unk_1E6;
        x["r4z"] = va->unk_1E4;
        x["r5y"] = va->unk_1EC;
        x["r5z"] = va->unk_1EA;
        x["r7y"] = va->unk_1F2;
        x["r7z"] = va->unk_1F0;
        x["tgt"] = { va->unk_1D8.x, va->unk_1D8.y, va->unk_1D8.z };
        x["t2"] = va->timer2;
    } else if (IsBari(params)) {
        x["sx"] = va->unk_1D8.x;
        x["sy"] = va->unk_1D8.y;
        x["t2"] = va->timer2;
        x["orb"] = va->unk_1A0;
        x["wrx"] = va->actor.world.rot.x;
        x["wry"] = va->actor.world.rot.y;
        x["cft"] = va->actor.colorFilterTimer;
        x["cfp"] = va->actor.colorFilterParams;
    }
}

static void BossVaSync_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossVa* va = (BossVa*)actor;
    int16_t params = va->actor.params;
    try {
        if (params == BOSSVA_BODY) {
            // Adopt the authority's combat statics: every part's draw and the
            // local door/stumps consume them.
            if (x.contains("cs")) *BossVa_CoopCsState() = x["cs"].get<int8_t>();
            if (x.contains("fp")) *BossVa_CoopFightPhase() = x["fp"].get<uint8_t>();
            if (x.contains("hp4")) *BossVa_CoopPhase4HP() = x["hp4"].get<int8_t>();
            if (x.contains("bs")) *BossVa_CoopBodyState() = x["bs"].get<uint8_t>();
            if (x.contains("kb")) *BossVa_CoopKillBari() = x["kb"].get<uint8_t>();
            if (x.contains("bari") && x["bari"].size() == 10) {
                u8* bari = BossVa_CoopBodyBari();
                for (int i = 0; i < 10; i++) {
                    bari[i] = x["bari"][i].get<uint8_t>();
                }
            }
            if (x.contains("glow")) va->bodyGlow = x["glow"].get<int16_t>();
            if (x.contains("p0")) va->unk_1A0 = x["p0"].get<float>();
            if (x.contains("p1")) va->unk_1A4 = x["p1"].get<float>();
            if (x.contains("yo")) va->actor.shape.yOffset = x["yo"].get<float>();
            if (x.contains("cr")) va->colliderBody.dim.radius = x["cr"].get<int16_t>();
            if (x.contains("ch")) va->colliderBody.dim.height = x["ch"].get<int16_t>();
            if (x.contains("cdf")) va->colliderBody.info.bumper.dmgFlags = x["cdf"].get<uint32_t>();
            // The shared 400-effect pool is stepped by the body's update —
            // keep it decaying while the body is suppressed.
            if (gPlayState != NULL) {
                BossVa_UpdateEffects(gPlayState);
            }
        } else if (IsSupport(params)) {
            if (x.contains("oc")) va->onCeiling = x["oc"].get<uint8_t>();
            if (x.contains("hrx")) va->headRot.x = x["hrx"].get<int16_t>();
            if (x.contains("t")) va->timer = x["t"].get<int32_t>();
            if (x.contains("t2")) va->timer2 = x["t2"].get<int16_t>();
        } else if (IsZapper(params)) {
            if (x.contains("r4y")) va->unk_1E6 = x["r4y"].get<int16_t>();
            if (x.contains("r4z")) va->unk_1E4 = x["r4z"].get<int16_t>();
            if (x.contains("r5y")) va->unk_1EC = x["r5y"].get<int16_t>();
            if (x.contains("r5z")) va->unk_1EA = x["r5z"].get<int16_t>();
            if (x.contains("r7y")) va->unk_1F2 = x["r7y"].get<int16_t>();
            if (x.contains("r7z")) va->unk_1F0 = x["r7z"].get<int16_t>();
            if (x.contains("tgt") && x["tgt"].size() == 3) {
                va->unk_1D8.x = x["tgt"][0].get<float>();
                va->unk_1D8.y = x["tgt"][1].get<float>();
                va->unk_1D8.z = x["tgt"][2].get<float>();
            }
            if (x.contains("t2")) va->timer2 = x["t2"].get<int16_t>();
        } else if (IsBari(params)) {
            if (x.contains("sx")) va->unk_1D8.x = x["sx"].get<float>();
            if (x.contains("sy")) va->unk_1D8.y = x["sy"].get<float>();
            if (x.contains("t2")) va->timer2 = x["t2"].get<int16_t>();
            if (x.contains("orb")) va->unk_1A0 = x["orb"].get<float>();
            if (x.contains("wrx")) va->actor.world.rot.x = x["wrx"].get<int16_t>();
            if (x.contains("wry")) va->actor.world.rot.y = x["wry"].get<int16_t>();
            if (x.contains("cft")) va->actor.colorFilterTimer = x["cft"].get<uint8_t>();
            if (x.contains("cfp")) va->actor.colorFilterParams = x["cfp"].get<int16_t>();
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[BossVaSync] extras parse error: {}", ex.what());
    }
}

static bool BossVaSync_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    BossVa* va = (BossVa*)actor;
    int16_t params = va->actor.params;
    ESYNC_LOG("[BossVaSync] params={} phase {}->{}", params, fromPhase, toPhase);
    if (toPhase != VA_DONE || gPlayState == NULL) {
        return false;
    }

    if (params == BOSSVA_BODY) {
        // Local defeat handoff: SetupBodyDeath drives the local sCsState to
        // DEATH_START, which drops every part's ShouldMirror — the distributed
        // death cutscene (body/zappers/supports) then runs natively here.
        *BossVa_CoopFightPhase() = kPhaseDeath;
        BossVa_SetupBodyDeath(va, gPlayState);
        Enemy_StartFinishingBlow(gPlayState, actor);
        ESYNC_LOG("[BossVaSync] defeat handoff (SetupBodyDeath called locally)");
        return true;
    }
    if (IsSupport(params)) {
        // Replay the cut locally: skeleton swap, dangling arm, stump spawn.
        // The local sFightPhase/sBodyState bumps are overwritten by the next
        // streamed statics block.
        BossVa_SetupSupportCut(va, gPlayState);
        return true;
    }
    if (IsBari(params)) {
        BossVa_KillBari(va, gPlayState); // bubbles + SetupBariDeath + timed Actor_Kill
        return true;
    }
    return false;
}

static bool BossVaSync_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossVa* va = (BossVa*)actor;
    if (IsLocalOnly(va->actor.params)) {
        return false;
    }
    // Intro and death cutscenes run locally on every client.
    if (*BossVa_CoopCsState() != kBattle) {
        return false;
    }
    return streamedPhase == VA_FIGHT;
}

void RegisterBossVaAdapter() {
    ActorSyncAdapter adapter;
    adapter.SerializeExtras = BossVaSync_SerializeExtras;
    adapter.DeserializeExtras = BossVaSync_DeserializeExtras;
    adapter.GetPhase = BossVaSync_GetPhase;
    adapter.OnPhaseChange = BossVaSync_OnPhaseChange;
    adapter.ShouldMirror = BossVaSync_ShouldMirror;
    EnemySync::RegisterAdapter(ACTOR_BOSS_VA, adapter);
}
