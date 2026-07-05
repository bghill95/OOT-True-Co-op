#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * RUPEE_CHANGE
 *
 * The shared co-op wallet. Every engine-side rupee delta (pickups, shop spends,
 * fees, minigames, scrubs) funnels through Rupees_ChangeBy, which fires the
 * OnRupeeChange hook -> SendPacket_RupeeChange. The receiver replays the DELTA so
 * simultaneous spends commute (A -40, B -15 => both land on T-55); the absolute
 * `total` is used only for drift logging and an occasional quiet-window snap.
 *
 * Rupee *items* are filtered out of the GIVE_ITEM channel (see HookHandlers.cpp
 * OnItemReceive and Packets/GiveItem.cpp) so a picked-up rupee's value travels
 * through this channel exactly once (no double-count).
 *
 * Echo-safety: a remote-applied Rupees_ChangeBy runs during ProcessIncomingPacketQueue,
 * where isProcessingIncomingPacket is set for the whole handler, so the send below
 * early-returns and the broadcast loop terminates after one hop.
 */

// Number of frames of local rupee-send silence required before the receiver is
// allowed to snap its accumulator to a peer's absolute total (~0.67s at 60fps).
static const uint32_t RUPEE_SNAP_QUIET_FRAMES = 40;

void Anchor::SendPacket_RupeeChange(s16 delta) {
    // IsSaveLoaded guarantees gPlayState != NULL. isProcessingIncomingPacket blocks
    // the echo from a remote-applied delta; isHandlingUpdateTeamState blocks the
    // initial-reconciliation write (which is a direct assignment anyway).
    if (!IsSaveLoaded() || isProcessingIncomingPacket || isHandlingUpdateTeamState ||
        !roomState.syncItemsAndFlags) {
        return;
    }

    // A no-op change carries no value; don't spend bandwidth or reset the quiet window.
    if (delta == 0) {
        return;
    }

    // Post-change absolute wallet value (rupees already spent/earned may still be
    // draining through the accumulator, so include it).
    s32 total = gSaveContext.rupees + gSaveContext.rupeeAccumulator;

    nlohmann::json payload;
    payload["type"] = RUPEE_CHANGE;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["delta"] = delta;
    payload["total"] = total;

    lastLocalRupeeSendFrame = gPlayState->gameplayFrames;

    SPDLOG_INFO("[Wallet] tx delta={} total={}", delta, total);

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_RupeeChange(nlohmann::json payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    s16 delta = payload["delta"].get<s16>();
    s32 total = payload["total"].get<s32>();

    // Snapshot the accumulator BEFORE applying: a snap is only safe when our own
    // wallet was idle (nothing pending to drain) as the peer's delta arrives.
    s16 preAccumulator = gSaveContext.rupeeAccumulator;

    // Apply the delta. This re-enters Rupees_ChangeBy -> OnRupeeChange ->
    // SendPacket_RupeeChange, but isProcessingIncomingPacket is set for the whole
    // queue drain, so that send early-returns: no echo, loop terminates in one hop.
    Rupees_ChangeBy(delta);

    s32 myTotal = gSaveContext.rupees + gSaveContext.rupeeAccumulator;
    s32 drift = total - myTotal;

    SPDLOG_INFO("[Wallet] rx delta={} total={} (drift={})", delta, total, drift);

    // Drift correction: only when (a) there is drift, (b) we have been quiet long
    // enough that no in-flight local delta explains it, and (c) our wallet was idle
    // pre-apply. Nudging the accumulator lets the HUD tick to the corrected value.
    if (drift != 0 && preAccumulator == 0 &&
        (gPlayState->gameplayFrames - lastLocalRupeeSendFrame) >= RUPEE_SNAP_QUIET_FRAMES) {
        SPDLOG_INFO("[Anchor] rupee drift {} vs {} (snapping {})", myTotal, total, drift);
        // drift is bounded by the wallet cap (<= 999), so it fits an s16 accumulator.
        gSaveContext.rupeeAccumulator += (s16)drift;
    }
}
