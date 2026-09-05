#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Boss_Fd/z_boss_fd.h"
#include "src/overlays/actors/ovl_Boss_Fd2/z_boss_fd2.h"
extern PlayState* gPlayState;
// File-scope (non-static) in the overlays; not exported by their headers.
void BossFd_Wait(BossFd* boss, PlayState* play);
void BossFd_CoopMirrorTick(BossFd* boss, PlayState* play);
void BossFd_CoopMirrorSfx(BossFd* boss);
void BossFd2_Wait(BossFd2* head, PlayState* play);
void BossFd2_Death(BossFd2* head, PlayState* play);
void BossFd2_Vulnerable(BossFd2* head, PlayState* play);
void BossFd2_Damaged(BossFd2* head, PlayState* play);
void BossFd2_BreatheFire(BossFd2* head, PlayState* play);
void BossFd2_SetupDeath(BossFd2* head, PlayState* play);
void BossFd2_CoopMirrorTick(BossFd2* head, PlayState* play, u8 hurt, u8 breathing);
}

/**
 * Volvagia (Fire Temple): two actors that hand the fight back and forth.
 *
 * BOSS_FD — the flying serpent (room-listed, health 24, the ONLY health of the
 * pair). Its Update is the intro cutscene (introState machine, local on every
 * client like Gohma), then BossFd_Fly (hole hopping, ceiling rock drops, chase
 * with fire breath) alternating with BossFd_Wait while the head is in a hole.
 * The body is a 100-slot ring of past head poses (bodySegsPos/Rot), the manes
 * another ring, both fed by the update — so the mirror, whose update is
 * suppressed, re-feeds them from the streamed head pose (world.pos from the
 * base stream, world.rot from extras) at the streamed ring index
 * (BossFd_CoopMirrorTick), and runs the ambient cosmetics, the fire breath and
 * the 180-slot effect pool locally: the breath effects are what burn a Link, and
 * each machine's pool burns its own. Draw / Z-target normally gate on
 * "actionFunc != Wait", which the idle mirror copy never leaves: coopVisible
 * (streamed) lifts that gate. One-shot sounds travel as a (seq, id) event.
 *
 * BOSS_FD2 — the head in the hole (child spawned in BossFd's Init: occurrence
 * key on both sides). Health 0 by construction (healthless): its JntSph takes
 * the hits and BossFd2_CollisionCheck damages the PARENT, so a mirror-side hit
 * on the head travels as a hit request keyed on the head, the authority's
 * synthetic AC_HIT lands on the head's collider, and the parent's health drops
 * — the parent's stream carries it back. FlexSkeleton streamed; extras: head
 * rotation (breath aim), animation frame (breath opacity), work[], hurt /
 * breathing flags, the face-plate collider type (from the parent's streamed
 * faceExposed), sound events with their one-shot particles.
 *
 * Defeat: the head's CollisionCheck zeroes the parent's health and runs
 * BossFd2_SetupDeath. On the streamed DEFEATED edge (head: actionFunc ==
 * BossFd2_Death; body: health == 0, same frame) the mirror runs the same
 * setup locally and releases both: the head's death cutscene signals the
 * body (FD2_SIGNAL_DEATH) whose local Wait then flies the skin-burn / bones /
 * skull sequence — camera, heart container, blue warp, clear flag on both
 * machines (deduped by stock Anchor sync).
 *
 * EN_VB_BALL — ceiling rocks (params 100) are fire-and-forget projectiles
 * (each machine drops its own copy, which splits and hurts its own Link);
 * chunks and bones are untracked local spawns. Bg_Vb_Sima (the collapsing
 * platform) only reads the parent's platformSignal during the local intro.
 *
 * Targeting: chase and breath aim use Anchor_NearestPlayer (local Link or
 * puppet) instead of GET_PLAYER.
 */

enum VolvagiaPhase : uint8_t {
    FD_PHASE_PREFIGHT = 0,
    FD_PHASE_FIGHT = 1,
    FD_PHASE_DEFEATED = 2,
};

// ---------------------------------------------------------------------------
// BOSS_FD (the flying serpent)
// ---------------------------------------------------------------------------

static uint8_t Volvagia_GetPhase(Actor* actor) {
    BossFd* fd = (BossFd*)actor;
    if (fd->introState != BFD_CS_NONE) {
        return FD_PHASE_PREFIGHT;
    }
    return actor->colChkInfo.health == 0 ? FD_PHASE_DEFEATED : FD_PHASE_FIGHT;
}

static void Volvagia_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossFd* fd = (BossFd*)actor;
    std::vector<int> w(fd->work, fd->work + BFD_SHORT_COUNT);
    x["w"] = w;
    x["t4"] = fd->timers[4];
    x["fb"] = fd->fireBreathTimer;
    x["fog"] = fd->fogMode;
    x["ss"] = fd->skinSegments;
    x["fe"] = fd->faceExposed;
    x["hp"] = fd->holePosition;
    x["wr"] = actor->world.rot;
    x["spd"] = actor->speedXZ;
    x["pul"] = fd->fwork[BFD_BODY_PULSE];
    x["vis"] = fd->actionFunc != BossFd_Wait;
    x["sfx"] = { fd->coopSfxSeq, fd->coopSfxId };
}

