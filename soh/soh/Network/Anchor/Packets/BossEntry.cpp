#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/BossEntry.h"
#include "soh/Network/Anchor/CoopWarp.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "variables.h"
#include "z64.h"
extern PlayState* gPlayState;
}

/**
 * BOSS_ENTRY
 *
 * Boss-room co-entry: { sceneNum, entranceIndex, targetTeamId }.
 *
 * SENDER (RegisterBossEntryHooks -> OnSceneInit): when a boss scene loads and this
 * load was NOT itself caused by a BOSS_ENTRY pull (echo latch), broadcast our
 * sceneNum + the boss-room entrance for that dungeon.
 *
 * RECEIVER (HandlePacket_BossEntry): three guards, then ALWAYS pull (user policy):
 *   1. already in that boss scene  -> ignore (covers simultaneous entry).
 *   2. we are DOWNED/spectating    -> ignore (Task 8's scene-follow owns us).
 *   3. otherwise -> latch + RequestEntranceWarp(bossEntrance, -1); boss intro plays
 *      naturally on load and CoopWarp's gate defers the warp past any cutscene /
 *      transition / pause.
 *
 * Cross-version safe: an unknown packet type falls through Anchor.cpp's dispatch
 * ladder to a no-op.
 */

namespace {

// One boss encounter. dungeonScene is documentation / cross-reference only; the
// runtime keys purely on bossScene (detect) and bossEntrance (warp target).
struct BossEntryInfo {
    s16 bossScene;
    s16 dungeonScene;
    s32 bossEntrance;
};

// The 9 boss encounters (Ganon-final battle excluded in v1). Every bossEntrance is
// the boss-room entrance for that dungeon, verified against entrance_table.h and
// cross-checked with the kaleido respawn-rewrite table (z_kaleido_scope_PAL.c:
// 4723-4749, which maps each boss entrance back to its dungeon entrance) and the
// D_80998288 BossDoorInfo table (z_door_shutter.c:165-173, the 6 late bosses).
constexpr BossEntryInfo sBossTable[] = {
    // bossScene (scene_table.h)   dungeonScene            bossEntrance (entrance_table.h)   boss
    { SCENE_DEKU_TREE_BOSS,        SCENE_DEKU_TREE,        ENTR_DEKU_TREE_BOSS_ENTRANCE },       // 0x40F Gohma
    { SCENE_DODONGOS_CAVERN_BOSS,  SCENE_DODONGOS_CAVERN,  ENTR_DODONGOS_CAVERN_BOSS_ENTRANCE }, // 0x40B King Dodongo
    { SCENE_JABU_JABU_BOSS,        SCENE_JABU_JABU,        ENTR_JABU_JABU_BOSS_ENTRANCE },       // 0x301 Barinade
    { SCENE_FOREST_TEMPLE_BOSS,    SCENE_FOREST_TEMPLE,    ENTR_FOREST_TEMPLE_BOSS_ENTRANCE },   // 0x00C Phantom Ganon
    { SCENE_FIRE_TEMPLE_BOSS,      SCENE_FIRE_TEMPLE,      ENTR_FIRE_TEMPLE_BOSS_ENTRANCE },     // 0x305 Volvagia
    { SCENE_WATER_TEMPLE_BOSS,     SCENE_WATER_TEMPLE,     ENTR_WATER_TEMPLE_BOSS_ENTRANCE },    // 0x417 Morpha
    { SCENE_SPIRIT_TEMPLE_BOSS,    SCENE_SPIRIT_TEMPLE,    ENTR_SPIRIT_TEMPLE_BOSS_ENTRANCE },   // 0x08D Twinrova
    { SCENE_SHADOW_TEMPLE_BOSS,    SCENE_SHADOW_TEMPLE,    ENTR_SHADOW_TEMPLE_BOSS_ENTRANCE },   // 0x413 Bongo Bongo
    { SCENE_GANONDORF_BOSS,        SCENE_GANONS_TOWER,     ENTR_GANONDORF_BOSS_0 },              // 0x41F Ganondorf
};

const BossEntryInfo* FindBossByScene(s16 sceneNum) {
    for (const BossEntryInfo& e : sBossTable) {
        if (e.bossScene == sceneNum) {
            return &e;
        }
    }
    return nullptr;
}

// Echo latch. The RECEIVER sets this immediately before queuing its own pull warp,
// so the boss-scene OnSceneInit that warp causes does NOT re-broadcast BOSS_ENTRY
// (which would yank the original player back -> infinite pull). Consumed on the
// next boss-scene OnSceneInit. Set/consumed only on the game thread, so no lock.
bool sWarpedInByBossEntry = false;

} // namespace

