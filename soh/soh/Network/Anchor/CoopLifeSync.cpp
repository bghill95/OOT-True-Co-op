#include "CoopLifeSync.h"
#include "Anchor.h"
#include "EnemySync.h"
#include "CoopWarp.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "objects/gameplay_keep/gameplay_keep.h" // gPlayerAnim_link_derth_rebirth (OTR asset path)
extern PlayState* gPlayState;

// Engine-internal player death setup with no public header decl (same forward-decl
// pattern HookHandlers.cpp uses for player/actor internals). It plays the collapse
// anim and arms gameOverCtx; we re-invoke it to re-kill the corpse after a
// scene-follow warp spawns Link standing. (gPlayerAnim_link_derth_rebirth is not a
// LinkAnimationHeader symbol but a static const char[] OTR path from the asset header
// above — cast at the call site, matching CrawlSpeed.cpp.)
void func_80836448(PlayState* play, Player* thisx, LinkAnimationHeader* anim);
}

// ---------------------------------------------------------------------------
// Local life state
//
// Superset of the wire LifeState enum: it adds the transient DYING sub-state,
// which has NO wire value and rides as ALIVE. While DYING (death anim playing,
// no fairy) we still count as a live authority/partner; we only announce DOWNED
// once the anim parks at the WAIT_GROUND funnel. REVIVING is detected straight
// off gameOverCtx.state's revive range and is disjoint from the DEATH range, so a
// fairy revive can never be routed through DYING/DOWNED/spectate (requirement 4).
// ---------------------------------------------------------------------------
namespace {

enum class LocalLife { Alive, Reviving, Dying, Downed, GameOver };

LocalLife sLife = LocalLife::Alive;

// Set by VB_ADVANCE_TO_GAME_OVER_MENU the frame it blocks the WAIT_GROUND ->
// DELAY_MENU advance (partner still up). This is what distinguishes DOWNED
// (parked AT the funnel) from DYING (anim still playing) — both sit at
// gameOverCtx.state == GAMEOVER_DEATH_WAIT_GROUND. Cleared whenever we leave the
// no-fairy death path (back to INACTIVE, or into the revive range).
bool sBlockedFunnel = false;

// One-shot latch so we queue at most ONE scene-follow position warp per scene
// divergence. ExecutePositionWarp's void-damage suppression uses a single static
// hook id, so two concurrent position warps would clobber it — never queue two.
bool sFollowWarpPending = false;

// Frames left to drive Environment_FadeOutGameOverLights after entering DOWNED,
// undoing any residual game-over dim before the VB fade gate fully took hold.
s32 sUndimFrames = 0;

// Last enemy-authority id we observed, for handoff diagnostics.
uint32_t sLastAuthorityId = UINT32_MAX;

Anchor* A() {
    return Anchor::Instance;
}

// The wire LifeState this local state broadcasts. DYING rides as ALIVE.
u8 WireOf(LocalLife s) {
    switch (s) {
        case LocalLife::Reviving:
            return LIFE_STATE_REVIVING;
        case LocalLife::Downed:
            return LIFE_STATE_DOWNED;
        case LocalLife::GameOver:
            return LIFE_STATE_GAME_OVER;
        case LocalLife::Alive:
        case LocalLife::Dying:
        default:
            return LIFE_STATE_ALIVE;
    }
}

// Any online, save-loaded peer currently alive or mid-fairy-revive. Scene-agnostic
// on purpose: a partner alive in ANOTHER scene still blocks our game-over (we
// spectate / scene-follow them). A disconnected peer is !online, so a partner
// leaving mid-DOWNED flips this false and releases our game-over.
bool PartnerAlive() {
    if (A() == nullptr) {
        return false;
    }
    for (auto& [id, c] : A()->clients) {
        if (c.self || !c.online || !c.isSaveLoaded) {
            continue;
        }
        if (IsClientAlive(c)) {
            return true;
        }
    }
    return false;
}

// The peer we spectate: first online, save-loaded, alive/reviving peer. Re-resolved
// every frame (never cached — Task 2 puppet discipline). Returns a pointer into the
// clients map, valid only within this tick (no map mutation while we hold it).
AnchorClient* FindLivingPeer() {
    if (A() == nullptr) {
        return nullptr;
    }
    for (auto& [id, c] : A()->clients) {
        if (c.self || !c.online || !c.isSaveLoaded) {
            continue;
        }
        if (IsClientAlive(c)) {
            return &c;
        }
    }
    return nullptr;
}

// Edge-triggered state change: log + broadcast the wire value. Broadcasting on a
// same-wire edge (e.g. ALIVE->DYING, both wire-ALIVE) is a harmless idempotent
// resend that keeps myLifeState honest.
void SetLife(LocalLife next, const char* label) {
    if (sLife == next) {
        return;
    }
    SPDLOG_INFO("[LifeSync] local {}", label);
    sLife = next;
    if (A() != nullptr) {
        A()->SendPacket_PlayerLifeState(WireOf(next));
    }
}

// Derive the local life state from the death-flow context this frame.
void UpdateLocalLife(PlayState* play) {
    s16 gs = play->gameOverCtx.state;

    // REVIVING first (fairy path). gameOverCtx enters the revive range the SAME
    // frame func_80836448 consumes a fairy — never passing through any DEATH_*
    // state — so a fairy revive can never fall through to DYING/DOWNED/spectate.
    if (gs >= GAMEOVER_REVIVE_START) {
        sBlockedFunnel = false;
        SetLife(LocalLife::Reviving, "ALIVE->REVIVING");
        return;
    }

    if (gs == GAMEOVER_INACTIVE) {
        // Not in any death flow. Return to ALIVE once actually alive again (revive
        // fade-out done, or post-game-over respawn restored health). The health
        // guard skips the 1-frame window where lethal damage has zeroed health but
        // func_80836448 has not yet armed gameOverCtx.
        sBlockedFunnel = false;
        if (sLife != LocalLife::Alive && gSaveContext.health > 0) {
            SetLife(LocalLife::Alive, "->ALIVE");
        }
        return;
    }

    // No-fairy death path: gs in [DEATH_START(1) .. DEATH_MENU(4)].
    if (gs >= GAMEOVER_DEATH_DELAY_MENU) {
        // The funnel advanced (VB allowed it because the partner is also down):
        // vanilla game-over menu is running / about to run.
        SetLife(LocalLife::GameOver, "DOWNED->GAME_OVER");
        return;
    }

    // gs is DEATH_START or DEATH_WAIT_GROUND: dying, or parked at the funnel.
    if (sBlockedFunnel) {
        SetLife(LocalLife::Downed, "DYING->DOWNED");
    } else {
        SetLife(LocalLife::Dying, "ALIVE->DYING");
    }
}

// Spectate: chase the living peer's puppet. Same-scene -> free-cam follow; different
// scene -> a single position warp to follow them, corpse re-killed after transition.
void UpdateSpectate(PlayState* play) {
    AnchorClient* peer = FindLivingPeer();
    if (peer == nullptr) {
        // No living peer left (died / disconnected). The state machine will move us
        // to GAME_OVER on the next VB advance (PartnerAlive() is now false); nothing
        // to follow in the meantime.
        return;
    }

    if (peer->sceneNum != play->sceneNum) {
        // Living peer is in another scene: follow with one position warp. respawnFlag=1
        // suppresses entrance cutscenes (desired for a corpse follow). The live
        // cross-scene position isn't streamed (PLAYER_UPDATE is same-scene only), so we
        // land via the peer's entrance with their last-known pose; once same-scene the
        // camera re-acquires the puppet.
        if (!sFollowWarpPending) {
            RequestPositionWarp((s16)peer->entranceIndex, 0, peer->posRot);
            sFollowWarpPending = true;
            SPDLOG_INFO("[LifeSync] spectate follow client={} scene={} (warp)", peer->clientId, peer->sceneNum);
        }
        return;
    }
    sFollowWarpPending = false;

    // Same scene: free-cam chase. Only after the death OnePoint subcam has released
    // the active camera back to the main cam (activeCamera == MAIN_CAM); writing the
    // main cam while a subcam is active would be invisible and fight the subcam.
    if (play->activeCamera != MAIN_CAM) {
        return;
    }
    Camera* cam = GET_ACTIVE_CAM(play);
    if (cam == nullptr) {
        return;
    }

    // CAM_SET_FREE0 dispatches to Camera_Unique6, which never overwrites at/eye — so
    // our writes hold across frames. Re-applied every frame: the idempotent
    // same-setting call re-latches the priority bit, keeping bg/water triggers from
    // stealing the setting back while the world runs.
    Camera_ChangeSetting(cam, CAM_SET_FREE0);

    Vec3f p = peer->posRot.pos;
    s16 yaw = peer->posRot.rot.y;
    f32 dist = 140.0f;
    Vec3f targetAt = { p.x, p.y + 40.0f, p.z };
    // Behind the puppet: pos - forward*dist, forward = (sinS(yaw), cosS(yaw)).
    Vec3f targetEye = { p.x - dist * Math_SinS(yaw), p.y + 60.0f, p.z - dist * Math_CosS(yaw) };

    // Lerp toward the target so the handover from the main cam (and puppet motion)
    // reads smoothly rather than snapping each frame.
    f32 t = 0.25f;
    cam->at.x += (targetAt.x - cam->at.x) * t;
    cam->at.y += (targetAt.y - cam->at.y) * t;
    cam->at.z += (targetAt.z - cam->at.z) * t;
    cam->eye.x += (targetEye.x - cam->eye.x) * t;
    cam->eye.y += (targetEye.y - cam->eye.y) * t;
    cam->eye.z += (targetEye.z - cam->eye.z) * t;
    cam->eyeNext = cam->eye;
    cam->roll = 0;
    cam->fov = 60.0f;

    static uint32_t sSpectateLog = 0;
    if ((sSpectateLog++ % 90) == 0) {
        SPDLOG_INFO("[LifeSync] spectate follow client={}", peer->clientId);
    }
}

// Called each frame by the Anchor per-frame dispatcher (registered on OnGameFrameUpdate;
// game.c:356 — after Play_Update/Play_Draw, so it reads this frame's death-flow state and
// the VB flag the player update already set). The dispatcher runs it AFTER the packet-
// queue drain and BEFORE EnemySync/BgmSync so it SETS myLifeState in the same frame those
// consumers READ it (see HookHandlers.cpp for the documented order).
void Tick() {
    if (A() == nullptr || gPlayState == nullptr || !A()->IsSaveLoaded()) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }

