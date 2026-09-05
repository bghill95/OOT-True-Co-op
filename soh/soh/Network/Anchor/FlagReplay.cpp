#include "FlagReplay.h"
#include "DynaSync.h"
#include "FxSync.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>
#include <vector>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Door_Gerudo/z_door_gerudo.h"
#include "src/overlays/actors/ovl_Bg_Ddan_Kd/z_bg_ddan_kd.h"
#include "src/overlays/actors/ovl_Bg_Jya_Megami/z_bg_jya_megami.h"
#include "src/overlays/actors/ovl_Bg_Heavy_Block/z_bg_heavy_block.h"
#include "src/overlays/actors/ovl_Obj_Bean/z_obj_bean.h"
#include "src/overlays/actors/ovl_Bg_Po_Event/z_bg_po_event.h"
#include "src/overlays/actors/ovl_Bg_Bdan_Switch/z_bg_bdan_switch.h"
#include "src/overlays/actors/ovl_En_Jj/z_en_jj.h"
#include "src/overlays/actors/ovl_Demo_Kankyo/z_demo_kankyo.h"
// Door-of-Time actions: defined non-static in z_demo_kankyo.c, not in its header.
void DemoKankyo_SetupAction(DemoKankyo* thisx, DemoKankyoActionFunc actionFunc);
void DemoKankyo_SetupType(DemoKankyo* thisx, PlayState* play);
void DemoKankyo_UpdateDoorOfTime(DemoKankyo* thisx, PlayState* play);
extern PlayState* gPlayState;

// Non-static vanilla action functions (declared only in their .c files).
void func_8099485C(DoorGerudo* self, PlayState* play); // wait (closed, or already raised)
void func_8099496C(DoorGerudo* self, PlayState* play); // unlock delay -> slide up
void BgDdanKd_CheckForExplosions(BgDdanKd* self, PlayState* play);
void BgDdanKd_LowerStairs(BgDdanKd* self, PlayState* play);
void BgJyaMegami_DetectLight(BgJyaMegami* self, PlayState* play);
void BgJyaMegami_SetupExplode(BgJyaMegami* self);
void BgHeavyBlock_Wait(BgHeavyBlock* self, PlayState* play);
void BgHeavyBlock_DoNothing(BgHeavyBlock* self, PlayState* play);
void ObjBean_WaitForBean(ObjBean* self, PlayState* play);
void ObjBean_SetupWaitForWater(ObjBean* self);
extern u8 sBgPoEventPuzzleState; // z_bg_po_event.c file-scope puzzle bits
void BgPoEvent_BlockIdle(BgPoEvent* self, PlayState* play);
void BgPoEvent_BlockPush(BgPoEvent* self, PlayState* play);
void BgPoEvent_BlockFall(BgPoEvent* self, PlayState* play);
void BgPoEvent_BlockReset(BgPoEvent* self, PlayState* play);
void BgPoEvent_BlockSolved(BgPoEvent* self, PlayState* play);
void BgPoEvent_CheckBlock(BgPoEvent* self);
// Jabu-Jabu buttons (z_bg_bdan_switch.c): setup functions arm the next state.
void func_8086D4B4(BgBdanSwitch* self, PlayState* play); // set the switch flag (+ chime)
void func_8086D5E0(BgBdanSwitch* self, PlayState* play); // BLUE / YELLOW: up, waiting
void func_8086D67C(BgBdanSwitch* self);                  // BLUE / YELLOW: -> lowering
void func_8086D754(BgBdanSwitch* self, PlayState* play); // BLUE / YELLOW: down
void func_8086D888(BgBdanSwitch* self, PlayState* play); // HEAVY: up, waiting
void func_8086D8CC(BgBdanSwitch* self, PlayState* play); // HEAVY: half-lowering
void func_8086DA1C(BgBdanSwitch* self, PlayState* play); // HEAVY: half down, waiting for Ruto
void func_8086DAC4(BgBdanSwitch* self, PlayState* play); // HEAVY: half-raising
void func_8086D944(BgBdanSwitch* self);                  // HEAVY: -> lowering to pressed
void func_8086DB68(BgBdanSwitch* self, PlayState* play); // TALL: up, waiting for a hit
void func_8086DC30(BgBdanSwitch* self);                  // TALL: -> lowering
void func_8086DCE8(BgBdanSwitch* self, PlayState* play); // TALL: down
void func_8086DDA8(BgBdanSwitch* self);                  // TALL: -> raising
// Jabu-Jabu himself (z_en_jj.c).
void EnJj_SetupAction(EnJj* self, EnJjActionFunc actionFunc);
void EnJj_WaitForFish(EnJj* self, PlayState* play);
void EnJj_WaitToOpenMouth(EnJj* self, PlayState* play);
}

