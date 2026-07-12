#include "soh/Network/Anchor/EnemySync.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/JsonConversions.hpp"
#include "soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h"
#include <libultraship/libultraship.h>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstring>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"
#include "soh/ActorDB.h"

extern "C" {
#include "variables.h"
#include "functions.h"

extern PlayState* gPlayState;
}

namespace EnemySync {

// How long a remote-damage expectation lives before being discarded (frames).
// Covers enemies that consume the synthetic hit late (i-frames, knockback states).
static constexpr uint8_t EXPECTED_DAMAGE_LIFETIME = 60;
// Frames to wait for a lethal synthetic hit to play out before force-killing.
static constexpr uint8_t REMOTE_DEATH_GRACE = 30;
// Echo guard for death broadcasts. A latch would be wrong here: enemies like
// En_Karebaba regrow from the same actor instance, so death sync must re-arm.
static constexpr uint8_t DEATH_COOLDOWN_FRAMES = 60;
// How long a destroyed actor's key blocks fuzzy packet resolution (frames).
// Without this, a trailing packet for a just-destroyed enemy can fuzzy-match
// onto a different nearby enemy of the same type and mis-kill it.
static constexpr uint16_t RECENTLY_DEAD_TTL_FRAMES = 100;
// A mirrored enemy whose stream went quiet this long reverts to local AI.
static constexpr uint64_t STREAM_STALE_TICKS = 30;
// Stream cache entries older than this are pruned.
static constexpr uint64_t STREAM_PRUNE_TICKS = 100;
// Upper bound for streamed skeletons (BossGoma is 87 including the root row).
static constexpr int32_t MAX_STREAM_LIMBS = 128;
// Colliders captured per tracked actor. Must cover the worst real enemy: En_St
// (Skulltula) carries 6 ColliderCylinders + 1 ColliderJntSph = 7 distinct
// persistent colliders, all submitted over its life (AC on cyl 0-2, OC on cyl
// 3-5), so a cap of 6 dropped its 7th every frame. 12 leaves headroom; overflow
// still logs a canary.
static constexpr size_t MAX_TRACKED_COLLIDERS = 12;
// High bit marks host-assigned dynamic-spawn keys (bit 63 is provably free:
// PackKey's roomNum passes through uint8_t into bits 48-55).
static constexpr uint64_t DYNAMIC_KEY_BIT = 1ULL << 63;
// Living dynamic spawns re-broadcast on this cadence so mirrors self-heal.
static constexpr uint64_t SPAWN_REBROADCAST_TICKS = 60;

// Collider classes an actor submitted this frame (authority side), streamed so
// the mirror only re-submits classes the real AI is currently exposing.
enum SubmitMaskBit : uint8_t {
    SUBMIT_AT = 1 << 0,
    SUBMIT_AC = 1 << 1,
    SUBMIT_OC = 1 << 2,
};

struct TrackedState {
    uint64_t key;
    Vec3f spawnPos;
    uint8_t prevHealth;
    // Damage we applied on behalf of a remote client; matching local health drops
    // are consumed instead of re-broadcast, preventing echo loops.
    uint8_t expectedRemoteDamage = 0;
    uint8_t expectedRemoteDamageTimer = 0;
    uint8_t pendingKillFrames = 0; // >0: counting down to a force-kill
    uint8_t deathCooldown = 0;     // >0: a death was just sent/received; don't re-broadcast or re-apply
    Collider* acCollider = nullptr; // last AC collider this actor registered

    // --- M2 mirroring ---
    std::vector<Collider*> colliders; // captured at Collider_SetBase* time
    SkelAnime* skelAnime = nullptr;   // captured at SkelAnime_Init* time
    uint8_t submitMask = 0;           // authority: SubmitMaskBits seen this frame
    bool suppressed = false;          // mirror: update currently suppressed
    bool dying = false;               // death handoff latch: never re-suppress
    bool cullForced = false;          // we set ACTOR_FLAG_UPDATE_CULLING_DISABLED
    bool hadUncullFlag = false;       // ...and this was its original state
    uint64_t lastHitReqTick = 0;      // one hit-request per actor per tick
    // --- dynamic spawns / adapters ---
    int16_t spawnActorId = 0;         // identity at spawn (params mutate at runtime)
    uint16_t spawnParams = 0;
    bool dynamicKey = false;          // key is host-assigned (bit 63)
    bool remoteSpawned = false;       // spawned from an ENEMY_SPAWN packet
    bool needsSpawnBroadcast = false; // authority: announce this dynamic spawn
    bool projectile = false;          // whitelisted enemy projectile: fire-and-forget replica, never streamed/mirrored
    uint8_t phase = 0;                // last adapter phase seen/applied
};

// Latest streamed state per enemy key (game-thread only; written by the queued
// ENEMY_STATE handler, read by the ShouldActorUpdate mirror step).
struct RemoteEnemyState {
    uint64_t rxTick = 0;
    Vec3f pos{};
    Vec3s rot{};
    Vec3f focusPos{};
    Vec3f scale{};
    bool hasScale = false;
    uint8_t health = 0;
    uint8_t mask = 0;
    uint8_t phase = 0;
    std::vector<Vec3s> joints;
    // Streamed world-space vertices of each SUBMITTED quad AT/AC collider, 4 Vec3f
    // per quad, in the authority's st.colliders capture order. Empty when this
    // enemy submitted no quad class this frame (the whole Deku Tree roster).
    std::vector<Vec3f> quadVerts;
    nlohmann::json extras;
};

static std::unordered_map<uint64_t, Actor*> keyToActor;
static std::unordered_map<Actor*, TrackedState> tracked;
static std::unordered_map<uint64_t, RemoteEnemyState> remoteStates;

// Keys of recently destroyed tracked actors, aged out in Tick(). Checked before
// the fuzzy fallback so stale identities never resolve onto other enemies.
static std::unordered_map<uint64_t, uint16_t> recentlyDeadKeys;

// Static-key death ledger: remote kills received while the enemy's room was
// unloaded, applied when that room's copy inits (OnEnemyActorInit). Scene-scoped
// (cleared in Reset), no TTL — an enemy stays dead for the whole scene visit.
static std::unordered_map<uint64_t, uint8_t> pendingRemoteKills;

// Occurrence counters for (actorId, params) within the currently loaded room.
// Occurrence order follows the room's setup list, which is identical on every
// client, and resets on room change — so indices match no matter what order each
// player visited rooms in, or whether they re-entered one.
static std::unordered_map<uint32_t, uint16_t> spawnCounts;
static int16_t countersRoomNum = -1;

// Ring of synthetic attacker elements; acHitInfo pointers handed to enemies must
// stay valid until their update consumes them a frame or two later.
static ColliderInfo syntheticToucherPool[8];
static uint8_t syntheticToucherIndex = 0;

// Colliders and skeletons are set up inside the overlay's init func, which runs
// BEFORE the OnActorInit hook — so captures land in these rings first and are
// claimed (by actor pointer / address range) when tracking begins.
struct PendingColliderSetup {
    Actor* actor = nullptr;
    Collider* collider = nullptr;
};
static PendingColliderSetup colliderSetupRing[16];
static uint8_t colliderSetupRingIndex = 0;
static SkelAnime* skelAnimeRing[8];
static uint8_t skelAnimeRingIndex = 0;

static uint64_t tickCounter = 0;
static uint32_t cachedAuthorityId = UINT32_MAX;

// Task 2 (W1 Step 2): per-tick perception-target cache. Holds the clientIds of
// same-scene live puppets; the local player is always an implicit target, so a
// non-empty cache means a second player is present. We cache IDs (not Actor*) and
// re-resolve client.player at read time: puppets are Actor_Kill'd / respawned from
// the network thread (RefreshClientActors) and a killed puppet is freed during the
// game loop's PLAYER/NPC actor pass — which runs BEFORE the ENEMY pass where this
// cache is read — so a cached raw Actor* could dangle. clientIds re-resolve safely.
static std::vector<uint32_t> sPerceptionTargets;

// Dynamic-spawn plumbing: keys assigned before an actor's deferred init has
// run ({key, remoteSpawned}), and the local dynamic id counter.
static std::unordered_map<Actor*, std::pair<uint64_t, bool>> pendingDynamicKeys;
static uint64_t dynamicKeyCounter = 0;
static int16_t lastRosterRoom = -1;

uint64_t PackKey(int16_t roomNum, uint16_t setupIndex, int16_t actorId, uint16_t params) {
    return ((uint64_t)(uint8_t)roomNum << 48) | ((uint64_t)setupIndex << 32) | ((uint64_t)(uint16_t)actorId << 16) |
           (uint64_t)params;
}

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("EnemySync"), 1);
}

bool VerboseSync() {
    // Not "EnemySync.Verbose": the nested-JSON CVar store can't hold a value at
    // "EnemySync" (the M1 master toggle) and an object under it at the same time.
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("EnemySyncVerbose"), 0);
}

bool MirroringEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("EnemySyncMirroring"), 1);
}

uint32_t CurrentAuthorityId() {
    return cachedAuthorityId;
}

bool IsLocalAuthority() {
    return cachedAuthorityId != UINT32_MAX && cachedAuthorityId == Anchor::Instance->ownClientId;
}

bool IsSuppressed(Actor* actor) {
    auto it = tracked.find(actor);
    return it != tracked.end() && it->second.suppressed;
}

