/*
 * File: z_obj_roomtimer.c
 * Overlay: ovl_Obj_Roomtimer
 * Description: Starts Timer 1 with a value specified in params
 */

#include "z_obj_roomtimer.h"

#define FLAGS ACTOR_FLAG_UPDATE_CULLING_DISABLED

void ObjRoomtimer_Init(Actor* thisx, PlayState* play);
void ObjRoomtimer_Destroy(Actor* thisx, PlayState* play);
void ObjRoomtimer_Update(Actor* thisx, PlayState* play);

void func_80B9D054(ObjRoomtimer* this, PlayState* play);
void func_80B9D0B0(ObjRoomtimer* this, PlayState* play);

const ActorInit Obj_Roomtimer_InitVars = {
    ACTOR_OBJ_ROOMTIMER,
    ACTORCAT_ENEMY,
    FLAGS,
    OBJECT_GAMEPLAY_KEEP,
    sizeof(ObjRoomtimer),
    (ActorFunc)ObjRoomtimer_Init,
    (ActorFunc)ObjRoomtimer_Destroy,
    (ActorFunc)ObjRoomtimer_Update,
    (ActorFunc)NULL,
    NULL,
};

void ObjRoomtimer_Init(Actor* thisx, PlayState* play) {
    ObjRoomtimer* this = (ObjRoomtimer*)thisx;
    s16 params = this->actor.params;

    this->switchFlag = (params >> 10) & 0x3F;
    this->actor.params = params & 0x3FF;
    params = this->actor.params;

    if (params != 0x3FF) {
        if (params > 600) {
            this->actor.params = 600;
        } else {
            this->actor.params = params;
        }
    }

    this->actionFunc = func_80B9D054;
}

void ObjRoomtimer_Destroy(Actor* thisx, PlayState* play) {
    ObjRoomtimer* this = (ObjRoomtimer*)thisx;

    if ((this->actor.params != 0x3FF) && (gSaveContext.timerSeconds > 0)) {
        gSaveContext.timerState = 10;
    }
}

void FxSync_OnRoomTimerStart(s16 seconds);
s32 Anchor_IsSessionActive(void);

void func_80B9D054(ObjRoomtimer* this, PlayState* play) {
    if (this->actor.params != 0x3FF) {
        // Co-op: every client runs its own copy of this actor and starts its own
        // countdown the frame ITS player walks in, so entering seconds apart
        // gives two different clocks — and the timeout below only void-outs
        // whoever ran out first. Adopt a countdown already running instead of
        // restarting it, and announce ours so the other side can do the same.
        //
        // The test is on timerState, NOT on timerSeconds: nothing in the game
        // ever writes timerSeconds back to 0 (func_80088B34 and the heat timer
        // only ever assign a positive value, and stopping a clock just sets
        // timerState = 0). Keying off the leftover seconds would leave this
        // actor permanently disarmed — offline too — for anyone who had once
        // been in a hot room or left a timed room early. timerState 5 is what
        // func_80088B34 sets for a running countdown; anything below that means
        // no clock of ours is ticking.
        if (!Anchor_IsSessionActive() || gSaveContext.timerState < 5) {
            func_80088B34(this->actor.params);
            FxSync_OnRoomTimerStart(this->actor.params);
        }
    }

    Actor_ChangeCategory(play, &play->actorCtx, &this->actor, ACTORCAT_PROP);
    this->actionFunc = func_80B9D0B0;
}

void func_80B9D0B0(ObjRoomtimer* this, PlayState* play) {
    if (Flags_GetTempClear(play, this->actor.room)) {
        if (this->actor.params != 0x3FF) {
            gSaveContext.timerState = 10;
        }
        Flags_SetClear(play, this->actor.room);
        Flags_SetSwitch(play, this->switchFlag);
        Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
        Actor_Kill(&this->actor);
    } else {
        if ((this->actor.params != 0x3FF) && (gSaveContext.timerSeconds == 0)) {
            Audio_PlaySoundGeneral(NA_SE_OC_ABYSS, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                   &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
            Play_TriggerVoidOut(play);
            Actor_Kill(&this->actor);
        }
    }
}

void ObjRoomtimer_Update(Actor* thisx, PlayState* play) {
    ObjRoomtimer* this = (ObjRoomtimer*)thisx;

    this->actionFunc(this, play);
}