// Last gameplay frame on which one of the Amy-puzzle blocks was found mirroring
// a remote stream: while fresh, the remote copy owns the puzzle state machine.
static uint32_t sAmyRemoteTick = 0;

// The countdown block (type 1) was falling on our side: announce the timer it
// starts when it lands (the mirror never runs BlockFall).
static bool sAmyTimerArmed = false;

// Jabu-Jabu blue buttons: true when OUR player is the one holding it down, so
// the remote-hold rule below does not keep it pressed after we step off.
static std::unordered_map<Actor*, bool> sBdanLocalPress;

// BG_JYA_BLOCK is Actor_Kill'ed by its own Init when the flag is missing, so
// there is nothing left to replay on. Remember the spawn entry of every copy
// killed that way and re-spawn it once the flag arrives from the peer.
struct KilledJyaBlock {
    Vec3f pos;
    Vec3s rot;
    int16_t params;
    int16_t room;
    bool done;
};
static std::vector<KilledJyaBlock> sJyaBlocks;

namespace FlagReplay {

void PerFrameTick() {
    if (gPlayState == NULL || sJyaBlocks.empty() || LINK_AGE_IN_YEARS != YEARS_CHILD) {
        return;
    }
    for (auto& b : sJyaBlocks) {
        if (b.done || b.room != gPlayState->roomCtx.curRoom.num || !Flags_GetSwitch(gPlayState, b.params & 0x3F)) {
            continue;
        }
        b.done = true;
        // A live copy already exists when the room was re-entered after the flag
        // arrived (Init kept it): never double a solid block.
        for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; a != NULL; a = a->next) {
            if (a->id == ACTOR_BG_JYA_BLOCK && a->update != NULL && a->params == b.params) {
                return;
            }
        }
        Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_BG_JYA_BLOCK, b.pos.x, b.pos.y, b.pos.z, b.rot.x,
                    b.rot.y, b.rot.z, b.params, false);
    }
}

