#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/BgmSync.h"
#include "soh/cvar_prefixes.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "z64.h"
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * BGM SYNC (Task 11 / M4 W3 step 2) — rendezvous-seek + drift heartbeat.
 *
 * Policy (user decision): each player hears the music for the area THEY are in. When
 * both players are in the SAME area the MAIN-BGM track is position-locked so there is
 * no flam/echo. The only offset is entry-time skew (each instance starts the area
 * track when its own player loads the scene). The fix is a RENDEZVOUS-SEEK: the
 * ARRIVING client seeks its MAIN BGM to the RESIDENT peer's scriptCounter, so the
 * resident's music is NEVER restarted.
 *
 * Packets (all gated by the local CVar gRemote.Anchor.BgmSync, default on — NOT by
 * roomState.syncItemsAndFlags):
 *   BGM_POS_REQUEST {sceneNum}                 arriver -> same-scene peers on scene load
 *   BGM_POS {seqId, scriptCounter, sceneNum}   resident's reply -> requester; arriver seeks
 *   BGM_RESTART {seqId, sceneNum}              restart-mode / long-track fallback (both restart)
 *   BGM_STATE {seqId, scriptCounter, sceneNum} quiet ~5s heartbeat; higher clientId re-seeks
 *
 * Seek engine: Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, seqId, targetTick) starts the
 * sequence and fast-forwards scriptCounter to targetTick with notes suppressed — the
 * skipTicks unit is exactly the scriptCounter unit Audio_GetBgmPlaybackInfo reports (see
 * code_800F9280.c). The resident is never asked to seek; only the arriver does.
 *
 * Guards on any seek (arriver / heartbeat): payload seqId == our active MAIN seqId
 * (& 0xFF), our MAIN not NA_BGM_DISABLED, not mid-ocarina, scene unchanged. A day/night
 * or boss-theme mismatch degrades to a graceful no-op — we never seek to a track we
 * aren't playing.
 *
 * Cross-workstream: also hosts the spectate game-over-music suppression (Task 8). While
 * DOWNED with a living partner the vanilla no-fairy death game-over fanfare is replaced
 * by the live scene BGM (Environment_PlaySceneSequence).
 *
 * Cross-version safe: unknown packet types fall through Anchor.cpp's dispatch to a no-op.
 */

namespace {

// ~5s between drift heartbeats at 60fps (frame-counted in the OnGameFrameUpdate tick).
constexpr uint32_t BGM_HEARTBEAT_FRAMES = 300;
// Post-scene-load window (frames) during which the arriver retries BGM_POS_REQUEST until
// it gets a reply and seeks; reset on OnTransitionEnd. BGM starts a few frames into the
// new scene, so we wait through that settle.
constexpr int32_t BGM_REQUEST_WINDOW = 120;
// Frames between retried BGM_POS_REQUEST sends inside the window.
constexpr int32_t BGM_REQUEST_THROTTLE = 8;
// Default drift threshold (~250ms of sequence-player updates) before a heartbeat reseek.
constexpr int32_t BGM_DRIFT_THRESHOLD_DEFAULT = 48;
// Skip-ticks cap (~20 min of updates). A resident older than this would cost an
// unreasonably long synchronous fast-forward, so we fall back to restart-both instead.
constexpr uint32_t BGM_SKIP_TICK_CAP = 216000;

// Rendezvous resolved for the current scene visit (set on seek / restart; blocks
// further requests and duplicate seeks). Reset on OnTransitionEnd.
bool sSeekDone = false;
// Restart-mode one-shot latch for the current scene visit.
bool sRestartBroadcastDone = false;
// Frames left in the post-transition request window.
int32_t sReqFramesLeft = 0;
// Frames until the next retried request send.
int32_t sReqThrottle = 0;
// Frames since the last heartbeat send.
uint32_t sHeartbeatFrames = 0;
// Previous myLifeState, for the DOWNED-entry edge that triggers the spectate restore.
u8 sPrevLifeState = LIFE_STATE_ALIVE;

Anchor* A() {
    return Anchor::Instance;
}

bool BgmSyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("BgmSync"), 1) != 0;
}

// gRemote.Anchor.BgmSyncMode: 0 = rendezvous-seek (default), 1 = restart-both (plan-B,
// no seek helpers — restarts the resident too; use if seek misbehaves with HD audio).
bool RestartMode() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("BgmSyncMode"), 0) == 1;
}

// True while an ocarina / song message mode is active (MSGMODE 0x09..0x1F). We suppress
// reseeks here: the ocarina doesn't restart MAIN BGM and a reseek would fight it.
bool OcarinaActive() {
    if (gPlayState == nullptr) {
        return false;
    }
    u16 m = gPlayState->msgCtx.msgMode;
    return m >= MSGMODE_OCARINA_STARTING && m <= MSGMODE_OCARINA_AWAIT_INPUT;
}

