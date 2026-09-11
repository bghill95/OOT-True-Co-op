#include "z_bg_mizu_shutter.h"
#include "objects/object_mizu_objects/object_mizu_objects.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"

#define FLAGS ACTOR_FLAG_UPDATE_CULLING_DISABLED

#define SIZE_PARAM (((u16)this->dyna.actor.params >> 0xC) & 0xF)
#define TIMER_PARAM (((u16)this->dyna.actor.params >> 6) & 0x3F)

void BgMizuShutter_Init(BgMizuShutter* this, PlayState* play);
void BgMizuShutter_Destroy(BgMizuShutter* this, PlayState* play);
void BgMizuShutter_Update(BgMizuShutter* this, PlayState* play);
void BgMizuShutter_Draw(BgMizuShutter* this, PlayState* play);

void BgMizuShutter_WaitForTimer(BgMizuShutter* this, PlayState* play);
void BgMizuShutter_WaitForSwitch(BgMizuShutter* this, PlayState* play);
void BgMizuShutter_Move(BgMizuShutter* this, PlayState* play);
void BgMizuShutter_WaitForCutscene(BgMizuShutter* this, PlayState* play);

// Co-op (Anchor/DummyPlayer.cpp)
s32 Anchor_ShouldPlayCameraCutscene(Actor* actor, f32 range);
s32 Anchor_IsSessionActive(void);

const ActorInit Bg_Mizu_Shutter_InitVars = {
    ACTOR_BG_MIZU_SHUTTER,
    ACTORCAT_PROP,
    FLAGS,
    OBJECT_MIZU_OBJECTS,
    sizeof(BgMizuShutter),
    (ActorFunc)BgMizuShutter_Init,
    (ActorFunc)BgMizuShutter_Destroy,
    (ActorFunc)BgMizuShutter_Update,
    (ActorFunc)BgMizuShutter_Draw,
    NULL,
};

static Gfx* sDisplayLists[] = { gObjectMizuObjectsShutterDL_007130, gObjectMizuObjectsShutterDL_0072D0 };

static CollisionHeader* sCollisionHeaders[] = {
    &gObjectMizuObjectsShutterCol_007250,
    &gObjectMizuObjectsShutterCol_0073F0,
};

static Vec3f sDisplacements[] = {
    { 0.0f, 100.0f, 0.0f },
    { 0.0f, 140.0f, 0.0f },
};

static InitChainEntry sInitChain[] = {
    ICHAIN_F32(uncullZoneScale, 1500, ICHAIN_CONTINUE),
    ICHAIN_F32(uncullZoneDownward, 1100, ICHAIN_CONTINUE),
    ICHAIN_F32(uncullZoneForward, 1000, ICHAIN_CONTINUE),
    ICHAIN_VEC3F_DIV1000(scale, 100, ICHAIN_STOP),
};