bool IsDying(Actor* actor) {
    auto it = tracked.find(actor);
    return it != tracked.end() && it->second.dying;
}

bool IsTrackedEnemy(Actor* actor) {
    return tracked.contains(actor);
}

static bool IsTrackedCategory(Actor* actor) {
    return actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS;
}

// Whitelisted enemy projectiles, replicated via fire-and-forget spawn (never
// streamed): both are deterministic ballistic actors after spawn — fixed
// world.rot.y, constant speedXZ = 10, no mid-flight re-homing — so each machine
// runs its own copy's physics. Both are ACTORCAT_PROP by the time the enemy-sync
// hooks fire (the Deku nut from its InitVars; the Octorok rock via
// Actor_ChangeCategory inside its Init at z_en_okuta.c:156, which runs before the
// OnActorInit/OnActorSpawn hooks), so they slip past IsTrackedCategory and are
// untracked today. The rock shares ACTOR_EN_OKUTA with its live parent Octorok,
// so gate on the already-changed PROP category to catch only the projectile.
static bool IsSyncedProjectile(Actor* actor) {
    return actor->id == ACTOR_EN_NUTSBALL || (actor->id == ACTOR_EN_OKUTA && actor->category == ACTORCAT_PROP);
}

// EN_GOMA params >= 6: hatch debris (>=10) and defeat limb pieces (>=100) are
// short-lived local effects spawned on BOTH clients (limb pieces spawn from
// PostLimbDraw during the locally-run defeat) — tracking or replicating them
// would double-spawn and pollute the stream.
static bool IsTrackingExcluded(Actor* actor) {
    return actor->id == ACTOR_EN_GOMA && (uint16_t)actor->params >= 6;
}

// En_Floormas's split/merge logic manipulates parent/child links across three
// actors with unguarded derefs — mirroring it risks crashes on stream loss.
// It stays permanently local (M1 shared-HP still applies).
static bool IsMirrorBlocked(Actor* actor) {
    return actor->id == ACTOR_EN_FLOORMAS;
}

template <typename F> static void ForEachColliderInfo(Collider* collider, F fn) {
    switch (collider->shape) {
        case COLSHAPE_CYLINDER:
            fn(&((ColliderCylinder*)collider)->info);
            break;
        case COLSHAPE_QUAD:
            fn(&((ColliderQuad*)collider)->info);
            break;
        case COLSHAPE_JNTSPH: {
            ColliderJntSph* jntSph = (ColliderJntSph*)collider;
            for (int32_t i = 0; i < jntSph->count; i++) {
                fn(&jntSph->elements[i].info);
            }
            break;
        }
        case COLSHAPE_TRIS: {
            ColliderTris* tris = (ColliderTris*)collider;
            for (int32_t i = 0; i < tris->count; i++) {
                fn(&tris->elements[i].info);
            }
            break;
        }
    }
}

static void Reset() {
    keyToActor.clear();
    tracked.clear();
    remoteStates.clear();
    spawnCounts.clear();
    recentlyDeadKeys.clear();
    pendingRemoteKills.clear();
    pendingDynamicKeys.clear();
    sPerceptionTargets.clear();
    countersRoomNum = -1;
    lastRosterRoom = -1;
    memset(colliderSetupRing, 0, sizeof(colliderSetupRing));
    memset(skelAnimeRing, 0, sizeof(skelAnimeRing));
}

// ---------------------------------------------------------------------------
// Authority election & culling control
// ---------------------------------------------------------------------------

// Lowest clientId among same-scene, save-loaded clients (self included). When
// requireAlive is set, only ALIVE/REVIVING clients are eligible and our own id is
// seeded only if we are alive — a downed enemy-authority hands off to the survivor
// within ~1 tick. Returns UINT32_MAX when nobody qualifies.
static uint32_t ElectAuthority(bool requireAlive) {
    uint32_t best = UINT32_MAX;
    bool ownAlive =
        Anchor::Instance->myLifeState == LIFE_STATE_ALIVE || Anchor::Instance->myLifeState == LIFE_STATE_REVIVING;
    if (!requireAlive || ownAlive) {
        best = Anchor::Instance->ownClientId;
    }
    for (auto& [clientId, client] : Anchor::Instance->clients) {
        if (client.self || !client.online || !client.isSaveLoaded) {
            continue;
        }
        if (client.sceneNum != gPlayState->sceneNum) {
            continue;
        }
        if (requireAlive && !IsClientAlive(client)) {
            continue;
        }
        best = std::min(best, clientId);
    }
    return best;
}

static uint32_t ComputeAuthorityClientId() {
    if (Anchor::Instance == nullptr || !Anchor::Instance->IsSaveLoaded() || gPlayState == NULL) {
        return UINT32_MAX;
    }
    // Prefer the alive-eligible election so a dead player never keeps enemy authority.
    uint32_t alive = ElectAuthority(true);
    if (alive != UINT32_MAX) {
        return alive;
    }
    // Nobody in-scene is alive (both dead): fall back to the aliveness-agnostic
    // election so the authority id stays put — no flapping while the shared
    // game-over sequence runs. The 30-tick stale stream fallback remains the crash net.
    return ElectAuthority(false);
}

static bool AnySameScenePeer() {
    if (Anchor::Instance == nullptr || gPlayState == NULL) {
        return false;
    }
    for (auto& [clientId, client] : Anchor::Instance->clients) {
        if (!client.self && client.online && client.isSaveLoaded && client.sceneNum == gPlayState->sceneNum) {
            return true;
        }
    }
    return false;
}

// The culling gate in Actor_UpdateAll sits BEFORE the ShouldActorUpdate hook, so
// both the authority (AI for enemies near the REMOTE player) and the mirror (the
// hook must fire to apply the stream) need update-culling disabled while syncing.
static void ForceUncull(Actor* actor, TrackedState& st) {
    if (!st.cullForced) {
        st.hadUncullFlag = (actor->flags & ACTOR_FLAG_UPDATE_CULLING_DISABLED) != 0;
        st.cullForced = true;
    }
    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
}

static void RestoreCull(Actor* actor, TrackedState& st) {
    if (st.cullForced && !st.hadUncullFlag) {
        actor->flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
    }
    st.cullForced = false;
}

// ---------------------------------------------------------------------------
// Capture: colliders + skeleton (setup-time, so mirrors have them on frame one)
// ---------------------------------------------------------------------------

static void AddCollider(Actor* actor, TrackedState& st, Collider* collider) {
    if (std::find(st.colliders.begin(), st.colliders.end(), collider) != st.colliders.end()) {
        return;
    }
    if (st.colliders.size() >= MAX_TRACKED_COLLIDERS) {
        SPDLOG_WARN("[EnemySync] Collider overflow id={} key={:#x} (cap {})", actor->id, st.key,
                    MAX_TRACKED_COLLIDERS);
        return;
    }
    st.colliders.push_back(collider);
}

static void OnColliderSetup(Actor* actor, Collider* collider) {
    if (actor == NULL || collider == NULL || !IsTrackedCategory(actor)) {
        return;
    }
    auto it = tracked.find(actor);
    if (it != tracked.end()) {
        AddCollider(actor, it->second, collider);
        return;
    }
    // Not tracked yet — likely inside the overlay init, before OnActorInit.
    colliderSetupRing[colliderSetupRingIndex] = { actor, collider };
    colliderSetupRingIndex = (colliderSetupRingIndex + 1) % 16;
}

// Ring hygiene: a freed actor's ZeldaArena block can be reused by the very next
// spawn, which would then claim stale Collider*/SkelAnime* pointers.
static void ScrubPendingCaptures(Actor* actor) {
    for (auto& pending : colliderSetupRing) {
        if (pending.actor == actor) {
            pending = {};
        }
    }
    size_t instanceSize = ActorDB::Instance->RetrieveEntry(actor->id).entry.instanceSize;
    uintptr_t base = (uintptr_t)actor;
    for (auto& skel : skelAnimeRing) {
        if (skel != nullptr && (uintptr_t)skel >= base && (uintptr_t)skel < base + instanceSize) {
            skel = nullptr;
        }
    }
}

static void OnSkelAnimeInitCapture(SkelAnime* skelAnime) {
    skelAnimeRing[skelAnimeRingIndex] = skelAnime;
    skelAnimeRingIndex = (skelAnimeRingIndex + 1) % 8;
}