// Compensation ticks (>= 0) added to a seek target — the peer's sample is a fraction of
// an RTT stale, so seeking slightly ahead reduces residual lag. Default 0 (measure on rig).
u32 SeekCompensation() {
    int32_t comp = CVarGetInteger(CVAR_REMOTE_ANCHOR("BgmSyncCompensation"), 0);
    return comp > 0 ? (u32)comp : 0;
}

void ResetLocal() {
    sSeekDone = false;
    sRestartBroadcastDone = false;
    sReqFramesLeft = 0;
    sReqThrottle = 0;
    sHeartbeatFrames = 0;
    sPrevLifeState = LIFE_STATE_ALIVE;
}

// Ticked on OnGameFrameUpdate while connected.
void Tick() {
    Anchor* a = A();
    if (a == nullptr || gPlayState == nullptr || !a->IsSaveLoaded()) {
        return;
    }

    bool enabled = BgmSyncEnabled();

    // --- Spectate game-over-music suppression (Task 8 cross-workstream) ---
    // On the edge into DOWNED (spectating with a living partner — Task 8 only sets
    // DOWNED when the death funnel is blocked because a partner is alive), the vanilla
    // no-fairy death has already run NA_BGM_GAME_OVER + gSaveContext.seqId=DISABLED
    // (z_player.c:3554-3557). Restore the live area BGM (mirrors z_play.c:636-638) so
    // the spectator hears the world, not the game-over jingle. Real both-dead game over
    // reaches LIFE_STATE_GAME_OVER (not DOWNED), so it is left untouched.
    if (enabled) {
        u8 life = a->myLifeState;
        if (life == LIFE_STATE_DOWNED && sPrevLifeState != LIFE_STATE_DOWNED) {
            Environment_PlaySceneSequence(gPlayState);
            gSaveContext.seqId = gPlayState->sequenceCtx.seqId;
            gSaveContext.natureAmbienceId = gPlayState->sequenceCtx.natureAmbienceId;
            SPDLOG_INFO("[Bgm] spectate restore scene bgm (seqId={})", gPlayState->sequenceCtx.seqId & 0xFF);
        } else if (life == LIFE_STATE_GAME_OVER && sPrevLifeState == LIFE_STATE_DOWNED) {
            // Sequential both-dead: this player died FIRST and spectated as DOWNED (so we
            // had replaced the game-over jingle with live scene BGM above), then the
            // partner also died, advancing us DOWNED->GAME_OVER (z_player.c:9525-9526).
            // Vanilla played NA_BGM_GAME_OVER only ONCE, at the initial func_80836448
            // death — which we suppressed — and does NOT re-play it on this transition, so
            // the real game-over screen would otherwise run under scene BGM. Restore the
            // game-over music now, mirroring z_player.c:3554-3557 exactly. This fires only
            // on the DOWNED->GAME_OVER edge; the never-entered-DOWNED path (ALIVE->
            // GAME_OVER, near-simultaneous death) still carries vanilla game-over music and
            // never enters this branch, so there is no double-play. Audio only — the Task 8
            // game-over dim/menu (VB_FADE_IN_GAME_OVER_LIGHTS, un-suppressed once the
            // partner is no longer alive) is untouched.
            func_800F6AB0(0);
            Audio_PlayFanfare(NA_BGM_GAME_OVER);
            gSaveContext.seqId = (u8)NA_BGM_DISABLED;
            gSaveContext.natureAmbienceId = NATURE_ID_DISABLED;
            SPDLOG_INFO("[Bgm] downed->game-over restore game-over music");
        }
        sPrevLifeState = life;
    } else {
        sPrevLifeState = a->myLifeState;
    }

    if (!enabled) {
        return;
    }

    // --- Debug verification gate: locally re-seek current BGM +N ticks (B1 proof) ---
    // Set gRemote.Anchor.BgmSeekTest to N and this one instance fast-forwards its MAIN
    // BGM by N sequence-player ticks — proving the 0x85 skip path works before any
    // networking relies on it. One-shot (cleared after firing).
    int32_t seekTest = CVarGetInteger(CVAR_REMOTE_ANCHOR("BgmSeekTest"), 0);
    if (seekTest != 0) {
        CVarClear(CVAR_REMOTE_ANCHOR("BgmSeekTest"));
        u16 seqId;
        u32 pos = Audio_GetBgmPlaybackInfo(&seqId);
        if (seqId != NA_BGM_DISABLED) {
            u32 target = (seekTest > 0) ? pos + (u32)seekTest : pos;
            Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, seqId & 0xFF, target);
            SPDLOG_INFO("[Bgm] debug local reseek seqId={} from tick={} to tick={} (+{})", seqId & 0xFF, pos,
                        target, seekTest);
        } else {
            SPDLOG_INFO("[Bgm] debug local reseek skipped: no active MAIN bgm");
        }
    }

    bool ocarina = OcarinaActive();

    // --- Arriver rendezvous request (throttled window after scene load) ---
    if (!sSeekDone && sReqFramesLeft > 0 && !ocarina) {
        u16 seqId;
        (void)Audio_GetBgmPlaybackInfo(&seqId);
        if (seqId != NA_BGM_DISABLED) {
            if (RestartMode()) {
                // Restart mode: no seek. Re-lock by restarting our own track from 0 and
                // telling same-scene peers to do the same. NOTE: this DOES restart the
                // resident — the accepted plan-B tradeoff (off by default).
                if (!sRestartBroadcastDone) {
                    a->SendPacket_BgmRestart(seqId & 0xFF);
                    Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, seqId & 0xFF, 0);
                    sRestartBroadcastDone = true;
                    sSeekDone = true;
                    SPDLOG_INFO("[Bgm] restart-mode rendezvous seqId={}", seqId & 0xFF);
                }
            } else if (sReqThrottle == 0) {
                a->SendPacket_BgmPosRequest();
                sReqThrottle = BGM_REQUEST_THROTTLE;
            } else {
                sReqThrottle--;
            }
        }
        sReqFramesLeft--;
    }

    // --- Drift heartbeat (quiet, ~5s) ---
    if (++sHeartbeatFrames >= BGM_HEARTBEAT_FRAMES) {
        sHeartbeatFrames = 0;
        a->SendPacket_BgmState();
    }
}

} // namespace

