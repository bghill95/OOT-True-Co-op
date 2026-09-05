#include "Anchor.h"
#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/FxSync.h"
#include "soh/Enhancements/nametag.h"
#include "soh/Enhancements/item-tables/ItemTableManager.h"
#include "soh/Enhancements/randomizer/randomizer.h"
#include <unordered_map>
#include "soh/frame_interpolation.h"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
void func_8083AE40(Player* player, s16 objectId); // z_player.c: DMA the get-item object into giObjectSegment
}

// Get-item id last applied per puppet (the object DMA is only re-issued on change).
static std::unordered_map<uint32_t, int16_t> sPuppetGi;
// Model group last pushed through Player_SetModels for each puppet (see the
// cutscene-item block in DummyPlayer_Update). -1 = never.
static std::unordered_map<uint32_t, s32> sPuppetModelsGroup;

// Hijacking player->zTargetActiveTimer (unused s32 for the dummy) to store the clientId for convenience
#define DUMMY_CLIENT_ID player->zTargetActiveTimer

static DamageTable DummyPlayerDamageTable = {
    /* Deku nut      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Deku stick    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Slingshot     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Explosive     */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Boomerang     */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Normal arrow  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Hammer swing  */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Hookshot      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Kokiri sword  */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master sword  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant's Knife */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Fire arrow    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice arrow     */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light arrow   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Unk arrow 1   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 2   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 3   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Fire magic    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice magic     */ DMG_ENTRY(3, PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light magic   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Shield        */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Mirror Ray    */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Kokiri spin   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant spin    */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master spin   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Kokiri jump   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant jump    */ DMG_ENTRY(8, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master jump   */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Unknown 1     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Unblockable   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Hammer jump   */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Unknown 2     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
};