    LocalLife prev = sLife;
    UpdateLocalLife(gPlayState);

    if (sLife == LocalLife::Downed && prev != LocalLife::Downed) {
        // Just parked. Undo any death dim for a few frames and reset the follow latch.
        sUndimFrames = 50;
        sFollowWarpPending = false;
    }

    if (sLife == LocalLife::Downed) {
        UpdateSpectate(gPlayState);
        if (sUndimFrames > 0) {
            Environment_FadeOutGameOverLights(gPlayState);
            sUndimFrames--;
        }
    } else {
        sFollowWarpPending = false;
    }

    // Authority-handoff diagnostic (grep token). Reads EnemySync's cached election,
    // which the alive-eligibility change flips within ~1 tick of a DOWNED broadcast.
    uint32_t auth = EnemySync::CurrentAuthorityId();
    if (auth != sLastAuthorityId) {
        SPDLOG_INFO("[LifeSync] authority handoff old={} new={}", sLastAuthorityId, auth);
        sLastAuthorityId = auth;
    }
}

void ResetLocal() {
    sLife = LocalLife::Alive;
    sBlockedFunnel = false;
    sFollowWarpPending = false;
    sUndimFrames = 0;
    sLastAuthorityId = UINT32_MAX;
}

} // namespace

// Per-frame entry point, called by the Anchor per-frame dispatcher in explicit tick
// order (see HookHandlers.cpp). Kept separate from the hook registration so the order
// is guaranteed by call sequence, not by unordered_map iteration.
void CoopLifeSyncTick() {
    Tick();
}

