#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/TimeSync.h"
#include "soh/Network/Anchor/EnemySync.h" // ESYNC_LOG
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * TIME_SYNC
 *
 * World-clock sync. The lowest online clientId is the time authority and
 * broadcasts dayTime + day counters once a second; everyone else follows
 * (clocks diverge structurally: time freezes indoors, so one player entering a
 * house forks the clocks).
 *
 * Sun's Song is a multi-frame fast-forward (SUNSSONG_SPEED_TIME), not a jump:
 * while a client fast-forwards locally it ignores the stream, and when the
 * fast-forward lands it publishes the landing time with "force" — applied by
 * everyone, authority included, which then keeps streaming from there.
 */

static constexpr uint64_t BROADCAST_TICKS = 20;   // once a second
static constexpr int16_t APPLY_THRESHOLD = 0x100; // ~1.3 in-game minutes: don't fight local increment

// dayTime jump (in one frame) that counts as a direct set rather than natural
// flow: indoor-style Sun's Song (fade + reload), sleeping, debug-warp time
// override. SPEED_TIME advances 400/frame, far below this.
static constexpr int16_t JUMP_THRESHOLD = 0x2000;

static bool IsDaytimeValue(uint16_t t) {
    return t >= 0x4555 && t < 0xC001; // the boundaries z_parameter uses
}

static uint64_t sTick = 0;
static bool sWasSpeedingTime = false;
static uint16_t sPrevDayTime = 0;
static bool sSuppressJumpBroadcast = false; // the jump we're about to see is a remote apply, not ours

static bool IsTimeAuthority() {
    for (auto& [id, client] : Anchor::Instance->clients) {
        if (!client.self && client.online && client.isSaveLoaded && id < Anchor::Instance->ownClientId) {
            return false;
        }
    }
    return true;
}

void Anchor::SendPacket_TimeSync(bool force) {
    if (!IsSaveLoaded() || gPlayState == NULL) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = TIME_SYNC;
    payload["t"] = gSaveContext.dayTime;
    payload["d"] = gSaveContext.totalDays;
    payload["bgs"] = gSaveContext.bgsDayCount;
    if (force) {
        payload["force"] = 1;
    }

    // Time is global: every online peer, whatever their scene.
    for (auto& [clientId, client] : clients) {
        if (client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_TimeSync(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == NULL || !payload.contains("clientId") || !payload.contains("t")) {
        return;
    }
    // Our own fast-forward wins until it lands (the landing publishes "force").
    if (gSaveContext.sunsSongState == SUNSSONG_SPEED_TIME) {
        return;
    }

    bool force = payload.value("force", 0) != 0;
    // Periodic stream: only the sender that outranks us drives our clock
    // (self-consistent election, no handshake needed).
    if (!force && payload["clientId"].get<uint32_t>() > ownClientId) {
        return;
    }

    uint16_t t = payload["t"].get<uint16_t>();
    int16_t diff = (int16_t)(t - gSaveContext.dayTime); // u16 wrap-aware

    // A peer force-set the clock across day/night (Sun's Song, sleep, debug
    // warp). A bare dayTime snap skips every transition trigger — sky, ambience
    // and spawns stay stale until the next scene load, and time-frozen areas
    // (Kakariko, graveyard...) never repaint at all. Replay the exact vanilla
    // recipe the time-frozen-area Sun's Song uses: fade + nextDayTime + reload.
    if (force && IsDaytimeValue(t) != IsDaytimeValue(gSaveContext.dayTime) &&
        gSaveContext.sunsSongState == SUNSSONG_INACTIVE) {
        bool toDay = IsDaytimeValue(t);
        gSaveContext.nextDayTime = t;
        gPlayState->transitionType = toDay ? TRANS_TYPE_FADE_WHITE_FAST : TRANS_TYPE_FADE_BLACK_FAST;
        gSaveContext.nextTransitionType = toDay ? TRANS_TYPE_FADE_WHITE : TRANS_TYPE_FADE_BLACK;
        gPlayState->unk_11DE9 = 1;
        gSaveContext.totalDays = payload.value("d", gSaveContext.totalDays);
        gSaveContext.bgsDayCount = payload.value("bgs", gSaveContext.bgsDayCount);
        sSuppressJumpBroadcast = true; // the reload's own jump is this apply, not a new local event
        ESYNC_LOG("[TimeSync] remote day/night flip: fading to t={:#x}", t);
        return;
    }

    if (force || diff > APPLY_THRESHOLD || diff < -APPLY_THRESHOLD) {
        gSaveContext.dayTime = t;
        gSaveContext.totalDays = payload.value("d", gSaveContext.totalDays);
        gSaveContext.bgsDayCount = payload.value("bgs", gSaveContext.bgsDayCount);
        ESYNC_LOG("[TimeSync] applied t={:#x} (diff={}) force={}", t, diff, force);
    }
}

void TimeSyncTick() {
    if (!Anchor::Instance->IsSaveLoaded() || gPlayState == NULL) {
        sPrevDayTime = gSaveContext.dayTime; // no jump detection across save load
        return;
    }
    sTick++;

    bool speeding = gSaveContext.sunsSongState == SUNSSONG_SPEED_TIME;
    if (sWasSpeedingTime && !speeding) {
        // Our Sun's Song fast-forward just landed: everyone adopts it.
        Anchor::Instance->SendPacket_TimeSync(true);
        ESYNC_LOG("[TimeSync] suns song landed, forcing t={:#x}", (uint16_t)gSaveContext.dayTime);
    }
    sWasSpeedingTime = speeding;

    // Direct clock set outside a fast-forward: Sun's Song in a time-frozen area
    // (fade + reload lands at 0x0/0x8001 with no SPEED_TIME phase), sleeping,
    // debug-warp overrides... Broadcast it whoever we are — a force outranks
    // the periodic stream everywhere.
    int16_t jump = (int16_t)((uint16_t)gSaveContext.dayTime - sPrevDayTime);
    if (!speeding && (jump > JUMP_THRESHOLD || jump < -JUMP_THRESHOLD)) {
        if (sSuppressJumpBroadcast) {
            sSuppressJumpBroadcast = false; // this jump was a remote apply
        } else {
            Anchor::Instance->SendPacket_TimeSync(true);
            ESYNC_LOG("[TimeSync] direct time set detected, forcing t={:#x}", (uint16_t)gSaveContext.dayTime);
        }
    }
    sPrevDayTime = gSaveContext.dayTime;

    if (!speeding && IsTimeAuthority() && (sTick % BROADCAST_TICKS) == 0) {
        Anchor::Instance->SendPacket_TimeSync(false);
    }
}