static void ClaimPendingCaptures(Actor* actor, TrackedState& st) {
    for (auto& pending : colliderSetupRing) {
        if (pending.actor == actor && pending.collider != nullptr) {
            AddCollider(actor, st, pending.collider);
            pending = {};
        }
    }
    size_t instanceSize = ActorDB::Instance->RetrieveEntry(actor->id).entry.instanceSize;
    uintptr_t base = (uintptr_t)actor;
    for (auto& skel : skelAnimeRing) {
        if (skel == nullptr) {
            continue;
        }
        uintptr_t addr = (uintptr_t)skel;
        if (addr >= base && addr < base + instanceSize) {
            if (st.skelAnime == nullptr) {
                st.skelAnime = skel;
            }
            skel = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// Packet resolution & M1 hit/death application
// ---------------------------------------------------------------------------

Actor* FindActorForPacket(uint64_t key, int16_t actorId, Vec3f homePos) {
    auto it = keyToActor.find(key);
    if (it != keyToActor.end()) {
        return it->second;
    }

    // A packet for a just-destroyed enemy must never resolve onto a different
    // actor — trailing hits/deaths after a destroy are normal, not a desync.
    if (recentlyDeadKeys.contains(key)) {
        ESYNC_LOG("[EnemySync] DROP packet for recently-dead key={:#x} id={}", key, actorId);
        return nullptr;
    }

    // Dynamic keys are exact by construction — a miss just means the spawn
    // packet hasn't been processed (or the enemy already despawned).
    if (key & DYNAMIC_KEY_BIT) {
        ESYNC_LOG("[EnemySync] Unmapped dynamic key={:#x} id={}", key, actorId);
        return nullptr;
    }

    uint16_t params = (uint16_t)(key & 0xFFFF);
    int16_t roomNum = (int16_t)(uint8_t)((key >> 48) & 0xFF);

    // Fuzzy fallback: same actor type and params, nearest spawn position. A hit
    // here means spawn order diverged between clients — log it as a canary.
    Actor* best = nullptr;
    float bestDistSq = 250.0f * 250.0f; // beyond this it's likely a different enemy
    for (auto& [actor, state] : tracked) {
        // Compare against spawn-time params from the key: many actors mutate
        // actor->params at runtime (En_Karebaba uses it as a state/timer field).
        if (state.dynamicKey || actor->id != actorId || (uint16_t)(state.key & 0xFFFF) != params) {
            continue;
        }
        float dx = state.spawnPos.x - homePos.x;
        float dy = state.spawnPos.y - homePos.y;
        float dz = state.spawnPos.z - homePos.z;
        float distSq = dx * dx + dy * dy + dz * dz;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = actor;
        }
    }

    if (best != nullptr) {
        SPDLOG_WARN("[EnemySync] Fuzzy-matched enemy actorId={} params={} (exact key miss — spawn order desync?)",
                    actorId, params);
    } else if (gPlayState != NULL && gPlayState->roomCtx.curRoom.num == roomNum) {
        // Only a desync signal when we actually have that room loaded; enemies in
        // other rooms legitimately don't exist locally.
        SPDLOG_WARN("[EnemySync] No match for enemy key={:#x} actorId={} params={} room={}", key, actorId, params,
                    roomNum);
    }
    return best;
}

void ApplyRemoteHit(Actor* actor, uint8_t damage, uint32_t dmgFlags, Vec3s hitPos, uint8_t remoteHealth,
                    Actor* attacker) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        SPDLOG_WARN("[EnemySync] ApplyRemoteHit: resolved actor not in tracked map!");
        return;
    }
    TrackedState& state = it->second;

    ESYNC_LOG("[EnemySync] HIT apply id={} dmg={} viaCollider={}", actor->id, damage, state.acCollider != nullptr);

    // Reconcile drift: local health should never exceed the remote pre-hit pool
    uint8_t remotePreHit = (uint8_t)std::min<int>(remoteHealth + damage, 255);
    if (actor->colChkInfo.health > remotePreHit) {
        actor->colChkInfo.health = remotePreHit;
        state.prevHealth = remotePreHit;
    }

    ColliderInfo* synthetic = &syntheticToucherPool[syntheticToucherIndex];
    syntheticToucherIndex = (syntheticToucherIndex + 1) % 8;
    memset(synthetic, 0, sizeof(ColliderInfo));
    synthetic->toucher.dmgFlags = dmgFlags != 0 ? dmgFlags : DMG_SLASH;
    synthetic->toucher.damage = damage;
    synthetic->toucherFlags = TOUCH_ON | TOUCH_HIT;

    // Prime the same state a real collision-check hit would have left behind,
    // then let the enemy's own update consume it (hurt anim, knockback, death).
    // Same trick Anchor already uses for destructible walls in HookHandlers.cpp.
    actor->colChkInfo.damage = damage;

    if (state.acCollider != nullptr) {
        Collider* col = state.acCollider;
        col->acFlags |= AC_HIT;
        col->ac = attacker;
        ForEachColliderInfo(col, [&](ColliderInfo* info) {
            info->bumperFlags |= BUMP_HIT;
            info->acHitInfo = synthetic;
            info->bumper.hitPos = hitPos;
        });
    } else {
        // No collider seen yet (e.g. custom boss damage paths): apply directly
        actor->colChkInfo.health = (actor->colChkInfo.health > damage) ? actor->colChkInfo.health - damage : 0;
        state.prevHealth = actor->colChkInfo.health;
        return;
    }

    state.expectedRemoteDamage = (uint8_t)std::min<int>(state.expectedRemoteDamage + damage, 255);
    state.expectedRemoteDamageTimer = EXPECTED_DAMAGE_LIFETIME;
}

void ApplyRemoteDeath(Actor* actor, Actor* attacker, bool permanent) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;
    ESYNC_LOG("[EnemySync] DEATH apply id={} cooldown={} alive={} permanent={}", actor->id, state.deathCooldown,
              actor->update != NULL, permanent);
    if (state.deathCooldown > 0) {
        return; // already died locally (or handled a remote death) within the echo window
    }
    if (actor->update == NULL) {
        state.deathCooldown = DEATH_COOLDOWN_FRAMES;
        return; // already dead locally
    }
    state.deathCooldown = DEATH_COOLDOWN_FRAMES; // local death reaction must not re-broadcast

    // The dying latch engages only for deaths we actually apply (echoes and
    // duplicates above must not strand a live enemy unculled/unmirrored).
    ReleaseForDeath(actor);

    uint8_t health = actor->colChkInfo.health;
    Vec3s hitPos = { (int16_t)actor->world.pos.x, (int16_t)actor->world.pos.y, (int16_t)actor->world.pos.z };
    ApplyRemoteHit(actor, health > 0 ? health : 1, 0, hitPos, 0, attacker);
    // Only Actor_Kill deaths force-kill as a last resort. Defeat-hook enemies
    // (En_Karebaba regrowers) consume hits in few action states — force-killing
    // one that never consumed the synthetic hit would permanently despawn an
    // enemy the sender's copy regrows.
    if (permanent) {
        state.pendingKillFrames = REMOTE_DEATH_GRACE;
    }
}

// ---------------------------------------------------------------------------
// M2: suppression, stream application, hit forwarding
// ---------------------------------------------------------------------------

static void BeginSuppression(Actor* actor, TrackedState& st) {
    st.suppressed = true;
    ForceUncull(actor, st);
    // Pin M1's HP-drop detector to the streamed pool so releasing suppression
    // never manufactures a phantom "local" hit out of streamed damage.
    st.prevHealth = actor->colChkInfo.health;
    st.expectedRemoteDamage = 0;
    st.expectedRemoteDamageTimer = 0;
    ESYNC_LOG("[EnemySync] MIRROR suppress=on key={:#x} id={}", st.key, actor->id);
}

static void EndSuppression(Actor* actor, TrackedState& st, const char* reason) {
    st.suppressed = false;
    st.prevHealth = actor->colChkInfo.health;
    st.expectedRemoteDamage = 0;
    st.expectedRemoteDamageTimer = 0;
    ESYNC_LOG("[EnemySync] MIRROR suppress=off key={:#x} reason={}", st.key, reason);
}

void ReleaseForDeath(Actor* actor) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& st = it->second;
    st.dying = true;
    if (st.suppressed) {
        EndSuppression(actor, st, "death");
    }
    remoteStates.erase(st.key);
    // Keep update culling disabled: the death must play out even if this enemy
    // is far from the local player (the other player killed it across the room).
    ForceUncull(actor, st);
}

// Reads this frame's collision results (CollisionCheck_Damage already resolved
// the damage table, even for suppressed actors) and forwards them to the
// authority as a hit request. Local feedback: red damage flash; the hit spark
// and SFX already played via the local collision pass.
static void DetectAndForwardLocalHits(Actor* actor, TrackedState& st) {
    if (st.lastHitReqTick == tickCounter) {
        return;
    }

    uint32_t dmgFlags = 0;
    Vec3s hitPos = { (int16_t)actor->world.pos.x, (int16_t)actor->world.pos.y, (int16_t)actor->world.pos.z };
    bool hit = false;

    auto scan = [&](Collider* col) {
        if (col == nullptr || col->actor != actor || !(col->acFlags & AC_HIT)) {
            return;
        }
        hit = true;
        ForEachColliderInfo(col, [&](ColliderInfo* info) {
            if (dmgFlags == 0 && (info->bumperFlags & BUMP_HIT) && info->acHitInfo != NULL) {
                dmgFlags = info->acHitInfo->toucher.dmgFlags;
                hitPos = info->bumper.hitPos;
            }
            info->bumperFlags &= ~BUMP_HIT;
        });
        col->acFlags &= ~AC_HIT; // consume: one swing must produce one request
    };
    for (Collider* col : st.colliders) {
        scan(col);
    }
    scan(st.acCollider);

    if (!hit) {
        return;
    }
    st.lastHitReqTick = tickCounter;

    uint8_t damage = actor->colChkInfo.damage;
    actor->colChkInfo.damage = 0;

    Anchor::Instance->SendPacket_EnemyHitRequest(actor, st.key, damage, dmgFlags, hitPos);

    // Immediate local reaction while the authority's verdict streams back.
    Actor_SetColorFilter(actor, 0x4000, 255, 0, 8);
}

