#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/CutsceneSync.h"
#include "soh/Network/Anchor/CoopWarp.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <chrono>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "z64.h"
extern PlayState* gPlayState;
}

/**
 * CUTSCENE_SYNC
 *
 * Story-cutscene sync: { kind, sceneNum, entranceIndex, cutsceneIndex, csFlag,
 * targetTeamId }. When one player triggers a story cutscene, the partner is pulled
 * to the same spot and the cutscene is replayed LOCALLY on the partner (same
 * "replay locally" discipline as boss co-entry).
 *
 * Two kinds, both replayed locally on the partner:
 *
 *   kind 0 - ENTRANCE CS (sEntranceCutsceneTable: Deku Tree intro, Hyrule Field
 *     intro, castle grounds, ...). SENDER: the VB_PLAY_ENTRANCE_CS hook fires when
 *     the trigger arms locally (z_demo.c:2202). It does NOT block the local
 *     cutscene (*should is left untouched) and broadcasts kind=0 with the
 *     eventChkInf guard flag (csFlag). RECEIVER: clears the guard bit DIRECTLY
 *     (CLEAR_EVENTCHKINF - no Flags_UnsetEventChkInf, so no UNSET_FLAG echo), latches,
 *     and entrance-warps; the entrance CS re-arms and replays on load. The guard
 *     flag was already SET before the VB (z_demo.c:2201) and rides SET_FLAG to the
 *     partner, so clearing it locally is exactly what re-arms the trigger. The
 *     partner's replay re-SETs the flag (idempotent) -> the shared flag reconverges.
 *
 *   kind 1 - SCENE-LAYER CS (blue-warp exits, Deku Tree death speech, conditional
 *     entrance rewrites: cutsceneIndex >= 0xFFF0 at OnSceneInit). SENDER: OnSceneInit
 *     detects the resolved cutsceneIndex and broadcasts kind=1 with the FINAL
 *     entranceIndex (post-conditional-rewrite). RECEIVER: latches and entrance-warps
 *     with the cutsceneIndex, mirroring the z_demo.c:2226-2227 writes.
 *
 * No re-broadcast loop: the pulled-in partner replays the cutscene locally, which
 * re-fires the SAME detector (VB for kind 0, OnSceneInit for kind 1). A per-kind
 * echo latch, keyed on the expected entrance, consumes exactly that one self-replay
 * and suppresses the re-broadcast (so the original player is never yanked back).
 *
 * No end-barrier: the two cutscenes may finish at slightly different times; both end
 * up in the same scene and flag/item sync keeps the world consistent.
 *
 * Cross-version safe: an unknown packet type falls through Anchor.cpp's dispatch
 * ladder to a no-op.
 */

