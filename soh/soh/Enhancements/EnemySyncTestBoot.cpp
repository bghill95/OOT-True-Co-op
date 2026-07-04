#include <libultraship/bridge.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "functions.h"

extern "C" {
#include "z64.h"
#include "variables.h"
#include "overlays/gamestates/ovl_file_choose/file_choose.h"
extern PlayState* gPlayState;
}

/**
 * Test-rig boot harness for the enemy-sync co-op verification ladder.
 *
 * TestAutoLoadFile (1-3): on boot, skip the title screen and load that save slot
 * through the real Sram_OpenSave path. The debug warp screen's 0xFF debug save
 * can't be used for this — Anchor::IsSaveLoaded() rejects fileNum 0xFF, which
 * silently disables all networking. Requires the slot's .sav to exist and
 * DeveloperTools.DebugEnabled to be off (on, slot 1 boots to map select).
 *
 * TestAutoWarpEntrance (hex entrance index, -1 = off): one-shot instant warp on
 * the first player update after the save loads.
 */

#define CVAR_TEST_AUTO_LOAD_NAME CVAR_REMOTE_ANCHOR("TestAutoLoadFile")
#define CVAR_TEST_AUTO_LOAD_VALUE CVarGetInteger(CVAR_TEST_AUTO_LOAD_NAME, 0)
#define CVAR_TEST_AUTO_WARP_NAME CVAR_REMOTE_ANCHOR("TestAutoWarpEntrance")
#define CVAR_TEST_AUTO_WARP_VALUE CVarGetInteger(CVAR_TEST_AUTO_WARP_NAME, -1)

static bool sAutoWarpDone = false;

static void OnZTitleUpdateAutoLoad(void* gameState) {
    TitleContext* titleContext = (TitleContext*)gameState;

    gSaveContext.seqId = (u8)NA_BGM_DISABLED;
    gSaveContext.natureAmbienceId = 0xFF;
    gSaveContext.gameMode = GAMEMODE_FILE_SELECT;
    titleContext->state.running = false;

    SET_NEXT_GAMESTATE(&titleContext->state, FileChoose_Init, FileChooseContext);
}

static void OnFileChooseMainAutoLoad(void* gameState) {
    FileChooseContext* fileChooseContext = (FileChooseContext*)gameState;

    fileChooseContext->buttonIndex = CVAR_TEST_AUTO_LOAD_VALUE - 1; // slot N -> FS_BTN_SELECT_FILE_N
    fileChooseContext->menuMode = FS_MENU_MODE_SELECT;
    fileChooseContext->selectMode = SM_LOAD_GAME;
}

static void OnPlayerUpdateAutoWarp() {
    if (sAutoWarpDone || gPlayState == NULL) {
        return;
    }
    // Wait out intro/opening cutscenes: warping with cutsceneIndex >= 0xFFF0
    // resolves a cutscene scene-layer (entrance + offset) and lands the two
    // clients in different scene setups — mismatched enemy occurrence keys.
    if (Player_InCsMode(gPlayState) || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        return;
    }
    sAutoWarpDone = true;

    gSaveContext.cutsceneIndex = 0;
    gSaveContext.nextCutsceneIndex = 0;
    gPlayState->nextEntranceIndex = CVAR_TEST_AUTO_WARP_VALUE;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.nextTransitionType = TRANS_TYPE_INSTANT;
}

void RegisterEnemySyncTestBoot() {
    COND_HOOK(OnZTitleUpdate, CVAR_TEST_AUTO_LOAD_VALUE >= 1 && CVAR_TEST_AUTO_LOAD_VALUE <= 3,
              OnZTitleUpdateAutoLoad);
    COND_HOOK(OnFileChooseMain, CVAR_TEST_AUTO_LOAD_VALUE >= 1 && CVAR_TEST_AUTO_LOAD_VALUE <= 3,
              OnFileChooseMainAutoLoad);
    COND_HOOK(OnPlayerUpdate, CVAR_TEST_AUTO_WARP_VALUE >= 0, OnPlayerUpdateAutoWarp);
}

static RegisterShipInitFunc initFunc(RegisterEnemySyncTestBoot, { CVAR_TEST_AUTO_LOAD_NAME, CVAR_TEST_AUTO_WARP_NAME });