void BgMizuShutter_Init(BgMizuShutter* thisx, PlayState* play) {
    s32 pad;
    BgMizuShutter* this = (BgMizuShutter*)thisx;
    s32 pad2;
    CollisionHeader* sp30 = NULL;
    s32 pad3;

    Actor_ProcessInitChain(&this->dyna.actor, sInitChain);
    this->displayList = sDisplayLists[SIZE_PARAM];
    DynaPolyActor_Init(&this->dyna, DPM_PLAYER);
    CollisionHeader_GetVirtual(sCollisionHeaders[SIZE_PARAM], &sp30);
    this->dyna.bgId = DynaPoly_SetBgActor(play, &play->colCtx.dyna, &this->dyna.actor, sp30);
    if ((SIZE_PARAM == BGMIZUSHUTTER_SMALL) || (SIZE_PARAM == BGMIZUSHUTTER_LARGE)) {
        this->closedPos = this->dyna.actor.world.pos;
        this->timer = 0;
        this->timerMax = TIMER_PARAM * 20;
        Matrix_RotateY(this->dyna.actor.world.rot.y * (M_PI / 0x8000), MTXMODE_NEW);
        Matrix_RotateX(this->dyna.actor.world.rot.x * (M_PI / 0x8000), MTXMODE_APPLY);
        Matrix_RotateZ(this->dyna.actor.world.rot.z * (M_PI / 0x8000), MTXMODE_APPLY);
        Matrix_MultVec3f(&sDisplacements[SIZE_PARAM], &this->openPos);
        this->openPos.x += this->dyna.actor.world.pos.x;
        this->openPos.y += this->dyna.actor.world.pos.y;
        this->openPos.z += this->dyna.actor.world.pos.z;
        // Vanilla resets a timed door to shut when the room loads. In co-op that
        // reset was BROADCAST: Flags_UnsetSwitch only fires its hook when the flag
        // WAS set -- precisely the window during which the door stands open -- and
        // SendPacket_UnsetFlag goes out ungated. Walking into room 0, 8 or 21 while
        // the peer had just opened the door cleared HIS flag, and his door shut on
        // him mid-Move.
        //
        // The reset itself must STAY. Skipping it (first attempt, 2026-09-11) let
        // the room load find the flag set, which sends this copy straight into
        // BgMizuShutter_WaitForTimer with timer == 0 -- and that function
        // decrements BEFORE testing for zero, so the counter goes 0, -1, -2 and the
        // door never closes again. Flags 19 and 27 are below 0x20, so they live in
        // actorCtx.flags.swch and Play_SaveSceneFlags writes them to the SAVE FILE:
        // one player opening the door and walking through was enough to leave it
        // open permanently. Strictly worse than the defect being fixed.
        // So: clear the bit exactly as Flags_UnsetSwitch does, minus its hook. A
        // room load is local housekeeping, not a world event worth broadcasting.
        if (this->timerMax != 0x3F * 20) {
            s32 switchFlag = (u16)this->dyna.actor.params & 0x3F;

            if (Anchor_IsSessionActive()) {
                if (switchFlag < 0x20) {
                    play->actorCtx.flags.swch &= ~(1 << switchFlag);
                } else {
                    play->actorCtx.flags.tempSwch &= ~(1 << (switchFlag - 0x20));
                }
            } else {
                Flags_UnsetSwitch(play, switchFlag);
            }
            this->dyna.actor.world.pos = this->closedPos;
        }
        if (Flags_GetSwitch(play, (u16)this->dyna.actor.params & 0x3F)) {
            this->dyna.actor.world.pos = this->openPos;
            this->actionFunc = BgMizuShutter_WaitForTimer;
        } else {
            this->actionFunc = BgMizuShutter_WaitForSwitch;
        }
    }
}

void BgMizuShutter_Destroy(BgMizuShutter* thisx, PlayState* play) {
    s32 pad;
    BgMizuShutter* this = (BgMizuShutter*)thisx;

    DynaPoly_DeleteBgActor(play, &play->colCtx.dyna, this->dyna.bgId);
}

void BgMizuShutter_WaitForSwitch(BgMizuShutter* this, PlayState* play) {
    if (Flags_GetSwitch(play, (u16)this->dyna.actor.params & 0x3F)) {
        // Co-op: the flag travels, so BOTH clients arrive here the moment either
        // one hits the crystal -- right for the door, wrong for the camera.
        //
        // The distance guard alone does NOT cover the case that matters, and
        // believing it did was the first version of this fix. Cutscene 4510 is the
        // only branch that runs Player_SetCsActionWithHaltedActors(play, NULL, 8)
        // and takes the player's CONTROLS away; it is reached only when
        // world.rot.x is steep, and the only two doors in the game with a non-zero
        // rot.x are the pair of grates in MIZUsin room 2 -- where no actor sits
        // further than 490 units away, so xzDistToPlayer < 800.0f is always true
        // for anyone who has the room loaded at all. Both clients kept losing
        // their controls; the guard only ever removed a pan.
        // So in a session that branch is downgraded to OnePointCutscene_Attention,
        // which takes the camera and nothing else. Solo is untouched.
        // What this does NOT fix, measured and accepted: the peer still gets the
        // camera taken for ~31 frames, because no guard can tell who struck the
        // crystal. Two small departures from vanilla come with the downgrade --
        // Attention plays NA_SE_SY_CORRECT_CHIME where 4510 was silent, and 4510
        // froze the ENEMY and MISC actor categories while Attention freezes
        // nothing, so the room's En_Ny and En_Sb keep moving during the opening.
        // Both are preferable to a player who cannot move.
        if (Anchor_ShouldPlayCameraCutscene(&this->dyna.actor, 800.0f)) {
            if (ABS(this->dyna.actor.world.rot.x) > 0x2C60 && !Anchor_IsSessionActive()) {
                OnePointCutscene_Init(play, 4510, -99, &this->dyna.actor, MAIN_CAM);
            } else {
                OnePointCutscene_Attention(play, &this->dyna.actor);
            }
        }
        this->actionFunc = BgMizuShutter_WaitForCutscene;
        this->timer = 30;
    }
}

