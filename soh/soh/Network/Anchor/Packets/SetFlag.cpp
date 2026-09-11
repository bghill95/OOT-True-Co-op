#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"

#include <bit>

extern "C" {
#include "functions.h"
#include "variables.h"

extern PlayState* gPlayState;
}

// The Gold Skulltula count is a DERIVED value in co-op, never a transported one.
//
// Vanilla keeps gsTokens and gsFlags in step because a single function writes both:
// Item_Give (z_parameter.c:1947-1948) sets the quest bit and increments the count in
// consecutive lines. The moment those two travel on different channels they drift,
// and a five-angle review found the same defect from three directions:
//
//   - UpdateTeamState merged the BITS with |= but the COUNT with max(). For two sets
//     that are not nested, popcount(A|B) > max(|A|,|B|), and the gap is PERMANENT:
//     the merged bit stops that Skulltula from ever respawning, so the missing token
//     can never be re-collected and nothing ever recounts.
//   - En_Si increments at pickup (z_en_si.c:126) but only sets the bit when the
//     textbox closes (z_en_si.c:144). A peer's SET_FLAG landing inside that window
//     found the bit unset and added a second token for one Skulltula.
//   - Bottle Adventure (Restorations/BottleAdventure.cpp:160-168) uses gsTokens as
//     scratch storage for an item id, behind NO cvar and no rando guard -- it is
//     armed in every session. max() promoted that scrambled value, then carried it
//     to the other save, where nothing could bring it back down.
//
// Recomputing from the bits removes all three at once: the result depends neither on
// arrival order, nor on a window, nor on whatever the local count happened to hold.
// The quest bit goes with it, because Item_Give used to set it and the entire Gold
// Skulltula panel of the pause menu is gated on it (z_kaleido_collect.c:735) -- a
// player whose tokens all came from the peer had a count with nothing drawn to show.
//
// Randomizer is deliberately left alone: RSK_STARTING_SKULLTULA_TOKEN
// (randomizer/savefile.cpp:144) grants tokens with no bits behind them, so a popcount
// would erase them. There the token stays on the item channel exactly as it was
// before this branch -- including its own double-count under tokensanity OFF, which
// is pre-existing and not made worse here. UpdateTeamState keeps a rando-only max()
// for the same reason: a count that cannot be derived still has to not go backwards.
//
// One vanilla case is knowingly wrong: SaveManager::InitFileMaxed (SaveManager.cpp:907)
// sets gsTokens to 100 without setting any bit, so the first GS packet takes such a
// file back to 0. It is reachable only through DebugSaveFileMode, and a debug save is
// not a state this path owes anything to.
void Anchor_RecomputeGsTokensFromFlags() {
    if (IS_RANDO) {
        return;
    }

    int32_t count = 0;
    for (size_t i = 0; i < ARRAY_COUNT(gSaveContext.gsFlags); i++) {
        count += std::popcount(static_cast<uint32_t>(gSaveContext.gsFlags[i]));
    }

    gSaveContext.inventory.gsTokens = (s16)count;
    if (count > 0) {
        gSaveContext.inventory.questItems |= gBitFlags[QUEST_SKULL_TOKEN];
    }
}

/**
 * SET_FLAG
 *
 * Fired when a flag is set in the save context
 */

void Anchor::SendPacket_SetFlag(s16 sceneNum, s16 flagType, s16 flag) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = SET_FLAG;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["addToQueue"] = true;
    payload["sceneNum"] = sceneNum;
    payload["flagType"] = flagType;
    payload["flag"] = flag;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SetFlag(nlohmann::json payload) {
    if (!IsSaveLoaded() || !roomState.syncItemsAndFlags) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    s16 flagType = payload["flagType"].get<s16>();
    s16 flag = payload["flag"].get<s16>();

    if (sceneNum == SCENE_ID_MAX) {
        auto effect = new GameInteractionEffect::SetFlag();
        effect->parameters[0] = flagType;
        effect->parameters[1] = flag;
        effect->Apply();

        // Gold Skulltulas: the COUNT is DERIVED from the bits, never transported.
        // See the ITEM_SKULL_TOKEN exclusion in HookHandlers for the other half.
        if (flagType == FLAG_GS_TOKEN) {
            Anchor_RecomputeGsTokensFromFlags();
        }

        // Special case: If King Zora moved, and the player has Ruto's Letter, convert it to an empty bottle
        if (flagType == FLAG_EVENT_CHECK_INF && flag == EVENTCHKINF_KING_ZORA_MOVED &&
            Inventory_HasSpecificBottle(ITEM_LETTER_RUTO)) {
            Inventory_ReplaceItem(gPlayState, ITEM_LETTER_RUTO, ITEM_BOTTLE);
        }
    } else {
        // The water temple water level (0x1C, 0x1D, 0x1E) used to be excluded here.
        // That is right for upstream Anchor, where each player has their OWN world and
        // must keep their own level; it is backwards for a shared world. Excluded, the
        // two players stand in different geometry: one swims where the other walks, and
        // the entrance is not in the same place. Bg_Mizu_Water is entirely flag-driven
        // and re-reads all three every frame in WaitForAction (z_bg_mizu_water.c:171),
        // so replicating them is enough -- both levels converge on their own, each
        // client running its own Math_StepToF. Bg_Mizu_Water_Init unsets all three and
        // sets the current one, which is a no-op for the peer: all-clear yields action
        // index 0, and re-setting the level it already holds is skipped as well.

        // Special case: Ignore forest temple elevator flag, stored at 0x1B.
        if (sceneNum == SCENE_FOREST_TEMPLE && flagType == FLAG_SCENE_SWITCH && flag == 0x1B) {
            return;
        }

        auto effect = new GameInteractionEffect::SetSceneFlag();
        effect->parameters[0] = sceneNum;
        effect->parameters[1] = flagType;
        effect->parameters[2] = flag;
        effect->Apply();
    }
}
