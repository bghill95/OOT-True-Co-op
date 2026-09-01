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

static uint64_t sTick = 0;
static bool sWasSpeedingTime = false;
static bool sRemoteFastForward = false; // our ride was remote-triggered: don't re-broadcast the landing

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

    // A peer's Sun's Song: a hard dayTime snap skips every day/night transition
    // trigger — the sky, ambience and spawns stay stale until the next scene
    // load (looked like "sync didn't happen"). Outdoors, ride the exact same
    // engine fast-forward the singer rode (z_parameter picks it up from
    // SUNSSONG_START and lands on the same dawn/dusk boundary). Indoors the sky
    // is not visible: silent snap below, and the next scene load repaints.
    if (force && gPlayState->envCtx.timeIncrement != 0 && gSaveContext.sunsSongState == SUNSSONG_INACTIVE &&
        (diff > APPLY_THRESHOLD || diff < -APPLY_THRESHOLD)) {
        gSaveContext.sunsSongState = SUNSSONG_START;
        sRemoteFastForward = true;
        ESYNC_LOG("[TimeSync] remote suns song: riding local fast-forward");
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
        return;
    }
    sTick++;

    bool speeding = gSaveContext.sunsSongState == SUNSSONG_SPEED_TIME;
    if (sWasSpeedingTime && !speeding) {
        if (sRemoteFastForward) {
            sRemoteFastForward = false; // a peer's song we replayed: no echo
        } else {
            // Our Sun's Song fast-forward just landed: everyone adopts it.
            Anchor::Instance->SendPacket_TimeSync(true);
            ESYNC_LOG("[TimeSync] suns song landed, forcing t={:#x}", (uint16_t)gSaveContext.dayTime);
        }
    }
    sWasSpeedingTime = speeding;

    if (!speeding && IsTimeAuthority() && (sTick % BROADCAST_TICKS) == 0) {
        Anchor::Instance->SendPacket_TimeSync(false);
    }
}