void RegisterHooks(bool isConnected) {
    if (!isConnected) {
        sBdanLocalPress.clear();
        sJyaBlocks.clear();
    }

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) {
        sBdanLocalPress.clear();
        sJyaBlocks.clear();
        sAmyTimerArmed = false;
    });
    COND_HOOK(OnActorDestroy, isConnected, [](void* actor) { sBdanLocalPress.erase((Actor*)actor); });

    // Gerudo Fortress cell grate. func_8099485C both waits closed AND idles
    // once raised (Init puts an already-unlocked door there at home.y+200), so
    // the "still closed" test is positional. The peer's unlock already
    // decremented OUR key count (OnDungeonKeyUsed -> UpdateDungeonItems) and
    // set the flag: jump straight to the raise, never through the key branch.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_DOOR_GERUDO, isConnected, [](void* refActor, bool* should) {
        DoorGerudo* door = static_cast<DoorGerudo*>(refActor);
        Actor* a = &door->dyna.actor;
        if (door->actionFunc == func_8099485C && door->unk_164 == 0 && a->world.pos.y < a->home.pos.y + 100.0f &&
            Flags_GetSwitch(gPlayState, a->params & 0x3F)) {
            door->unk_166 = 10; // same unlock delay Init arms for a closed door
            door->actionFunc = func_8099496C;
            Audio_PlayActorSound2(a, NA_SE_EV_CHAIN_KEY_UNLOCK);
        }
    });

    // Dodongo's Cavern falling stairs: vanilla needs two distinct explosions
    // to hit its cylinder. Bomb replicas carry no AT (BombSync strips them so
    // damage is never dealt twice), so only the bomber's copy ever lowers.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_DDAN_KD, isConnected, [](void* refActor, bool* should) {
        BgDdanKd* stairs = static_cast<BgDdanKd*>(refActor);
        if (stairs->actionFunc == BgDdanKd_CheckForExplosions &&
            Flags_GetSwitch(gPlayState, stairs->dyna.actor.params)) {
            stairs->actionFunc = BgDdanKd_LowerStairs; // re-sets the flag at rest (no-op, already set)
        }
    });

    // Spirit Temple statue face: crumbles under sustained explosion AC hits
    // (lightTimer > 40), then explodes. Same AT reason as the stairs. Replay
    // the explosion directly; skip the bomber's OnePointCutscene (it would
    // steal the peer's camera wherever they stand).
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_MEGAMI, isConnected, [](void* refActor, bool* should) {
        BgJyaMegami* face = static_cast<BgJyaMegami*>(refActor);
        Actor* a = &face->dyna.actor;
        if (face->actionFunc == BgJyaMegami_DetectLight && Flags_GetSwitch(gPlayState, a->params & 0x3F)) {
            BgJyaMegami_SetupExplode(face);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &a->world.pos, 100, NA_SE_EV_FACE_EXPLOSION);
        }
    });

    // Spirit Temple child-side silver block: Init kills it when its flag is not
    // set yet. Remember where it was so PerFrameTick can bring it back once the
    // peer sets the flag (Init re-runs, finds the flag, keeps the block).
    COND_ID_HOOK(OnActorInit, ACTOR_BG_JYA_BLOCK, isConnected, [](void* refActor) {
        Actor* a = static_cast<Actor*>(refActor);
        if (a->update != NULL) {
            return; // kept: the flag was already set
        }
        for (auto& b : sJyaBlocks) {
            if (b.params == a->params && b.room == a->room) {
                b.done = false; // room re-entered, still flagless: arm again
                return;
            }
        }
        sJyaBlocks.push_back(KilledJyaBlock{ a->world.pos, a->shape.rot, a->params, a->room, false });
    });

    // Golden Gauntlets pillar: lifted/thrown only through the local player's
    // carry state (not CarrySync). Replay what Init does when it finds the flag
    // set: breakable -> gone, castle-exterior one -> parked at its landing
    // spot. Pieces fly only on the thrower's screen.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HEAVY_BLOCK, isConnected, [](void* refActor, bool* should) {
        BgHeavyBlock* block = static_cast<BgHeavyBlock*>(refActor);
        Actor* a = &block->dyna.actor;
        if (block->actionFunc != BgHeavyBlock_Wait || !Flags_GetSwitch(gPlayState, (a->params >> 8) & 0x3F)) {
            return;
        }
        switch (a->params & 0xFF) {
            case HEAVYBLOCK_BREAKABLE:
                Actor_Kill(a);
                *should = false;
                break;
            case HEAVYBLOCK_UNBREAKABLE_OUTSIDE_CASTLE:
                block->actionFunc = BgHeavyBlock_DoNothing;
                a->shape.rot.x = a->world.rot.x = 0x8AD0;
                a->shape.rot.y = a->world.rot.y = 0xC000;
                a->shape.rot.z = a->world.rot.z = 0x0;
                a->world.pos.x = 1704.0f;
                a->world.pos.y = 1504.0f;
                a->world.pos.z = 516.0f;
                break;
            default:
                break;
        }
    });

    // Magic bean soil (child): the planter's copy sets the flag from
    // WaitForBean; ours must leave WaitForBean too or the sprout never shows
    // and the spot still accepts a second bean. (The adult leaf platform is a
    // DynaSync ride-claimable: whoever stands on it streams the ride.)
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_BEAN, isConnected, [](void* refActor, bool* should) {
        ObjBean* bean = static_cast<ObjBean*>(refActor);
        if (LINK_AGE_IN_YEARS == YEARS_CHILD && bean->actionFunc == ObjBean_WaitForBean &&
            Flags_GetSwitch(gPlayState, bean->dyna.actor.params & 0x3F)) {
            ObjBean_SetupWaitForWater(bean);
        }
    });

    // Jabu-Jabu buttons. None of their states re-reads the flag to go DOWN, and
    // the blue one even UNSETS the flag when our untouched copy "releases" 6
    // frames after the peer pressed it. Follow the flag both ways, and while
    // the peer holds a blue button (flag set, nobody on our copy, we never
    // pressed it ourselves) keep our copy pressed. unk_1DA is the OnePoint
    // camera wait: zeroed so the replayed motion starts at once.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BDAN_SWITCH, isConnected, [](void* refActor, bool* should) {
        BgBdanSwitch* sw = static_cast<BgBdanSwitch*>(refActor);
        Actor* a = &sw->dyna.actor;
        int32_t type = a->params & 0xFF;
        bool flag = Flags_GetSwitch(gPlayState, (a->params >> 8) & 0x3F);
        switch (type) {
            case BLUE:
            case YELLOW: {
                bool pressedHere = (type == BLUE) ? DynaPolyActor_IsSwitchPressed(&sw->dyna) != 0
                                                  : DynaPolyActor_IsPlayerOnTop(&sw->dyna) != 0;
                bool& mine = sBdanLocalPress[a];
                if (sw->actionFunc == func_8086D5E0) {
                    mine = pressedHere;
                    if (flag && !pressedHere) {
                        func_8086D67C(sw);
                        sw->unk_1DA = 0;
                    }
                } else if (sw->actionFunc == func_8086D754) {
                    if (pressedHere) {
                        mine = true;
                    }
                    if (type == BLUE) {
                        if (flag && !pressedHere && !mine) {
                            sw->unk_1D8 = 6; // held down over there: never release + unset here
                        } else if (!flag && pressedHere) {
                            // Both players stood on it and the OTHER one stepped
                            // off first: their copy unset the flag on the way up
                            // while ours is still held down. Vanilla only sets
                            // the flag on the way down, so nothing would ever
                            // set it again — re-assert it while we hold it.
                            func_8086D4B4(sw, gPlayState);
                        }
                    }
                    // YELLOW raises by itself once the flag drops (vanilla).
                }
                break;
            }
            case YELLOW_HEAVY: // needs Ruto set down on it: only the carrier's copy can see that
                if (flag && (sw->actionFunc == func_8086D888 || sw->actionFunc == func_8086D8CC ||
                             sw->actionFunc == func_8086DA1C || sw->actionFunc == func_8086DAC4)) {
                    func_8086D944(sw);
                    sw->unk_1DA = 0;
                }
                break;
            case YELLOW_TALL_1:
            case YELLOW_TALL_2: // hit-toggled: the flag carries the toggle both ways
                if (flag && sw->actionFunc == func_8086DB68) {
                    func_8086DC30(sw);
                    sw->unk_1DA = 0;
                } else if (!flag && type == YELLOW_TALL_2 && sw->actionFunc == func_8086DCE8) {
                    func_8086DDA8(sw);
                    sw->unk_1DA = 0;
                }
                break;
            default:
                break;
        }
    });

    // Jabu-Jabu: the event flag rides the flag sync, but the copy that did not
    // offer the fish stays in WaitForFish forever, mouth shut, body collision
    // solid. Move it to the post-cutscene wait; the mouth then opens for our
    // player on approach exactly as it does after a reload.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_JJ, isConnected, [](void* refActor, bool* should) {
        EnJj* jj = static_cast<EnJj*>(refActor);
        if (jj->dyna.actor.params == JABUJABU_MAIN && jj->actionFunc == EnJj_WaitForFish &&
            Flags_GetEventChkInf(EVENTCHKINF_OFFERED_FISH_TO_JABU_JABU)) {
            EnJj_SetupAction(jj, EnJj_WaitToOpenMouth); // no cutscene on this side
        }
    });

    // ---- Temple of Time, Door of Time (DEMO_KANKYO type DOOR_OF_TIME) ----
    // The door halves are this actor; the collision is its DOOR_TOKI child. The
    // Song of Time cutscene opens it through an env flag (Flags_GetEnv 2) that
    // only the singer's cutscene raises, and DoorToki_Update drops the collision
    // the moment EVENTCHKINF_OPENED_THE_DOOR_OF_TIME arrives. On the other side
    // the flag came through the save sync first: a closed door with no
    // collision, walked straight through. Play the vanilla opening here (102
    // frames, then it kills the child and itself exactly as after the cutscene).
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_DEMO_KANKYO, isConnected, [](void* refActor, bool* should) {
        DemoKankyo* dk = static_cast<DemoKankyo*>(refActor);
        if (dk->actor.params == DEMOKANKYO_DOOR_OF_TIME && dk->actionFunc == DemoKankyo_SetupType &&
            dk->actor.child != NULL && Flags_GetEventChkInf(EVENTCHKINF_OPENED_THE_DOOR_OF_TIME)) {
            DemoKankyo_SetupAction(dk, DemoKankyo_UpdateDoorOfTime);
        }
    });

    // ---- Forest Temple, Amy's picture-block puzzle (BG_PO_EVENT types 0/1) ----
    // Vanilla solves it from file statics: each push end only refreshes the bits
    // of the pushed block and its parent, and the defeat flag comes from Amy's
    // death (En_Po_Sisters), so blocks read their flag at Init only. Co-op model:
    // pushes stream through DynaSync claims (anyone can push), the copy that is
    // NOT mirroring runs the puzzle (re-checks every block each idle frame so a
    // solve finished by a REMOTE push is seen and spawns Amy once), the mirroring
    // copy keeps only its grab/push states alive and follows the stream through
    // the scripted motions (fall / reset rise / solved sink). The driver holds a
    // DynaSync soft claim on the whole set while its puzzle is live, so a peer
    // entering the room later (even the scene authority, whose fresh blocks
    // would otherwise stream their Init pose) mirrors the puzzle in progress.
    COND_VB_SHOULD(VB_AMY_SOLVE, isConnected, {
        if (*should && gPlayState != NULL && (gPlayState->gameplayFrames - sAmyRemoteTick) <= 2) {
            *should = false; // the remote copy drives the puzzle: it spawns Amy, not us
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_PO_EVENT, isConnected, [](void* refActor, bool* should) {
        BgPoEvent* pe = static_cast<BgPoEvent*>(refActor);
        Actor* a = &pe->dyna.actor;
        // Init-only flag: the sisters' defeat flag removes the whole set (blocks,
        // Amy's painting, or a portrait chain), exactly what Init does on reload.
        if (Flags_GetSwitch(gPlayState, a->params)) {
            Actor_Kill(a);
            *should = false;
            return;
        }
        if (pe->type >= 2) {
            return; // paintings: replayed through FX_EV_POEPAINT (FxSync)
        }
        if (sBgPoEventPuzzleState != 0) {
            DynaSync::HoldSoftClaim(a); // our puzzle is live: keep driving unless outranked
        }
        bool idle = pe->actionFunc == BgPoEvent_BlockIdle;
        if (idle && pe->dyna.unk_150 == 0.0f && pe->direction == 0) {
            // BlockPush computes from home.pos; keep it on the streamed rest spot so
            // the next local push starts where the block really is (no snap-back).
            a->home.pos.x = a->world.pos.x;
            a->home.pos.z = a->world.pos.z;
        }
        if (!DynaSync::IsRemoteDriven(a)) {
            if (idle && pe->type == 0 && a->child != NULL) {
                BgPoEvent_CheckBlock(pe); // refresh this block's bit from live positions
            }
            if (pe->type == 1) {
                // The countdown starts in BlockFall's landing frame (func_80088B34);
                // the mirror never runs it, so hand it the seconds we armed.
                if (pe->actionFunc == BgPoEvent_BlockFall) {
                    sAmyTimerArmed = true;
                } else if (idle && sAmyTimerArmed) {
                    sAmyTimerArmed = false;
                    FxSync::SendAmyTimer(pe->timer);
                }
            }
            return;
        }
        sAmyRemoteTick = gPlayState->gameplayFrames;
        float y = a->world.pos.y;
        if (idle || pe->actionFunc == BgPoEvent_BlockPush) {
            if (idle && y > 434.0f) { // rising: the puzzle is being reset over there
                pe->actionFunc = BgPoEvent_BlockReset;
                *should = false;
                if (pe->type == 1) {
                    gSaveContext.timerState = 0xA; // hide the failed countdown, like Destroy does
                }
            } else if (idle && y < 432.0f) { // sinking below rest: solved over there
                pe->actionFunc = BgPoEvent_BlockSolved;
                *should = false;
                if (pe->type == 1) {
                    gSaveContext.timerState = 0xA; // BlockIdle's solve hides it on the driver
                }
            }
            return; // idle/push stay live so our player can grab it
        }
        if (pe->actionFunc != BgPoEvent_BlockSolved && fabsf(y - 433.0f) < 0.5f) {
            pe->actionFunc = BgPoEvent_BlockIdle; // landed at rest on the stream: pushable again
            pe->direction = 0;
        }
        *should = false; // wait / shake / fall / reset / solved: the stream moves it
    });
}

} // namespace FlagReplay
