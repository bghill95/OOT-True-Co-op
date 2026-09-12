/*
 * File: z_bg_mizu_water.c
 * Overlay: ovl_Bg_Mizu_Water
 * Description: Water plane in Water Temple. Changes height based on switches 0x1C, 0x1D, 0x1E.
 */

#include "z_bg_mizu_water.h"
#include "objects/object_mizu_objects/object_mizu_objects.h"

s32 Anchor_IsSessionActive(void);

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

void BgMizuWater_Init(Actor* thisx, PlayState* play);
void BgMizuWater_Destroy(Actor* thisx, PlayState* play);
void BgMizuWater_Update(Actor* thisx, PlayState* play);
void BgMizuWater_Draw(Actor* thisx, PlayState* play);

void BgMizuWater_WaitForAction(BgMizuWater* this, PlayState* play);
void BgMizuWater_ChangeWaterLevel(BgMizuWater* this, PlayState* play);

typedef struct {
    s32 switchFlag;
    s32 yDiff;
} WaterLevel;

static WaterLevel sWaterLevels[] = {
    { 0x00, 0 },
    { WATER_TEMPLE_WATER_F3_FLAG, 0 },
    { WATER_TEMPLE_WATER_F2_FLAG, WATER_TEMPLE_WATER_F2_Y - WATER_TEMPLE_WATER_F3_Y },
    { WATER_TEMPLE_WATER_F1_FLAG, WATER_TEMPLE_WATER_F1_Y - WATER_TEMPLE_WATER_F3_Y },
};

const ActorInit Bg_Mizu_Water_InitVars = {
    ACTOR_BG_MIZU_WATER,
    ACTORCAT_BG,
    FLAGS,
    OBJECT_MIZU_OBJECTS,
    sizeof(BgMizuWater),
    (ActorFunc)BgMizuWater_Init,
    (ActorFunc)BgMizuWater_Destroy,
    (ActorFunc)BgMizuWater_Update,
    (ActorFunc)BgMizuWater_Draw,
    NULL,
};

static f32 sUnused1 = 0;
static f32 sUnused2 = 110.0f;

static u32 sWaterBoxIndexes[] = { 2, 3, 5, 7, 12, 20, 21, 22 };

static InitChainEntry sInitChain[] = {
    ICHAIN_VEC3F(scale, 1, ICHAIN_STOP),
};

u32 BgMizuWater_GetWaterLevelActionIndex(s16 switchFlag, PlayState* play) {
    u32 ret;

    if (bREG(0) != 0) {
        switch (bREG(1)) {
            case 0:
                Flags_SetSwitch(play, WATER_TEMPLE_WATER_F1_FLAG);
                break;
            case 1:
                Flags_SetSwitch(play, WATER_TEMPLE_WATER_F2_FLAG);
                break;
            case 2:
                Flags_SetSwitch(play, WATER_TEMPLE_WATER_F3_FLAG);
                break;
        }
        bREG(0) = 0;
    }
    if (Flags_GetSwitch(play, WATER_TEMPLE_WATER_F1_FLAG) && (switchFlag != WATER_TEMPLE_WATER_F1_FLAG)) {
        ret = 3;
    } else if (Flags_GetSwitch(play, WATER_TEMPLE_WATER_F2_FLAG) && (switchFlag != WATER_TEMPLE_WATER_F2_FLAG)) {
        ret = 2;
    } else if (Flags_GetSwitch(play, WATER_TEMPLE_WATER_F3_FLAG) && (switchFlag != WATER_TEMPLE_WATER_F3_FLAG)) {
        ret = 1;
    } else {
        ret = 0;
    }

    return ret;
}

void BgMizuWater_SetWaterBoxesHeight(WaterBox* waterBoxes, s16 height) {
    u32 i;

    for (i = 0; i < 8; i++) {
        waterBoxes[sWaterBoxIndexes[i]].ySurface = height;
    }
}

