#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"

extern "C" {
extern PlayState* gPlayState;
}

/**
 * UPDATE_DUNGEON_ITEMS
 *
 * Carries the per-dungeon boss key / compass / map bits, and the small-key count.
 *
 * The two halves need opposite treatment, and getting that wrong cost a boss key.
 *
 * `dungeonItems` is a BITMASK (macros.h, CHECK_DUNGEON_ITEM = & gBitFlags[item]).
 * Its bits are only ever gained, never lost, so the absolute value travels and
 * the receiver ORs it in. OR is commutative and idempotent: two players opening
 * the boss-key chest and the map chest in the same breath both keep both bits,
 * whatever order the packets land in. The previous plain assignment made the
 * later packet erase the other player's bit -- and losing the boss key that way
 * leaves the dungeon unfinishable.
 *
 * `dungeonKeys` is a COUNT that goes up and down, so no absolute value can be
 * merged. It travels as a DELTA, exactly like the shared wallet in
 * Packets/RupeeChange.cpp and for the same reason: simultaneous changes have to
 * commute (A picks one up, B spends one => both land on the same total). The
 * absolute count still travels, but only for drift logging and a quiet-window
 * snap, again mirroring the wallet.
 */

// Frames of local key-send silence required before the receiver may snap its
// count to a peer's absolute value (~0.67s at 60fps). Below that, a disagreement
// is more likely explained by a key event of ours still in flight.
static const uint32_t DUNGEON_KEY_SNAP_QUIET_FRAMES = 40;

void Anchor::SendPacket_UpdateDungeonItems(s8 keyDelta) {
    // isProcessingIncomingPacket blocks the echo of a remotely applied change.
    // It mattered little while this packet was idempotent; with a delta an echo
    // would count the key twice.
    if (!IsSaveLoaded() || isProcessingIncomingPacket || !roomState.syncItemsAndFlags) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = UPDATE_DUNGEON_ITEMS;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["mapIndex"] = gSaveContext.mapIndex;
    payload["dungeonItems"] = gSaveContext.inventory.dungeonItems[gSaveContext.mapIndex];
    payload["dungeonKeys"] = gSaveContext.inventory.dungeonKeys[gSaveContext.mapIndex];
    payload["keyDelta"] = keyDelta;

    if (keyDelta != 0 && gPlayState != NULL) {
        lastLocalDungeonKeySendFrame = gPlayState->gameplayFrames;
    }

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_UpdateDungeonItems(nlohmann::json payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    // mapIndex comes straight off the wire and indexes two fixed arrays inside
    // gSaveContext: unchecked, a malformed or mismatched-build packet writes past
    // them, into the middle of the save.
    u16 mapIndex = payload["mapIndex"].get<u16>();
    if (mapIndex >= ARRAY_COUNT(gSaveContext.inventory.dungeonItems) ||
        mapIndex >= ARRAY_COUNT(gSaveContext.inventory.dungeonKeys)) {
        SPDLOG_WARN("[Anchor] UPDATE_DUNGEON_ITEMS: mapIndex {} out of range, dropped", mapIndex);
        return;
    }

    // Bits: merge, never replace.
    gSaveContext.inventory.dungeonItems[mapIndex] |= payload["dungeonItems"].get<u8>();

    s8 remoteKeys = payload["dungeonKeys"].get<s8>();

    // A peer on a build from before this channel carried a delta sends no field.
    // Fall back to its old behaviour rather than silently dropping its key event:
    // wrong under a race, but a race is rarer than a version mismatch.
    if (!payload.contains("keyDelta")) {
        gSaveContext.inventory.dungeonKeys[mapIndex] = remoteKeys;
        return;
    }

    s8 keyDelta = payload["keyDelta"].get<s8>();
    if (keyDelta != 0) {
        gSaveContext.inventory.dungeonKeys[mapIndex] += keyDelta;
    }

    s8 myKeys = gSaveContext.inventory.dungeonKeys[mapIndex];
    s32 drift = (s32)remoteKeys - (s32)myKeys;

    // Drift correction, same rule as the wallet: only when we have been quiet long
    // enough that no key event of ours can still be in flight to explain it. A
    // dropped packet would otherwise leave the two saves apart for good, since
    // nothing else resynchronises these two arrays on reconnect.
    if (drift != 0 && gPlayState != NULL &&
        (gPlayState->gameplayFrames - lastLocalDungeonKeySendFrame) >= DUNGEON_KEY_SNAP_QUIET_FRAMES) {
        SPDLOG_INFO("[Anchor] dungeon {} key drift {} vs {} (snapping)", mapIndex, myKeys, remoteKeys);
        gSaveContext.inventory.dungeonKeys[mapIndex] = remoteKeys;
    }
}