static void SubmitColliders(Actor* actor, TrackedState& st, RemoteEnemyState& r) {
    uint8_t mask = r.mask;
    if (gPlayState == NULL || mask == 0) {
        return;
    }
    // Cursor into r.quadVerts (4 Vec3f per submitted quad). The authority appended
    // one 4-vertex set per quad that passed the AT/AC predicate below, in this same
    // st.colliders order, so the nth qualifying quad here pairs with the nth set.
    size_t quadCursor = 0;
    for (Collider* col : st.colliders) {
        if (col == nullptr || col->actor != actor) {
            continue;
        }
        if (col->shape == COLSHAPE_CYLINDER) {
            // Repositioned from the streamed world.pos (ApplyPose ran first) — accurate.
            Collider_UpdateCylinder(actor, (ColliderCylinder*)col);
        } else if (col->shape == COLSHAPE_QUAD) {
            // Quad AT/AC vertices are AI-set; the suppressed mirror never recomputes
            // them, so drive them from the stream. A quad that qualifies for AT/AC
            // submission but has NO fresh verts this frame is skipped entirely —
            // stale quad geometry lands false hits at old positions, worse than a
            // missing collider. (Same predicate the authority used to decide which
            // quads to stream, so cursor and quad iteration stay in lockstep.)
            bool quadAtAc = ((mask & SUBMIT_AT) && (col->atFlags & AT_ON)) ||
                            ((mask & SUBMIT_AC) && (col->acFlags & AC_ON));
            if (quadAtAc) {
                if (quadCursor + 4 <= r.quadVerts.size()) {
                    Collider_SetQuadVertices((ColliderQuad*)col, &r.quadVerts[quadCursor],
                                             &r.quadVerts[quadCursor + 1], &r.quadVerts[quadCursor + 2],
                                             &r.quadVerts[quadCursor + 3]);
                    quadCursor += 4;
                } else {
                    continue; // qualifies but no fresh verts: never submit stale quad geometry
                }
            }
        } else if (col->shape == COLSHAPE_TRIS) {
            // Tris vertices are likewise AI-set and NOT streamed this milestone
            // (out of scope) — submitting them would land false hits at stale
            // positions. Skip AT/AC/OC submission entirely; log once as a canary so
            // a later Tris-bearing enemy surfaces instead of silently mis-hitting.
            static bool sTrisCanary = false;
            if (!sTrisCanary) {
                sTrisCanary = true;
                SPDLOG_WARN("[EnemySync] MIRROR skipping COLSHAPE_TRIS submission id={} key={:#x} "
                            "(AI-set tris geometry not streamed)",
                            actor->id, st.key);
            }
            continue;
        }
        // JntSph world spheres refresh in the enemy's PostLimbDraw (still runs on
        // suppressed mirrors); cylinders repositioned above; quads carry fresh
        // streamed verts. Exposure is bounded by the authority's mask (only frames
        // the real AI submitted that class).
        if ((mask & SUBMIT_AC) && (col->acFlags & AC_ON)) {
            CollisionCheck_SetAC(gPlayState, &gPlayState->colChkCtx, col);
        }
        if ((mask & SUBMIT_AT) && (col->atFlags & AT_ON)) {
            CollisionCheck_SetAT(gPlayState, &gPlayState->colChkCtx, col);
        }
        if ((mask & SUBMIT_OC) && (col->ocFlags1 & OC1_ON)) {
            CollisionCheck_SetOC(gPlayState, &gPlayState->colChkCtx, col);
        }
    }
}

static void ApplyPose(Actor* actor, TrackedState& st, RemoteEnemyState& r) {
    actor->world.pos = r.pos;
    actor->shape.rot = r.rot;
    actor->focus.pos = r.focusPos;
    if (r.hasScale) {
        actor->scale = r.scale;
    }
    actor->colChkInfo.health = r.health;
    st.prevHealth = r.health;

    if (st.skelAnime != nullptr && !r.joints.empty()) {
        if ((int32_t)r.joints.size() == st.skelAnime->limbCount && st.skelAnime->jointTable != nullptr) {
            memcpy(st.skelAnime->jointTable, r.joints.data(), r.joints.size() * sizeof(Vec3s));
        } else {
            SPDLOG_WARN("[EnemySync] limb mismatch key={:#x} local={} remote={}", st.key,
                        st.skelAnime != nullptr ? st.skelAnime->limbCount : -1, r.joints.size());
        }
    }
}

// Task 2 (W1 Step 2): authority-side nearest-player perception.
//
// The engine writes each actor's xzDistToPlayer / yDistToPlayer / xyzDistToPlayerSq /
// yawTowardsPlayer against the LOCAL player only (z_actor.c:2666-2670), immediately
// before the ShouldActorUpdate hook fires. So an authority-run enemy never notices a
// remote player standing next to its puppet. Re-point those four fields at whichever
// perception target (the local player or a same-scene puppet) is nearest, matching
// the engine's exact formulas and sign conventions so every downstream facing / aggro
// / distance check (Actor_IsFacingPlayer, Deku Baba emerge, Octorok appear window, …)
// reads the intended player.
//
// Targets are re-resolved from clientId here (not from a cached Actor*): a puppet can
// be killed and freed on an earlier actor-category pass of this same frame, so a
// cross-frame raw pointer could dangle. Each clientId re-resolves to the live puppet
// and is re-validated with the same liveness predicate used to build the cache.
static void OverridePlayerPerception(Actor* actor) {
    if (gPlayState == NULL || Anchor::Instance == nullptr) {
        return;
    }
    Player* localPlayer = GET_PLAYER(gPlayState);
    if (localPlayer == NULL) {
        return;
    }

    // Baseline is the local player — identical to what the engine just computed.
    Actor* best = &localPlayer->actor;
    f32 bestXZ = Actor_WorldDistXZToActor(actor, best);
    f32 bestY = Actor_HeightDiff(actor, best);
    f32 bestSq = bestXZ * bestXZ + bestY * bestY; // == SQ(xz) + SQ(y), engine's xyzDistToPlayerSq

    for (uint32_t clientId : sPerceptionTargets) {
        auto it = Anchor::Instance->clients.find(clientId);
        if (it == Anchor::Instance->clients.end()) {
            continue;
        }
        AnchorClient& c = it->second;
        // Re-validate: the cache was built last tick; a puppet may have parked,
        // died, changed scene, or been despawned since.
        if (!c.online || !c.isSaveLoaded || c.sceneNum != gPlayState->sceneNum || c.player == nullptr ||
            c.player->actor.update == NULL || (c.stateFlags1 & PLAYER_STATE1_DEAD)) {
            continue;
        }
        Actor* cand = &c.player->actor;
        f32 xz = Actor_WorldDistXZToActor(actor, cand);
        f32 y = Actor_HeightDiff(actor, cand);
        f32 sq = xz * xz + y * y;
        if (sq < bestSq) {
            bestSq = sq;
            bestXZ = xz;
            bestY = y;
            best = cand;
        }
    }

    // Write exactly the four fields the engine derives at z_actor.c:2666-2670, with
    // the engine's own helpers so signs/composition match bit-for-bit:
    //   xzDistToPlayer    = Actor_WorldDistXZToActor(actor, target)   (>= 0)
    //   yDistToPlayer     = Actor_HeightDiff(actor, target)           (target.y - actor.y)
    //   xyzDistToPlayerSq = SQ(xzDistToPlayer) + SQ(yDistToPlayer)
    //   yawTowardsPlayer  = Actor_WorldYawTowardActor(actor, target)
    actor->xzDistToPlayer = bestXZ;
    actor->yDistToPlayer = bestY;
    actor->xyzDistToPlayerSq = bestSq;
    actor->yawTowardsPlayer = Actor_WorldYawTowardActor(actor, best);
}

