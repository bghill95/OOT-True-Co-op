#include "Anchor.h"
#include "EnemySync.h"
#include "DynaSync.h"
#include "BombfSync.h"
#include "BombSync.h"
#include "BreakSync.h"
#include "FxSync.h"
#include "CarrySync.h"
#include "FlagReplay.h"
#include "PuppetFairy.h"
#include "CoopWarp.h"
#include "CoopLifeSync.h"
#include "BossEntry.h"
#include "CutsceneSync.h"
#include "BgmSync.h"
#include "TimeSync.h"
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "variables.h"
#include "functions.h"
extern EffectSsInfo sEffectSsInfo; // z_effect_soft_sprite.c (no header declares it)
void Anchor_ForgetCaptureVictim(Actor* enemy); // Packets/CapturePlayer.cpp
#include "src/overlays/actors/ovl_Bg_Bombwall/z_bg_bombwall.h"
#include "src/overlays/actors/ovl_Bg_Breakwall/z_bg_breakwall.h"
#include "src/overlays/actors/ovl_Bg_Gnd_Soulmeiro/z_bg_gnd_soulmeiro.h"
#include "src/overlays/actors/ovl_Bg_Haka_Zou/z_bg_haka_zou.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hamstep/z_bg_hidan_hamstep.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hrock/z_bg_hidan_hrock.h"
#include "src/overlays/actors/ovl_Bg_Ice_Shelter/z_bg_ice_shelter.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombchuiwa/z_bg_jya_bombchuiwa.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombiwa/z_bg_jya_bombiwa.h"
#include "src/overlays/actors/ovl_Bg_Mizu_Bwall/z_bg_mizu_bwall.h"
#include "src/overlays/actors/ovl_Bg_Spot08_Bakudankabe/z_bg_spot08_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot11_Bakudankabe/z_bg_spot11_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot17_Bakudankabe/z_bg_spot17_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Maruta/z_bg_ydan_maruta.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Sp/z_bg_ydan_sp.h"
#include "src/overlays/actors/ovl_Demo_Kekkai/z_demo_kekkai.h"
#include "src/overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "src/overlays/actors/ovl_En_Door/z_en_door.h"
#include "src/overlays/actors/ovl_En_Si/z_en_si.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
#include "src/overlays/actors/ovl_Item_B_Heart/z_item_b_heart.h"
#include "src/overlays/actors/ovl_Obj_Bombiwa/z_obj_bombiwa.h"
#include "src/overlays/actors/ovl_Obj_Hamishi/z_obj_hamishi.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Dalm/z_bg_hidan_dalm.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Kowarerukabe/z_bg_hidan_kowarerukabe.h"

extern PlayState* gPlayState;

void func_8086ED70(BgBombwall* bgBombwall, PlayState* play);
void BgBreakwall_Wait(BgBreakwall* bgBreakwall, PlayState* play);
void func_8087B284(BgGndSoulmeiro* bgGndSoulmeiro, PlayState* play);
s32 DemoKekkai_CheckEventFlag(s32 params);
void DemoKekkai_TrialBarrierDispel(Actor* thisx, PlayState* play);
void func_80883000(BgHakaZou* bgHakaZou, PlayState* play);
void func_808887C4(BgHidanHamstep* bgHidanHamstep, PlayState* play);
void func_808896B8(BgHidanHrock* bgHidanHrock, PlayState* play);
void func_8089107C(BgIceShelter* bgIceShelter, PlayState* play);
void func_808911BC(BgIceShelter* bgIceShelter);
void ObjBombiwa_Break(ObjBombiwa* objBombiwa, PlayState* play);
void ObjHamishi_Break(ObjHamishi* objHamishi, PlayState* play);
void BgJyaBombchuiwa_WaitForExplosion(BgJyaBombchuiwa* bgJyaBombchuiwa, PlayState* play);
void BgMizuBwall_Idle(BgMizuBwall* bgMizuBwall, PlayState* play);
void func_808B6BC0(BgSpot17Bakudankabe* bgSpot17Bakudankabe, PlayState* play);
void func_808BF078(BgYdanMaruta* bgYdanMaruta, PlayState* play);
void BgYdanSp_FloorWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_WallWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_BurnWeb(BgYdanSp* bgYdanSp, PlayState* play);
void EnDoor_Idle(EnDoor* enDoor, PlayState* play);
}