/**
 * Co-op: write a scene switch flag WITHOUT broadcasting it.
 *
 * Flags_SetSwitch / Flags_UnsetSwitch fire GameInteractor_ExecuteOnSceneFlag{Set,Unset}
 * (z_actor.c:690, :706) whenever the bit actually changes, and HookHandlers.cpp:145-149
 * turns every one of those into a SET_FLAG / UNSET_FLAG packet. That is right for a
 * world event and wrong for the housekeeping an *_Init does on a room load: the peer
 * did not load our room, so our reset overwrites the state HE is standing in.
 *
 * Same shape as the fix already in z_bg_mizu_shutter.c:97-109. Kept local to each
 * overlay for the same reason Anchor_IsSessionActive is re-declared in each of them.
 */
static void BgMizuWater_WriteSwitchLocal(PlayState* play, s32 flag, s32 value) {
    if (!Anchor_IsSessionActive()) {
        if (value) {
            Flags_SetSwitch(play, flag);
        } else {
            Flags_UnsetSwitch(play, flag);
        }
        return;
    }

    if (flag < 0x20) {
        if (value) {
            play->actorCtx.flags.swch |= (1 << flag);
        } else {
            play->actorCtx.flags.swch &= ~(1 << flag);
        }
    } else {
        if (value) {
            play->actorCtx.flags.tempSwch |= (1 << (flag - 0x20));
        } else {
            play->actorCtx.flags.tempSwch &= ~(1 << (flag - 0x20));
        }
    }
}

void BgMizuWater_Init(Actor* thisx, PlayState* play) {
    BgMizuWater* this = (BgMizuWater*)thisx;
    f32 initialActorY;
    WaterBox* waterBoxes;
    s32 waterLevelActionIndex;

    waterBoxes = play->colCtx.colHeader->waterBoxes;
    this->type = this->actor.params & 0xFF;
    this->switchFlag = (this->actor.params >> 8) & 0xFF;
    Actor_ProcessInitChain(&this->actor, sInitChain);
    initialActorY = this->actor.world.pos.y;
    this->baseY = initialActorY;
    this->targetY = initialActorY;

    switch (this->type) {
        case 0:
            if (bREG(15) == 0) {
                osSyncPrintf("<コンストラクト>%x %x %x\n", Flags_GetSwitch(play, WATER_TEMPLE_WATER_F1_FLAG),
                             Flags_GetSwitch(play, WATER_TEMPLE_WATER_F2_FLAG),
                             Flags_GetSwitch(play, WATER_TEMPLE_WATER_F3_FLAG));
            }
            waterLevelActionIndex = BgMizuWater_GetWaterLevelActionIndex(-1, play);
            this->actor.world.pos.y = sWaterLevels[waterLevelActionIndex].yDiff + this->baseY;
            BgMizuWater_SetWaterBoxesHeight(waterBoxes, this->actor.world.pos.y);
            this->actor.params = sWaterLevels[waterLevelActionIndex].switchFlag;
            // Co-op: this whole block is a NORMALISATION, not a decision. It reads the
            // three level flags (GetWaterLevelActionIndex, :56-84), clears all three and
            // re-sets exactly the one it just read -- so locally its net effect is the
            // identity, except on a virgin save where all three are clear and it picks F1.
            // Nothing here is news, and every bit of it used to go on the wire.
            //
            // Two ways that hurt. (1) Transient: the peer gets UNSET 0x1C, and until the
            // SET 0x1C lands one frame later he stands with no level flag at all. His
            // WaitForAction then takes the action-index-0 branch, which since the co-op
            // ocarina fix calls Message_CloseTextbox (:197-213) -- a peer paused on the
            // ocarina has his textbox shut by our room load. (2) Stale write: if our copy
            // of the level is behind his (we were out of the scene, or a packet is in
            // flight), the pair drags HIS water to OUR level. Absolute-value sync, last
            // writer wins: the later action is simply lost.
            //
            // The unsets cannot be made local on their own. Unset locally + set on the
            // wire would send the SET unpaired (Flags_SetSwitch broadcasts because we just
            // cleared the bit ourselves, so previouslyOff is true), leaving the peer with
            // TWO level flags at once -- and GetWaterLevelActionIndex prefers F1 > F2 > F3,
            // so his water jumps. Strictly worse than the defect. The block is atomic: all
            // of it stays local, or none of it does.
            //
            // The water level itself keeps replicating -- see SetFlag.cpp:117-126. Only
            // this room-load echo of it stops. Both clients derive the same value from the
            // same flags anyway, so nothing is lost by keeping it at home.
            BgMizuWater_WriteSwitchLocal(play, WATER_TEMPLE_WATER_F1_FLAG, false);
            BgMizuWater_WriteSwitchLocal(play, WATER_TEMPLE_WATER_F2_FLAG, false);
            BgMizuWater_WriteSwitchLocal(play, WATER_TEMPLE_WATER_F3_FLAG, false);

            switch (this->actor.params) {
                case 0x1E:
                    BgMizuWater_WriteSwitchLocal(play, WATER_TEMPLE_WATER_F3_FLAG, true);
                    break;
                case 0x1D:
                    BgMizuWater_WriteSwitchLocal(play, WATER_TEMPLE_WATER_F2_FLAG, true);
                    break;
                case 0x1C:
                default:
                    BgMizuWater_WriteSwitchLocal(play, WATER_TEMPLE_WATER_F1_FLAG, true);
                    break;
            }
            this->targetY = this->actor.world.pos.y;
            break;
        case 1:
            break;
        case 2:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->actor.world.pos.y = this->baseY + 85.0f;
            }
            waterBoxes[6].ySurface = this->actor.world.pos.y;
            break;
        case 3:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->actor.world.pos.y = this->baseY + 110.0f;
                this->targetY = this->actor.world.pos.y;
            }
            waterBoxes[8].ySurface = this->actor.world.pos.y;
            break;
        case 4:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->actor.world.pos.y = this->baseY + 160.0f;
                this->targetY = this->actor.world.pos.y;
            }
            waterBoxes[16].ySurface = this->actor.world.pos.y;
            break;
    }

    this->actionFunc = BgMizuWater_WaitForAction;
}