// Per-frame entry point, called by the Anchor per-frame dispatcher in explicit tick
// order (see HookHandlers.cpp). Runs after CoopLifeSync so it reads this frame's
// myLifeState for the DOWNED-entry spectate restore.
void BgmSyncTick() {
    Tick();
}

// MARK: - Senders

void Anchor::SendPacket_BgmPosRequest() {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BGM_POS_REQUEST;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["quiet"] = true;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::SendPacket_BgmPos(uint32_t targetClientId, u16 seqId, u32 scriptCounter, s16 sceneNum) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BGM_POS;
    payload["seqId"] = seqId;
    payload["scriptCounter"] = scriptCounter;
    payload["sceneNum"] = sceneNum;
    payload["targetClientId"] = targetClientId;
    payload["quiet"] = true;

    SendJsonToRemote(payload);
}

void Anchor::SendPacket_BgmRestart(u16 seqId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BGM_RESTART;
    payload["seqId"] = seqId;
    payload["sceneNum"] = gPlayState->sceneNum;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::SendPacket_BgmState() {
    if (!IsSaveLoaded()) {
        return;
    }

    u16 seqId;
    u32 pos = Audio_GetBgmPlaybackInfo(&seqId);
    if (seqId == NA_BGM_DISABLED) {
        return; // nothing to lock on this frame
    }

    nlohmann::json payload;
    payload["type"] = BGM_STATE;
    payload["seqId"] = seqId;
    payload["scriptCounter"] = pos;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["quiet"] = true;

    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

// MARK: - Handlers

// Resident side: reply with our MAIN BGM position. Does NOT touch our own audio — the
// resident's music is never interrupted (correctness concern #2).
void Anchor::HandlePacket_BgmPosRequest(nlohmann::json payload) {
    if (!BgmSyncEnabled() || !IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    s16 payloadScene = payload["sceneNum"].get<s16>();
    if (payloadScene != gPlayState->sceneNum) {
        return; // only same-scene residents reply
    }

    uint32_t requester = payload["clientId"].get<uint32_t>();

    u16 seqId;
    u32 pos = Audio_GetBgmPlaybackInfo(&seqId);
    if (seqId == NA_BGM_DISABLED) {
        return; // no BGM to report
    }

    SendPacket_BgmPos(requester, seqId, pos, gPlayState->sceneNum);
}

// Arriver side: seek our MAIN BGM to the resident's position (one-shot per scene visit).
void Anchor::HandlePacket_BgmPos(nlohmann::json payload) {
    if (!BgmSyncEnabled() || !IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }
    if (sSeekDone || OcarinaActive()) {
        return;
    }

    u16 payloadSeqId = payload["seqId"].get<u16>();
    u32 payloadPos = payload["scriptCounter"].get<u32>();
    s16 payloadScene = payload["sceneNum"].get<s16>();

    if (payloadScene != gPlayState->sceneNum) {
        return; // scene-unchanged guard
    }

    u16 ownSeqId;
    (void)Audio_GetBgmPlaybackInfo(&ownSeqId);
    if (ownSeqId == NA_BGM_DISABLED) {
        return; // our BGM not up yet — leave the window open to retry
    }
    if ((payloadSeqId & 0xFF) != (ownSeqId & 0xFF)) {
        // Different track (day/night skew, boss theme, still settling). Graceful no-op;
        // don't consume the one-shot so the retry window can still catch a match.
        SPDLOG_INFO("[Bgm] pos ignored: track mismatch peer={} own={}", payloadSeqId & 0xFF, ownSeqId & 0xFF);
        return;
    }

    // Long-track fallback: a very old resident would cost an unreasonable synchronous
    // fast-forward. Re-lock by restarting both from 0 instead (rare; documented).
    if (payloadPos > BGM_SKIP_TICK_CAP) {
        SPDLOG_INFO("[Bgm] peer pos {} over skip cap -> restart fallback", payloadPos);
        SendPacket_BgmRestart(ownSeqId & 0xFF);
        Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, ownSeqId & 0xFF, 0);
        sSeekDone = true;
        return;
    }

    u32 target = payloadPos + SeekCompensation();
    Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, ownSeqId & 0xFF, target);
    sSeekDone = true;
    SPDLOG_INFO("[Bgm] seek to tick={} (peer pos={})", target, payloadPos);
}

// Restart-both: restart our MAIN BGM from 0 if it matches the payload track. Only sent in
// restart mode or the long-track fallback (both accepted restart-the-resident cases).
void Anchor::HandlePacket_BgmRestart(nlohmann::json payload) {
    if (!BgmSyncEnabled() || !IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }
    if (OcarinaActive()) {
        return;
    }

    u16 payloadSeqId = payload["seqId"].get<u16>();

    u16 ownSeqId;
    (void)Audio_GetBgmPlaybackInfo(&ownSeqId);
    if (ownSeqId == NA_BGM_DISABLED || (payloadSeqId & 0xFF) != (ownSeqId & 0xFF)) {
        return;
    }

    Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, ownSeqId & 0xFF, 0);
    SPDLOG_INFO("[Bgm] restart to seqId={}", ownSeqId & 0xFF);
}