namespace {

// Scene-layer cutscene sentinel. gSaveContext.cutsceneIndex (s32) holds 0 (or any
// value < 0xFFF0) for an ordinary load and 0xFFF0..0xFFFF for the 16 scene-layer
// cutscene slots. This is the engine's own test (z_play.c:465, z_demo.c:2199/2222):
// resolved at Play_Init BEFORE OnSceneInit fires, and reset to 0 when a scene-layer
// cutscene ends (z_demo.c:2096), so it never lingers into a later ordinary load.
constexpr s32 SCENE_LAYER_CS_MIN = 0xFFF0;

// Per-kind echo latch. The RECEIVER arms one immediately before queuing its own pull
// warp; the detector that the replayed cutscene re-fires consumes it and stays quiet
// (breaks the pull loop). Keyed on the expected entrance so a stale latch cannot
// wrongly suppress an unrelated later trigger. Set/consumed only on the game thread.
struct ReplayLatch {
    bool armed = false;
    s32 entrance = -1;
};

ReplayLatch sEntranceCsReplay;   // kind 0, consumed by the VB_PLAY_ENTRANCE_CS hook
ReplayLatch sSceneLayerCsReplay; // kind 1, consumed by the OnSceneInit detector

bool ConsumeLatch(ReplayLatch& latch, s32 entrance) {
    if (latch.armed && latch.entrance == entrance) {
        latch.armed = false;
        return true;
    }
    return false;
}

// --- Pull-replay reconciliation --------------------------------------------------
//
// Clearing the guard bit at receipt (kind 0) only stays convergent if the partner's
// Cutscene_HandleEntranceTriggers actually re-SETs it on load (z_demo.c:2201). That
// re-SET can fail to happen, leaving the bit CLEAR here while the sender keeps it SET
// (a recurring desync that re-triggers the CS on every future entry), and leaving the
// armed latch to silently suppress the next legitimate same-entrance broadcast:
//   (a) age mismatch  - z_demo.c:2199 requires linkAge == requiredAge; a child-only
//                       entrance CS pulled onto an adult partner never re-arms;
//   (b) stale cutsceneIndex >= 0xFFF0 fails the same precondition;
//   (c) the queued pull warp EXPIRES (30 s) or is SUPERSEDED (boss-entry / spectate
//                       follow) before it ever loads the scene.
//
// We therefore track the in-flight replay and reconcile it each frame. The entrance
// CS trigger runs EXACTLY ONCE per load, synchronously in Play_Init (z_play.c:512),
// strictly before the first post-load frame update. So the first frame we observe
// ourselves at the target scene+entrance the outcome is already final: if the CS
// fired the VB consumed the latch and z_demo.c:2201 SET the bit; if the latch is
// still armed the CS did NOT fire and the bit is still CLEAR. Either way we force
// convergence with a RAW SET_EVENTCHKINF (no OnFlagSet -> no SET_FLAG echo, mirroring
// the raw CLEAR at receipt) and disarm the latch, tying the latch lifetime to the warp.
struct PendingReplay {
    bool active = false;
    u8 kind = 0;
    s32 csFlag = -1; // kind 0: the eventChkInf bit we cleared; -1 for kind 1
    s32 entrance = -1;
    s16 sceneNum = 0;
    std::chrono::steady_clock::time_point deadline;
};

PendingReplay sPendingReplay; // single in-flight replay (story cutscenes are sequential)

// Longer than CoopWarp's 30 s WARP_EXPIRY plus the (instant) transition + load, so a
// merely-deferred warp (partner mid-dialog) is always resolved by the scene-arrival
// path; the timeout path only fires once the warp is provably dead.
constexpr auto RECONCILE_TIMEOUT = std::chrono::seconds(40);

ReplayLatch& LatchForKind(u8 kind) {
    return (kind == 0) ? sEntranceCsReplay : sSceneLayerCsReplay;
}

// Force an in-flight replay to converge: re-SET the guard bit it cleared (kind 0) if
// its latch is still armed, then disarm. Called on timeout and before arming a new
// replay, so an unfired replay can never leave a desynced flag or a wedged latch.
void ResolvePendingReplay() {
    if (!sPendingReplay.active) {
        return;
    }
    ReplayLatch& latch = LatchForKind(sPendingReplay.kind);
    if (latch.armed) {
        if (sPendingReplay.kind == 0 && sPendingReplay.csFlag >= 0 &&
            !GET_EVENTCHKINF(sPendingReplay.csFlag)) {
            SET_EVENTCHKINF(sPendingReplay.csFlag);
        }
        latch.armed = false;
    }
    sPendingReplay.active = false;
}

// Track a freshly-issued replay. Flushes any prior in-flight replay first (latest
// wins, mirroring CoopWarp's single pending warp) so no earlier latch/flag is orphaned.
// MUST be called BEFORE (re-)arming this replay's latch, since the flush inspects the
// current latch state.
void ArmPendingReplay(u8 kind, s32 csFlag, s32 entrance, s16 sceneNum) {
    ResolvePendingReplay();
    sPendingReplay.active = true;
    sPendingReplay.kind = kind;
    sPendingReplay.csFlag = csFlag;
    sPendingReplay.entrance = entrance;
    sPendingReplay.sceneNum = sceneNum;
    sPendingReplay.deadline = std::chrono::steady_clock::now() + RECONCILE_TIMEOUT;
}

void ReconcilePendingReplay() {
    if (!sPendingReplay.active || gPlayState == nullptr) {
        return;
    }
    ReplayLatch& latch = LatchForKind(sPendingReplay.kind);

    // The self-replay detector consumed the latch: the cutscene fired and (kind 0)
    // z_demo.c:2201 re-SET the guard bit. Converged.
    if (!latch.armed) {
        sPendingReplay.active = false;
        return;
    }

    // We are at the target scene + entrance. Play_Init already ran the entrance-CS
    // trigger (z_play.c:512) this load, so a still-armed latch means the CS did NOT
    // fire (age mismatch / stale cutsceneIndex). Re-SET the bit we cleared.
    if (gPlayState->sceneNum == sPendingReplay.sceneNum &&
        gSaveContext.entranceIndex == sPendingReplay.entrance) {
        if (sPendingReplay.kind == 0 && sPendingReplay.csFlag >= 0 &&
            !GET_EVENTCHKINF(sPendingReplay.csFlag)) {
            SET_EVENTCHKINF(sPendingReplay.csFlag);
            SPDLOG_INFO("[CutsceneSync] reconcile: CS did not fire on load, re-SET guard flag {}",
                        sPendingReplay.csFlag);
        }
        latch.armed = false;
        sPendingReplay.active = false;
        return;
    }

    // Never reached the target scene before the deadline: the pull warp expired or was
    // superseded. Undo the clear and disarm the latch so it cannot wedge.
    if (std::chrono::steady_clock::now() >= sPendingReplay.deadline) {
        if (sPendingReplay.kind == 0 && sPendingReplay.csFlag >= 0 &&
            !GET_EVENTCHKINF(sPendingReplay.csFlag)) {
            SET_EVENTCHKINF(sPendingReplay.csFlag);
            SPDLOG_INFO("[CutsceneSync] reconcile: pull warp did not land, re-SET guard flag {}",
                        sPendingReplay.csFlag);
        }
        latch.armed = false;
        sPendingReplay.active = false;
    }
}

} // namespace