void BgMizuWater_Destroy(Actor* thisx, PlayState* play) {
}

void BgMizuWater_WaitForAction(BgMizuWater* this, PlayState* play) {
    s32 pad;
    s32 waterLevelActionIndex;
    s16 prevSwitchFlag;

    switch (this->type) {
        case 0:
            prevSwitchFlag = this->actor.params;
            waterLevelActionIndex = BgMizuWater_GetWaterLevelActionIndex(this->actor.params, play);
            if (waterLevelActionIndex != 0) {
                if (prevSwitchFlag != sWaterLevels[waterLevelActionIndex].switchFlag) {
                    // Co-op: the three water level flags replicate now, so this branch
                    // runs on BOTH clients. Cutscene 3120 is not just a camera move --
                    // z_onepointdemo.c does Player_SetCsActionWithHaltedActors(NULL, 8)
                    // with a 100-frame timer, so the peer would be FROZEN for a hundred
                    // frames, wherever they stand in the temple, every time the other
                    // one plays the song.
                    //
                    // A distance test is the wrong tool here (the plane spans the whole
                    // dungeon). MSGMODE_PAUSED is exact instead: En_Okarina_Tag sets it
                    // on the client that played, and nothing clears it until the
                    // Message_CloseTextbox at the end of ChangeWaterLevel -- so it holds
                    // for precisely the client this cutscene belongs to, and for the
                    // whole window. Offline the guard is skipped and vanilla is intact.
                    if (!Anchor_IsSessionActive() || play->msgCtx.msgMode == MSGMODE_PAUSED) {
                        OnePointCutscene_Init(play, 3120, -100 - waterLevelActionIndex, NULL, MAIN_CAM);
                    }
                    this->actor.params = sWaterLevels[waterLevelActionIndex].switchFlag;
                    this->targetY = sWaterLevels[waterLevelActionIndex].yDiff + this->baseY;
                }
            } else if (Anchor_IsSessionActive() && (play->msgCtx.msgMode == MSGMODE_PAUSED)) {
                // Co-op: release the ocarina pause when the song turned out to change
                // nothing. En_Okarina_Tag sets MSGMODE_PAUSED unconditionally on a
                // completed song in this scene (z_en_okarina_tag.c:157), and the ONLY
                // thing that lifts it is the Message_CloseTextbox at the end of
                // ChangeWaterLevel -- which never runs when the water is already at the
                // level being asked for, because GetWaterLevelActionIndex returns 0 for
                // the flag we already hold.
                //
                // Vanilla can reach that too, by playing the song at the socket of the
                // level already active, but a solo player has no reason to. Replicating
                // the flags makes it ordinary: the moment one player sets a level, the
                // other playing at that same socket gets a song that does nothing, and
                // the message system stays inert until some other textbox opens.
                // Guarded on the session, so single-player keeps its exact vanilla
                // behaviour, quirk included.
                Message_CloseTextbox(play);
            }
            if ((prevSwitchFlag != this->actor.params) && (prevSwitchFlag != 0)) {
                Flags_UnsetSwitch(play, prevSwitchFlag);
            }
            break;
        case 1:
            break;
        case 2:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->targetY = this->baseY + 85.0f;
            } else {
                this->targetY = this->baseY;
            }
            break;
        case 3:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->targetY = this->baseY + 110.0f;
            } else {
                this->targetY = this->baseY;
            }
            break;
        case 4:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->targetY = this->baseY + 160.0f;
            } else {
                this->targetY = this->baseY;
            }
    }

    if (this->targetY != this->actor.world.pos.y) {
        this->actionFunc = BgMizuWater_ChangeWaterLevel;
    }
}

