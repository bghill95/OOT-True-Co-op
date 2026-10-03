/*
 * File: z_en_eg.c
 * Overlay: ovl_En_Eg
 * Description: Triggers a void out (used in the tower collapse sequence?)
 */

#include "z_en_eg.h"
#include "vt.h"

#define FLAGS ACTOR_FLAG_UPDATE_CULLING_DISABLED

void EnEg_Init(Actor* thisx, PlayState* play);
void EnEg_Destroy(Actor* thisx, PlayState* play);
void EnEg_Update(Actor* thisx, PlayState* play);
void EnEg_Draw(Actor* thisx, PlayState* play);

void func_809FFDC8(EnEg* this, PlayState* play);

static s32 voided = false;

static EnEgActionFunc sActionFuncs[] = {
    func_809FFDC8,
};

const ActorInit En_Eg_InitVars = {
    ACTOR_EN_EG,
    ACTORCAT_ITEMACTION,
    FLAGS,
    OBJECT_ZL2,
    sizeof(EnEg),
    (ActorFunc)EnEg_Init,
    (ActorFunc)EnEg_Destroy,
    (ActorFunc)EnEg_Update,
    (ActorFunc)EnEg_Draw,
    NULL,
};

void EnEg_PlayVoidOutSFX() {
    Sfx_PlaySfxCentered2(NA_SE_OC_ABYSS);
}

void EnEg_Destroy(Actor* thisx, PlayState* play) {
    voided = false;
}

void EnEg_Init(Actor* thisx, PlayState* play) {
    EnEg* this = (EnEg*)thisx;

    this->action = 0;
}

void FxSync_OnTowerTimerStart(s16 seconds);
void FxSync_OnTowerVoid(void);
s32 Anchor_IsSessionActive(void);

// The vanilla body of func_809FFDC8, factored out so the peer-driven path below
// runs the identical sequence.
static void EnEg_VoidOut(PlayState* play) {
    // Void the player out
    Play_TriggerRespawn(play);
    gSaveContext.respawnFlag = -2;
    Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_STOP);
    play->transitionType = TRANS_TYPE_FADE_BLACK;
    EnEg_PlayVoidOutSFX();
    voided = true;
}

// Co-op: the collapse failed on the other player's screen. Throw us out too.
//
// Chosen over "eject neither" deliberately. Every player in this sequence
// respawns at ENTR_GANONS_TOWER_COLLAPSE_EXTERIOR_0 (z_sram.c:60-61, and
// Play_LoadToLastEntrance at z_play.c:2138), so a collective void lands P1 and P2
// on the same tile and the run continues; a one-sided void leaves the other
// player alone inside a scene whose fail state has already fired, with no
// mechanism anywhere to bring the party back together. "Eject neither" would
// have meant deleting the only fail state the sequence has.
//
// Latches `voided` so our own copy does not fire a second one, and never
// announces — FxSync calls this on the receive side only, so there is no echo.
void EnEg_VoidOutFromPeer(PlayState* play) {
    if (voided || (play == NULL)) {
        return;
    }
    EnEg_VoidOut(play);
}

// Ticks down to the next re-announcement of a live collapse countdown. Shared by
// every En_Eg instance on purpose: the rate cap is what matters, not the actor.
static s32 sTowerTimerHeartbeat = 0;

