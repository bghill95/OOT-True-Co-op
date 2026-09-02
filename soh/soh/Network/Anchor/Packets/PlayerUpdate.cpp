#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * PLAYER_UPDATE
 *
 * Contains real-time data necessary to update other clients in the same scene as the player
 *
 * Sent every frame to other clients within the same scene
 *
 * Note: This packet is sent _a lot_, so please do not include any unnecessary data in it
 */

void Anchor::SendPacket_PlayerUpdate() {
    if (!IsSaveLoaded()) {
        return;
    }

    uint32_t currentPlayerCount = 0;
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            currentPlayerCount++;
        }
    }
    if (currentPlayerCount == 0) {
        return;
    }

    Player* player = GET_PLAYER(gPlayState);
    nlohmann::json payload;

    payload["type"] = PLAYER_UPDATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["entranceIndex"] = gSaveContext.entranceIndex;
    payload["linkAge"] = gSaveContext.linkAge;
    payload["posRot"]["pos"] = player->actor.world.pos;
    payload["posRot"]["rot"] = player->actor.shape.rot;
    // While carrying an actor overhead, the arms-up pose lives in
    // upperSkelAnime and only reaches skelAnime.jointTable via a deferred
    // AnimationContext copy task that runs AFTER this snapshot — so the
    // streamed table would always show the pre-blend (arms down) pose.
    // Re-apply the blend here, using z_player.c's sUpperBodyLimbCopyMap
    // (upper body + head + arms limbs).
    static const uint8_t upperLimbCopyMap[24] = {
        false, false, false, false, false, false, false, false, false, false, // none/root/waist/legs
        true,  true,  true,  true,  true,  true,  true,  true,  true,  true,  // upper/head/hat/collar/arms
        true,  true,  false, false                                            // sheath/torso
    };
    bool upperOverride = (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) != 0;
    std::vector<int> jointArray;
    for (size_t i = 0; i < 24; i++) {
        Vec3s joint = (upperOverride && upperLimbCopyMap[i]) ? player->upperSkelAnime.jointTable[i]
                                                             : player->skelAnime.jointTable[i];
        jointArray.push_back(joint.x);
        jointArray.push_back(joint.y);
        jointArray.push_back(joint.z);
    }
    payload["jointTable"] = jointArray;
    payload["upperLimbRot"] = player->upperLimbRot;
    payload["currentBoots"] = player->currentBoots;
    payload["currentShield"] = player->currentShield;
    payload["currentTunic"] = player->currentTunic;
    payload["stateFlags1"] = player->stateFlags1;
    payload["stateFlags2"] = player->stateFlags2;
    payload["buttonItem0"] = gSaveContext.equips.buttonItems[0];
    payload["itemAction"] = player->itemAction;
    payload["heldItemAction"] = player->heldItemAction;
    payload["modelGroup"] = player->modelGroup;
    payload["invincibilityTimer"] = player->invincibilityTimer;
    payload["unk_862"] = player->unk_862;
    payload["stickFlame"] = player->unk_860;
    payload["stickLen"] = player->unk_85C;
    payload["actionVar1"] = player->av1.actionVar1;
    if ((player->stateFlags1 & PLAYER_STATE1_BOOMERANG_THROWN) && player->boomerangActor != NULL &&
        player->boomerangActor->update != NULL) {
        payload["boomPos"] = player->boomerangActor->world.pos;
        payload["boomYaw"] = player->boomerangActor->shape.rot.y;
    }
    payload["quiet"] = true;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_PlayerUpdate(nlohmann::json payload) {
    uint32_t clientId = payload["clientId"].get<uint32_t>();

    bool shouldRefreshActors = false;

    if (clients.contains(clientId)) {
        auto& client = clients[clientId];

        if (client.linkAge != payload["linkAge"].get<s32>()) {
            shouldRefreshActors = true;
        }

        client.sceneNum = payload["sceneNum"].get<s16>();
        client.entranceIndex = payload["entranceIndex"].get<s32>();
        client.linkAge = payload["linkAge"].get<s32>();
        client.posRot = payload["posRot"].get<PosRot>();
        std::vector<int> jointArray = payload["jointTable"];
        for (int i = 0; i < 24; i++) {
            client.jointTable[i].x = jointArray[i * 3];
            client.jointTable[i].y = jointArray[i * 3 + 1];
            client.jointTable[i].z = jointArray[i * 3 + 2];
        }
        client.upperLimbRot = payload["upperLimbRot"].get<Vec3s>();
        client.currentBoots = payload["currentBoots"].get<s8>();
        client.currentShield = payload["currentShield"].get<s8>();
        client.currentTunic = payload["currentTunic"].get<s8>();
        client.stateFlags1 = payload["stateFlags1"].get<u32>();
        client.stateFlags2 = payload["stateFlags2"].get<u32>();
        client.buttonItem0 = payload["buttonItem0"].get<u8>();
        client.itemAction = payload["itemAction"].get<s8>();
        client.heldItemAction = payload["heldItemAction"].get<s8>();
        client.modelGroup = payload["modelGroup"].get<u8>();
        client.invincibilityTimer = payload["invincibilityTimer"].get<s8>();
        client.unk_862 = payload["unk_862"].get<s16>();
        client.dekuStickFlame = payload.value("stickFlame", (s16)0);
        client.dekuStickLength = payload.value("stickLen", 0.0f);
        client.actionVar1 = payload["actionVar1"].get<s8>();
        client.boomActive = payload.contains("boomPos");
        if (client.boomActive) {
            client.boomPos = payload["boomPos"].get<Vec3f>();
            client.boomYaw = payload.value("boomYaw", (s16)0);
        }
    }

    if (shouldRefreshActors) {
        RefreshClientActors();
    }
}