void BgMizuWater_ChangeWaterLevel(BgMizuWater* this, PlayState* play) {
    s32 pad;
    s16 prevSwitchFlag;
    s32 waterLevelActionIndex;
    WaterBox* waterBoxes;

    waterBoxes = play->colCtx.colHeader->waterBoxes;
    switch (this->type) {
        case 0:
            prevSwitchFlag = this->actor.params;
            waterLevelActionIndex = BgMizuWater_GetWaterLevelActionIndex(this->actor.params, play);
            if (waterLevelActionIndex != 0) {
                if (prevSwitchFlag != sWaterLevels[waterLevelActionIndex].switchFlag) {
                    this->actor.params = sWaterLevels[waterLevelActionIndex].switchFlag;
                    this->targetY = sWaterLevels[waterLevelActionIndex].yDiff + this->baseY;
                }
            }

            if ((prevSwitchFlag != this->actor.params) && (prevSwitchFlag != 0)) {
                Flags_UnsetSwitch(play, prevSwitchFlag);
            }

            if (Math_StepToF(&this->actor.world.pos.y, this->targetY, 5.0f)) {
                play->roomCtx.unk_74[0] = 0;
                this->actionFunc = BgMizuWater_WaitForAction;
                // Co-op: this closes the textbox En_Okarina_Tag left paused on the
                // client that played the song. Now that the level change runs on both,
                // the peer would have whatever textbox they happen to have open torn
                // down instead. MSGMODE_PAUSED is precisely the state the ocarina tag
                // set, so it identifies the right client; offline the guard is skipped
                // entirely and vanilla is untouched.
                if (!Anchor_IsSessionActive() || play->msgCtx.msgMode == MSGMODE_PAUSED) {
                    Message_CloseTextbox(play);
                }
            }
            BgMizuWater_SetWaterBoxesHeight(play->colCtx.colHeader->waterBoxes, this->actor.world.pos.y);
            break;
        case 1:
            break;
        case 2:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->targetY = this->baseY + 85.0f;
            } else {
                this->targetY = this->baseY;
            }
            if (Math_StepToF(&this->actor.world.pos.y, this->targetY, 1.0f)) {
                play->roomCtx.unk_74[0] = 0;
                this->actionFunc = BgMizuWater_WaitForAction;
            }
            waterBoxes[6].ySurface = this->actor.world.pos.y;
            break;
        case 3:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->targetY = this->baseY + 110.0f;
            } else {
                this->targetY = this->baseY;
            }
            if (Math_StepToF(&this->actor.world.pos.y, this->targetY, 1.0f)) {
                play->roomCtx.unk_74[0] = 0;
                this->actionFunc = BgMizuWater_WaitForAction;
            }
            waterBoxes[8].ySurface = this->actor.world.pos.y;
            break;
        case 4:
            if (Flags_GetSwitch(play, this->switchFlag)) {
                this->targetY = this->baseY + 160.0f;
            } else {
                this->targetY = this->baseY;
            }
            if (Math_StepToF(&this->actor.world.pos.y, this->targetY, 1.0f)) {
                play->roomCtx.unk_74[0] = 0;
                this->actionFunc = BgMizuWater_WaitForAction;
            }
            waterBoxes[16].ySurface = this->actor.world.pos.y;
            break;
    }

    if (this->targetY < this->actor.world.pos.y) {
        func_800AA000(0.0f, 0x78, 0x14, 0xA);
        func_8002F948(&this->actor, NA_SE_EV_WATER_LEVEL_DOWN - SFX_FLAG);
    } else if (this->targetY > this->actor.world.pos.y) {
        func_800AA000(0.0f, 0x78, 0x14, 0xA);
        func_8002F948(&this->actor, NA_SE_EV_WATER_LEVEL_DOWN - SFX_FLAG);
    }
}

