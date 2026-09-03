#include "FlagReplay.h"
#include "DynaSync.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Door_Gerudo/z_door_gerudo.h"
#include "src/overlays/actors/ovl_Bg_Ddan_Kd/z_bg_ddan_kd.h"
#include "src/overlays/actors/ovl_Bg_Jya_Megami/z_bg_jya_megami.h"
#include "src/overlays/actors/ovl_Bg_Heavy_Block/z_bg_heavy_block.h"
#include "src/overlays/actors/ovl_Obj_Bean/z_obj_bean.h"
#include "src/overlays/actors/ovl_Bg_Po_Event/z_bg_po_event.h"
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
void BgPoEvent_BlockReset(BgPoEvent* self, PlayState* play);
void BgPoEvent_BlockSolved(BgPoEvent* self, PlayState* play);
void BgPoEvent_CheckBlock(BgPoEvent* self);
}

// Last gameplay frame on which one of the Amy-puzzle blocks was found mirroring
// a remote stream: while fresh, the remote copy owns the puzzle state machine.
static uint32_t sAmyRemoteTick = 0;

namespace FlagReplay {

void RegisterHooks(bool isConnected) {
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
    // and the spot still accepts a second bean.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_BEAN, isConnected, [](void* refActor, bool* should) {
        ObjBean* bean = static_cast<ObjBean*>(refActor);
        if (LINK_AGE_IN_YEARS == YEARS_CHILD && bean->actionFunc == ObjBean_WaitForBean &&
            Flags_GetSwitch(gPlayState, bean->dyna.actor.params & 0x3F)) {
            ObjBean_SetupWaitForWater(bean);
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
    // the scripted motions (fall / reset rise / solved sink).
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
            return;
        }
        sAmyRemoteTick = gPlayState->gameplayFrames;
        float y = a->world.pos.y;
        if (idle || pe->actionFunc == BgPoEvent_BlockPush) {
            if (idle && y > 434.0f) { // rising: the puzzle is being reset over there
                pe->actionFunc = BgPoEvent_BlockReset;
                *should = false;
            } else if (idle && y < 432.0f) { // sinking below rest: solved over there
                pe->actionFunc = BgPoEvent_BlockSolved;
                *should = false;
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