static void Volvagia_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossFd* fd = (BossFd*)actor;
    try {
        if (x.contains("w") && x["w"].size() == BFD_SHORT_COUNT) {
            for (int i = 0; i < BFD_SHORT_COUNT; i++) {
                fd->work[i] = (int16_t)x["w"][i].get<int>();
            }
        }
        if (x.contains("t4")) fd->timers[4] = x["t4"].get<int16_t>();
        if (x.contains("fb")) fd->fireBreathTimer = x["fb"].get<int16_t>();
        if (x.contains("fog")) fd->fogMode = x["fog"].get<uint8_t>();
        if (x.contains("ss")) fd->skinSegments = x["ss"].get<int16_t>();
        if (x.contains("fe")) fd->faceExposed = x["fe"].get<uint8_t>();
        if (x.contains("hp")) fd->holePosition = x["hp"].get<Vec3f>();
        if (x.contains("wr")) actor->world.rot = x["wr"].get<Vec3s>();
        if (x.contains("spd")) actor->speedXZ = x["spd"].get<float>();
        if (x.contains("pul")) fd->fwork[BFD_BODY_PULSE] = x["pul"].get<float>();
        if (x.contains("vis")) fd->coopVisible = x["vis"].get<bool>() ? 1 : 0;
        if (x.contains("sfx") && x["sfx"].size() == 2) {
            fd->coopSfxSeq = x["sfx"][0].get<uint8_t>();
            fd->coopSfxId = x["sfx"][1].get<uint16_t>();
        }
        // Once per streamed frame (the cached extras are re-applied every game
        // frame): the lava splash quake on the surface crossing, sound events,
        // then the body ring / cosmetics / effect pool tick.
        if (gPlayState != NULL && fd->coopMirrorMove != fd->work[BFD_MOVE_TIMER]) {
            fd->coopMirrorMove = fd->work[BFD_MOVE_TIMER];
            if (fd->work[BFD_SPLASH_TIMER] == 10 && fd->coopMirrorSplash != 10) {
                func_80033E1C(gPlayState, 1, 80, 0x5000);
            }
            fd->coopMirrorSplash = fd->work[BFD_SPLASH_TIMER];
            BossFd_CoopMirrorSfx(fd);
            BossFd_CoopMirrorTick(fd, gPlayState);
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[VolvagiaSync] extras parse error: {}", ex.what());
    }
}

static bool Volvagia_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    ESYNC_LOG("[VolvagiaSync] body phase {}->{}", fromPhase, toPhase);
    if (toPhase == FD_PHASE_DEFEATED) {
        // The head's local death cutscene (released on the same streamed edge)
        // signals our Wait with FD2_SIGNAL_DEATH; from there the local Fly plays
        // the skin burn, bones, skull, heart container.
        BossFd* fd = (BossFd*)actor;
        fd->coopVisible = 0;
        actor->colChkInfo.health = 0;
        ESYNC_LOG("[VolvagiaSync] body released for the death sequence");
        return true;
    }
    return false;
}

static bool Volvagia_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossFd* fd = (BossFd*)actor;
    // Local intro first (it is this actor's Update); never mirror the defeat.
    return streamedPhase == FD_PHASE_FIGHT && fd->introState == BFD_CS_NONE;
}

// ---------------------------------------------------------------------------
// BOSS_FD2 (the head in the hole)
// ---------------------------------------------------------------------------

static BossFd* VolvagiaHead_Parent(Actor* actor) {
    Actor* parent = actor->parent;
    if (parent == NULL || parent->update == NULL || parent->id != ACTOR_BOSS_FD) {
        return NULL;
    }
    return (BossFd*)parent;
}

static bool VolvagiaHead_Always(Actor* actor) {
    return true;
}

static uint8_t VolvagiaHead_GetPhase(Actor* actor) {
    BossFd2* fd2 = (BossFd2*)actor;
    BossFd* fd = VolvagiaHead_Parent(actor);
    if (fd2->actionFunc == BossFd2_Death) {
        return FD_PHASE_DEFEATED;
    }
    if (fd != NULL && fd->introState != BFD_CS_NONE) {
        return FD_PHASE_PREFIGHT;
    }
    return FD_PHASE_FIGHT;
}