void BgMizuWater_Update(Actor* thisx, PlayState* play) {
    BgMizuWater* this = (BgMizuWater*)thisx;
    s32 posY;
    s32 unk0;
    s32 unk1;
    s32 pad;

    if (bREG(15) == 0) {
        osSyncPrintf("%x %x %x\n", Flags_GetSwitch(play, WATER_TEMPLE_WATER_F1_FLAG),
                     Flags_GetSwitch(play, WATER_TEMPLE_WATER_F2_FLAG),
                     Flags_GetSwitch(play, WATER_TEMPLE_WATER_F3_FLAG));
    }
    if (this->type == 0) {
        posY = this->actor.world.pos.y;
        unk0 = 0;
        unk1 = 0;
        if (posY < WATER_TEMPLE_WATER_F1_Y) {
            unk0 = 0;
            unk1 = (posY - WATER_TEMPLE_WATER_B1_Y) / (WATER_TEMPLE_WATER_F1_Y - WATER_TEMPLE_WATER_B1_Y) * 200;
        } else if (posY < WATER_TEMPLE_WATER_F2_Y) {
            unk0 = 1;
            unk1 = 255 - (s32)((posY - WATER_TEMPLE_WATER_F1_Y) / (WATER_TEMPLE_WATER_F2_Y - WATER_TEMPLE_WATER_F1_Y) *
                               (255 - 160));
        } else if (posY <= WATER_TEMPLE_WATER_F3_Y) {
            unk0 = 2;
            unk1 = 255 - (s32)((posY - WATER_TEMPLE_WATER_F2_Y) / (WATER_TEMPLE_WATER_F3_Y - WATER_TEMPLE_WATER_F2_Y) *
                               (255 - 160));
        }
        play->roomCtx.unk_74[1] = ((u8)unk0 << 8) | (unk1 & 0xFF);
    }

    this->actionFunc(this, play);
}

void BgMizuWater_Draw(Actor* thisx, PlayState* play) {
    BgMizuWater* this = (BgMizuWater*)thisx;
    s32 gameplayFrames;

    OPEN_DISPS(play->state.gfxCtx);
    gameplayFrames = play->gameplayFrames;
    Gfx_SetupDL_25Xlu(play->state.gfxCtx);

    gSPSegment(POLY_XLU_DISP++, 0x0C,
               Gfx_TwoTexScroll(play->state.gfxCtx, 0, -gameplayFrames * 1, gameplayFrames * 1, 32, 32, 1, 0,
                                -gameplayFrames * 1, 32, 32));

    gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    gDPSetEnvColor(POLY_XLU_DISP++, 255, 255, 255, 128);

    gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, 102);

    gSPDisplayList(POLY_XLU_DISP++, gObjectMizuObjectsWaterDL_004B20);

    CLOSE_DISPS(play->state.gfxCtx);
}
