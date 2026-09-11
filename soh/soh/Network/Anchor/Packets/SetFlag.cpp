#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"

extern "C" {
#include "functions.h"

extern PlayState* gPlayState;
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
        // Gold Skulltulas: the COUNT is derived from the flag, not carried by the
        // item channel (see the ITEM_SKULL_TOKEN exclusion in HookHandlers).
        //
        // The token actor is not tracked -- EnSw_Init only re-categorises type 0 to
        // ACTORCAT_ENEMY, and the gold variants are types 1..4, so they stay
        // ACTORCAT_NPC and every client simulates its own. Both players can
        // therefore kill and collect the SAME Skulltula, and with the count on the
        // item channel each pickup added one everywhere: one Skulltula, four tokens.
        //
        // Reading the bit first makes it idempotent instead. The GS bits are the
        // authority on how many have been collected, a second pickup of the same one
        // finds its bit already set, and nothing is added.
        //
        // Vanilla only, matching the exclusion side: under the randomizer the token stays
        // on the item channel (tokensanity can place it where no GS flag is ever set), so
        // counting it here as well would be the double it is meant to prevent.
        bool gsTokenWasUnset = false;
        if (flagType == FLAG_GS_TOKEN && !IS_RANDO) {
            gsTokenWasUnset = !(GET_GS_FLAGS((flag & 0x1F00) >> 8) & (flag & 0xFF));
        }

        auto effect = new GameInteractionEffect::SetFlag();
        effect->parameters[0] = flagType;
        effect->parameters[1] = flag;
        effect->Apply();

        if (gsTokenWasUnset) {
            gSaveContext.inventory.gsTokens++;
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