// The mirror step. Runs in the actor's own update slot (ShouldActorUpdate hook):
// after this frame's collision checks (so AC_HIT results are readable) and
// before draw (so streamed positions always win over OC pushback).
static void OnShouldEnemyUpdate(Actor* actor, bool* should) {
    if (!SyncEnabled() || !MirroringEnabled() || gPlayState == NULL) {
        return;
    }
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& st = it->second;

    // Step 3: whitelisted projectiles are fire-and-forget — never suppressed, never
    // mirrored. Each machine runs its own deterministic copy's physics to completion
    // and kills it locally, so leave *should untouched (local AI) and bail before any
    // perception/suppression logic below.
    if (st.projectile) {
        return;
    }

    // Task 2 (W1 Step 2): while we own this enemy's AI, let it perceive the nearest
    // player (local or a same-scene puppet) instead of only the local Link. This
    // MUST run before the became-authority early-return below — being the authority
    // is exactly the case we care about. Guards: authority only; at least one puppet
    // present (implicit local + a cached puppet ⇒ the design's targets.size() > 1);
    // not mid death-handoff; not a projectile (belt-and-suspenders — the early return
    // above already excludes them); never the permanently-local Floormaster.
    if (IsLocalAuthority() && !sPerceptionTargets.empty() && !st.dying && !st.projectile && !IsMirrorBlocked(actor)) {
        OverridePlayerPerception(actor);
    }

    if (cachedAuthorityId == UINT32_MAX || IsLocalAuthority()) {
        // Handover-to-self: drop any latched suppression here, or hits stay
        // silently dropped forever (EnemyHit/EnemyHitRequest gate on it).
        if (st.suppressed) {
            EndSuppression(actor, st, "became-authority");
        }
        return; // we own the AI (or nothing is elected): run it
    }
    if (st.dying || IsMirrorBlocked(actor)) {
        return; // death handoff / blocklisted: local simulation runs
    }

    auto rs = remoteStates.find(st.key);
    if (rs == remoteStates.end() || (tickCounter - rs->second.rxTick) > STREAM_STALE_TICKS) {
        if (st.suppressed) {
            EndSuppression(actor, st, "stale");
        }
        return; // no fresh stream: local AI (M1 behavior), never suppress into silence
    }
    RemoteEnemyState& r = rs->second;

    const ActorSyncAdapter* adapter = GetAdapter(actor->id);

    // Phase edges fire before the ShouldMirror gate: the defeat edge arrives with
    // a phase the gate would reject, but the handoff must still run.
    if (adapter != nullptr && r.phase != st.phase) {
        uint8_t fromPhase = st.phase;
        st.phase = r.phase;
        if (adapter->OnPhaseChange != nullptr) {
            ApplyPose(actor, st, r); // hand off from the final streamed pose
            if (adapter->OnPhaseChange(actor, fromPhase, r.phase)) {
                st.dying = true;
                if (st.suppressed) {
                    EndSuppression(actor, st, "phase");
                }
                remoteStates.erase(st.key);
                ForceUncull(actor, st); // the local sequence must play even at distance
                return;                 // leave *should true: local simulation starts now
            }
        }
    }

    if (adapter != nullptr && adapter->ShouldMirror != nullptr && !adapter->ShouldMirror(actor, r.phase)) {
        if (st.suppressed) {
            EndSuppression(actor, st, "phase-gate");
        }
        return; // e.g. intro cutscene: runs locally on every client
    }

    if (!st.suppressed) {
        BeginSuppression(actor, st);
    }

    DetectAndForwardLocalHits(actor, st);

    ApplyPose(actor, st, r);
    if (adapter != nullptr && adapter->DeserializeExtras != nullptr && !r.extras.is_null()) {
        adapter->DeserializeExtras(actor, r.extras);
    }

    SubmitColliders(actor, st, r);

    *should = false;
}

// ---------------------------------------------------------------------------
// M2: authority stream (snapshot + send, once per game frame)
// ---------------------------------------------------------------------------

static nlohmann::json SnapshotEnemy(Actor* actor, TrackedState& st) {
    nlohmann::json e;
    e["key"] = st.key;
    e["pos"] = actor->world.pos;
    e["rot"] = actor->shape.rot;
    e["fpos"] = actor->focus.pos;
    e["hp"] = actor->colChkInfo.health;
    // actor.scale is animated in Update (e.g. Deku Baba emerge 0.5x->1x); the
    // suppressed mirror never recomputes it, so the head skeleton would inherit
    // the frozen buried scale. Base-Actor field → base stream, applied to every
    // mirrored enemy.
    e["scale"] = actor->scale;
    e["mask"] = st.submitMask;
    SkelAnime* skel = st.skelAnime;
    if (skel != nullptr && skel->jointTable != nullptr && skel->limbCount > 0 && skel->limbCount <= MAX_STREAM_LIMBS) {
        std::vector<int> jt;
        jt.reserve((size_t)skel->limbCount * 3);
        for (int32_t i = 0; i < skel->limbCount; i++) {
            jt.push_back(skel->jointTable[i].x);
            jt.push_back(skel->jointTable[i].y);
            jt.push_back(skel->jointTable[i].z);
        }
        e["jt"] = jt;
    }
    // Quad AT/AC vertex streaming: the mirror's suppressed AI never recomputes these
    // AI-set world-space vertices, so stream the four dim.quad[] Vec3fs (12 floats)
    // of every quad this actor submitted for AT/AC this frame. Cylinders reposition
    // from world.pos and JntSph spheres refresh in PostLimbDraw, so only quads are
    // stale on the mirror. Capture order == SubmitColliders' st.colliders iteration
    // on both sides, and the predicate here mirrors SubmitColliders' exactly, so the
    // nth streamed set pairs with the nth qualifying quad on the mirror. Absent for
    // the whole Deku Tree roster (no quad AT/AC enemies).
    if (st.submitMask & (SUBMIT_AT | SUBMIT_AC)) {
        std::vector<float> qv;
        for (Collider* col : st.colliders) {
            if (col == nullptr || col->actor != actor || col->shape != COLSHAPE_QUAD) {
                continue;
            }
            bool quadAtAc = ((st.submitMask & SUBMIT_AT) && (col->atFlags & AT_ON)) ||
                            ((st.submitMask & SUBMIT_AC) && (col->acFlags & AC_ON));
            if (!quadAtAc) {
                continue;
            }
            ColliderQuad* quad = (ColliderQuad*)col;
            for (int32_t v = 0; v < 4; v++) {
                qv.push_back(quad->dim.quad[v].x);
                qv.push_back(quad->dim.quad[v].y);
                qv.push_back(quad->dim.quad[v].z);
            }
        }
        if (!qv.empty()) {
            e["qv"] = qv;
        }
    }
    const ActorSyncAdapter* adapter = GetAdapter(actor->id);
    if (adapter != nullptr) {
        if (adapter->GetPhase != nullptr) {
            st.phase = adapter->GetPhase(actor);
            e["phase"] = st.phase;
        }
        if (adapter->SerializeExtras != nullptr) {
            nlohmann::json extras;
            adapter->SerializeExtras(actor, extras);
            e["extras"] = extras;
        }
    }
    return e;
}

void IngestEnemyState(const nlohmann::json& payload) {
    try {
        if (!payload.contains("enemies")) {
            return;
        }
        for (const auto& e : payload["enemies"]) {
            if (!e.contains("key") || !e.contains("pos") || !e.contains("rot") || !e.contains("hp")) {
                continue;
            }
            uint64_t key = e["key"].get<uint64_t>();
            RemoteEnemyState& r = remoteStates[key];
            r.rxTick = tickCounter;
            r.pos = e["pos"].get<Vec3f>();
            r.rot = e["rot"].get<Vec3s>();
            r.focusPos = e.contains("fpos") ? e["fpos"].get<Vec3f>() : r.pos;
            if (e.contains("scale")) {
                r.scale = e["scale"].get<Vec3f>();
                r.hasScale = true;
            }
            r.health = e["hp"].get<uint8_t>();
            r.mask = e.contains("mask") ? e["mask"].get<uint8_t>() : 0;
            r.phase = e.contains("phase") ? e["phase"].get<uint8_t>() : 0;
            if (e.contains("extras")) {
                r.extras = e["extras"];
            }
            r.joints.clear();
            if (e.contains("jt")) {
                const auto& jt = e["jt"];
                size_t n = jt.size() / 3;
                if (jt.size() % 3 == 0 && n <= (size_t)MAX_STREAM_LIMBS) {
                    r.joints.reserve(n);
                    for (size_t i = 0; i < n; i++) {
                        Vec3s v;
                        v.x = (int16_t)jt[i * 3 + 0].get<int>();
                        v.y = (int16_t)jt[i * 3 + 1].get<int>();
                        v.z = (int16_t)jt[i * 3 + 2].get<int>();
                        r.joints.push_back(v);
                    }
                } else {
                    SPDLOG_WARN("[EnemySync] Bad jt size {} for key={:#x}", jt.size(), key);
                }
            }
            // Quad AT/AC vertices (Step 4). Cleared unconditionally so a quad that
            // stopped attacking (qv absent this frame) never re-submits last frame's
            // stale verts. Present only while a quad-AT/AC enemy is submitting.
            r.quadVerts.clear();
            if (e.contains("qv")) {
                const auto& qv = e["qv"];
                size_t n = qv.size();
                // Must be whole Vec3f (mult of 3) and whole quads (mult of 12),
                // capped at every captured collider being a quad.
                if (n % 12 == 0 && n <= (size_t)MAX_TRACKED_COLLIDERS * 12) {
                    r.quadVerts.reserve(n / 3);
                    for (size_t i = 0; i + 2 < n; i += 3) {
                        Vec3f v;
                        v.x = qv[i + 0].get<float>();
                        v.y = qv[i + 1].get<float>();
                        v.z = qv[i + 2].get<float>();
                        r.quadVerts.push_back(v);
                    }
                } else {
                    SPDLOG_WARN("[EnemySync] Bad qv size {} for key={:#x}", n, key);
                }
            }
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] IngestEnemyState parse error: {}", ex.what());
    }
}

