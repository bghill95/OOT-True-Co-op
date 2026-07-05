#include "CoopWarp.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include <libultraship/libultraship.h>

#include <chrono>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

// A single pending warp request (latest wins). Held here rather than in a FIFO:
// these fire at most once and a newer request should supersede an older one.
namespace {

enum class PendingWarpKind { None, Entrance, Position };

struct PendingWarp {
    PendingWarpKind kind = PendingWarpKind::None;
    s16 entranceIndex = 0;
    s32 cutsceneIndex = -1; // Entrance warp only (s32: scene-layer indices are 0xFFF0+, which do not fit s16)
    s16 room = 0;           // Position warp only
    PosRot dest = {};       // Position warp only
    std::chrono::steady_clock::time_point queuedAt;
};

PendingWarp sPending;

// Drop a queued request that never found a safe window rather than warp into a
// stale/bad state.
constexpr auto WARP_EXPIRY = std::chrono::seconds(30);

// Plain entrance warp — mirrors EnemySyncTestBoot.cpp. respawnFlag is deliberately
// left untouched (0) so Cutscene_HandleEntranceTriggers (respawnFlag <= 0 at
// z_demo.c:2200) still arms; a synced entrance/scene-layer cutscene replays on load.
void ExecuteEntranceWarp(const PendingWarp& w) {
    if (w.cutsceneIndex >= 0) {
        gSaveContext.cutsceneIndex = w.cutsceneIndex;
    }
    gPlayState->nextEntranceIndex = w.entranceIndex;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.nextTransitionType = TRANS_TYPE_INSTANT;

    SPDLOG_INFO("[CoopWarp] warp entrance={} cs={}", w.entranceIndex, w.cutsceneIndex);
}

// Exact-position warp — mirrors TeleportTo.cpp. respawnFlag = 1 routes through the
// RESPAWN_MODE_DOWN slot and SUPPRESSES entrance-CS triggers (z_demo.c:2200). We
// also one-shot-suppress the void damage that a void-out respawn would inflict,
// same as TeleportTo, so a system warp never costs the player a heart.
void ExecutePositionWarp(const PendingWarp& w) {
    gPlayState->nextEntranceIndex = w.entranceIndex;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = w.entranceIndex;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = (u8)w.room;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = w.dest.pos;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = w.dest.rot.y;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0xDFF;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gSaveContext.respawnFlag = 1;

    static HOOK_ID hookId = 0;
    hookId = REGISTER_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, {
        *should = false;
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(hookId);
    });

    SPDLOG_INFO("[CoopWarp] warp entrance={} cs={}", w.entranceIndex, -1);
}

// Drain: fire the pending warp only in a safe window. Gate proves no premature
// warp — a queued warp cannot fire during a cutscene (Player_InCsMode), an
// in-progress transition (transitionTrigger != OFF), or the pause / game-over
// menu (pauseCtx.state != 0).
void DrainPendingWarp() {
    if (sPending.kind == PendingWarpKind::None || gPlayState == nullptr) {
        return;
    }

    if (std::chrono::steady_clock::now() - sPending.queuedAt > WARP_EXPIRY) {
        SPDLOG_INFO("[CoopWarp] dropping stale warp entrance={}", sPending.entranceIndex);
        sPending.kind = PendingWarpKind::None;
        return;
    }

    if (Player_InCsMode(gPlayState) || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF ||
        gPlayState->pauseCtx.state != 0) {
        return;
    }

    // Single-shot: clear before executing (the warp itself sets transitionTrigger).
    PendingWarp w = sPending;
    sPending.kind = PendingWarpKind::None;

    if (w.kind == PendingWarpKind::Entrance) {
        ExecuteEntranceWarp(w);
    } else {
        ExecutePositionWarp(w);
    }
}

} // namespace

// Per-frame entry point, called by the Anchor per-frame dispatcher in explicit tick
// order (see HookHandlers.cpp).
void CoopWarpTick() {
    DrainPendingWarp();
}

void RequestEntranceWarp(s16 entranceIndex, s32 cutsceneIndex) {
    sPending.kind = PendingWarpKind::Entrance;
    sPending.entranceIndex = entranceIndex;
    sPending.cutsceneIndex = cutsceneIndex;
    sPending.queuedAt = std::chrono::steady_clock::now();
}

void RequestPositionWarp(s16 entranceIndex, s16 room, PosRot dest) {
    sPending.kind = PendingWarpKind::Position;
    sPending.entranceIndex = entranceIndex;
    sPending.room = room;
    sPending.dest = dest;
    sPending.queuedAt = std::chrono::steady_clock::now();
}

void RegisterCoopWarpHooks(bool isConnected) {
    // Drop any in-flight request on disconnect so a reconnect starts clean.
    if (!isConnected) {
        sPending.kind = PendingWarpKind::None;
    }

    // NOTE: the queued-warp drain is NOT registered here; it is driven by the Anchor
    // per-frame dispatcher (CoopWarpTick) so the Anchor-internal tick order is explicit.
    // The disconnect path above still clears the pending slot.
}
