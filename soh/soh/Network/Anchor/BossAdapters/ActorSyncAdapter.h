#ifndef NETWORK_ANCHOR_ACTOR_SYNC_ADAPTER_H
#define NETWORK_ANCHOR_ACTOR_SYNC_ADAPTER_H
#ifdef __cplusplus

#include <cstdint>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
}

/**
 * Per-actorId extension point for enemies whose fidelity needs more than
 * pos/rot/jointTable — bosses with update-computed draw params, phase-gated
 * cutscenes, and defeat sequences. Every callback is optional (nullptr).
 */
struct ActorSyncAdapter {
    // Authority: serialize extra draw-relevant fields into the stream entry.
    void (*SerializeExtras)(Actor* actor, nlohmann::json& extras) = nullptr;
    // Mirror: apply streamed extras (each applied frame, while suppressed).
    void (*DeserializeExtras)(Actor* actor, const nlohmann::json& extras) = nullptr;
    // Authority: derive a small phase enum from actor fields (never actionFunc).
    uint8_t (*GetPhase)(Actor* actor) = nullptr;
    // Mirror: react to a streamed phase edge (final streamed pose is already
    // applied). Return true to release the actor from mirroring for good —
    // the local simulation takes over (e.g. defeat cutscene).
    bool (*OnPhaseChange)(Actor* actor, uint8_t fromPhase, uint8_t toPhase) = nullptr;
    // Mirror: gate suppression; false = run the actor locally this frame
    // (e.g. intro cutscenes must run on every client).
    bool (*ShouldMirror)(Actor* actor, uint8_t streamedPhase) = nullptr;
    // Mirror: this actor places its own cylinder colliders somewhere other than
    // world.pos (Phantom Ganon: chest limb / spear tip). ApplyPose and
    // SubmitColliders then leave cylinder centres alone; DeserializeExtras sets them.
    bool customColliderPos = false;
    // colChkInfo.health is not this actor's life (FX actors, mounts, decoys):
    // stream it and announce its spawn regardless of health, and never turn one
    // of its Actor_Kills into an ENEMY_DIED. Optional, per instance.
    bool (*IsHealthless)(Actor* actor) = nullptr;
    // For healthless actors: mirror an authority-side Actor_Kill as ENEMY_DESPAWN
    // (dynamic AND occurrence keys). Leave false for copies every client kills
    // itself (a boss's mount at the end of a locally-run death cutscene).
    bool (*ReplicateKill)(Actor* actor) = nullptr;
    // This adapter already replays the actor's sounds itself (its own CoopSfx /
    // CoopMirrorTick path), so the generic per-actor sfx stream must skip it or
    // the mirror hears everything twice.
    bool ownsSfx = false;
    // The generic mirror step bails on `actor->draw == NULL` and hands the copy
    // back to its local AI, because a deferred-init enemy has no draw until its
    // object bank arrives and suppressing it there leaves an invisible ghost.
    // Some bosses null their OWN draw as part of the fight (Phantom Ganon and his
    // horse do it for the whole painting phase, while they are inside a portrait).
    // For those the heuristic is exactly backwards: it releases the mirror right
    // when the boss is doing its choreography, and the released copy spawns its
    // own decoys and effects alongside the replicated ones. Set this when the
    // adapter knows how to drive a draw-less actor.
    bool mirrorWhileDrawless = false;
};

namespace EnemySync {

const ActorSyncAdapter* GetAdapter(int16_t actorId);
void RegisterAdapter(int16_t actorId, const ActorSyncAdapter& adapter);
// Registers all built-in adapters (Gohma, EnGoma). Idempotent.
void RegisterBuiltInAdapters();

} // namespace EnemySync

#endif // __cplusplus
#endif // NETWORK_ANCHOR_ACTOR_SYNC_ADAPTER_H