// Per-game-frame bookkeeping (driven by the Anchor per-frame dispatcher, which runs
// this AFTER the packet-queue drain and AFTER CoopLifeSync::Tick so ComputeAuthorityClientId
// reads this frame's myLifeState — see HookHandlers.cpp): election, authority streaming,
// cull management, aging.
static void Tick() {
    tickCounter++;

    for (auto it = recentlyDeadKeys.begin(); it != recentlyDeadKeys.end();) {
        if (--it->second == 0) {
            it = recentlyDeadKeys.erase(it);
        } else {
            ++it;
        }
    }

    cachedAuthorityId = ComputeAuthorityClientId();

    // Task 2 (W1 Step 2): rebuild the per-tick perception-target cache. Cleared
    // unconditionally so a mid-scene sync-off (early-return below), a scene change,
    // or a departing puppet can never leave a stale target behind for next tick.
    sPerceptionTargets.clear();

    if (gPlayState == NULL || !SyncEnabled() || !MirroringEnabled()) {
        // One cleanup sweep so a mid-scene CVar flip doesn't strand suppressed
        // enemies (their dropped M1 hits would desync HP one-way) or forced culls.
        for (auto& [actor, st] : tracked) {
            if (st.suppressed) {
                EndSuppression(actor, st, "sync-off");
            }
            if (st.cullForced && !st.dying) {
                RestoreCull(actor, st);
            }
            st.submitMask = 0;
        }
        return;
    }

    bool peerInScene = AnySameScenePeer();

    // Cache clientIds of same-scene live puppets for authority-side nearest-player
    // perception (OverridePlayerPerception re-resolves + re-validates each one at
    // read time). Predicate matches the design's cache-inclusion test exactly; !self
    // excludes the local player, which is always an implicit perception target.
    if (Anchor::Instance != nullptr) {
        for (auto& [cid, c] : Anchor::Instance->clients) {
            if (!c.self && c.online && c.isSaveLoaded && c.sceneNum == gPlayState->sceneNum &&
                c.player != nullptr && c.player->actor.update != NULL && !(c.stateFlags1 & PLAYER_STATE1_DEAD)) {
                sPerceptionTargets.push_back(cid);
            }
        }
    }

    if ((tickCounter % 100) == 0 && VerboseSync()) {
        int peers = 0, sameScene = 0;
        for (auto& [cid, c] : Anchor::Instance->clients) {
            if (!c.self && c.online) {
                peers++;
                if (c.isSaveLoaded && c.sceneNum == gPlayState->sceneNum) {
                    sameScene++;
                }
            }
        }
        ESYNC_LOG("[EnemySync] ELECT auth={} own={} peersOnline={} sameScene={} tracked={} remote={}",
                  cachedAuthorityId, Anchor::Instance->ownClientId, peers, sameScene, tracked.size(),
                  remoteStates.size());
    }

    if (IsLocalAuthority() && peerInScene) {
        nlohmann::json enemies = nlohmann::json::array();
        for (auto& [actor, st] : tracked) {
            if (actor->update == NULL || IsMirrorBlocked(actor)) {
                st.submitMask = 0;
                continue;
            }
            ForceUncull(actor, st);
            // Belt-and-suspenders for the F1 latch: a suppressed enemy that was
            // culled (so its update never fired the ShouldActorUpdate clear) still
            // gets released here now that we own its AI.
            if (st.suppressed) {
                EndSuppression(actor, st, "became-authority");
            }
            // Dying enemies leave the stream so both sides play the death
            // locally — except adapter-managed bosses, which must stream their
            // defeat phase edge (the mirror releases itself on that edge).
            // Projectiles carry health 0 by design, so exclude them from the
            // health==0 exit here — they must fall through to the spawn broadcast
            // below (they are announced once, then run local physics on each side).
            bool leftStream = st.deathCooldown > 0 || st.dying ||
                              (actor->colChkInfo.health == 0 && GetAdapter(actor->id) == nullptr && !st.projectile);
            if (leftStream) {
                st.submitMask = 0;
                continue;
            }
            // Step 3: never stream projectiles (deterministic ballistic actors run
            // their own physics on every machine — streaming would add ~20 pkt/s per
            // nut and a suppressed copy could not be deflected locally).
            if (!st.projectile) {
                enemies.push_back(SnapshotEnemy(actor, st));
            }
            st.submitMask = 0;

            // Announce dynamic spawns (deferred to the tick: Actor_SpawnAsChild
            // wires actor->parent only after Actor_Spawn returns, so parent
            // linkage is resolvable here but not inside the OnActorSpawn hook).
            // Rebroadcast ALL living dynamic spawns, including ones we inherited via
            // ENEMY_SPAWN (remoteSpawned): after a handover the new authority must
            // re-announce them so a (re)joining mirror maps the key instead of
            // hitting "Unmapped dynamic key". Idempotent — HandleRemoteSpawn ignores
            // keys it already knows. Projectiles are excluded from rebroadcast: they
            // are fire-and-forget (announced once via needsSpawnBroadcast), and a
            // re-announce after the other side's copy already died would resurrect it.
            bool rebroadcast = st.dynamicKey && !st.projectile && (tickCounter % SPAWN_REBROADCAST_TICKS) == 0;
            // Relax the health>0 gate for projectiles (they spawn with health 0).
            if ((st.needsSpawnBroadcast || rebroadcast) && (actor->colChkInfo.health > 0 || st.projectile)) {
                st.needsSpawnBroadcast = false;
                uint64_t parentKey = 0;
                if (actor->parent != NULL) {
                    auto pit = tracked.find(actor->parent);
                    if (pit != tracked.end()) {
                        parentKey = pit->second.key;
                    }
                }
                // Projectiles: send world.rot, not shape.rot. Flight direction lives
                // in world.rot.y (fixed at spawn, consumed by Actor_MoveXZGravity),
                // whereas both projectiles zero their cosmetic shape.rot.y on their
                // first update (z_en_nutsball.c:101 / z_en_okuta.c:158) — which runs
                // the same frame they spawn, before this broadcast. Sending shape.rot
                // would replicate a zeroed yaw and the mirror copy would fly the wrong
                // way. Regular dynamic spawns keep shape.rot (their facing).
                Vec3s spawnRot = st.projectile ? actor->world.rot : actor->shape.rot;
                Anchor::Instance->SendPacket_EnemySpawn(st.key, st.spawnActorId, st.spawnParams, actor->world.pos,
                                                        spawnRot, actor->room, parentKey);
            }
        }
        if (!enemies.empty()) {
            Anchor::Instance->SendPacket_EnemyState(enemies);
            if ((tickCounter % 60) == 0) {
                ESYNC_LOG("[EnemySync] STREAM tx n={}", enemies.size());
            }
        }
    } else if (IsLocalAuthority() && !peerInScene) {
        for (auto& [actor, st] : tracked) {
            st.submitMask = 0;
            if (st.suppressed) {
                EndSuppression(actor, st, "became-authority");
            }
            if (st.cullForced && !st.dying) {
                RestoreCull(actor, st);
            }
        }
    } else {
        // Mirror: keep enemies with a fresh stream reachable by the hook even at
        // distance; ones whose stream went quiet fall back to vanilla culling.
        uint64_t freshCount = 0;
        for (auto& [actor, st] : tracked) {
            st.submitMask = 0;
            if (actor->update == NULL || st.dying || IsMirrorBlocked(actor)) {
                continue;
            }
            auto rs = remoteStates.find(st.key);
            bool fresh = rs != remoteStates.end() && (tickCounter - rs->second.rxTick) <= STREAM_STALE_TICKS;
            if (fresh) {
                ForceUncull(actor, st);
                freshCount++;
            } else if (st.cullForced && !st.suppressed) {
                RestoreCull(actor, st);
            }
        }
        if (freshCount > 0 && (tickCounter % 60) == 0) {
            ESYNC_LOG("[EnemySync] STREAM rx fresh={}", freshCount);
        }

        // Ghost-enemy repair: on room entry, ask the authority what's actually
        // alive in here (deaths that happened before we were listening).
        int16_t curRoom = gPlayState->roomCtx.curRoom.num;
        if (curRoom != lastRosterRoom) {
            lastRosterRoom = curRoom;
            if (peerInScene) {
                Anchor::Instance->SendPacket_EnemyRosterRequest(curRoom);
            }
        }
    }

    for (auto it = remoteStates.begin(); it != remoteStates.end();) {
        if (tickCounter - it->second.rxTick > STREAM_PRUNE_TICKS) {
            it = remoteStates.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Actor lifecycle hooks (M1, extended with capture claiming)
// ---------------------------------------------------------------------------

static void OnEnemyActorInit(Actor* actor) {
    if (!(IsTrackedCategory(actor) || IsSyncedProjectile(actor)) || gPlayState == NULL || IsTrackingExcluded(actor)) {
        return;
    }

    TrackedState state;
    state.spawnPos = actor->world.pos;
    state.prevHealth = actor->colChkInfo.health;
    state.spawnActorId = actor->id;
    state.spawnParams = (uint16_t)actor->params;
    state.projectile = IsSyncedProjectile(actor);

    auto pending = pendingDynamicKeys.find(actor);
    if (pending != pendingDynamicKeys.end()) {
        // Deferred-init dynamic spawn: OnActorSpawn/HandleRemoteSpawn already
        // assigned its key. Dynamics never touch the occurrence counters (those
        // must stay identical across clients regardless of AI timing).
        state.key = pending->second.first;
        state.dynamicKey = true;
        state.remoteSpawned = pending->second.second;
        state.needsSpawnBroadcast = !state.remoteSpawned;
        pendingDynamicKeys.erase(pending);
    } else {
        int16_t roomNum = gPlayState->roomCtx.curRoom.num;
        if (roomNum != countersRoomNum) {
            spawnCounts.clear();
            countersRoomNum = roomNum;
        }
        uint32_t comboKey = ((uint32_t)(uint16_t)actor->id << 16) | (uint16_t)actor->params;
        uint16_t occurrence = spawnCounts[comboKey]++;
        state.key = PackKey(roomNum, occurrence, actor->id, (uint16_t)actor->params);
    }

    keyToActor[state.key] = actor;
    tracked[actor] = state;

    TrackedState& st = tracked[actor];
    ClaimPendingCaptures(actor, st);

    ESYNC_LOG("[EnemySync] TRACK id={} params={:#06x} key={:#x} hp={} colliders={} skel={}", actor->id,
              (uint16_t)actor->params, st.key, st.prevHealth, st.colliders.size(),
              st.skelAnime != nullptr ? st.skelAnime->limbCount : 0);

    // Death ledger: this enemy was killed remotely while its room was unloaded.
    // Now that its copy exists, apply the kill quietly (same as ReconcileRoster's
    // ghost-kill: set the cooldown so OnEnemyActorKill won't re-broadcast, no drops).
    auto ledger = pendingRemoteKills.find(st.key);
    if (ledger != pendingRemoteKills.end()) {
        ESYNC_LOG("[EnemySync] ROSTER ledger-kill key={:#x} id={}", st.key, actor->id);
        st.deathCooldown = DEATH_COOLDOWN_FRAMES;
        pendingRemoteKills.erase(ledger);
        Actor_Kill(actor);
    }
}

static uint64_t NextDynamicKey() {
    return DYNAMIC_KEY_BIT | ((uint64_t)(Anchor::Instance->ownClientId & 0xFFFF) << 40) |
           (++dynamicKeyCounter & 0xFFFFFFFFFF);
}

static void ReKeyDynamic(Actor* actor, TrackedState& st, uint64_t newKey, bool remoteSpawned) {
    // Undo the occurrence-count identity OnEnemyActorInit just assigned so the
    // deterministic counters stay in lockstep across clients.
    keyToActor.erase(st.key);
    uint32_t comboKey = ((uint32_t)(uint16_t)actor->id << 16) | st.spawnParams;
    auto sc = spawnCounts.find(comboKey);
    if (sc != spawnCounts.end() && sc->second > 0) {
        sc->second--;
    }
    st.key = newKey;
    st.dynamicKey = true;
    st.remoteSpawned = remoteSpawned;
    st.needsSpawnBroadcast = !remoteSpawned;
    keyToActor[newKey] = actor;
    ESYNC_LOG("[EnemySync] DYNKEY id={} key={:#x} remote={}", actor->id, newKey, remoteSpawned);
}

// Fires inside Actor_Spawn. Room-listed actors spawn while numSetupActors != 0
// (the drain loop zeroes it only after spawning every entry) — those keep their
// deterministic occurrence keys. Anything else is a runtime AI spawn and gets a
// local dynamic key here; HandleRemoteSpawn re-keys packet-driven spawns right
// after Actor_Spawn returns (this hook can't tell them apart — nested child
// spawns during an actor's init would race a pass-through key).
static void OnEnemyActorSpawn(Actor* actor) {
    if (!(IsTrackedCategory(actor) || IsSyncedProjectile(actor)) || gPlayState == NULL || IsTrackingExcluded(actor) ||
        !MirroringEnabled()) {
        return;
    }
    if (gPlayState->numSetupActors != 0) {
        return;
    }

    auto it = tracked.find(actor);
    if (it != tracked.end()) {
        ReKeyDynamic(actor, it->second, NextDynamicKey(), false);
    } else {
        // Object not loaded yet: init is deferred; OnEnemyActorInit claims this.
        pendingDynamicKeys[actor] = { NextDynamicKey(), false };
    }
}

void HandleRemoteSpawn(uint64_t key, int16_t actorId, uint16_t params, Vec3f pos, Vec3s rot, int16_t roomNum,
                       uint64_t parentKey) {
    if (gPlayState == NULL || keyToActor.contains(key) || recentlyDeadKeys.contains(key)) {
        return;
    }
    if (gPlayState->roomCtx.curRoom.num != roomNum) {
        ESYNC_LOG("[EnemySync] SPAWN rx dropped (room {} not loaded) key={:#x}", roomNum, key);
        return;
    }

    Actor* actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z,
                               (int16_t)params, false);
    if (actor == NULL) {
        SPDLOG_WARN("[EnemySync] SPAWN rx failed for actorId={} key={:#x}", actorId, key);
        return;
    }

    // Adopt the host's key, replacing the local dynamic key OnActorSpawn assigned.
    auto it = tracked.find(actor);
    if (it != tracked.end()) {
        keyToActor.erase(it->second.key);
        it->second.key = key;
        it->second.dynamicKey = true;
        it->second.remoteSpawned = true;
        it->second.needsSpawnBroadcast = false;
        keyToActor[key] = actor;
    } else {
        pendingDynamicKeys[actor] = { key, true }; // deferred init claims it
    }

    // Parent linkage: some children hard-deref actor->parent (Gohma larvae
    // update their parent's childrenGohmaState on death).
    if (parentKey != 0) {
        auto parent = keyToActor.find(parentKey);
        if (parent != keyToActor.end()) {
            actor->parent = parent->second;
        }
    }

    ESYNC_LOG("[EnemySync] SPAWN rx id={} key={:#x} parentResolved={}", actorId, key,
              parentKey != 0 && actor->parent != NULL);
}

void HandleRemoteDespawn(uint64_t key) {
    auto it = keyToActor.find(key);
    if (it == keyToActor.end() || it->second->update == NULL) {
        return;
    }
    auto st = tracked.find(it->second);
    if (st != tracked.end()) {
        st->second.deathCooldown = DEATH_COOLDOWN_FRAMES; // removal, not a death: never re-broadcast
    }
    ESYNC_LOG("[EnemySync] DESPAWN rx key={:#x}", key);
    Actor_Kill(it->second);
}

void NoteUnresolvedRemoteKill(uint64_t key) {
    // Dynamic spawns don't exist until their ENEMY_SPAWN is processed, and they
    // don't re-materialize on room load — a death for an unmapped dynamic key is
    // just packet ordering, not a ghost we'll meet later. Only static room-listed
    // enemies can be re-entered alive, so only static keys need the ledger.
    if (key & DYNAMIC_KEY_BIT) {
        return;
    }
    pendingRemoteKills[key] = 1;
}

nlohmann::json BuildRoster(int16_t roomNum) {
    nlohmann::json entries = nlohmann::json::array();
    for (auto& [actor, st] : tracked) {
        // Projectiles are fire-and-forget (no shared HP, no death sync) — never put
        // them in the roster, or a mirror would ghost-kill its own local copy.
        if (actor->room != roomNum || st.projectile) {
            continue;
        }
        nlohmann::json e;
        e["key"] = st.key;
        e["alive"] = actor->update != NULL && st.deathCooldown == 0 && !st.dying;
        e["hp"] = actor->colChkInfo.health;
        entries.push_back(e);
    }
    return entries;
}

void ReconcileRoster(int16_t roomNum, const nlohmann::json& entries) {
    try {
        std::unordered_map<uint64_t, std::pair<bool, uint8_t>> authority; // key -> {alive, hp}
        for (const auto& e : entries) {
            authority[e["key"].get<uint64_t>()] = { e["alive"].get<bool>(), e["hp"].get<uint8_t>() };
        }
        for (auto& [actor, st] : tracked) {
            // Projectiles never appear in a roster (BuildRoster skips them), so an
            // absent key must not be read as "authority killed it" — skip them here
            // too, or a mirror's live local copy would be ghost-killed on room entry.
            if (actor->room != roomNum || st.projectile || st.dying || actor->update == NULL || IsMirrorBlocked(actor)) {
                continue;
            }
            auto it = authority.find(st.key);
            if (it == authority.end() || !it->second.first) {
                // The authority no longer has this enemy alive: it died before we
                // were listening. Kill the ghost quietly — no death code, no drops.
                ESYNC_LOG("[EnemySync] ROSTER ghost-kill key={:#x} id={}", st.key, actor->id);
                st.deathCooldown = DEATH_COOLDOWN_FRAMES;
                Actor_Kill(actor);
            } else if (it->second.second != actor->colChkInfo.health) {
                actor->colChkInfo.health = it->second.second;
                st.prevHealth = it->second.second;
            }
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("[EnemySync] ReconcileRoster parse error: {}", ex.what());
    }
}

static void OnEnemyActorUpdate(Actor* actor) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;

    // Step 3: projectiles carry no shared HP — damage is canonical per machine
    // (each copy natively ATs its own local Link). Skip the HP-drop detector and
    // the remote-damage/force-kill bookkeeping entirely; none of it applies (their
    // deathCooldown / pendingKillFrames / expectedRemoteDamage are never set).
    if (state.projectile) {
        return;
    }

    if (state.deathCooldown > 0) {
        state.deathCooldown--;
    }

    if (state.pendingKillFrames > 0) {
        state.pendingKillFrames--;
        if (state.pendingKillFrames == 0 && actor->update != NULL) {
            // The lethal synthetic hit didn't take (custom damage path) — force it
            actor->colChkInfo.health = 0;
            Actor_Kill(actor);
            return;
        }
    }

    uint8_t health = actor->colChkInfo.health;

    if (health < state.prevHealth) {
        uint8_t delta = state.prevHealth - health;
        // Consume drops we caused on behalf of remote clients instead of echoing
        uint8_t remoteShare = std::min(delta, state.expectedRemoteDamage);
        state.expectedRemoteDamage -= remoteShare;
        uint8_t localShare = delta - remoteShare;

        ESYNC_LOG("[EnemySync] HP drop id={} {}->{} localShare={} remoteShare={}", actor->id, state.prevHealth, health,
                  localShare, remoteShare);

        if (localShare > 0) {
            uint32_t dmgFlags = 0;
            Vec3s hitPos = { (int16_t)actor->world.pos.x, (int16_t)actor->world.pos.y,
                             (int16_t)actor->world.pos.z };
            if (state.acCollider != nullptr) {
                ForEachColliderInfo(state.acCollider, [&](ColliderInfo* info) {
                    if (dmgFlags == 0 && info->acHitInfo != NULL) {
                        dmgFlags = info->acHitInfo->toucher.dmgFlags;
                        hitPos = info->bumper.hitPos;
                    }
                });
            }
            Anchor::Instance->SendPacket_EnemyHit(actor, state.key, localShare, dmgFlags, hitPos, health);
        }
    }
    state.prevHealth = health;

    if (state.expectedRemoteDamage > 0 && state.expectedRemoteDamageTimer > 0) {
        state.expectedRemoteDamageTimer--;
        if (state.expectedRemoteDamageTimer == 0) {
            // The synthetic collider hit was never consumed by the enemy's update
            // (custom damage path / i-frame swallow). Converge HP directly so a
            // forwarded hit is never silently lost — same fallback ApplyRemoteHit
            // uses when no collider is seen. Health-only: let the enemy's own
            // update / defeat hook react to reaching 0. We deliberately do NOT
            // force Actor_Kill here — that would permanently despawn a regrower
            // (En_Karebaba) the sender's copy regrows (review finding #2). The
            // lethal case is already covered by the permanent-death force-kill net
            // (pendingKillFrames, above). prevHealth is set alongside health so the
            // HP-drop detector doesn't re-broadcast a spurious ENEMY_HIT echo.
            uint8_t resid = state.expectedRemoteDamage;
            ESYNC_LOG("[EnemySync] expired debt force-applied id={} key={:#x} dmg={}", actor->id, state.key, resid);
            actor->colChkInfo.health = (actor->colChkInfo.health > resid) ? actor->colChkInfo.health - resid : 0;
            state.prevHealth = actor->colChkInfo.health;
            state.expectedRemoteDamage = 0;
        }
    }
}

static void OnEnemyActorKill(Actor* actor) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;

    // Step 3: projectiles (Deku nut / Octorok rock) are fire-and-forget. Each
    // machine spawns, simulates, and kills its OWN copy (wall/floor/AT/AC/OC hit or
    // the ~11 s self-timer), so never broadcast a death or despawn for them. A
    // trailing removal packet would either target an already-dead copy on the peer
    // (blocked, but noisy) or, worse, race a stale spawn — and there is no shared
    // HP to reconcile. (This early return supersedes the old "Octorok rock on
    // impact" despawn branch below, which pre-dated the projectile carve-out and
    // wrongly assumed the rock was a tracked, despawn-replicated dynamic spawn.)
    if (state.projectile) {
        return;
    }

    // Only broadcast real deaths (health exhausted), not despawns/cleanup kills
    if (state.deathCooldown == 0 && actor->colChkInfo.health == 0) {
        state.deathCooldown = DEATH_COOLDOWN_FRAMES;
        Anchor::Instance->SendPacket_EnemyDied(actor, state.key, /*permanent=*/true);
    } else if (state.deathCooldown == 0 && state.dynamicKey && MirroringEnabled() && IsLocalAuthority()) {
        // Non-death removal of a replicated dynamic spawn: mirrors must remove their
        // copy too. (Projectiles are handled by the early return above.)
        state.deathCooldown = DEATH_COOLDOWN_FRAMES;
        Anchor::Instance->SendPacket_EnemyDespawn(state.key);
    }
}

// Enemy_StartFinishingBlow-style defeats. This is the only death signal that exists
// for enemies that never route damage through colChkInfo.health (e.g. En_Karebaba,
// which also never calls Actor_Kill — it regrows from the same actor).
static void OnEnemyDefeated(Actor* actor) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;

    // The enemy is visibly playing its death now — never force-kill on top of it
    state.pendingKillFrames = 0;

    if (state.deathCooldown > 0) {
        return; // defeat caused by a remote death we already applied — don't echo
    }
    state.deathCooldown = DEATH_COOLDOWN_FRAMES;
    ESYNC_LOG("[EnemySync] DEFEAT id={} -> broadcasting death", actor->id);
    // Defeat-hook deaths (regrowers like En_Karebaba never Actor_Kill): the
    // receiver must not force-kill if its copy can't consume the lethal hit.
    Anchor::Instance->SendPacket_EnemyDied(actor, state.key, /*permanent=*/false);
}