// Per-frame entry point, called by the Anchor per-frame dispatcher in explicit tick
// order (see HookHandlers.cpp).
void CutsceneSyncTick() {
    ReconcilePendingReplay();
}

void Anchor::SendPacket_CutsceneSync(u8 kind, s16 sceneNum, s32 entranceIndex, s32 cutsceneIndex, s16 csFlag) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = CUTSCENE_SYNC;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["kind"] = kind;
    payload["sceneNum"] = sceneNum;
    payload["entranceIndex"] = entranceIndex;
    payload["cutsceneIndex"] = cutsceneIndex;
    payload["csFlag"] = csFlag;

    SendJsonToRemote(payload);

    SPDLOG_INFO("[CutsceneSync] tx kind={} scene={} cs={}", kind, sceneNum, cutsceneIndex);
}

void Anchor::HandlePacket_CutsceneSync(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    u8 kind = payload["kind"].get<u8>();
    s16 sceneNum = payload["sceneNum"].get<s16>();
    s32 entranceIndex = payload["entranceIndex"].get<s32>();
    s32 cutsceneIndex = payload["cutsceneIndex"].get<s32>();
    s16 csFlag = payload["csFlag"].get<s16>();

    // Guard 1: already in the target scene -> ignore. Covers simultaneous triggers:
    // if both players fire the same entrance CS they are both already in the scene
    // when they process the other's packet, so neither warps -> no double warp.
    if (gPlayState->sceneNum == sceneNum) {
        SPDLOG_INFO("[CutsceneSync] rx ignored (already in scene {})", sceneNum);
        return;
    }

    // Guard 2: we are downed / spectating / game-over -> ignore. Task 8's scene-follow
    // owns the dead player's camera; a downed player must not be yanked into a
    // cutscene. >= LIFE_STATE_DOWNED covers DOWNED and GAME_OVER.
    if (myLifeState >= LIFE_STATE_DOWNED) {
        SPDLOG_INFO("[CutsceneSync] rx ignored (downed/game-over)");
        return;
    }

    if (kind == 0) {
        // Track this replay first (flushes any prior one before we touch the latch),
        // so the reconciler guarantees the guard bit ends up SET again even if the
        // replay never fires (age/stale/expired/superseded).
        ArmPendingReplay(0, csFlag, entranceIndex, sceneNum);
        // Clear the entrance-CS guard bit DIRECTLY (no Flags_UnsetEventChkInf -> no
        // UNSET_FLAG echo). The flag was SET on the sender before the VB and rode
        // SET_FLAG to us, so it is (or is about to be, over the ordered stream) set
        // here; clearing it re-arms Cutscene_HandleEntranceTriggers so the SAME
        // entrance CS plays locally on load. If flag sync is off it is already clear
        // and this is a harmless no-op.
        if (csFlag >= 0) {
            CLEAR_EVENTCHKINF(csFlag);
        }
        // Arm the echo latch BEFORE queuing the warp so our own VB_PLAY_ENTRANCE_CS
        // on load does not re-broadcast (which would pull the sender back).
        sEntranceCsReplay = { true, entranceIndex };
        RequestEntranceWarp((s16)entranceIndex, -1); // -1: entrance CS re-arms & plays naturally
        SPDLOG_INFO("[CutsceneSync] rx -> replay entrance={}", entranceIndex);
    } else if (kind == 1) {
        ArmPendingReplay(1, -1, entranceIndex, sceneNum);
        // Arm the echo latch BEFORE queuing the warp so our own OnSceneInit on load
        // does not re-broadcast the scene-layer CS.
        sSceneLayerCsReplay = { true, entranceIndex };
        RequestEntranceWarp((s16)entranceIndex, cutsceneIndex); // primes cutsceneIndex (mirrors z_demo.c:2226-2227)
        SPDLOG_INFO("[CutsceneSync] rx -> replay entrance={}", entranceIndex);
    } else {
        SPDLOG_INFO("[CutsceneSync] rx ignored (unknown kind {})", kind);
    }
}