void DummyPlayer_Init(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->actorIndexToClientId[actor->params];
    DUMMY_CLIENT_ID = clientId;

    if (!Anchor::Instance->clients.contains(DUMMY_CLIENT_ID)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[DUMMY_CLIENT_ID];

    // Hack to account for usage of gSaveContext in Player_Init
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;

    // #region modeled after EnTorch2_Init and Player_Init
    actor->room = -1;
    player->itemAction = player->heldItemAction = -1;
    player->heldItemId = ITEM_NONE;
    Player_UseItem(play, player, ITEM_NONE);
    Player_SetModelGroup(player, Player_ActionToModelGroup(player, player->heldItemAction));
    play->playerInit(player, play, gPlayerSkelHeaders[client.linkAge]);

    play->func_11D54(player, play);
    // #endregion

    player->cylinder.base.acFlags = AC_ON | AC_TYPE_PLAYER;
    player->cylinder.base.ocFlags2 = OC2_TYPE_1;
    player->cylinder.info.bumperFlags = BUMP_ON | BUMP_HOOKABLE | BUMP_NO_HITMARK;
    player->actor.flags |= ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER;
    player->cylinder.dim.radius = 30;
    player->actor.colChkInfo.damageTable = &DummyPlayerDamageTable;

    gSaveContext.linkAge = originalAge;

    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (!isGlobalRoom) {
        NameTag_RegisterForActorWithOptions(actor, client.name.c_str(), {});
    }
}

void Math_Vec3s_Copy(Vec3s* dest, Vec3s* src) {
    dest->x = src->x;
    dest->y = src->y;
    dest->z = src->z;
}

// Update the actor with new data from the client
void DummyPlayer_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    if (!Anchor::Instance->clients.contains(DUMMY_CLIENT_ID)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[DUMMY_CLIENT_ID];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        actor->world.pos.x = -9999.0f;
        actor->world.pos.y = -9999.0f;
        actor->world.pos.z = -9999.0f;
        actor->shape.shadowAlpha = 0;
        return;
    }

    actor->shape.shadowAlpha = 255;
    Math_Vec3s_Copy(&player->upperLimbRot, &client.upperLimbRot);
    Math_Vec3s_Copy(&actor->shape.rot, &client.posRot.rot);
    Math_Vec3f_Copy(&actor->world.pos, &client.posRot.pos);
    player->skelAnime.jointTable = client.jointTable;
    player->currentBoots = client.currentBoots;
    player->currentShield = client.currentShield;
    player->currentTunic = client.currentTunic;
    player->stateFlags1 = client.stateFlags1;
    player->stateFlags2 = client.stateFlags2;
    player->itemAction = client.itemAction;
    player->heldItemAction = client.heldItemAction;
    player->invincibilityTimer = client.invincibilityTimer;
    player->unk_862 = client.unk_862;
    player->unk_85C = client.dekuStickLength; // stick draw scale + weapon-tip reach (z_player_lib.c)
    player->unk_858 = client.bowString;       // bow string draw amount — stale value froze the bow drawn
    player->currentMask = client.currentMask; // mask is drawn from the Player struct by Player_Draw
    player->meleeWeaponState = client.meleeWeaponState; // swing trail; the puppet got its own blure in Player_Init
    // Get-item presentation (item held overhead, bottle catch, trade show-off):
    // unk_862 was already streamed, but the entry + object were never set, so the
    // puppet drew nothing. Player_Init gave it a giObjectSegment buffer; load the
    // owner's object into it the same way Player does (func_8083AE40).
    {
        auto itg = sPuppetGi.find(client.clientId);
        if (itg == sPuppetGi.end() || itg->second != client.giId) {
            sPuppetGi[client.clientId] = client.giId;
            if (client.giId < 0) {
                player->getItemEntry = GetItemEntry GET_ITEM_NONE;
            } else {
                GetItemEntry entry = (client.giMod == MOD_RANDOMIZER)
                    ? Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(client.giId)).GetGIEntry_Copy()
                    : ItemTableManager::Instance->RetrieveItemEntry(MOD_NONE, client.giId);
                player->getItemEntry = entry;
                if (player->giObjectSegment != NULL) {
                    func_8083AE40(player, entry.objectId);
                }
            }
        }
    }
    player->av1.actionVar1 = client.actionVar1;

    // Burning deku stick: on the real player the flame is an effect spawned
    // every UPDATE frame at the weapon tip (Player_UpdateBurningDekuStick) —
    // the puppet never runs that, so its lit stick looked cold. The tip is
    // valid here: the puppet's full player draw filled meleeWeaponInfo last
    // frame. Constants match z_player.c's D_808547A4..C0.
    if (client.dekuStickFlame != 0 && client.heldItemAction == PLAYER_IA_DEKU_STICK) {
        static Vec3f flameVel = { 0.0f, 0.5f, 0.0f };
        static Vec3f flameAccel = { 0.0f, 0.5f, 0.0f };
        static Color_RGBA8 flamePrim = { 255, 255, 100, 255 };
        static Color_RGBA8 flameEnv = { 255, 50, 0, 0 };
        f32 intensity = 1.0f;
        if (client.dekuStickFlame > 200) {
            intensity = (210 - client.dekuStickFlame) / 10.0f;
        } else if (client.dekuStickFlame < 20) {
            intensity = client.dekuStickFlame / 20.0f;
        }
        func_8002836C(play, &player->meleeWeaponInfo[0].tip, &flameVel, &flameAccel, &flamePrim, &flameEnv,
                      intensity * 200.0f, 0, 8);
    }

    // Boomerang in flight: the real EN_BOOM exists only on the thrower's game.
    FxSync::TickPuppetBoomerang(client.clientId, client.boomActive, client.boomPos, client.boomYaw, client.boomPitch);
    // Arrows (incl. elemental/nut/seed) and hookshot, same model.
    FxSync::TickPuppetProjectiles(client.clientId, actor);
    // Charged spin attack glow + release blast (EN_M_THUNDER), parented to us.
    FxSync::TickPuppetSpinCharge(client.clientId, actor);
    // Mount: the rider's horse, replicated from its streamed pose and gait.
    FxSync::TickPuppetHorse(client.clientId, actor, client.horseActive, client.horseType, client.horsePos,
                            client.horseRot, client.horseAnim, client.horseFrame, client.horseSpeed);

    {
        // Hack to account for usage of gSaveContext
        s32 originalAge = gSaveContext.linkAge;
        gSaveContext.linkAge = client.linkAge;
        u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
        gSaveContext.equips.buttonItems[0] = client.buttonItem0;

        // "Cutscene items" -- bottle drink / pour / bug release, ocarina, spells
        // -- go through Player_SetupActionPreserveItemAction, which swaps the
        // hand + sheath models for itemAction via Player_SetModels WITHOUT
        // touching modelGroup (itemAction != heldItemAction for the whole
        // action, then Player_SetupAction restores both). Mirroring modelGroup
        // alone left the puppet holding its previous item while the owner drank
        // from a bottle or played the ocarina.
        s32 modelsGroup = client.modelGroup;
        // Bounded before use: Player_ActionToModelGroup indexes a table with it,
        // and itemAction comes straight off the wire (a corrupt or version-
        // skewed peer would read past sActionModelGroups into garbage model
        // groups, which Player_SetModels then uses as further table indices).
        if (client.itemAction >= PLAYER_IA_NONE && client.itemAction < PLAYER_IA_MAX &&
            client.itemAction != client.heldItemAction) {
            modelsGroup = Player_ActionToModelGroup(player, client.itemAction);
        }
        auto cur = sPuppetModelsGroup.try_emplace(client.clientId, -1).first;
        if (player->modelGroup != client.modelGroup) {
            Player_SetModelGroup(player, client.modelGroup);
            cur->second = client.modelGroup;
        }
        if (cur->second != modelsGroup) {
            Player_SetModels(player, modelsGroup);
            cur->second = modelsGroup;
        }

        gSaveContext.linkAge = originalAge;
        gSaveContext.equips.buttonItems[0] = originalButtonItem0;
    }

    if (Anchor::Instance->roomState.pvpMode == 0 ||
        (Anchor::Instance->roomState.pvpMode == 1 &&
         client.teamId == CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default"))) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    actor->flags &= ~ACTOR_FLAG_LOCK_ON_DISABLED;

    // Tracked enemies already damage the remote player locally on their own
    // machine (mirrored AT colliders) — relaying the puppet hit too would deal
    // double damage when PvP mode has these colliders active.
    if (player->cylinder.base.acFlags & AC_HIT && player->invincibilityTimer == 0 &&
        !(EnemySync::MirroringEnabled() && player->cylinder.base.ac != NULL &&
          EnemySync::IsTrackedEnemy(player->cylinder.base.ac))) {
        Anchor::Instance->SendPacket_DamagePlayer(client.clientId, player->actor.colChkInfo.damageEffect,
                                                  player->actor.colChkInfo.damage);
        if (player->actor.colChkInfo.damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
            Actor_SetColorFilter(&player->actor, 0, 0xFF, 0, 24);
        } else {
            player->invincibilityTimer = 20;
        }
    }

    Collider_UpdateCylinder(&player->actor, &player->cylinder);

    if (!(player->stateFlags2 & PLAYER_STATE2_FROZEN)) {
        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_HANGING_OFF_LEDGE |
                                     PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_ON_HORSE))) {
            CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);
        }

        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_DAMAGED)) &&
            (player->invincibilityTimer <= 0)) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &player->cylinder.base);

            if (player->invincibilityTimer < 0) {
                CollisionCheck_SetAT(play, &play->colChkCtx, &player->cylinder.base);
            }
        }
    }

    if (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE)) {
        player->actor.colChkInfo.mass = MASS_IMMOVABLE;
    } else {
        player->actor.colChkInfo.mass = 50;
    }

    Collider_ResetCylinderAC(play, &player->cylinder.base);
}

