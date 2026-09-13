/*
 * File: z_bg_gnd_darkmeiro.c
 * Overlay: ovl_Bg_Gnd_Darkmeiro
 * Description: Shadow trial actors (invisible path, clear block, and timer)
 */

#include "z_bg_gnd_darkmeiro.h"
#include "objects/object_demo_kekkai/object_demo_kekkai.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

// Co-op: bit of `actionFlags` remembering the value THIS copy last wrote to the
// base switch flag. Free in both branches that use actionFlags (the clear block
// uses 0x2, the block timer 0x4 and 0x8).
#define DARKMEIRO_COOP_FLAG_WRITTEN 0x10

// Co-op: "this timer is running ORPHANED" — its torch flag went away without our
// own expiry clearing it. Deliberately NOT bits 0x4 / 0x8: those mean "armed by a
// torch that is still lit", and the main branch tests them FIRST. Reusing them for
// the orphaned countdown is what made the first version of this fix re-arm nothing
// when the torch came back on — it fell into `if (actionFlags & 4)` and resumed the
// stale remainder instead of seeding a fresh 304, which is precisely the defect the
// comment below claims to avoid. On separate bits, a re-light lands in the `else`
// and gets its full cycle.
#define DARKMEIRO_COOP_ORPHAN_1 0x20
#define DARKMEIRO_COOP_ORPHAN_2 0x40

s32 Anchor_IsSessionActive(void); // soh/Network/Anchor/DummyPlayer.cpp

void BgGndDarkmeiro_Init(Actor* thisx, PlayState* play);
void BgGndDarkmeiro_Destroy(Actor* thisx, PlayState* play);
void BgGndDarkmeiro_Update(Actor* thisx, PlayState* play);
void BgGndDarkmeiro_DrawInvisiblePath(Actor* thisx, PlayState* play);
void BgGndDarkmeiro_DrawSwitchBlock(Actor* thisx, PlayState* play);
void BgGndDarkmeiro_DrawStaticBlock(Actor* thisx, PlayState* play);

void BgGndDarkmeiro_Noop(BgGndDarkmeiro* this, PlayState* play);
void BgGndDarkmeiro_UpdateBlockTimer(BgGndDarkmeiro* this, PlayState* play);
void BgGndDarkmeiro_UpdateStaticBlock(BgGndDarkmeiro* this, PlayState* play);
void BgGndDarkmeiro_UpdateSwitchBlock(BgGndDarkmeiro* this, PlayState* play);

const ActorInit Bg_Gnd_Darkmeiro_InitVars = {
    ACTOR_BG_GND_DARKMEIRO,
    ACTORCAT_PROP,
    FLAGS,
    OBJECT_DEMO_KEKKAI,
    sizeof(BgGndDarkmeiro),
    (ActorFunc)BgGndDarkmeiro_Init,
    (ActorFunc)BgGndDarkmeiro_Destroy,
    (ActorFunc)BgGndDarkmeiro_Update,
    NULL,
    NULL,
};

void BgGndDarkmeiro_ToggleBlock(BgGndDarkmeiro* this, PlayState* play) {
    if (this->actionFlags & 2) {
        if (this->timer1 == 0) {
            func_8003EBF8(play, &play->colCtx.dyna, this->dyna.bgId);
            this->actionFlags &= ~2;
        }
    } else if (this->timer1 != 0) {
        func_8003EC50(play, &play->colCtx.dyna, this->dyna.bgId);
        this->actionFlags |= 2;
    }
}

