#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include <unordered_map>

void RegisterGohmaAdapter();
void RegisterEnGomaAdapter();
void RegisterEnDekubabaAdapter();

namespace EnemySync {

static std::unordered_map<int16_t, ActorSyncAdapter> adapters;

const ActorSyncAdapter* GetAdapter(int16_t actorId) {
    auto it = adapters.find(actorId);
    return it != adapters.end() ? &it->second : nullptr;
}

void RegisterAdapter(int16_t actorId, const ActorSyncAdapter& adapter) {
    adapters[actorId] = adapter;
}

void RegisterBuiltInAdapters() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    RegisterGohmaAdapter();
    RegisterEnGomaAdapter();
    RegisterEnDekubabaAdapter();
}

} // namespace EnemySync