void RegisterCutsceneSyncHooks(bool isConnected) {
    // Drop stale latches / pending replay on (dis)connect so a reconnect can't suppress
    // the first legitimate broadcast or reconcile against a stale scene.
    if (!isConnected) {
        sEntranceCsReplay = {};
        sSceneLayerCsReplay = {};
        sPendingReplay = {};
    }

    // NOTE: the per-frame reconcile is NOT registered here; it is driven by the Anchor
    // per-frame dispatcher (CutsceneSyncTick) so the Anchor-internal tick order is explicit.
    // The disconnect path above still drops the latches / pending replay.

    // SENDER kind 0: the entrance-CS trigger armed locally (z_demo.c:2202). Args are
    // (flag, entrance) per the engine call site, read as s32 (default arg promotion),
    // matching timesaver_hook_handlers.cpp's VB_PLAY_ENTRANCE_CS handler. We do NOT
    // touch *should (the local cutscene must still play).
    COND_VB_SHOULD(VB_PLAY_ENTRANCE_CS, isConnected, {
        s32 flag = va_arg(args, s32);
        s32 entrance = va_arg(args, s32);

        // We armed this entrance CS because WE were pulled in: consume the latch and
        // do NOT re-broadcast (that would pull the original player right back).
        if (ConsumeLatch(sEntranceCsReplay, entrance)) {
            SPDLOG_INFO("[CutsceneSync] tx suppressed (self-replay) kind=0 entrance={}", entrance);
        } else if (Anchor::Instance != nullptr) {
            Anchor::Instance->SendPacket_CutsceneSync(0, gPlayState->sceneNum, entrance,
                                                      gSaveContext.cutsceneIndex, (s16)flag);
        }
    });

    // SENDER kind 1: a scene-layer cutscene load. cutsceneIndex is resolved by
    // Play_Init before OnSceneInit fires. Broadcast the FINAL entranceIndex so any
    // conditional entrance rewrite (z_demo.c Cutscene_HandleConditionalTriggers) is
    // reproduced by warping the partner straight to the rewritten destination.
    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) {
        s32 cutsceneIndex = gSaveContext.cutsceneIndex;
        if (cutsceneIndex < SCENE_LAYER_CS_MIN) {
            return; // ordinary load, not a scene-layer cutscene
        }

        s32 entrance = gSaveContext.entranceIndex;
        if (ConsumeLatch(sSceneLayerCsReplay, entrance)) {
            SPDLOG_INFO("[CutsceneSync] tx suppressed (self-replay) kind=1 entrance={}", entrance);
            return;
        }

        if (Anchor::Instance != nullptr) {
            Anchor::Instance->SendPacket_CutsceneSync(1, sceneNum, entrance, cutsceneIndex, -1);
        }
    });
}
