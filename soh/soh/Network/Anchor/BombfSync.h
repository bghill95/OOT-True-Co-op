#ifndef NETWORK_ANCHOR_BOMBF_SYNC_H
#define NETWORK_ANCHOR_BOMBF_SYNC_H
#ifdef __cplusplus

#include <cstdint>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
}

/**
 * BombfSync — Bomb Flowers (En_Bombf) across clients.
 *
 * The scene-placed FLOWER never moves; plucking (or a chain explosion / burning
 * stick) spawns a dynamic BODY actor that the player carries and throws. Two
 * events are replicated, owner -> same-scene peers:
 *
 *  - THROW: sent when a locally-simulated BODY is released (or spawned already
 *    loose by a chain reaction). Carries kinematics + fuse timer; the receiver
 *    spawns a COSMETIC replica that runs local ballistics and explodes on its
 *    own synced fuse. Replicas have all colliders stripped every frame (no AT/
 *    AC/OC): no double damage to enemies (the owner's sim deals it through the
 *    normal enemy-hit pipeline), no pickup, no chain-triggering — switch flags
 *    ride the existing flag sync.
 *
 *  - PLUCK: sent when a local flower resets (bomb taken). The receiver empties
 *    its copy of that flower (flowerBombScale=0, regrow timer) so both screens
 *    show the same bare stem. Flower identity = PackKey(room, ActorListIndex,
 *    id, params) — flowers are always scene-placed.
 *
 * The carry phase itself is not mirrored (the bomb appears on the remote
 * screen at the throw). Regrown flowers keep working: the flower actor is
 * permanent, only its scale/timer cycle.
 */
namespace BombfSync {

void RegisterHooks(bool isConnected);
bool SyncEnabled();
// Applies a received BOMBF_EVENT payload (game thread, after scene guards).
void Ingest(const nlohmann::json& payload);

} // namespace BombfSync

#endif // __cplusplus
#endif // NETWORK_ANCHOR_BOMBF_SYNC_H