void BgGndDarkmeiro_Init(Actor* thisx, PlayState* play2) {
    PlayState* play = play2;
    CollisionHeader* colHeader = NULL;
    BgGndDarkmeiro* this = (BgGndDarkmeiro*)thisx;

    this->updateFunc = BgGndDarkmeiro_Noop;
    Actor_SetScale(&this->dyna.actor, 0.1f);
    switch (this->dyna.actor.params & 0xFF) {
        case DARKMEIRO_INVISIBLE_PATH:
            this->dyna.actor.draw = BgGndDarkmeiro_DrawInvisiblePath;
            this->dyna.actor.flags |= ACTOR_FLAG_REACT_TO_LENS;
            break;
        case DARKMEIRO_CLEAR_BLOCK:
            CollisionHeader_GetVirtual(&gClearBlockCol, &colHeader);
            this->dyna.bgId = DynaPoly_SetBgActor(play, &play->colCtx.dyna, &this->dyna.actor, colHeader);
            if (((this->dyna.actor.params >> 8) & 0x3F) == 0x3F) {
                this->updateFunc = BgGndDarkmeiro_UpdateStaticBlock;
                this->dyna.actor.draw = BgGndDarkmeiro_DrawStaticBlock;
            } else {
                this->actionFlags = this->timer1 = this->timer2 = 0;
                thisx->draw = BgGndDarkmeiro_DrawSwitchBlock;
                this->updateFunc = BgGndDarkmeiro_UpdateSwitchBlock;
                if (!Flags_GetSwitch(play, (this->dyna.actor.params >> 8) & 0x3F)) {
                    func_8003EBF8(play, &play->colCtx.dyna, this->dyna.bgId);
                } else {
                    this->timer1 = 64;
                    this->actionFlags |= 2;
                }
            }
            break;
        case DARKMEIRO_BLOCK_TIMER:
            this->actionFlags = this->timer1 = this->timer2 = 0;
            this->updateFunc = BgGndDarkmeiro_UpdateBlockTimer;
            thisx->draw = NULL;
            // Co-op: vanilla seeds a re-entered room's timers with 64, the exact
            // threshold the update tail tests with `>=`. One frame later the timer is
            // 63 and the tail clears the base flag: offline that only shortens YOUR
            // own blocks, but the clear is a networked scene-flag change, so P2
            // walking into room 12 wiped the blocks out from under P1. Seed a full
            // cycle instead while a session is live -- it is never shorter than the
            // peer's remaining time, so the peer's own expiry still decides when the
            // blocks go, and the expiry path below keeps the two copies aligned.
            if (Flags_GetSwitch(play, ((this->dyna.actor.params >> 8) & 0x3F) + 1)) {
                this->timer1 = Anchor_IsSessionActive() ? 304 : 64;
                this->actionFlags |= 4;
            }
            if (Flags_GetSwitch(play, ((this->dyna.actor.params >> 8) & 0x3F) + 2)) {
                this->timer2 = Anchor_IsSessionActive() ? 304 : 64;
                this->actionFlags |= 8;
            }
            // Vanilla wrote the base flag here (Set with a timer live, Unset
            // otherwise). Both writes are redundant -- the update tail recomputes the
            // same predicate from the same two timers on the very next frame -- so
            // only seed the memo of what this copy last wrote, and let the tail's
            // first decision be an edge rather than a blind absolute write.
            //
            // Of the two, it is the *Set* that mattered, not the Unset: to reach the
            // Unset both timers must be 0, which means N+1 and N+2 are both locally
            // clear, and no reachable state has N set while they are -- the tail keeps
            // N slaved to the timers, and func_80031B14's `tempSwch &= 0xFFFFFF`
            // (z_actor.c:3226) wipes all three together. The Unset was a no-op.
            //
            // Known limit, not fixed here: the tail drops N as soon as a timer falls
            // below 64 (:201) but only clears N+1 when it reaches 0 (:144), so for 63
            // ticks per cycle N+1 is set while N is clear. P1 loading the room inside
            // that window seeds a full 304, decides `blocksWanted` on the next frame
            // and re-Sets N -- P2's seventeen blocks come back up after correctly
            // going away. Vanilla did exactly the same thing one frame earlier, via
            // the Set removed just above, so this is not a regression; closing it
            // needs the two thresholds reconciled, which is a change to the puzzle's
            // vanilla timing and belongs in its own commit.
            if (Flags_GetSwitch(play, (this->dyna.actor.params >> 8) & 0x3F)) {
                this->actionFlags |= DARKMEIRO_COOP_FLAG_WRITTEN;
            }
            break;
    }
}

void BgGndDarkmeiro_Destroy(Actor* thisx, PlayState* play2) {
    PlayState* play = play2;
    BgGndDarkmeiro* this = (BgGndDarkmeiro*)thisx;

    if ((this->dyna.actor.params & 0xFF) == 1) {
        DynaPoly_DeleteBgActor(play, &play->colCtx.dyna, this->dyna.bgId);
    }
}

void BgGndDarkmeiro_Noop(BgGndDarkmeiro* this, PlayState* play) {
}