static void OnEnemyActorDestroy(Actor* actor) {
    ScrubPendingCaptures(actor);
    pendingDynamicKeys.erase(actor);
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    recentlyDeadKeys[it->second.key] = RECENTLY_DEAD_TTL_FRAMES;
    remoteStates.erase(it->second.key);
    keyToActor.erase(it->second.key);
    tracked.erase(it);
}

static void OnColliderSetAC(Actor* actor, Collider* collider) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    it->second.acCollider = collider;
    it->second.submitMask |= SUBMIT_AC;
    AddCollider(actor, it->second, collider);
}

// Per-frame entry point, called by the Anchor per-frame dispatcher in explicit tick
// order (see HookHandlers.cpp). Kept separate from RegisterHooks so the order relative
// to CoopLifeSync (which SETS myLifeState) is guaranteed by call sequence rather than
// by unordered_map iteration.
void PerFrameTick() {
    Tick();
}

void RegisterHooks(bool isConnected) {
    // The Release logger is async and only durable on flush. Verbose runs flush
    // every INFO line so the test rig can grep logs live (and after taskkill);
    // otherwise flush on the WARN canaries only.
    spdlog::default_logger()->flush_on(VerboseSync() ? spdlog::level::info : spdlog::level::warn);
    ESYNC_LOG("[EnemySync] RegisterHooks(isConnected={})", isConnected);

    // While disconnected the OnActorDestroy/OnSceneInit hooks are unregistered,
    // so every map would go stale; a reconnect's first Tick would then iterate
    // dangling Actor* pointers. Drop everything — tracking rebuilds naturally
    // (scene reload / next OnActorInit).
    if (!isConnected) {
        Reset();
    }

    RegisterBuiltInAdapters();

    // NOTE: the per-frame Tick is NOT registered here; it is driven by the Anchor
    // per-frame dispatcher (EnemySync::PerFrameTick) so its order relative to
    // CoopLifeSync (myLifeState producer) is guaranteed. The disconnect path above still
    // runs Reset().

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_HOOK(OnActorInit, isConnected, [](void* actor) {
        if (SyncEnabled()) {
            OnEnemyActorInit((Actor*)actor);
        }
    });

    COND_HOOK(OnActorSpawn, isConnected, [](void* actor) {
        if (SyncEnabled()) {
            OnEnemyActorSpawn((Actor*)actor);
        }
    });

    COND_HOOK(ShouldActorUpdate, isConnected, [](void* actor, bool* should) {
        OnShouldEnemyUpdate((Actor*)actor, should);
    });

    // Bug C: En_Encount1 (Stalchild/Leever spawner) is ACTORCAT_PROP, so it's
    // untracked and runs on every client — a mirror would spawn its own children
    // on top of the authority's replicated ones. Freeze the spawner on mirrors;
    // they receive the children exclusively via ENEMY_SPAWN. When the authority
    // leaves, cachedAuthorityId recomputes to self next Tick and it resumes.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_ENCOUNT1, isConnected, [](void* actor, bool* should) {
        if (SyncEnabled() && MirroringEnabled() && cachedAuthorityId != UINT32_MAX && !IsLocalAuthority() &&
            AnySameScenePeer()) {
            *should = false;
        }
    });

    COND_HOOK(OnActorUpdate, isConnected, [](void* actor) {
        if (SyncEnabled()) {
            OnEnemyActorUpdate((Actor*)actor);
        }
    });

    COND_HOOK(OnActorKill, isConnected, [](void* actor) {
        if (SyncEnabled()) {
            OnEnemyActorKill((Actor*)actor);
        }
    });

    COND_HOOK(OnEnemyDefeat, isConnected, [](void* actor) {
        if (SyncEnabled() && actor != NULL) {
            OnEnemyDefeated((Actor*)actor);
        }
    });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actor) { OnEnemyActorDestroy((Actor*)actor); });

    COND_HOOK(OnCollisionCheckSetAC, isConnected, [](void* actor, void* collider) {
        if (actor != NULL) {
            OnColliderSetAC((Actor*)actor, (Collider*)collider);
        }
    });

    COND_HOOK(OnCollisionCheckSetAT, isConnected, [](void* actor, void* collider) {
        if (actor != NULL) {
            auto it = tracked.find((Actor*)actor);
            if (it != tracked.end()) {
                it->second.submitMask |= SUBMIT_AT;
                AddCollider((Actor*)actor, it->second, (Collider*)collider);
            }
        }
    });

    COND_HOOK(OnCollisionCheckSetOC, isConnected, [](void* actor, void* collider) {
        if (actor != NULL) {
            auto it = tracked.find((Actor*)actor);
            if (it != tracked.end()) {
                it->second.submitMask |= SUBMIT_OC;
                AddCollider((Actor*)actor, it->second, (Collider*)collider);
            }
        }
    });

    COND_HOOK(OnColliderSetup, isConnected, [](void* actor, void* collider) {
        OnColliderSetup((Actor*)actor, (Collider*)collider);
    });

    COND_HOOK(OnSkelAnimeInit, isConnected, [](void* skelAnime) {
        if (skelAnime != NULL) {
            OnSkelAnimeInitCapture((SkelAnime*)skelAnime);
        }
    });
}

} // namespace EnemySync
