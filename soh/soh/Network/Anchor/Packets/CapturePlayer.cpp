#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/PuppetRef.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <unordered_map>

extern "C" {
#include "macros.h"
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * CAPTURE_PLAYER
 *
 * An enemy grabbed a player and is about to move them — a Wallmaster hauling
 * them back to the room entrance, a Gerudo throwing them in a cell.
 *
 * Most of the game's grabbers do not need this. Of the seven actors that write
 * nextEntranceIndex or call Play_TriggerRespawn from an enemy context, five are
 * ACTORCAT_NPC (En_Ge1, En_Ge2, En_Heishi1/3/4): every client simulates its own
 * copy against its own Link, so each capture already targets the right player.
 * Only the two ACTORCAT_ENEMY ones are tracked and simulated by a single
 * machine — En_Wallmas and En_GeldB — and there the grab has to be delivered
 * across the wire or it lands on whoever happens to own the enemy.
 *
 * Deliberately a "dry" capture: we send the destination, not the grab animation.
 * The victim sees the hand close on them (the enemy's pose is streamed like any
 * other) and is then moved. Replicating the held-in-the-air part would mean
 * driving the remote player's own state machine, which is a much bigger change
 * for a second or two of animation.
 */

// Which player each tracked grabber last closed on. Kept here rather than in the
// actor structs so no overlay layout changes: the grab and the actual warp are
// several frames apart (En_GeldB decides in SpinAttack and warps from Draw).
static std::unordered_map<Actor*, uint32_t> sCaptureVictim;

void Anchor::SendPacket_CapturePlayer(u32 clientId, u8 mode, s16 entrance) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = CAPTURE_PLAYER;
    payload["targetClientId"] = clientId;
    payload["mode"] = mode;
    payload["entrance"] = entrance;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_CapturePlayer(nlohmann::json payload) {
    if (gPlayState == NULL) {
        return;
    }
    Player* self = GET_PLAYER(gPlayState);
    if (self == NULL || (self->stateFlags1 & PLAYER_STATE1_DEAD)) {
        return;
    }
    // Already leaving: a second capture landing inside the same transition would
    // stack two destinations and send us to the wrong one.
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        return;
    }

    u8 mode = payload.value("mode", (u8)0);
    s16 entrance = payload.value("entrance", (s16)0);

    if (mode == 0) {
        Play_TriggerRespawn(gPlayState);
    } else {
        gPlayState->nextEntranceIndex = entrance;
        gPlayState->transitionType = TRANS_TYPE_CIRCLE(TCA_STARBURST, TCC_BLACK, TCS_FAST);
        gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    }
    SPDLOG_DEBUG("[Anchor] captured by remote enemy: mode={} entrance={}", mode, entrance);
}

extern "C" {

// Remember who this grabber closed on, so the warp — which may be several frames
// later, and in En_GeldB's case inside Draw — knows where to send it.
//
// Returns 1 when `victim` is a player at all: our own Link, or another
// player's puppet. Callers need that test and cannot do it themselves — a
// puppet is an EN_OE2 in ACTORCAT_NPC, not ACTORCAT_PLAYER, so the obvious
// category check silently excludes exactly the case this exists for.
s32 Anchor_NoteCaptureVictim(Actor* enemy, Actor* victim) {
    if (enemy == NULL || victim == NULL || gPlayState == NULL) {
        return 0;
    }
    if (victim == &GET_PLAYER(gPlayState)->actor) {
        sCaptureVictim.erase(enemy); // ours: the caller warps itself
        return 1;
    }
    if (Anchor::Instance != nullptr) {
        for (auto& [cid, client] : Anchor::Instance->clients) {
            if (!client.self && client.player != nullptr && &client.player->actor == victim) {
                sCaptureVictim[enemy] = cid;
                return 1;
            }
        }
    }
    sCaptureVictim.erase(enemy);
    return 0; // not a player at all
}

// Deliver the capture noted above. Returns 1 when it was sent to a remote player
// (the caller must NOT warp itself), 0 when the victim is our own Link and the
// caller should run the vanilla warp.
s32 Anchor_DeliverCapture(Actor* enemy, u8 mode, s16 entrance) {
    if (Anchor::Instance == nullptr || !Anchor::Instance->isConnected || enemy == NULL) {
        return 0;
    }
    auto it = sCaptureVictim.find(enemy);
    if (it == sCaptureVictim.end()) {
        return 0;
    }
    uint32_t clientId = it->second;
    sCaptureVictim.erase(it);
    // The peer may have left between the grab and the warp.
    if (PuppetRef::Resolve(clientId) == nullptr) {
        return 1; // consumed: do not warp ourselves in their place
    }
    Anchor::Instance->SendPacket_CapturePlayer(clientId, mode, entrance);
    return 1;
}

void Anchor_ForgetCaptureVictim(Actor* enemy) {
    sCaptureVictim.erase(enemy);
}
}
