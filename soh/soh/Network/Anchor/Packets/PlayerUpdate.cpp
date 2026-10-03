#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/FxSync.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
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
    // Same deferred-task problem for the root limb: while an animation drives
    // movement (movementFlags & 8: rolling, climbing, riding, ...) the root's
    // translation is consumed by AnimationContext_MoveActor AFTER this snapshot
    // and replaced by baseTransl (SkelAnime_UpdateTranslation) before drawing.
    // Streaming the raw root put the puppet's hips a full anim-translation off
    // (rider 37 units above the saddle). Apply the same substitution here.
    bool rootMoves = (player->skelAnime.movementFlags & 8) != 0;
    std::vector<int> jointArray;
    for (size_t i = 0; i < 24; i++) {
        Vec3s joint = (upperOverride && upperLimbCopyMap[i]) ? player->upperSkelAnime.jointTable[i]
                                                             : player->skelAnime.jointTable[i];
        if (i == 0 && rootMoves) {
            joint.x = player->skelAnime.baseTransl.x;
            joint.z = player->skelAnime.baseTransl.z;
            if (player->skelAnime.movementFlags & ANIM_FLAG_UPDATEY) {
                joint.y = player->skelAnime.baseTransl.y;
            }
        }
        jointArray.push_back(joint.x);
        jointArray.push_back(joint.y);
        jointArray.push_back(joint.z);
    }
    payload["jointTable"] = jointArray;
    payload["upperLimbRot"] = player->upperLimbRot;
    payload["shapeYOffset"] = player->actor.shape.yOffset;
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
    payload["bowStr"] = player->unk_858;                 // bow string draw amount
    payload["mask"] = player->currentMask;               // worn mask (Keaton/Skull/Spooky/Bunny/...)
    payload["mws"] = player->meleeWeaponState;           // sword/stick swing trail
    payload["bgs"] = (gSaveContext.swordHealth <= 0.0f); // snapped Giant's Knife
    payload["giId"] = (player->unk_862 != 0) ? (s16)player->getItemEntry.getItemId : (s16)-1; // item held up
    payload["giMod"] = (s8)player->getItemEntry.modIndex;
    payload["actionVar1"] = player->av1.actionVar1;
    // Where the actor we carry actually sits, and which way it faces. Both are
    // written by Player_PostLimbDraw (the draw pass) on this side, so the values
    // read here -- before this frame's actor updates -- are last frame's drawn
    // pose. Guessing the position from the puppet's head put cuccos (which
    // ride lower than a pot) through the puppet's chest; guessing the yaw
    // from the puppet left the cucco's own per-frame random wriggle in
    // charge, so it faced nowhere in particular.
    if ((player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) && player->heldActor != NULL &&
        player->heldActor->update != NULL) {
        payload["carryPos"] = player->heldActor->world.pos;
        payload["carryRot"] = player->heldActor->shape.rot;
    }
    if ((player->stateFlags1 & PLAYER_STATE1_BOOMERANG_THROWN) && player->boomerangActor != NULL &&
        player->boomerangActor->update != NULL) {
        payload["boomPos"] = player->boomerangActor->world.pos;
        payload["boomYaw"] = player->boomerangActor->world.rot.y;   // flight yaw (EnBoom_Draw basis)
        payload["boomPitch"] = player->boomerangActor->world.rot.x; // flight pitch
    }
    // Mount: the horse under us exists only on our game (scene-placed, player
    // driven). While rideActor is set (mounting through dismount) the peers
    // drive a replica from this pose and gait (FxSync::TickPuppetHorse).
    if (player->rideActor != NULL && player->rideActor->update != NULL && player->rideActor->id == ACTOR_EN_HORSE) {
        EnHorse* horse = (EnHorse*)player->rideActor;
        payload["horsePos"] = horse->actor.world.pos;
        payload["horseRot"] = horse->actor.shape.rot; // yaw + body tilt
        payload["horseType"] = (s8)horse->type;
        payload["horseAnim"] = (s8)horse->animationIdx;
        payload["horseFrame"] = horse->skin.skelAnime.curFrame;
        payload["horseSpeed"] = horse->skin.skelAnime.playSpeed;
    }
    FxSync::CollectProjectiles(payload); // arrows + hookshot in flight
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
        // Fixed 24-limb walk over a vector whose length comes off the wire: a short
        // packet (peer on another build) read past the end. Take what fits.
        std::vector<int> jointArray = payload["jointTable"];
        size_t limbs = std::min<size_t>(24, jointArray.size() / 3);
        for (size_t i = 0; i < limbs; i++) {
            client.jointTable[i].x = jointArray[i * 3];
            client.jointTable[i].y = jointArray[i * 3 + 1];
            client.jointTable[i].z = jointArray[i * 3 + 2];
        }
        client.upperLimbRot = payload["upperLimbRot"].get<Vec3s>();
        // Absent on an older peer: 0 is the resting value, so the puppet simply keeps
        // the behaviour it had before.
        client.shapeYOffset = payload.value("shapeYOffset", 0.0f);
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
        client.bowString = payload.value("bowStr", 0.0f);
        client.currentMask = (u8)payload.value("mask", 0);
        client.meleeWeaponState = (s8)payload.value("mws", 0);
        client.bgsBroken = payload.value("bgs", false);
        client.giId = (s16)payload.value("giId", -1);
        client.giMod = (s8)payload.value("giMod", 0);
        client.actionVar1 = payload["actionVar1"].get<s8>();
        client.carryActive = payload.contains("carryPos");
        if (client.carryActive) {
            client.carryPos = payload["carryPos"].get<Vec3f>();
            client.carryRot = payload.value("carryRot", client.carryRot);
        }
        client.boomActive = payload.contains("boomPos");
        if (client.boomActive) {
            client.boomPos = payload["boomPos"].get<Vec3f>();
            client.boomYaw = payload.value("boomYaw", (s16)0);
            client.boomPitch = payload.value("boomPitch", (s16)0);
        }
        client.horseActive = payload.contains("horsePos");
        if (client.horseActive) {
            client.horsePos = payload["horsePos"].get<Vec3f>();
            client.horseRot = payload.value("horseRot", Vec3s{ 0, 0, 0 });
            client.horseType = (s8)payload.value("horseType", 0);
            client.horseAnim = (s8)payload.value("horseAnim", 0);
            client.horseFrame = payload.value("horseFrame", 0.0f);
            client.horseSpeed = payload.value("horseSpeed", 1.0f);
        }
        FxSync::IngestPlayerProjectiles(clientId, payload);
    }

    if (shouldRefreshActors) {
        RefreshClientActors();
    }
}
