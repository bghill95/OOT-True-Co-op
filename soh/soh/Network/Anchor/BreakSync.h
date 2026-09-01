#ifndef NETWORK_ANCHOR_BREAK_SYNC_H
#define NETWORK_ANCHOR_BREAK_SYNC_H
#ifdef __cplusplus

#include <nlohmann/json.hpp>

// Breakable-scenery destruction sync: pots, crates, small rocks, grass,
// beehives, bonked trees. The breaker announces; the receiver triggers its own
// copy's REAL vanilla break path (fragments, sounds, drops) by spoofing the
// per-actor break channel. See BreakSync.cpp for the per-actor table.
namespace BreakSync {

bool SyncEnabled();
void Ingest(const nlohmann::json& payload);
void RegisterHooks(bool isConnected);

} // namespace BreakSync

#endif
#endif