void BgMizuShutter_WaitForCutscene(BgMizuShutter* this, PlayState* play) {
    if (this->timer-- == 0) {
        Audio_PlayActorSound2(&this->dyna.actor, NA_SE_EV_METALDOOR_OPEN);
        this->actionFunc = BgMizuShutter_Move;
    }
}

void BgMizuShutter_Move(BgMizuShutter* this, PlayState* play) {
    if (Flags_GetSwitch(play, (u16)this->dyna.actor.params & 0x3F)) {
        Math_SmoothStepToF(&this->dyna.actor.world.pos.x, this->openPos.x, 1.0f, 4.0f, 0.1f);
        Math_SmoothStepToF(&this->dyna.actor.world.pos.y, this->openPos.y, 1.0f, 4.0f, 0.1f);
        Math_SmoothStepToF(&this->dyna.actor.world.pos.z, this->openPos.z, 1.0f, 4.0f, 0.1f);
        if ((this->dyna.actor.world.pos.x == this->openPos.x) && (this->dyna.actor.world.pos.y == this->openPos.y) &&
            (this->dyna.actor.world.pos.z == this->openPos.z)) {
            this->timer = this->timerMax;
            this->actionFunc = BgMizuShutter_WaitForTimer;
        }
    } else {
        Math_SmoothStepToF(&this->maxSpeed, 20.0f, 1.0f, 3.0f, 0.1f);
        Math_SmoothStepToF(&this->dyna.actor.world.pos.x, this->closedPos.x, 1.0f, this->maxSpeed, 0.1f);
        Math_SmoothStepToF(&this->dyna.actor.world.pos.y, this->closedPos.y, 1.0f, this->maxSpeed, 0.1f);
        Math_SmoothStepToF(&this->dyna.actor.world.pos.z, this->closedPos.z, 1.0f, this->maxSpeed, 0.1f);
        if ((this->dyna.actor.world.pos.x == this->closedPos.x) &&
            (this->dyna.actor.world.pos.y == this->closedPos.y) &&
            (this->dyna.actor.world.pos.z == this->closedPos.z)) {
            func_800AA000(this->dyna.actor.xyzDistToPlayerSq, 0x78, 0x14, 0xA);
            Audio_PlayActorSound2(&this->dyna.actor, NA_SE_EV_STONE_BOUND);
            this->actionFunc = BgMizuShutter_WaitForSwitch;
        }
    }
}

void BgMizuShutter_WaitForTimer(BgMizuShutter* this, PlayState* play) {
    if (this->timerMax != 0x3F * 20) {
        if (GameInteractor_Should(VB_SWITCH_TIMER_TICK, true, this, &this->timer)) {
            this->timer--;
        }
        func_8002F994(&this->dyna.actor, this->timer);
        if (this->timer == 0) {
            s32 switchFlag = (u16)this->dyna.actor.params & 0x3F;

            Audio_PlayActorSound2(&this->dyna.actor, NA_SE_EV_METALDOOR_CLOSE);
            // Same de-broadcast as the Init reset, and for a reason the Init change
            // CREATED: once each client clears its own copy on room load, the two
            // timers are armed at unrelated moments, so broadcasting this expiry
            // means whichever door closes FIRST slams the other player's door --
            // which BgMizuShutter_Move polls every frame and obeys immediately. The
            // gap is the distance between the two timers: up to 200 frames in room
            // 8, 100 in rooms 0 and 21. Each client now closes its own door on its
            // own timer. Opening still travels: Flags_SetSwitch is untouched, so
            // "one player opens it, it opens for both" still holds.
            if (Anchor_IsSessionActive()) {
                if (switchFlag < 0x20) {
                    play->actorCtx.flags.swch &= ~(1 << switchFlag);
                } else {
                    play->actorCtx.flags.tempSwch &= ~(1 << (switchFlag - 0x20));
                }
            } else {
                Flags_UnsetSwitch(play, switchFlag);
            }
            this->actionFunc = BgMizuShutter_Move;
        }
    }
}

void BgMizuShutter_Update(BgMizuShutter* thisx, PlayState* play) {
    s32 pad;
    BgMizuShutter* this = (BgMizuShutter*)thisx;

    this->actionFunc(this, play);
}

void BgMizuShutter_Draw(BgMizuShutter* thisx, PlayState* play) {
    s32 pad;
    BgMizuShutter* this = (BgMizuShutter*)thisx;

    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    if (this->displayList != NULL) {
        gSPDisplayList(POLY_OPA_DISP++, this->displayList);
    }
    CLOSE_DISPS(play->state.gfxCtx);
}