// Defined in Packets/SetFlag.cpp. C++ linkage on purpose, like its definition --
// declaring it inside the extern "C" block above would not link.
void Anchor_RecomputeGsTokensFromFlags();

void Anchor::RegisterHooks() {

    // #region Hooks that are required for basic Anchor functionality

    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
        SendPacket_UpdateClientState();

        if (IsSaveLoaded()) {
            RefreshClientActors();
        }
    });

    COND_HOOK(OnPresentFileSelect, isConnected, [&]() { SendPacket_UpdateClientState(); });

    COND_ID_HOOK(ShouldActorInit, ACTOR_PLAYER, isConnected, [&](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;

        if (refreshingActors) {
            // By the time we get here, the actor was already added to the ACTORCAT_PLAYER list, so we need to move it
            Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, actor, ACTORCAT_NPC);
            actor->id = ACTOR_EN_OE2;
            actor->category = ACTORCAT_NPC;
            actor->init = DummyPlayer_Init;
            actor->update = DummyPlayer_Update;
            actor->draw = DummyPlayer_Draw;
            actor->destroy = DummyPlayer_Destroy;
        }
    });

    COND_HOOK(OnPlayerUpdate, isConnected, [&]() {
        if (justLoadedSave) {
            justLoadedSave = false;
            SendPacket_RequestTeamState();
        }
        SendPacket_PlayerUpdate();
    });

    // NOTE: the packet-queue drain is NOT registered as its own OnGameFrameUpdate hook;
    // it is the FIRST step of the single Anchor per-frame dispatcher registered at the end
    // of this function (see Anchor_PerFrameTick below), which guarantees the Anchor-internal
    // tick order that map-iteration order does not.

    COND_HOOK(OnPlayerSfx, isConnected, [&](u16 sfxId) { SendPacket_PlayerSfx(sfxId); });

    // Shared wallet: broadcast every local rupee delta (echo-guarded inside the sender).
    COND_HOOK(OnRupeeChange, isConnected, [&](s16 rupeeChange) { SendPacket_RupeeChange(rupeeChange); });

    COND_HOOK(OnLoadGame, isConnected, [&](s16 fileNum) { justLoadedSave = true; });

    COND_HOOK(OnSaveFile, isConnected, [&](s16 fileNum, int sectionID) {
        if (sectionID == 0) {
            SendPacket_UpdateTeamState();
        }
    });

    COND_HOOK(OnFlagSet, isConnected, [&](s16 flagType, s16 flag) {
        SendPacket_SetFlag(SCENE_ID_MAX, flagType, flag);
        // A derived count has to be derived on BOTH sides, or the local pickup is the
        // one that gets lost. En_Si increments at pickup (z_en_si.c:126) but only sets
        // the bit when the textbox closes (:144); a peer's SET_FLAG for a DIFFERENT
        // Skulltula landing inside that window recomputes from bits that do not yet
        // include ours, and silently drops it. Recomputing here too closes the loop:
        // when our own bit finally lands, the count catches up with it.
        if (flagType == FLAG_GS_TOKEN) {
            Anchor_RecomputeGsTokensFromFlags();
        }
    });

    COND_HOOK(OnFlagUnset, isConnected,
              [&](s16 flagType, s16 flag) { SendPacket_UnsetFlag(SCENE_ID_MAX, flagType, flag); });

    COND_HOOK(OnSceneFlagSet, isConnected,
              [&](s16 sceneNum, s16 flagType, s16 flag) { SendPacket_SetFlag(sceneNum, flagType, flag); });

    COND_HOOK(OnSceneFlagUnset, isConnected,
              [&](s16 sceneNum, s16 flagType, s16 flag) { SendPacket_UnsetFlag(sceneNum, flagType, flag); });

    COND_HOOK(OnRandoSetCheckStatus, isConnected, [&](RandomizerCheck rc, RandomizerCheckStatus status) {
        if (!isHandlingUpdateTeamState) {
            SendPacket_SetCheckStatus(rc);
        }
    });

    COND_HOOK(OnRandoSetIsSkipped, isConnected, [&](RandomizerCheck rc, bool isSkipped) {
        if (!isHandlingUpdateTeamState) {
            SendPacket_SetCheckStatus(rc);
        }
    });

    COND_HOOK(OnRandoEntranceDiscovered, isConnected,
              [&](u16 entranceIndex, u8 isReversedEntrance) { SendPacket_EntranceDiscovered(entranceIndex); });

    COND_ID_HOOK(OnBossDefeat, ACTOR_BOSS_GANON2, isConnected, [&](void* refActor) { SendPacket_GameComplete(); });

    COND_HOOK(OnItemReceive, isConnected, [&](GetItemEntry itemEntry) {
        // Handle vanilla dungeon items a bit differently
        if (itemEntry.modIndex == MOD_NONE &&
            (itemEntry.itemId >= ITEM_KEY_BOSS && itemEntry.itemId <= ITEM_KEY_SMALL)) {
            // The range is boss key / compass / map / small key. Only the last is a
            // count; the other three are bits and merge on their own (see the
            // packet). Sending the delta is what lets two simultaneous key events
            // commute instead of one erasing the other.
            SendPacket_UpdateDungeonItems(itemEntry.itemId == ITEM_KEY_SMALL ? 1 : 0);
            return;
        }

        // A3 (channel exclusivity): rupee value now travels ONLY through RUPEE_CHANGE
        // (Rupees_ChangeBy -> OnRupeeChange). Drop pure rupee items from GIVE_ITEM so a
        // synced pickup is not counted twice (item channel + rupee channel). Only the
        // rupee items are filtered — shop merchandise (shields, nuts, ...) still
        // replicates through GIVE_ITEM. MOD_NONE ITEM_RUPEE_GREEN..ITEM_RUPEE_GOLD covers
        // green/blue/red/purple/gold; MOD_RANDOMIZER RG_GREEN_RUPEE..RG_HUGE_RUPEE covers
        // the rando rupee set (incl. Greg, whose RAND_INF flag still syncs via SET_FLAG).
        if ((itemEntry.modIndex == MOD_NONE && itemEntry.itemId >= ITEM_RUPEE_GREEN &&
             itemEntry.itemId <= ITEM_RUPEE_GOLD) ||
            (itemEntry.modIndex == MOD_RANDOMIZER && itemEntry.getItemId >= RG_GREEN_RUPEE &&
             itemEntry.getItemId <= RG_HUGE_RUPEE)) {
            return;
        }

        // Channel exclusivity, same shape as the rupees above: a Gold Skulltula token's
        // count is DERIVED from its GS bits rather than carried at all -- see
        // Anchor_RecomputeGsTokensFromFlags (Packets/SetFlag.cpp), which runs both when a
        // peer's flag arrives and when we set one of our own, just above.
        //
        // It has to leave this channel because the gold Skulltula is NOT tracked --
        // EnSw_Init re-categorises only type 0 to ACTORCAT_ENEMY and the gold ones are
        // types 1..4, so they stay ACTORCAT_NPC and each client runs its own copy. Both
        // players can kill and collect the same one; through GIVE_ITEM that counted twice
        // on both saves, while the flag is a bit and cannot be set twice.
        //
        // Vanilla only. Under the randomizer this exclusion would LOSE tokens: with
        // tokensanity on, RG_GOLD_SKULLTULA_TOKEN is itemId ITEM_SKULL_TOKEN / MOD_NONE
        // (item_list.cpp:60) but can be placed in a chest or bought from a shop, where no
        // GS flag is ever set -- so taking it off the item channel would leave nothing to
        // carry it. And with tokensanity OFF the count could not be rebuilt either:
        // RSK_STARTING_SKULLTULA_TOKEN grants tokens with no bits behind them, so the
        // popcount the receiving side uses would erase them.
        //
        // The randomizer therefore keeps the pre-branch behaviour in full, including its
        // own double-count when both players collect the same Skulltula under tokensanity
        // OFF. That is pre-existing, not introduced here, and closing it needs a count
        // that can survive tokens without bits -- a third rule, which is what this whole
        // correction was about removing.
        if (!IS_RANDO && itemEntry.modIndex == MOD_NONE && itemEntry.itemId == ITEM_SKULL_TOKEN) {
            return;
        }

        SendPacket_GiveItem(itemEntry.tableId, itemEntry.getItemId);
    });

    COND_HOOK(OnDungeonKeyUsed, isConnected, [&](uint16_t mapIndex) {
        // One key spent. The peer applies -1 rather than our absolute count, so two
        // players unlocking two different doors at once each lose exactly one key
        // instead of one of the two spends going missing.
        SendPacket_UpdateDungeonItems(-1);
    });

    // #endregion

    // #region Hooks that are purely to sync actor states across the clients, not super essential

    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_ITEM00, isConnected, [&](void* refActor) {
        EnItem00* actor = static_cast<EnItem00*>(refActor);

        if (Flags_GetCollectible(gPlayState, actor->collectibleFlag)) {
            Actor_Kill(&actor->actor);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BOMBWALL, isConnected, [&](void* refActor, bool* should) {
        BgBombwall* actor = static_cast<BgBombwall*>(refActor);

        if (actor->actionFunc == func_8086ED70 && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BREAKWALL, isConnected, [&](void* refActor, bool* should) {
        BgBreakwall* actor = static_cast<BgBreakwall*>(refActor);

        if (actor->actionFunc == BgBreakwall_Wait && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    // Ganon's Castle, Spirit trial (ganontika room 18: params 0x0A00 / 0x0A01 / 0x0A02,
    // switch flag 10). The web reads its flag at Init (z_bg_gnd_soulmeiro.c:74) and
    // then only inside func_8087B284, whose whole body is behind `if (!Flags_GetSwitch
    // (...))` (:163) -- so a flag arriving from the other client is never seen: the web
    // P1 burned stayed drawn and kept its AC cylinder up for P2, and P2 never got the
    // Mir_Ray beam that the burn spawns, which is what lights the room below. Re-read
    // the flag every frame and take the same branch Init would have taken. Same shape
    // as the Bg_Ice_Shelter hook further down, and the same shape the Demo_Kekkai hook
    // below uses -- one class of defect, "flag read once at Init", two actors.
    //
    // The other two thirds of this puzzle need nothing: params 1 and 2 (light source
    // and lit floor) run func_8087B350, which already re-reads the flag every frame
    // and just swaps `draw` (:178).
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_GND_SOULMEIRO, isConnected, [&](void* refActor, bool* should) {
        BgGndSoulmeiro* actor = static_cast<BgGndSoulmeiro*>(refActor);

        // The actionFunc test is what keeps this off the client that lit it: a copy
        // already burning runs func_8087AF38, which sets the flag itself at the
        // halfway point of its own effect (:116) and kills itself 20 frames later
        // (:121). Firing here on that flag would cut the effect the player triggered.
        if ((actor->actor.params & 0xFF) == 0 && actor->actionFunc == func_8087B284 &&
            Flags_GetSwitch(gPlayState, (actor->actor.params >> 8) & 0x3F)) {
            Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_MIR_RAY, actor->actor.world.pos.x,
                        actor->actor.world.pos.y, actor->actor.world.pos.z, 0, 0, 0, 9, true);
            actor->actor.draw = NULL;
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HAKA_ZOU, isConnected, [&](void* refActor, bool* should) {
        BgHakaZou* actor = static_cast<BgHakaZou*>(refActor);

        if (actor->actionFunc == func_80883000 && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HAMSTEP, isConnected, [&](void* refActor, bool* should) {
        BgHidanHamstep* actor = static_cast<BgHidanHamstep*>(refActor);

        if (actor->actionFunc == func_808887C4 && Flags_GetSwitch(gPlayState, (actor->dyna.actor.params >> 8) & 0xFF)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HROCK, isConnected, [&](void* refActor, bool* should) {
        BgHidanHrock* actor = static_cast<BgHidanHrock*>(refActor);

        if (actor->actionFunc == func_808896B8 && Flags_GetSwitch(gPlayState, actor->unk_16A)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_ICE_SHELTER, isConnected, [&](void* refActor, bool* should) {
        BgIceShelter* actor = static_cast<BgIceShelter*>(refActor);

        // Bit 6 says "this red ice sets no flag": for those instances params & 0x3F is
        // not a flag index at all but an unrelated field, and reading it melted the ice
        // as soon as some other actor happened to set that flag. Both vanilla sites
        // carry the guard (z_bg_ice_shelter.c:202 in Init, :425 when the melt ends);
        // the replay copied only half of it.
        if (actor->actionFunc == func_8089107C && !((actor->dyna.actor.params >> 6) & 1) &&
            Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            func_808911BC(actor);
            Audio_PlayActorSound2(&actor->dyna.actor, NA_SE_EV_ICE_MELT);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBCHUIWA, isConnected, [&](void* refActor, bool* should) {
        BgJyaBombchuiwa* actor = static_cast<BgJyaBombchuiwa*>(refActor);

        if (actor->actionFunc == BgJyaBombchuiwa_WaitForExplosion &&
            Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBIWA, isConnected, [&](void* refActor, bool* should) {
        BgJyaBombiwa* actor = static_cast<BgJyaBombiwa*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_MIZU_BWALL, isConnected, [&](void* refActor, bool* should) {
        BgMizuBwall* actor = static_cast<BgMizuBwall*>(refActor);

        if (actor->actionFunc == BgMizuBwall_Idle &&
            Flags_GetSwitch(gPlayState, ((u16)actor->dyna.actor.params >> 8) & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT08_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot08Bakudankabe* actor = static_cast<BgSpot08Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT11_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot11Bakudankabe* actor = static_cast<BgSpot11Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT17_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot17Bakudankabe* actor = static_cast<BgSpot17Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            func_808B6BC0(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->dyna.actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
            Actor_Kill(&actor->dyna.actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_MARUTA, isConnected, [&](void* refActor, bool* should) {
        BgYdanMaruta* actor = static_cast<BgYdanMaruta*>(refActor);

        if (actor->actionFunc == func_808BF078 && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_SP, isConnected, [&](void* refActor, bool* should) {
        BgYdanSp* actor = static_cast<BgYdanSp*>(refActor);

        if ((actor->actionFunc == BgYdanSp_FloorWebIdle || actor->actionFunc == BgYdanSp_WallWebIdle) &&
            Flags_GetSwitch(gPlayState, actor->isDestroyedSwitchFlag)) {
            BgYdanSp_BurnWeb(actor, gPlayState);
        }
    });

    // Ganon's Castle barriers -- seven instances, all in ganontika: params 0 is the
    // tower barrier (room 1), params 1..6 the six trial barriers (rooms 4, 11, 15, 13,
    // 19, 7). The EVENTCHKINF behind each one replicates already, but the actor only
    // consults it at Init (z_demo_kekkai.c:126). A trial P1 dispelled therefore stayed
    // whole for P2, who was still inside the room: the trial barriers run
    // DemoKekkai_TrialBarrierIdle as their update and submit AT and OC every single
    // frame with no state test at all (:255-271), so P2 stayed walled in AND took
    // contact damage from a barrier that no longer exists on P1's screen. Same defect
    // class and same remedy as the Bg_Gnd_Soulmeiro hook above: re-read each frame and
    // take the branch Init would have taken.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_DEMO_KEKKAI, isConnected, [&](void* refActor, bool* should) {
        DemoKekkai* actor = static_cast<DemoKekkai*>(refActor);

        // Never interrupt a dispel this client is already playing: the trial barrier
        // swaps its update to DemoKekkai_TrialBarrierDispel for the sage cutscene
        // (:265), and the tower barrier latches sfxFlag bit 0 when its own sequence
        // starts (:172) and sets the EVENTCHKINF itself 100 frames later (:179).
        // Without this guard the flag would land mid-animation and kill the actor that
        // is playing it.
        if ((actor->actor.update == DemoKekkai_TrialBarrierDispel) || (actor->sfxFlag & 1)) {
            return;
        }

        if (DemoKekkai_CheckEventFlag(actor->actor.params)) {
            if (actor->actor.params == KEKKAI_TOWER) {
                gPlayState->envCtx.unk_BF = 1;
            }
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_DOOR_SHUTTER, isConnected, [&](void* refActor, bool* should) {
        DoorShutter* actor = static_cast<DoorShutter*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            DECR(actor->unk_16E);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_DOOR, isConnected, [&](void* refActor, bool* should) {
        EnDoor* actor = static_cast<EnDoor*>(refActor);

        if (actor->actionFunc == EnDoor_Idle && Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            DECR(actor->lockTimer);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SI, isConnected, [&](void* refActor, bool* should) {
        EnSi* actor = static_cast<EnSi*>(refActor);

        if (GET_GS_FLAGS((actor->actor.params & 0x1F00) >> 8) & (actor->actor.params & 0xFF)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SW, isConnected, [&](void* refActor, bool* should) {
        EnSw* actor = static_cast<EnSw*>(refActor);

        if (GET_GS_FLAGS((actor->actor.params & 0x1F00) >> 8) & (actor->actor.params & 0xFF)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_ITEM_B_HEART, isConnected, [&](void* refActor, bool* should) {
        ItemBHeart* actor = static_cast<ItemBHeart*>(refActor);

        if (Flags_GetCollectible(gPlayState, 0x1F)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_BOMBIWA, isConnected, [&](void* refActor, bool* should) {
        ObjBombiwa* actor = static_cast<ObjBombiwa*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            ObjBombiwa_Break(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->actor.world.pos, 80, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_HAMISHI, isConnected, [&](void* refActor, bool* should) {
        ObjHamishi* actor = static_cast<ObjHamishi*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            ObjHamishi_Break(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_VB_SHOULD(VB_HAMMER_TOTEM_BREAK, isConnected, {
        BgHidanDalm* actor = va_arg(args, BgHidanDalm*);

        // switchFlag is (params >> 8) & 0xFF, and the file's own comment records
        // that the Gerudo Training Ground placements all carry 0xFF -- outside the
        // 0x00-0x3F switch range. Flags_GetSwitch then evaluates
        // tempSwch & (1 << (0xFF - 0x20)): a shift of 223, which the hardware masks
        // to 5 bits and turns into bit 31. Flags_SetSwitch(0xFF) in the vanilla
        // break path sets that very bit, so the FIRST 0xFF totem broken anywhere in
        // the scene made every other 0xFF totem read "already broken" and collapse
        // on its own -- each one dragging the player into a halted-actor cutscene.
        // Nobody read that bit before this hook existed; the hook created the link.
        //
        // Still `*should = true` rather than BgHidanDalm_BreakFromPeer: this hook is
        // consulted from inside the actor's own update, and BreakFromPeer rewrites
        // that actor's state, so calling it here would be re-entrant. Behind the
        // range guard the fallback now only fires for a totem whose flag really did
        // replicate, which is the case the vanilla branch is right for. FX_EV_TOTEM
        // normally arrives first and disarms it anyway.
        if (actor->switchFlag <= 0x3F && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            *should = true;
        }
    });

    COND_VB_SHOULD(VB_FIRE_TEMPLE_BOMBABLE_WALL_BREAK, isConnected, {
        BgHidanKowarerukabe* actor = va_arg(args, BgHidanKowarerukabe*);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params >> 8) & 0x3F)) {
            *should = true;
        }
    });

    // #endregion

    // A dying actor can leave particle effects behind that still point at it:
    // EffectSs entries keep a raw owner Actor* that several updates dereference
    // with no liveness guard (EffectSsGSpk reads actor->category before its
    // update != NULL check). Networked kills (EnemySync despawns/handoffs,
    // Bombf/Carry replicas) free actors at moments vanilla never does, which
    // turns that latent read-after-free into a real 0xc0000005 in
    // EffectSs_Update. NULL is handled by every effect type; sweep is ~100 slots.
    COND_HOOK(OnActorDestroy, isConnected, [](void* actorRef) {
        // Pending capture (CapturePlayer.cpp) keyed by the grabber's address:
        // drop it here or a dead Wallmaster leaves a dangling key that the next
        // actor allocated at the same address would inherit as its own victim.
        Anchor_ForgetCaptureVictim((Actor*)actorRef);

        if (sEffectSsInfo.table == NULL) {
            return;
        }
        for (s32 i = 0; i < sEffectSsInfo.tableSize; i++) {
            if (sEffectSsInfo.table[i].actor == (Actor*)actorRef) {
                sEffectSsInfo.table[i].actor = NULL;
            }
        }
    });

    // Each module below registers its NON-per-frame hooks (OnSceneInit, VBs, actor
    // hooks, ...) and does its connect/disconnect Reset. Their per-frame ticks are NOT
    // self-registered on OnGameFrameUpdate — they are invoked in an explicit, documented
    // order by the single Anchor_PerFrameTick dispatcher at the end of this function.
    // Registration order below therefore no longer implies execution order.

    // Co-op death / spectate / revival life-state machine + the two death-flow VBs
    // (see CoopLifeSync.cpp). Its per-frame tick (CoopLifeSyncTick) SETS myLifeState.
    RegisterCoopLifeSyncHooks(isConnected);

    // Shared enemy/boss HP + synced deaths (see EnemySync.cpp)
    EnemySync::RegisterHooks(isConnected);

    // Mirrored motion for whitelisted DynaPoly platforms/elevators (see DynaSync.cpp)
    DynaSync::RegisterHooks(isConnected);

    // Bomb Flower throw/pluck replication (see BombfSync.cpp)
    BombfSync::RegisterHooks(isConnected);

    // Player-placed bomb replication (see BombSync.cpp)
    BombSync::RegisterHooks(isConnected);

    // Breakable scenery: pots, crates, grass, rocks, beehives, tree bonks (see BreakSync.cpp)
    BreakSync::RegisterHooks(isConnected);

    // Magic-spell replicas + timed-torch lighting (see FxSync.cpp)
    FxSync::RegisterHooks(isConnected);

    // Scene-placed carryable props: rocks, pots (see CarrySync.cpp)
    CarrySync::RegisterHooks(isConnected);

    // Init-only flag readers reacting to a REMOTE flag (see FlagReplay.cpp)
    FlagReplay::RegisterHooks(isConnected);

    // Cosmetic companion fairy per remote-player puppet (see PuppetFairy.cpp)
    RegisterPuppetFairyHooks(isConnected);

    // Queued, gated system warps for boss co-entry / spectate / cutscene sync (see CoopWarp.cpp)
    RegisterCoopWarpHooks(isConnected);

    // Boss-room co-entry: pull the partner into the boss fight (see Packets/BossEntry.cpp)
    RegisterBossEntryHooks(isConnected);

    // Story-cutscene sync: pull the partner into entrance / scene-layer cutscenes,
    // replayed locally (see Packets/CutsceneSync.cpp)
    RegisterCutsceneSyncHooks(isConnected);

    // BGM rendezvous-seek sync: phase-lock the MAIN track when both players share an
    // area (arriver seeks to the resident) + spectate game-over-music suppression
    // (see Packets/BgmSync.cpp).
    RegisterBgmSyncHooks(isConnected);

    // ---- Anchor per-frame dispatcher --------------------------------------------------
    // GameInteractor::ExecuteHooks iterates an unordered_map, so per-hook execution order
    // is implementation-defined — NOT registration order. The Anchor layer has real
    // producer->consumer chains within a single frame, so we make the order EXPLICIT by
    // driving every Anchor OnGameFrameUpdate tick from ONE hook, in call sequence. All of
    // these ticks previously shared the exact same registration condition (isConnected),
    // so this single COND_HOOK(isConnected) dispatcher preserves each tick's condition; the
    // load-bearing orderings it guarantees are: (1) the packet-queue drain runs FIRST
    // (applies peer lifeState/wallet/warps), (2) CoopLifeSyncTick (SETS myLifeState) runs
    // before EnemySync::PerFrameTick (authority election READS myLifeState) and before
    // BgmSyncTick (spectate restore READS myLifeState). On disconnect this hook unregisters
    // (condition false), so no tick runs post-disconnect; each module's Reset still ran in
    // its Register*Hooks(false) call above.
    COND_HOOK(OnGameFrameUpdate, isConnected, [this]() {
        ProcessIncomingPacketQueue(); // drain: apply peer lifeState / wallet / warps
        CoopLifeSyncTick();           // SETS myLifeState
        EnemySync::PerFrameTick();    // authority election READS myLifeState
        DynaSync::PerFrameTick();     // platform streaming READS EnemySync's election
        FlagReplay::PerFrameTick();   // BG_JYA_BLOCK re-spawn once its flag arrives
        PuppetFairyTick();            // companion-fairy spawn/reap lifecycle
        CoopWarpTick();               // drain queued, gated system warps
        BossEntryTick();              // reconcile boss echo-latch (Fix A)
        CutsceneSyncTick();           // reconcile cutscene pull-replay
        BgmSyncTick();                // spectate restore READS myLifeState
        TimeSyncTick();               // world-clock broadcast/follow (see Packets/TimeSync.cpp)
    });
}
