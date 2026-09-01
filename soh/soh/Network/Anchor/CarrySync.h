#ifndef NETWORK_ANCHOR_CARRY_SYNC_H
#define NETWORK_ANCHOR_CARRY_SYNC_H
#ifdef __cplusplus

#include <cstdint>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
}

/**
 * CarrySync — scene-placed carryable props (rocks, pots) across clients.
 *
 * Unlike bomb-flower bodies (dynamic spawns, see BombfSync), these actors
 * exist on every client with a deterministic key (room + ActorListIndex), so
 * nothing is spawned: the carrier announces transitions and peers drive their
 * OWN copy of the same actor.
 *
 *  - CARRY: the receiver parents its copy to the carrier's puppet and rides it
 *    overhead. Parenting (not suppression) is the trick: the actor's own state
 *    machine advances to its held state natively, so...
 *  - THROW: ...clearing the parent + applying the streamed kinematics makes the
 *    actor's own release logic run — real local physics, real break-on-impact,
 *    real collectible drops on both screens (the EnemySync death discipline).
 *  - GONE: the carrier's copy died in a way the receiver can't replay (killed
 *    while held, voided) — the receiver just kills its copy.
 *
 * A copy being remote-carried submits no interactions the local player could
 * steal mid-carry (it is positioned out of grab flow); once thrown and landed
 * intact, either player can pick it up and the claim transfers naturally with
 * the next CARRY.
 */
namespace CarrySync {

void RegisterHooks(bool isConnected);
bool SyncEnabled();
void Ingest(uint32_t senderClientId, const nlohmann::json& payload);

} // namespace CarrySync

#endif // __cplusplus
#endif // NETWORK_ANCHOR_CARRY_SYNC_H
