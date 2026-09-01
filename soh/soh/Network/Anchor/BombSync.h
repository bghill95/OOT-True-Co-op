#ifndef NETWORK_ANCHOR_BOMB_SYNC_H
#define NETWORK_ANCHOR_BOMB_SYNC_H
#ifdef __cplusplus

#include <nlohmann/json.hpp>

// Player-placed bomb (EN_BOM) and bombchu (EN_BOM_CHU) replication, same
// design as BombfSync's dynamic bodies: per-client body id, cosmetic neutered
// replicas riding the owner's puppet while carried, THROW kinematics, DETONATE
// resync, GONE reaping. Fuse sparks/smoke/hiss/flash come from `timer` in the
// actors' own updates; a chu replica crawls surfaces by its own deterministic
// physics and is reaped just before its local fuse blows — the blast always
// comes from the owner's real EN_BOM. See BombSync.cpp.
namespace BombSync {

bool SyncEnabled();
void Ingest(const nlohmann::json& payload);
void RegisterHooks(bool isConnected);

} // namespace BombSync

#endif
#endif
