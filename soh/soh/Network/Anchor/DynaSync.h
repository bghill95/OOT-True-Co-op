#ifndef NETWORK_ANCHOR_DYNA_SYNC_H
#define NETWORK_ANCHOR_DYNA_SYNC_H
#ifdef __cplusplus

#include <cstdint>
#include <nlohmann/json.hpp>

extern "C" {
#include "z64.h"
}

/**
 * DynaSync
 *
 * Mirrors the MOTION of whitelisted, scene-placed DynaPoly actors (moving
 * platforms, elevators) under the same scene-authority model as EnemySync:
 * the authority runs the actor's own AI and streams pos/rot (DYNA_STATE,
 * entries sent only while moving, plus a periodic keyframe so mirrors stay
 * suppressed while the authority is present). Mirrors suppress the actor's
 * update via ShouldActorUpdate and apply the streamed transform in the
 * actor's own update slot.
 *
 * Collision follows for free: DynaPoly_UpdateBgActorTransforms (last call of
 * Actor_UpdateAll, z_actor.c) re-reads world.pos/shape.rot from every dyna
 * actor AFTER the update pass, so riding/carrying works against the streamed
 * transform on the mirror with no extra plumbing.
 *
 * Identity: PackKey(room, ActorListIndex, actorId, params). Only scene-placed
 * actors are tracked (ActorListIndex >= 0), so no occurrence counting and no
 * dynamic-key machinery is needed.
 *
 * Deliberately NOT tracked: flag-driven background state (Water Temple water
 * level, Forest Temple elevator, tower collapse) — Anchor's flag sync owns
 * those, and SetFlag.cpp already special-cases them.
 *
 * Known phase-A limitation: a mirror's suppressed actionFunc never advances,
 * so on authority handover the platform resumes local AI from its init state
 * at the streamed position. Harmless for cyclic platforms; scripted one-shots
 * may replay locally.
 */
namespace DynaSync {

// Registers/refreshes all DynaSync hooks. Called from Anchor::RegisterHooks with
// the current connection state; hooks unregister (and state resets) when
// disconnected.
void RegisterHooks(bool isConnected);

// Per-frame streaming (authority side). Driven by the Anchor per-frame
// dispatcher in explicit tick order, right after EnemySync::PerFrameTick
// (reuses its authority election).
void PerFrameTick();

// True when the user has dyna sync enabled (CVar, default on).
bool SyncEnabled();

// Applies a received DYNA_STATE payload (called by the packet handler on the
// game thread, after its scene guards). senderClientId decides acceptance:
// the scene authority's entries always apply; other senders' entries apply
// only when marked "own" (push-block claims), which then outrank the
// authority for that key while fresh.
void Ingest(uint32_t senderClientId, const nlohmann::json& payload);

} // namespace DynaSync

#endif // __cplusplus
#endif // NETWORK_ANCHOR_DYNA_SYNC_H