// Heartbeat receiver: log drift; if it exceeds threshold, only the HIGHER clientId
// re-seeks (deterministic tie-break — two instances can never ping-pong, concern #3).
void Anchor::HandlePacket_BgmState(nlohmann::json payload) {
    if (!BgmSyncEnabled() || !IsSaveLoaded() || gPlayState == nullptr) {
        return;
    }

    s16 payloadScene = payload["sceneNum"].get<s16>();
    if (payloadScene != gPlayState->sceneNum) {
        return;
    }

    uint32_t senderId = payload["clientId"].get<uint32_t>();
    u16 payloadSeqId = payload["seqId"].get<u16>();
    u32 payloadPos = payload["scriptCounter"].get<u32>();

    u16 ownSeqId;
    u32 ownPos = Audio_GetBgmPlaybackInfo(&ownSeqId);
    if (ownSeqId == NA_BGM_DISABLED || (payloadSeqId & 0xFF) != (ownSeqId & 0xFF)) {
        return; // different / no track — nothing to compare
    }

    s32 drift = (s32)(payloadPos - ownPos);
    SPDLOG_INFO("[Bgm] drift={} ticks (peer={} own={})", drift, payloadPos, ownPos);

    int32_t threshold = CVarGetInteger(CVAR_REMOTE_ANCHOR("BgmDriftThreshold"), BGM_DRIFT_THRESHOLD_DEFAULT);
    bool autoReseek = CVarGetInteger(CVAR_REMOTE_ANCHOR("BgmAutoReseek"), 1) != 0;
    s32 absDrift = drift < 0 ? -drift : drift;

    if (autoReseek && !OcarinaActive() && absDrift > threshold && ownClientId > senderId) {
        u32 target = payloadPos + SeekCompensation();
        if (target <= BGM_SKIP_TICK_CAP) {
            Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, ownSeqId & 0xFF, target);
            SPDLOG_INFO("[Bgm] reseek (higher-id) to tick={}", target);
        }
    }
}

// MARK: - Hook registration

void RegisterBgmSyncHooks(bool isConnected) {
    if (!isConnected) {
        ResetLocal();
    }

    // NOTE: the per-frame driver is NOT registered here; it is driven by the Anchor
    // per-frame dispatcher (BgmSyncTick) so its order relative to CoopLifeSync (the
    // myLifeState producer) is explicit. The disconnect path above still runs ResetLocal().

    // Scene load = a potential rendezvous. Re-open the request window and clear the
    // per-scene latches so only the ARRIVER (the client that just transitioned) asks a
    // resident for its position. The resident never transitions here, so it never
    // requests and is never seeked (concern #2).
    COND_HOOK(OnTransitionEnd, isConnected, [](int16_t sceneNum) {
        sSeekDone = false;
        sRestartBroadcastDone = false;
        sReqFramesLeft = BGM_REQUEST_WINDOW;
        sReqThrottle = 0;
    });
}
