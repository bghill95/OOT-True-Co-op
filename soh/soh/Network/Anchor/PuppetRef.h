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