void RegisterCoopLifeSyncHooks(bool isConnected) {
    if (!isConnected) {
        ResetLocal();
    }

    // NOTE: the per-frame state-machine tick is NOT registered here; it is driven by the
    // Anchor per-frame dispatcher (CoopLifeSyncTick) so its order relative to EnemySync /
    // BgmSync is guaranteed. The disconnect path above still runs ResetLocal().

    // VB 1: gate the death-anim funnel WAIT_GROUND -> DELAY_MENU (z_player.c). Block
    // (should=false) while the partner is alive/reviving so the dead player parks in
    // WAIT_GROUND — pauseCtx never reaches 8, so the world keeps running and we spectate
    // instead of freezing both screens. Allow (leave should=true) once the partner is
    // also down/disconnected, so vanilla game-over runs and both respawn together. The
    // funnel re-fires every frame while parked, so the resume is just the predicate
    // flipping. Setting sBlockedFunnel here is what tells the tick we are DOWNED (parked)
    // rather than still DYING (anim playing).
    COND_VB_SHOULD(VB_ADVANCE_TO_GAME_OVER_MENU, isConnected, {
        if (PartnerAlive()) {
            *should = false;
            sBlockedFunnel = true;
        }
    });

    // VB 2: gate GameOver_FadeInLights (z_play.c). Suppress the game-over screen dim
    // only during a SOLO no-fairy death (partner alive, gameOverCtx in the DEATH range),
    // so a spectating player's screen isn't tinted. Left untouched during a fairy revive
    // (revive range) so the vanilla revive lights play, and during a real both-dead game
    // over (partner not alive) so the dim shows as normal.
    COND_VB_SHOULD(VB_FADE_IN_GAME_OVER_LIGHTS, isConnected, {
        if (gPlayState != nullptr && PartnerAlive()) {
            s16 gs = gPlayState->gameOverCtx.state;
            if (gs >= GAMEOVER_DEATH_START && gs < GAMEOVER_REVIVE_START) {
                *should = false;
            }
        }
    });

    // Scene-follow corpse re-kill: after a follow-warp lands us in the peer's scene,
    // Link spawns standing with 0 health. Re-run the death setup so the corpse stays
    // dead (or, if a synced fairy was picked up meanwhile, health>0 skips this and the
    // fairy revive plays — accepted). Guarded to the DOWNED follow so normal respawn
    // transitions are untouched.
    COND_HOOK(OnTransitionEnd, isConnected, [](int16_t sceneNum) {
        if (sLife != LocalLife::Downed || gPlayState == nullptr) {
            return;
        }
        Player* player = GET_PLAYER(gPlayState);
        if (player != nullptr && gSaveContext.health == 0) {
            func_80836448(gPlayState, player, (LinkAnimationHeader*)gPlayerAnim_link_derth_rebirth);
            // T8#3: the re-kill re-arms gameOverCtx at DEATH_START, re-entering the DYING
            // anim. Without this, the transition (INACTIVE mid-load) already cleared
            // sBlockedFunnel, so the next UpdateLocalLife would regress DOWNED->DYING and
            // re-broadcast wire-ALIVE for ~4s — a spurious authority flap while we are
            // actually downed-spectating. We are still parked at the funnel (this handler
            // only runs while DOWNED, i.e. a partner is alive), so re-assert sBlockedFunnel:
            // the tick keeps broadcasting DOWNED and the corpse re-kill stays purely
            // cosmetic, never a wire life transition. The funnel VB still re-evaluates
            // PartnerAlive() every frame, so a both-dead game-over is unaffected.
            sBlockedFunnel = true;
        }
        sFollowWarpPending = false;
    });
}