void Anchor::SendPacket_BossEntry(s16 sceneNum, s32 entranceIndex) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BOSS_ENTRY;
    payload["targetTeamId"] = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
    payload["sceneNum"] = sceneNum;
    payload["entranceIndex"] = entranceIndex;

    SendJsonToRemote(payload);

    SPDLOG_INFO("[BossEntry] tx scene={} entrance={}", sceneNum, entranceIndex);
}

void Anchor::HandlePacket_BossEntry(nlohmann::json payload) {
    if (!IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    s32 entranceIndex = payload["entranceIndex"].get<s32>();

    // Resolve the warp target from OUR OWN table keyed by scene, so a garbled/foreign
    // entrance index can never warp us somewhere wrong (both peers share this build's
    // table). The wire entranceIndex is used only as a diagnostic cross-check.
    const BossEntryInfo* boss = FindBossByScene(sceneNum);
    if (boss == nullptr) {
        SPDLOG_INFO("[BossEntry] rx ignored (not a boss scene {})", sceneNum);
        return;
    }
    if (entranceIndex != boss->bossEntrance) {
        SPDLOG_WARN("[BossEntry] rx entrance mismatch: pkt={} table={}", entranceIndex, boss->bossEntrance);
    }

    // Guard 1: already in that boss scene -> ignore. On simultaneous entry both
    // sides are already in the scene when they process the other's packet, so
    // neither warps -> no double-warp.
    if (gPlayState->sceneNum == sceneNum) {
        SPDLOG_INFO("[BossEntry] rx ignored (already in scene {})", sceneNum);
        return;
    }

    // Guard 2: we are downed / spectating / game-over -> ignore. Task 8's scene-follow
    // owns the dead player's camera; a fully-dead or game-over player must not be yanked
    // into a boss fight either. >= LIFE_STATE_DOWNED covers DOWNED and GAME_OVER.
    if (myLifeState >= LIFE_STATE_DOWNED) {
        SPDLOG_INFO("[BossEntry] rx ignored (downed/game-over)");
        return;
    }

    // Guard 3: otherwise ALWAYS pull. Set the echo latch BEFORE queuing the warp so
    // the boss-scene OnSceneInit this causes stays quiet (breaks the pull loop).
    // cutsceneIndex -1 leaves the boss intro to play naturally on load.
    sWarpedInByBossEntry = true;
    RequestEntranceWarp((s16)boss->bossEntrance, -1);

    SPDLOG_INFO("[BossEntry] rx -> pull entrance={}", boss->bossEntrance);
}

void RegisterBossEntryHooks(bool isConnected) {
    // Drop a stale latch on (dis)connect so a reconnect can't suppress the first
    // legitimate broadcast.
    if (!isConnected) {
        sWarpedInByBossEntry = false;
    }

    // SENDER: detect boss-scene loads. Runs on every scene load; only boss scenes
    // are acted on. The latch makes a pulled-in load stay quiet.
    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) {
        const BossEntryInfo* boss = FindBossByScene(sceneNum);
        if (boss == nullptr) {
            return; // not a boss scene
        }

        // We loaded this boss scene because WE were pulled in: consume the latch and
        // do NOT re-broadcast (that would pull the original player right back).
        if (sWarpedInByBossEntry) {
            sWarpedInByBossEntry = false;
            SPDLOG_INFO("[BossEntry] tx suppressed (self-warped) scene={}", sceneNum);
            return;
        }

        if (Anchor::Instance != nullptr) {
            Anchor::Instance->SendPacket_BossEntry(sceneNum, boss->bossEntrance);
        }
    });
}