// Re-announce the clock we are running, once every 40 ticks.
//
// FxSync events are fire-and-forget, so the single announcement z_en_zl3.c makes
// when it arms the countdown only reaches players who are already in the scene.
// The tower sequence is not entered together — nothing in the code posts these
// entrances, they come from the scene data — so P2 routinely walks in a minute
// after P1 and would arm a fresh 180 against P1's 120.
//
// A heartbeat carrying the CURRENT remaining seconds fixes that without any
// late-join machinery: the tower falls at a wall-clock moment, not three minutes
// after each player personally noticed. Whoever already has a countdown refuses
// it (FxSync's CollapseTimerRunning), so this never resets anyone and never
// ping-pongs.
//
// 40 ticks is 2 s at this game's R_UPDATE_RATE = 3 (20 ticks/s, game.c:437), not
// the 60 fps the number looks like.
//
// How far behind an adopter actually ends up. Two candidates, and only one of them
// is real:
//
//   - Waiting up to 40 ticks for the next beat costs NOTHING. The beat carries the
//     CURRENT remainder (FxSync_OnTowerTimerStart(gSaveContext.subTimerSeconds)),
//     not a fresh 180, so a late beat delays the adoption without skewing the clock.
//   - The countdown's entry animation does cost. func_80088AA0 parks subTimerState
//     at 1 (z_parameter.c:3770) and only state 4 decrements the seconds
//     (:6203-6204). State 1 falls through to 2 in a single tick (:6135-6146, no
//     timer on that step), state 2 takes 20 and state 3 takes 20, so the first
//     decrement lands 41 ticks after the adoption. Against a sender already in
//     state 4 at an arbitrary point of its own 20-tick slice, that is 21 to 41
//     ticks, i.e. 1 to 2 s, plus up to another second of truncation to the second.
//
// Call it 2 to 3 s of drift on a 180 s clock, and it never closes:
// CollapseTimerRunning() is true from state 1, so every later heartbeat is refused.
// Well inside what the sequence tolerates, and FX_EV_TOWERVOID covers the end.
// (An earlier version of this comment said ~5 s, by counting the heartbeat wait as
// drift and giving state 1 a 20-tick step it does not have.)
//
// Known gap, NOT covered: 0x36 is a tempSwch flag, so it is per-scene, and the
// transport itself is same-scene (Packets/FxEvent.cpp:32 and :43). The escape runs
// across three scenes — ganon_sonogo, ganon_final, ganontikasonogo — so once the two
// players are in different ones, neither the heartbeat nor the void reaches the
// other. Closing that needs a channel that is not FxSync.
static void EnEg_HeartbeatTowerTimer(PlayState* play) {
    if (!Anchor_IsSessionActive() || !Flags_GetSwitch(play, 0x36)) {
        return;
    }
    // Same 1..4 countdown window as z_en_zl3.c's EnZl3_TowerTimerRunning: never
    // re-announce a stopwatch counting up (7..10) or an expiry (5/6).
    if ((gSaveContext.subTimerState < 1) || (gSaveContext.subTimerState > 4) || (gSaveContext.subTimerSeconds <= 0)) {
        return;
    }
    if (sTowerTimerHeartbeat > 0) {
        sTowerTimerHeartbeat--;
        return;
    }
    sTowerTimerHeartbeat = 40;
    FxSync_OnTowerTimerStart(gSaveContext.subTimerSeconds);
}

void func_809FFDC8(EnEg* this, PlayState* play) {
    EnEg_HeartbeatTowerTimer(play);

    if (!voided && (gSaveContext.subTimerSeconds < 1) && Flags_GetSwitch(play, 0x36) && (kREG(0) == 0)) {
        // Co-op: switch flag 0x36 replicates (HookHandlers.cpp:130 -> SetFlag.cpp,
        // and 0x36 is a tempSwch bit so it lands whenever the peer is in this same
        // scene) but the countdown that is supposed to accompany it does not: it
        // lives in gSaveContext, written locally by func_80B59AD0 (z_en_zl3.c:2638).
        // So the frame P1's escape cutscene ends, P2 receives 0x36 with
        // subTimerSeconds still at its save-file 0 — and the test above is
        // instantly true. P2 gets thrown out of the tower before he has taken a
        // step, with no clock ever having been shown to him.
        //
        // The fix is the same lesson as z_obj_roomtimer.c: read the state, not the
        // leftover seconds. subTimerState 0 means no clock of ours ever armed —
        // not "our clock expired", which in these four scenes parks the state at 6
        // for forty frames (z_parameter.c:6221-6222) before releasing it, far more
        // than the one frame this actor needs. So state 0 here can only be the
        // race above, and the right answer is to wait: either our own En_Zl3 arms
        // the clock, or FxSync hands us P1's (FX_EV_TOWERTIMER), or P1's own
        // expiry reaches us as FX_EV_TOWERVOID and EnEg_VoidOutFromPeer runs.
        //
        // Gated on an active session so a solo game keeps the vanilla behaviour
        // byte for byte, including a debug warp into the scene with 0x36 forced on.
        if (Anchor_IsSessionActive() && (gSaveContext.subTimerState == 0)) {
            return;
        }
        EnEg_VoidOut(play);
        // Ours ran out first: fail the sequence for the whole party, not just us.
        FxSync_OnTowerVoid();
    }
}

void EnEg_Update(Actor* thisx, PlayState* play) {
    EnEg* this = (EnEg*)thisx;
    s32 action = this->action;

    if (((action < 0) || (0 < action)) || (sActionFuncs[action] == NULL)) {
        // "Main Mode is wrong!!!!!!!!!!!!!!!!!!!!!!!!!"
        osSyncPrintf(VT_FGCOL(RED) "メインモードがおかしい!!!!!!!!!!!!!!!!!!!!!!!!!\n" VT_RST);
    } else {
        sActionFuncs[action](this, play);
    }
}

void EnEg_Draw(Actor* thisx, PlayState* play) {
}