void BgGndDarkmeiro_UpdateBlockTimer(BgGndDarkmeiro* this, PlayState* play) {
    s16 timeLeft;
    u16 blocksWanted;

    if (Flags_GetSwitch(play, ((this->dyna.actor.params >> 8) & 0x3F) + 1)) {
        if (this->actionFlags & 4) {
            if (this->timer1 > 0) {
                if (GameInteractor_Should(VB_SWITCH_TIMER_TICK, true, this, &this->timer1)) {
                    this->timer1--;
                }
            } else {
                Flags_UnsetSwitch(play, ((this->dyna.actor.params >> 8) & 0x3F) + 1);
                this->actionFlags &= ~4;
            }
        } else {
            // A re-light always gets a FULL cycle, even if an orphaned countdown of
            // ours was still running it down: drop the orphan and seed 304.
            this->actionFlags &= ~DARKMEIRO_COOP_ORPHAN_1;
            this->actionFlags |= 4;
            this->timer1 = 304;
            Audio_PlaySoundGeneral(NA_SE_EV_RED_EYE, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                   &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
        }
    } else if (Anchor_IsSessionActive() && (this->actionFlags & (4 | DARKMEIRO_COOP_ORPHAN_1))) {
        // Co-op: our torch flag went out without OUR timer running it down. Two ways
        // that happens, and only the second one is about the peer:
        //   - func_80031B14 does `actorCtx->flags.tempSwch &= 0xFFFFFF` (z_actor.c:3226)
        //     on a room transition, which wipes switch flags 0x38..0x3F. Room 12's
        //     timer is params 0x3802 and its two Obj_Syokudai are 0x1079 / 0x107A, so
        //     N, N+1 and N+2 are 0x38 / 0x39 / 0x3A -- all three inside the wiped byte.
        //     Only the transition INTO the room orphans a live copy: on the way out,
        //     z_room.c:658-660 sets prevRoom.num = -1 before calling func_80031B14, so
        //     our actor fails both room tests and is deleted by the same call that
        //     does the wipe. Coming in, the setup actor list has already been drained
        //     at the head of Actor_UpdateAll (z_actor.c:2601-2607) several frames
        //     before the player finishes the door, so our Init has read N+1 still set
        //     and armed 304 -- and then the wipe takes the flag away underneath it.
        //   - P2's copy expired first and its Flags_UnsetSwitch reached us.
        // Keep counting OUR timer down. Zeroing it here is what the first version of
        // this fix did, and it took the seventeen platforms out from under P1 three
        // seconds later while P2 still had them: the tail sees timer1 == 0, writes the
        // memo to 0, UpdateSwitchBlock stops refreshing the blocks and ToggleBlock
        // drops their collision. Freezing it -- vanilla's behaviour -- is no good
        // either: the NEXT lighting of the torch finds `actionFlags & 4` already set,
        // counts the stale remainder down instead of re-arming, and cuts the fresh
        // cycle short for both players. Running the countdown out normally is the only
        // option that neither strands P1 nor poisons the next cycle.
        // Gated on the session: offline the flag wipe happens too, and vanilla freezes.
        //
        // Hand the countdown over to its own bit. While it runs here, 0x4 must be
        // CLEAR, so that a re-light of the torch falls into the `else` above and
        // re-arms a full 304 instead of resuming this remainder. The tick goes
        // through VB_SWITCH_TIMER_TICK like the two in the main branch, or the
        // Switch Timer Multiplier enhancement would run the orphan at 1x while the
        // armed one obeys the setting.
        this->actionFlags &= ~4;
        this->actionFlags |= DARKMEIRO_COOP_ORPHAN_1;
        if (this->timer1 > 0) {
            if (GameInteractor_Should(VB_SWITCH_TIMER_TICK, true, this, &this->timer1)) {
                this->timer1--;
            }
        } else {
            this->actionFlags &= ~DARKMEIRO_COOP_ORPHAN_1;
        }
    }

    if (Flags_GetSwitch(play, ((this->dyna.actor.params >> 8) & 0x3F) + 2)) {
        if (this->actionFlags & 8) {
            if (this->timer2 > 0) {
                if (GameInteractor_Should(VB_SWITCH_TIMER_TICK, true, this, &this->timer2)) {
                    this->timer2--;
                }
            } else {
                Flags_UnsetSwitch(play, ((this->dyna.actor.params >> 8) & 0x3F) + 2);
                this->actionFlags &= ~8;
            }
        } else {
            this->actionFlags &= ~DARKMEIRO_COOP_ORPHAN_2;
            this->actionFlags |= 8;
            this->timer2 = 304;
            Audio_PlaySoundGeneral(NA_SE_EV_RED_EYE, &gSfxDefaultPos, 4, &gSfxDefaultFreqAndVolScale,
                                   &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
        }
    } else if (Anchor_IsSessionActive() && (this->actionFlags & (8 | DARKMEIRO_COOP_ORPHAN_2))) {
        // Same as timer 1 above: run our own countdown out on its own bit rather
        // than zero it, freeze it, or leave it holding 0x8.
        this->actionFlags &= ~8;
        this->actionFlags |= DARKMEIRO_COOP_ORPHAN_2;
        if (this->timer2 > 0) {
            if (GameInteractor_Should(VB_SWITCH_TIMER_TICK, true, this, &this->timer2)) {
                this->timer2--;
            }
        } else {
            this->actionFlags &= ~DARKMEIRO_COOP_ORPHAN_2;
        }
    }

    timeLeft = CLAMP_MIN(this->timer1, this->timer2);
    if (timeLeft > 0) {
        func_8002F994(&this->dyna.actor, timeLeft);
    }

    // Vanilla rewrote the base flag EVERY frame from two strictly local countdowns.
    // With both players in the room that is two writers of one scene flag running at
    // different phases: each Unset from the one that expired first was answered by a
    // Set from the other on its next frame, and the seventeen clear blocks flickered
    // for as long as anyone stayed. Write only when OUR OWN verdict changes. That is
    // behaviour-identical offline -- Flags_SetSwitch/Flags_UnsetSwitch already no-op
    // and emit nothing when the bit is unchanged (z_actor.c:688, :704), and this
    // actor is the only writer of flag N -- while online it makes the peer's verdict
    // stick instead of being overwritten a frame later.
    blocksWanted = ((this->timer1 >= 64) || (this->timer2 >= 64)) ? DARKMEIRO_COOP_FLAG_WRITTEN : 0;
    if (blocksWanted != (this->actionFlags & DARKMEIRO_COOP_FLAG_WRITTEN)) {
        if (blocksWanted) {
            this->actionFlags |= DARKMEIRO_COOP_FLAG_WRITTEN;
            Flags_SetSwitch(play, (this->dyna.actor.params >> 8) & 0x3F);
        } else {
            this->actionFlags &= ~DARKMEIRO_COOP_FLAG_WRITTEN;
            Flags_UnsetSwitch(play, (this->dyna.actor.params >> 8) & 0x3F);
        }
    }
}

void BgGndDarkmeiro_UpdateStaticBlock(BgGndDarkmeiro* this, PlayState* play) {
}

void BgGndDarkmeiro_UpdateSwitchBlock(BgGndDarkmeiro* this, PlayState* play) {
    if (this->timer1 > 0) {
        this->timer1--;
    }

    if (Flags_GetSwitch(play, (this->dyna.actor.params >> 8) & 0x3F)) {
        this->timer1 = 64;
    }

    BgGndDarkmeiro_ToggleBlock(this, play);
}

void BgGndDarkmeiro_Update(Actor* thisx, PlayState* play2) {
    BgGndDarkmeiro* this = (BgGndDarkmeiro*)thisx;
    PlayState* play = play2;

    this->updateFunc(this, play2);
}

void BgGndDarkmeiro_DrawInvisiblePath(Actor* thisx, PlayState* play) {
    Gfx_DrawDListXlu(play, gShadowTrialPathDL);
}

void BgGndDarkmeiro_DrawSwitchBlock(Actor* thisx, PlayState* play) {
    BgGndDarkmeiro* this = (BgGndDarkmeiro*)thisx;
    s16 vanishTimer;

    vanishTimer = this->timer1;
    if (vanishTimer != 0) {
        if (vanishTimer > 64) {
            this->timer2 = (this->timer2 < 120) ? this->timer2 + 8 : 127;
        } else if (vanishTimer > 16) {
            this->timer2 = (Math_CosS((u16)this->timer1 * 0x1000) * 64.0f) + 127.0f;
            if (this->timer2 > 127) {
                this->timer2 = 127;
            }
        } else {
            this->timer2 = vanishTimer * 8;
        }

        OPEN_DISPS(play->state.gfxCtx);
        //! @bug Due to a bug in the display list, the transparency data is not used.
        gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 198, 202, 208, this->timer2);
        CLOSE_DISPS(play->state.gfxCtx);

        Gfx_DrawDListXlu(play, gClearBlockDL);
    }
}

void BgGndDarkmeiro_DrawStaticBlock(Actor* thisx, PlayState* play) {
    OPEN_DISPS(play->state.gfxCtx);
    gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 198, 202, 208, 255);
    CLOSE_DISPS(play->state.gfxCtx);

    Gfx_DrawDListXlu(play, gClearBlockDL);
}