void DummyPlayer_Draw(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    if (!Anchor::Instance->clients.contains(DUMMY_CLIENT_ID)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[DUMMY_CLIENT_ID];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        return;
    }

    // Hack to account for usage of gSaveContext in Player_Draw
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;
    u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
    gSaveContext.equips.buttonItems[0] = client.buttonItem0;

    Player_Draw((Actor*)player, play);
    gSaveContext.linkAge = originalAge;
    gSaveContext.equips.buttonItems[0] = originalButtonItem0;
}

void DummyPlayer_Destroy(Actor* actor, PlayState* play) {
    uint32_t clientId = (uint32_t)((Player*)actor)->zTargetActiveTimer;
    // Player_Init gave the puppet a sword-trail blure (Effect_Add); Player_Destroy
    // is what frees it and never runs for a puppet. Each respawn (reconnect, room
    // change, RefreshClientActors) leaked one of the scene's 25 blures; once the
    // pool was empty the next puppet got TOTAL_EFFECT_COUNT and its first sword
    // swing crashed in EffectBlure_ChangeType(NULL). Effect_Delete ignores the
    // "none" index.
    Effect_Delete(play, ((Player*)actor)->meleeWeaponEffectIndex);
    ((Player*)actor)->meleeWeaponEffectIndex = TOTAL_EFFECT_COUNT;
    sPuppetGi.erase(clientId);
    sPuppetModelsGroup.erase(clientId);
    // The projectile replicas (arrows, hookshot, boomerang) are parented to this
    // puppet and only ticked from its update: reap them here, keyed by the id
    // the puppet carries itself. Matching `client.player == actor` from the
    // generic OnActorDestroy hook never fired: RefreshClientActors overwrites
    // client.player with the NEW puppet (or erases the client altogether)
    // before the old one reaches Actor_Delete. A hookshot in flight at the
    // moment a peer dropped therefore survived, parented to a freed block the
    // next puppet reused: a chain from the reconnected player's hand to a
    // fixed point in the air.
    FxSync::ReapClientProjectiles(clientId);
}

