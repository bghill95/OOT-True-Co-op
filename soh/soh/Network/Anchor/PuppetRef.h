#ifndef NETWORK_ANCHOR_PUPPET_REF_H
#define NETWORK_ANCHOR_PUPPET_REF_H
#ifdef __cplusplus

#include <cstdint>
#include "Anchor.h"

extern "C" {
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

namespace PuppetRef {

// A peer's dummy Player is only safe to touch while that client is online with
// an actor that is still updating: Anchor::RefreshClientActors kills every dummy
// player and re-assigns client.player only for live clients, so a peer that
// leaves mid-action strands a pointer to freed arena memory. Same guard as
// PuppetFairy's reap step — never cache the result across frames.
inline Actor* Resolve(uint32_t clientId) {
    auto it = Anchor::Instance->clients.find(clientId);
    if (it == Anchor::Instance->clients.end() || it->second.self || !it->second.online ||
        it->second.player == nullptr || it->second.player->actor.update == NULL) {
        return nullptr;
    }
    return (Actor*)it->second.player;
}

// Where a carried actor sits on a puppet.
//
// First choice is the position the carrier streams in PLAYER_UPDATE: it is the
// real one, computed by their Player code from their animated skeleton, and it
// arrives with the same latency as the puppet pose it belongs to. Every carried
// actor rides differently -- a pot sits above the head, a cucco much lower --
// so a single guessed offset put cuccos through the puppet's chest.
//
// The head-based estimate below is the fallback for the frames before the first
// carry packet lands (and for a carrier whose stream we have not seen yet).
inline Vec3f CarryAnchor(Actor* puppet) {
    Player* p = (Player*)puppet;

    if (Anchor::Instance != nullptr) {
        for (auto& [cid, client] : Anchor::Instance->clients) {
            if ((Actor*)client.player == puppet) {
                if (client.carryActive) {
                    return client.carryPos;
                }
                break;
            }
        }
    }

    Vec3f headPos = p->bodyPartsPos[PLAYER_BODYPART_HEAD];
    Vec3f anchor = puppet->world.pos;
    if (headPos.x != 0.0f || headPos.y != 0.0f || headPos.z != 0.0f) {
        anchor.y = headPos.y + 12.0f;
    } else {
        anchor.y += 38.0f;
    }
    return anchor;
}

// Which way the carried actor faces, from the same stream. On the carrier the
// yaw is re-written every draw by Player_PostLimbDraw (player yaw + the offset
// captured at pickup) AFTER the actor's own update — a cucco randomises its
// whole shape.rot every held update, and the draw is what puts the heading
// back. False before the first carry packet: caller keeps its own guess.
inline bool CarryRotation(Actor* puppet, Vec3s* out) {
    if (Anchor::Instance == nullptr || out == NULL) {
        return false;
    }
    for (auto& [cid, client] : Anchor::Instance->clients) {
        if ((Actor*)client.player == puppet) {
            if (client.carryActive) {
                *out = client.carryRot;
                return true;
            }
            return false;
        }
    }
    return false;
}

// Take `actor` out of our own player's hands if it is what he is holding.
// Mirrors the revoke path in EnBombf_GrowBomb (z_en_bombf.c): every field has to
// be cleared together or Link keeps the carry pose holding nothing.
inline void RevokeLocalHold(Actor* actor) {
    if (gPlayState == NULL) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == NULL || player->heldActor != actor) {
        return;
    }
    player->actor.child = NULL;
    player->heldActor = NULL;
    player->interactRangeActor = NULL;
    player->stateFlags1 &= ~PLAYER_STATE1_CARRYING_ACTOR;
    actor->parent = NULL;
}

} // namespace PuppetRef

#endif
#endif