static void VolvagiaHead_SerializeExtras(Actor* actor, nlohmann::json& x) {
    BossFd2* fd2 = (BossFd2*)actor;
    std::vector<int> w(fd2->work, fd2->work + FD2_SHORT_COUNT);
    x["w"] = w;
    x["hr"] = fd2->headRot;
    x["cf"] = fd2->skelAnime.curFrame;
    x["hurt"] = (fd2->actionFunc == BossFd2_Vulnerable) || (fd2->actionFunc == BossFd2_Damaged);
    x["br"] = fd2->actionFunc == BossFd2_BreatheFire;
    x["vis"] = fd2->actionFunc != BossFd2_Wait;
    x["sfx"] = { fd2->coopSfxSeq, fd2->coopSfxId };
}

static void VolvagiaHead_DeserializeExtras(Actor* actor, const nlohmann::json& x) {
    BossFd2* fd2 = (BossFd2*)actor;
    try {
        if (x.contains("w") && x["w"].size() == FD2_SHORT_COUNT) {
            for (int i = 0; i < FD2_SHORT_COUNT; i++) {
                fd2->work[i] = (int16_t)x["w"][i].get<int>();
            }
        }
        if (x.contains("hr")) fd2->headRot = x["hr"].get<Vec3s>();
        if (x.contains("cf")) fd2->skelAnime.curFrame = x["cf"].get<float>();
        if (x.contains("vis")) fd2->coopVisible = x["vis"].get<bool>() ? 1 : 0;
        if (x.contains("sfx") && x["sfx"].size() == 2) {
            fd2->coopSfxSeq = x["sfx"][0].get<uint8_t>();
            fd2->coopSfxId = x["sfx"][1].get<uint16_t>();
        }
        if (gPlayState != NULL && fd2->coopMirrorVar != fd2->work[FD2_VAR_TIMER]) {
            fd2->coopMirrorVar = fd2->work[FD2_VAR_TIMER];
            uint8_t hurt = x.contains("hurt") && x["hurt"].get<bool>();
            uint8_t breathing = x.contains("br") && x["br"].get<bool>();
            BossFd2_CoopMirrorTick(fd2, gPlayState, hurt, breathing);
            // UpdateCosmetics re-derives the attention flag from focus.pos; the
            // streamed hookshot bit (stunned head) survives it.
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[VolvagiaSync] head extras parse error: {}", ex.what());
    }
}

static bool VolvagiaHead_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    ESYNC_LOG("[VolvagiaSync] head phase {}->{}", fromPhase, toPhase);
    if (toPhase == FD_PHASE_DEFEATED) {
        // Same steps as the lethal branch of BossFd2_CollisionCheck, minus the
        // damage: the local death cutscene starts from the final streamed pose.
        BossFd2* fd2 = (BossFd2*)actor;
        BossFd* fd = VolvagiaHead_Parent(actor);
        if (fd != NULL) {
            fd->actor.colChkInfo.health = 0;
        }
        fd2->coopVisible = 0;
        BossFd2_SetupDeath(fd2, gPlayState);
        fd2->work[FD2_DAMAGE_FLASH_TIMER] = 10;
        fd2->work[FD2_INVINC_TIMER] = 30000;
        Audio_QueueSeqCmd(0x1 << 28 | SEQ_PLAYER_BGM_MAIN << 24 | 0x100FF);
        Audio_PlayActorSound2(actor, NA_SE_EN_VALVAISA_DEAD);
        Enemy_StartFinishingBlow(gPlayState, actor);
        ESYNC_LOG("[VolvagiaSync] head defeat handoff (SetupDeath called locally)");
        return true;
    }
    return false;
}

static bool VolvagiaHead_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossFd* fd = VolvagiaHead_Parent(actor);
    return streamedPhase == FD_PHASE_FIGHT && (fd == NULL || fd->introState == BFD_CS_NONE);
}

void RegisterVolvagiaAdapters() {
    {
        ActorSyncAdapter adapter;
        adapter.SerializeExtras = Volvagia_SerializeExtras;
        adapter.DeserializeExtras = Volvagia_DeserializeExtras;
        adapter.GetPhase = Volvagia_GetPhase;
        adapter.OnPhaseChange = Volvagia_OnPhaseChange;
        adapter.ShouldMirror = Volvagia_ShouldMirror;
        adapter.ownsSfx = true; // its mirror tick replays this actor's sounds itself
        EnemySync::RegisterAdapter(ACTOR_BOSS_FD, adapter);
    }
    {
        ActorSyncAdapter adapter;
        adapter.SerializeExtras = VolvagiaHead_SerializeExtras;
        adapter.DeserializeExtras = VolvagiaHead_DeserializeExtras;
        adapter.GetPhase = VolvagiaHead_GetPhase;
        adapter.OnPhaseChange = VolvagiaHead_OnPhaseChange;
        adapter.ShouldMirror = VolvagiaHead_ShouldMirror;
        adapter.IsHealthless = VolvagiaHead_Always; // the parent's health is the fight's health
        adapter.ownsSfx = true; // its mirror tick replays this actor's sounds itself
        EnemySync::RegisterAdapter(ACTOR_BOSS_FD2, adapter);
    }
}