// z_map_exp.c draws one minimap arrow per remote player: the puppets alive in
// our scene. Fills `out` (at most `max`) and returns the count.
extern "C" s32 Anchor_GetPuppetActors(Actor** out, s32 max) {
    s32 n = 0;
    if (Anchor::Instance == nullptr || gPlayState == NULL || out == NULL) {
        return 0;
    }
    for (auto& [cid, client] : Anchor::Instance->clients) {
        if (n >= max) {
            break;
        }
        if (client.self || !client.online || client.player == NULL || client.player->actor.update == NULL ||
            client.sceneNum != gPlayState->sceneNum) {
            continue;
        }
        out[n++] = &client.player->actor;
    }
    return n;
}

// z_player_lib.c asks whether a Player struct is one of our puppets with a
// snapped Giant's Knife: 1/0 for a puppet, -1 for anything else (the local
// player, Dark Link's Player struct...) so the caller falls back to the save.
extern "C" s32 Anchor_PuppetBgsBroken(Player* p) {
    if (Anchor::Instance == nullptr || gPlayState == NULL || p == NULL || p == GET_PLAYER(gPlayState)) {
        return -1;
    }
    auto it = Anchor::Instance->clients.find((uint32_t)p->zTargetActiveTimer);
    if (it == Anchor::Instance->clients.end() || it->second.player != p) {
        return -1;
    }
    return it->second.bgsBroken ? 1 : 0;
}

// Client id behind a Player struct: our own id for the local Link, the puppet's
// client for a puppet, 0 offline / unknown. Lets boss code stream "who I grabbed".
extern "C" uint32_t Anchor_PlayerClientId(Player* p) {
    if (Anchor::Instance == nullptr || gPlayState == NULL || p == NULL) {
        return 0;
    }
    if (p == GET_PLAYER(gPlayState)) {
        return Anchor::Instance->ownClientId;
    }
    auto it = Anchor::Instance->clients.find((uint32_t)p->zTargetActiveTimer);
    if (it == Anchor::Instance->clients.end() || it->second.player != p) {
        return 0;
    }
    return it->first;
}

extern "C" int32_t Anchor_IsOwnClientId(uint32_t clientId) {
    return Anchor::Instance != nullptr && clientId != 0 && clientId == Anchor::Instance->ownClientId;
}

// Nearest live player (the local Link or a same-scene puppet) to `from`, by 3D
// distance. For boss code that reads player->actor.world.pos directly instead
// of the engine's xzDistToPlayer / yawTowardsPlayer (which EnemySync's
// OverridePlayerPerception already re-points at the nearest player) — with the
// bare GET_PLAYER, Phantom Ganon only ever threw at the authority's own Link.
// Offline, or with no puppet in range, this is exactly GET_PLAYER.
extern "C" Player* Anchor_NearestPlayer(Actor* from) {
    Player* best = GET_PLAYER(gPlayState);
    if (Anchor::Instance == nullptr || gPlayState == NULL || from == NULL || best == NULL) {
        return best;
    }
    f32 bestSq = Math3D_Vec3fDistSq(&from->world.pos, &best->actor.world.pos);
    for (auto& [cid, client] : Anchor::Instance->clients) {
        if (client.self || !client.online || !client.isSaveLoaded || client.player == NULL ||
            client.player->actor.update == NULL || client.sceneNum != gPlayState->sceneNum ||
            (client.stateFlags1 & PLAYER_STATE1_DEAD)) {
            continue;
        }
        f32 sq = Math3D_Vec3fDistSq(&from->world.pos, &client.player->actor.world.pos);
        if (sq < bestSq) {
            bestSq = sq;
            best = client.player;
        }
    }
    return best;
}
