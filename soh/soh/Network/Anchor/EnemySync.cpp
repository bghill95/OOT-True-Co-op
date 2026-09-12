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
// Below this many unsuppressed local updates a remote-spawned copy is assumed to
// still sit in its spawn/intro state (see ApplyRemoteDeath): its death is applied
// directly instead of handed to a state machine that never left the ground.
static constexpr uint16_t REMOTE_DEATH_MIN_LOCAL_UPDATES = 12;
// ...and the window such a copy gets to die on its own before the force-kill.
// Short: past this the enemy is visibly replaying its spawn animation instead of
// dying. Long enough for one damage-consuming update on any enemy that can.
static constexpr uint8_t REMOTE_DEATH_QUICK_GRACE = 8;
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
// Elements inside one collider, for the ragged per-element arrays (touchParams).
// Only a bound on what is accepted off the wire: a JntSph carries one per sphere,
// a Tris one per triangle. Counted across the overlays: the largest are 19
// (z_boss_dodongo.h items[19], z_boss_fd.h elements[19], z_boss_mo.h
// tentElements[19]); Bongo Bongo's hands carry 11. Do NOT lower this below 19.
static constexpr size_t MAX_COLLIDER_ELEMENTS = 32;
// Attack parameters resend cadence when nothing changed, so a peer that joined
// mid-fight or dropped the packet that armed an enemy still converges within a
// second. Must stay well under STREAM_STALE_TICKS.
static constexpr uint64_t TOUCH_KEYFRAME_TICKS = 20;
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
    uint32_t lethalDmgFlags = 0;   // dying: re-armed each frame until the enemy takes its death branch
    Actor* lethalAttacker = nullptr;
    uint8_t deathCooldown = 0;     // >0: a death was just sent/received; don't re-broadcast or re-apply
    Collider* acCollider = nullptr; // last AC collider this actor registered

    // --- M2 mirroring ---
    std::vector<Collider*> colliders; // captured at Collider_SetBase* time
    SkelAnime* skelAnime = nullptr;   // captured at SkelAnime_Init* time
    uint8_t submitMask = 0;           // authority: SubmitMaskBits seen this frame
    std::vector<uint16_t> pendingSfx; // authority: sounds this actor played this frame
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
    uint16_t localUpdates = 0;        // frames this copy's OWN AI ran unsuppressed (saturating)
    Vec3f lastSentScale{};            // authority: last scale actually put on the wire
    bool scaleSent = false;
    bool needsSpawnBroadcast = false; // authority: announce this dynamic spawn
    bool projectile = false;          // whitelisted enemy projectile: fire-and-forget replica, never streamed/mirrored
    uint8_t phase = 0;                // last adapter phase seen/applied
    // Authority: last per-element attack parameters, cylinder radii and collider
    // profile actually put on the wire. These are near-constant for seconds at a
    // time (an enemy arms for the frames its attack is live), and sending them
    // every frame cost as much as the whole jointTable on a multi-element actor —
    // so they go out on change, plus a keyframe (see TOUCH_KEYFRAME_TICKS) for
    // peers that joined or lost a packet. Same discipline as lastSentScale.
    std::vector<uint32_t> lastSentTouch;
    std::vector<int32_t> lastSentRadius;
    std::vector<uint32_t> lastSentProfile;
    uint64_t lastTouchSendTick = 0;
    bool touchSent = false;
    bool warnedColProfile = false; // mirror: the profile-mismatch canary, once per actor
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
    // shape.yOffset: a DRAW-only vertical offset, so it is invisible to the
    // streamed world.pos. Emerging enemies bury themselves with it (En_Skb sets
    // -8000 in Init and rises to 0, En_Anubice -4230, Barinade -1420): frozen on
    // a mirror the enemy is drawn deep underground while its collider, placed
    // from world.pos, still hits — invisible and dangerous. Base-Actor field, so
    // base stream, like scale.
    float yOffset = 0.0f;
    bool hasYOffset = false;
    uint8_t health = 0;
    uint8_t mask = 0;
    uint8_t phase = 0;
    std::vector<Vec3s> joints;
    // Streamed world-space vertices of each SUBMITTED quad AT/AC collider, 4 Vec3f
    // per quad, in the authority's st.colliders capture order. Empty when this
    // enemy submitted no quad class this frame (the whole Deku Tree roster).
    std::vector<Vec3f> quadVerts;
    // Per-collider AT/AC/OC enable bits (bit 0/1/2), in the authority's
    // st.colliders capture order. These live on the collider struct and are
    // toggled by the enemy's own AI (En_Bili floating vs burnt, En_Eiyer buried
    // vs ambushing, a Deku Baba's vulnerable window): the suppressed mirror
    // never runs that AI, so its copy stays frozen at whatever it held when the
    // stream took over. Frozen ON is harmless (the streamed mask still gates
    // the frame), frozen OFF is not: the enemy stops hurting the mirror player,
    // or the mirror player's hits stop registering at all. Empty = the
    // authority sent none (older build): fall back to the local flags.
    std::vector<uint8_t> colEnable;
    // Per-collider bumper.dmgFlags: WHICH weapons may hurt each collider. Several
    // enemies flip this at runtime to open and close their vulnerable window
    // (En_St and En_Ssh swap the stick bit between body and legs, En_Eiyer while
    // buried, En_Karebaba upright vs cut, the Poe sisters, Bongo Bongo's head).
    // The mirror's AI is suppressed, so its masks freeze at whatever they held
    // when the stream took over -- and a mask frozen shut resolves every hit to
    // zero damage, which the mirror then forwards as a dmg=0 request. The enemy
    // is simply invincible for that player (Well Skulltula, 2026-09-06).
    // Only single-element colliders are carried; multi-element ones keep their
    // own per-element masks. Empty = the authority sent none (older build).
    std::vector<uint32_t> colDmgFlags;
    // Per-ELEMENT attack parameters, two entries each, in the authority's
    // st.colliders order and ForEachColliderInfo order within each collider:
    //   [0] toucherFlags | (toucher.damage << 8) | (toucher.effect << 16)
    //   [1] toucher.dmgFlags
    // The counterpart of colDmgFlags, for the other direction: colDmgFlags says
    // what may hurt the enemy, this says what the enemy hurts US with. Neither
    // the damage nor the touch flags were carried, and plenty of enemies leave
    // them at TOUCH_NONE / zero in Init and only arm them for the frames the
    // attack is live (Dead Hand's bite and its burrow wave, z_en_dh.c:326-329 and
    // :386-389; every Bongo Bongo hand attack, BossSst_HandSetDamage). Frozen on
    // the mirror at their Init values, those enemies were simply HARMLESS to the
    // mirroring player -- the collider was submitted, armed and in the right
    // place, and passed straight through him. Empty = authority sent none.
    std::vector<uint32_t> touchParams;
    // Per-collider cylinder dim.radius, same index space as colEnable. AI-resized
    // cylinders (Dead Hand's burrow wave grows to 300*0.6, Bongo Bongo's shockwave
    // ring) keep their Init radius on a frozen mirror. Non-cylinders send 0.
    std::vector<int32_t> colRadius;
    // The shape and element count of each captured collider, same index space as
    // colEnable. Guards touchParams and colRadius: both are applied only when this
    // matches ours entry for entry, because st.colliders is ordered by first
    // submission and two clients can capture the same colliders in a different
    // order (see the send side). Empty = authority sent none.
    std::vector<uint32_t> colProfile;
    uint32_t actorFlags = 0;
    bool hasActorFlags = false;
    // Sound ids the authority's AI played for this actor this frame, replayed
    // once by the mirror step then cleared (see the mirror path).
    std::vector<uint16_t> sfx;
    nlohmann::json extras;
};

// Actor::flags bits carried by the stream (see ApplyPose). Deliberately narrow:
// everything else on that word is either owned by EnemySync on a mirror or
// describes the local client's own interaction with the actor.
static constexpr uint32_t STREAMED_ACTOR_FLAGS = ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE |
                                                 ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER | ACTOR_FLAG_HOOKSHOT_PULLS_ACTOR |
                                                 ACTOR_FLAG_SFX_FOR_PLAYER_BODY_HIT | ACTOR_FLAG_REACT_TO_LENS;

// Per-actor sound ids carried per frame. A looping sound (SFX_FLAG) is re-played
// by the AI every frame, so the cap bounds a stuck loop instead of growing the
// packet; one-shots are far below it.
static constexpr size_t MAX_STREAMED_SFX = 6;

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

// True when a collider's acHitInfo points into our synthetic toucher pool, i.e.
// the pending AC hit is one WE injected for the enemy's own update to consume.
static bool IsSyntheticHitInfo(const ColliderInfo* info) {
    return info != nullptr && info >= &syntheticToucherPool[0] && info < &syntheticToucherPool[8];
}

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

// Enemies that are NOT in an enemy category when the hooks fire. The Big Octo
// (Jabu-Jabu) is placed dormant by the scene and moves itself to ACTORCAT_PROP
// inside its Init (z_en_bigokuta.c), only becoming an ENEMY again from its own
// update when the fight starts — nothing re-scans it then, so it stayed
// untracked and each client fought its own copy.
// Phantom Ganon's horse (EN_FHG) is ACTORCAT_BG: it is the painting-phase AI
// (drives the boss's pose, picks paintings, spawns the warps / lightning / decoy
// riders), so it must be authority-run and mirrored like the boss it carries.
static bool IsForcedTracked(Actor* actor) {
    return actor->id == ACTOR_EN_BIGOKUTA || actor->id == ACTOR_EN_FHG;
}

// EN_FHG_FIRE params (z_en_fhg_fire.h), split by replication strategy.
enum CoopFhgFireParam : int16_t {
    FHGFIRE_P_LIGHTNING_STRIKE = 1, // fire-and-forget: drops, bursts, spawns its own trails/shocks
    FHGFIRE_P_LIGHTNING_SHOCK = 35, // child of a strike (local on every client)
    FHGFIRE_P_LIGHTNING_BURST = 36, // child of a strike / of a mirrored energy ball (local)
    FHGFIRE_P_SPEAR_LIGHT = 38,     // fire-and-forget: follows its parent boss copy's spear tip
    FHGFIRE_P_WARP_EMERGE = 39,     // fire-and-forget painting portal
    FHGFIRE_P_WARP_RETREAT = 40,    // fire-and-forget painting portal
    FHGFIRE_P_WARP_DEATH = 41,      // spawned by the locally-run death cutscene
    FHGFIRE_P_ENERGY_BALL = 50,     // the tennis ball: authority-run, streamed (adapter)
    FHGFIRE_P_LIGHTNING_TRAIL = 100 // child of a strike (local), 100..107
};

// EN_BB params (z_en_bb.h). Scene-placed Bubbles are all negative; the two
// non-negative values are the flame-trail chain links, never a real enemy.
enum CoopBbParam : int16_t {
    ENBB_P_FLAME_TRAIL = 0, // one of five links the bubble spawns behind itself
    ENBB_P_KILL_TRAIL = 11  // the same links, re-tagged when the bubble dies
};

static bool IsTrackedCategory(Actor* actor) {
    return actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS || IsForcedTracked(actor);
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
// King Dodongo's fire breath (EN_BDFIRE, one actor per flame, parented to the
// boss) is an ENEMY that never sets colChkInfo.health, so the health==0 gate
// dropped it before the spawn broadcast: the mirror never saw the breath.
// Announced as a projectile it runs its own animation on each side, reading
// its parent's firePos/unk_1E2/unk_1A2 (parent resolved from the boss key).
// Phantom Ganon's lightning strike, spear glow and painting portals are pure
// timed cosmetics (the strike's shock/burst children hit whichever Link is near
// them on each machine — the intended co-op semantics): announced once, then
// each side runs its own copy, reading its own boss / horse copy through the
// resolved parent link.
static bool IsSyncedProjectile(Actor* actor) {
    // Volvagia's ceiling rocks (params 100): announced once, then each machine
    // drops its own copy (gravity, split on landing into local 101/102 chunks,
    // AT cylinder against its own Link). Bones (>= 200) are spawned by the
    // locally-run death draw on every client.
    if (actor->id == ACTOR_EN_VB_BALL) {
        return actor->params == 100;
    }
    if (actor->id == ACTOR_EN_FHG_FIRE) {
        int16_t p = (int16_t)actor->params;
        return p == FHGFIRE_P_LIGHTNING_STRIKE || p == FHGFIRE_P_SPEAR_LIGHT || p == FHGFIRE_P_WARP_EMERGE ||
               p == FHGFIRE_P_WARP_RETREAT;
    }
    // NOT Twinrova's blasts, despite the temptation: they fail this list's
    // precondition. A whitelisted projectile must be ballistic from the spawn
    // broadcast onward, and a blast is not — BossTw_BlastFire (z_boss_tw.c:3920)
    // and BossTw_BlastIce (:4109) overwrite world.rot.x/y from GET_PLAYER on
    // their FIRST update and set speedXZ themselves, so the streamed spawn
    // rotation is discarded and each machine aims at its own Link. Two divergent
    // arcs would each spawn their own ground blast, and the mirror would get the
    // authority's on top: two damage zones where the fight has one.
    //
    // Running them locally would not buy participation either. Catching a blast
    // does not damage the sister through a collider — BossTw_BlastShieldCheck
    // tallies into the file-scope sShieldFireCharge / sShieldIceCharge, which no
    // packet carries — so a mirroring player charging a shield would still be
    // adding to a counter nobody reads. Making the final phase co-operative
    // needs those two counters replicated, which is a channel, not a list entry.
    //
    // Anubis fireball. The one enemy projectile that MUST run locally: the only
    // way to kill an Anubis is to bounce its fireball back with the Mirror
    // Shield, and that test is Player_HasMirrorShieldEquipped on the local Link
    // (z_en_anubice_fire.c:115). Streamed, it only ever ran on the authority, so
    // whether the Anubis could be killed at all depended on which machine
    // happened to own it — an authority without the shield made them invincible
    // for both players. Running locally also fixes its trail, which is drawn
    // from unk_160[] (positions written in Update, frozen on a mirror: the ball
    // was drawn at its launch point while its collider flew on).
    //
    // Safe to run locally: func_809B26EC derives the whole flight from world.rot
    // at a fixed speed of 15, and the spawn broadcast carries world.rot for
    // projectiles, so both machines compute the same arc.
    return actor->id == ACTOR_EN_NUTSBALL || actor->id == ACTOR_EN_BDFIRE ||
           actor->id == ACTOR_EN_ANUBICE_FIRE ||
           (actor->id == ACTOR_EN_OKUTA && actor->category == ACTORCAT_PROP);
}

// EN_GOMA params >= 6: hatch debris (>=10) and defeat limb pieces (>=100) are
// short-lived local effects spawned on BOTH clients (limb pieces spawn from
// PostLimbDraw during the locally-run defeat) — tracking or replicating them
// would double-spawn and pollute the stream.
static bool IsTrackingExcluded(Actor* actor) {
    // Demo_Effect: cutscene props (spiritual stones, medallions, light beams).
    // The Zora sapphire re-categorises itself to ACTORCAT_BOSS for Jabu's
    // cutscene, which dragged it into tracking / roster replication: a lone
    // spinning sapphire on the Temple of Time altar before the Song of Time.
    if (actor->id == ACTOR_DEMO_EFFECT) {
        return true;
    }
    // EN_BX (Jabu-Jabu electrified tentacle): a static, unkillable hazard with
    // no AI — its whole Update is "shock the LOCAL Link within 70 units" plus
    // the spark burst. Mirroring it suppressed that Update on the mirror
    // (BUG-005: Jiggly inert for the non-authority), and there is no state
    // worth streaming. Every client runs its own copy.
    if (actor->id == ACTOR_EN_BX) {
        return true;
    }
    // EN_TORCH2 (Dark Link). En_Blkobj, the illusion room, is ACTORCAT_PROP and so is
    // never tracked: its update runs on BOTH clients and its trigger is the LOCAL
    // Link's distance (z_en_blkobj.c:85), so every client spawns its own Dark Link
    // already. Tracking ours on top of that gave the mirror TWO -- the streamed one
    // and the one it built itself, which carries a key nobody knows -- fighting over
    // the overlay's file statics (sActionState, sInput, sAlpha...). Worse, the room
    // exit is gated on Actor_Find(EN_TORCH2) == NULL (:108), so the surplus copy left
    // the illusion wall up forever on that client.
    // Letting each client keep its own copy also settles what mirroring could never
    // fix: Dark Link's entire fight READS AND WRITES GET_PLAYER -- meleeWeaponState,
    // invincibilityTimer, the PLAYER_STATE3_PAUSE_ACTION_FUNC of the sword jump. The
    // mirror's parry, backflip and counter were inert, and a jump aimed at the mirror
    // froze the authority's Link. Running locally, each Dark Link reads the player it
    // is actually fighting, and sAlpha fades in and out for both.
    if (actor->id == ACTOR_EN_TORCH2) {
        return true;
    }
    // EN_FHG_FIRE children of a locally-run parent: the lightning strike's
    // trails / shocks / burst (each side runs its own strike copy), the burst a
    // mirrored energy ball spawns on impact (replayed by the adapter), and the
    // death warp (spawned by the locally-run death cutscene on every client).
    // Tracking them would announce doubles.
    if (actor->id == ACTOR_EN_FHG_FIRE) {
        int16_t p = (int16_t)actor->params;
        return p == FHGFIRE_P_LIGHTNING_SHOCK || p == FHGFIRE_P_LIGHTNING_BURST || p == FHGFIRE_P_WARP_DEATH ||
               p >= FHGFIRE_P_LIGHTNING_TRAIL;
    }
    // Volvagia rock chunks (101/102, split off a local rock) and bones (>= 200,
    // death draw): local on every client.
    if (actor->id == ACTOR_EN_VB_BALL) {
        return actor->params != 100;
    }
    // En_Bb flame trail: the bubble spawns five chain links and wires each one's
    // targetActor / parent / vTrailIdx in the statements right after Actor_Spawn.
    // Same shape as the Tailpasaran tails guarded by gActorInitDepth in
    // OnEnemyActorSpawn, but one step out of reach of that guard: EnBb spawns its
    // trail from an action func, not from Init, so the depth is 0 and the links
    // were getting dynamic keys and being announced. The peer built them from
    // Actor_Spawn alone -- targetActor NULL, since EnBb_Init nulls it -- and
    // EnBb_FlameTrail dereferences it unguarded on the first update (crash on the
    // main thread, z_en_bb.c:431, seen 2026-09-06). Nothing to replicate anyway:
    // the trail is decoration that each client draws behind the bubble it runs.
    if (actor->id == ACTOR_EN_BB) {
        int16_t p = (int16_t)actor->params;
        return p == ENBB_P_FLAME_TRAIL || p == ENBB_P_KILL_TRAIL;
    }
    return actor->id == ACTOR_EN_GOMA && (uint16_t)actor->params >= 6;
}

// Adapter-declared "colChkInfo.health is not this actor's life" (see
// ActorSyncAdapter::IsHealthless): streamed and announced regardless of health.
static bool Healthless(Actor* actor) {
    const ActorSyncAdapter* adapter = GetAdapter(actor->id);
    return adapter != nullptr && adapter->IsHealthless != nullptr && adapter->IsHealthless(actor);
}

static bool CustomColliderPos(Actor* actor) {
    const ActorSyncAdapter* adapter = GetAdapter(actor->id);
    return adapter != nullptr && adapter->customColliderPos;
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
        // Killed but not yet deleted: the table only drops it on Actor_Destroy,
        // so its key still answers for a frame. Never hand a corpse to a hit or
        // a death replay — and never fall through to the fuzzy path either, or
        // the packet would land on a DIFFERENT live instance of the same enemy.
        if (it->second->update == NULL) {
            ESYNC_LOG("[EnemySync] DROP packet for dying key={:#x} id={}", key, actorId);
            return nullptr;
        }
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
        if (state.dynamicKey || actor->id != actorId || (uint16_t)(state.key & 0xFFFF) != params ||
            actor->update == NULL) { // a corpse is still tracked until Actor_Delete
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

void NoteActorCollider(Actor* actor, Collider* col) {
    auto it = tracked.find(actor);
    if (it == tracked.end() || col == nullptr) {
        return;
    }
    TrackedState& st = it->second;
    for (Collider* c : st.colliders) {
        if (c == col) {
            return;
        }
    }
    if (st.colliders.size() < MAX_TRACKED_COLLIDERS) {
        st.colliders.push_back(col);
        ESYNC_LOG("[EnemySync] NoteActorCollider id={} shape={} count={}", actor->id, (int)col->shape,
                  st.colliders.size());
    }
    if (st.acCollider == nullptr) {
        st.acCollider = col;
    }
}

Actor* FakeAttacker(int16_t actorId, Vec3s hitPos) {
    // Pool of oversized zeroed actors: enemies may keep the pointer until their
    // next update and some cast+write through it (Barinade writes
    // EnBoom::returnTimer/moveTo into whatever wears ACTOR_EN_BOOM).
    static uint8_t pool[4][0x800];
    static uint8_t next = 0;
    uint8_t* slot = pool[next];
    next = (next + 1) % 4;
    memset(slot, 0, sizeof(pool[0]));
    Actor* fake = (Actor*)slot;
    fake->id = actorId;
    fake->category = ACTORCAT_ITEMACTION;
    fake->world.pos = { (float)hitPos.x, (float)hitPos.y, (float)hitPos.z };
    fake->home.pos = fake->world.pos;
    return fake;
}

// What vanilla would have written into colChkInfo.damageEffect for this blow.
//
// A real hit gets it from CollisionCheck_ApplyDamage (z_collision_check.c:3011,
// the write itself at :3041): walk the toucher's dmgFlags down to its top set
// bit, index the actor's own damage table with that position, and the high
// nibble of the entry is the effect.
// A remote hit never goes through that function -- ApplyRemoteHit stamps AC_HIT,
// BUMP_HIT and acHitInfo onto the collider by hand -- so damageEffect stayed at
// the 0 that CollisionCheck_ResetDamage leaves behind every frame.
//
// That is invisible for an enemy that reads colChkInfo.damage, and fatal for one
// that switches on damageEffect instead. Four are known to: En_Ny (z_en_ny.c:316),
// En_Ik -- Nabooru (z_en_ik.c:716), and both halves of Dead Hand (z_en_dh.c:489,
// z_en_dha.c:387). None of them ever consumed a remote blow: health never fell,
// the mirror's copy was force-killed 30 frames later with no death animation and
// no drop, and for Nabooru that wrote FINISHED_NABOORU_BATTLE into the save with
// the room still sealed.
//
// Nothing needs to travel for this. The damage table is a constant of the actor,
// identical on every client, and dmgFlags is already on the wire -- so recompute
// it exactly as vanilla does. The packet format is untouched.
static uint8_t ResolveDamageEffect(Actor* actor, uint32_t dmgFlags) {
    DamageTable* tbl = actor->colChkInfo.damageTable;
    if (tbl == NULL || dmgFlags == 0) {
        // Not quite what vanilla does: with no table it leaves damageEffect
        // untouched rather than clearing it. Same result here, because
        // CollisionCheck_ResetDamage has just zeroed it for this frame.
        return 0;
    }
    int32_t i = 0;
    uint32_t flags = dmgFlags;
    for (; i < 0x20; i++, flags >>= 1) {
        if (flags == 1) {
            break;
        }
    }
    if (i >= 0x20) {
        // Vanilla would index table[0x20], one past the end of u8 table[32]. It
        // cannot happen from here (callers substitute DMG_SLASH for 0) but the
        // bound is free.
        return 0;
    }
    return (uint8_t)((tbl->table[i] >> 4) & 0xF);
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

    // One local for the fallback: the synthetic toucher and the resolved effect must
    // always describe the SAME blow, or a later change to one desynchronises them.
    uint32_t effectiveFlags = dmgFlags != 0 ? dmgFlags : DMG_SLASH;

    ColliderInfo* synthetic = &syntheticToucherPool[syntheticToucherIndex];
    syntheticToucherIndex = (syntheticToucherIndex + 1) % 8;
    memset(synthetic, 0, sizeof(ColliderInfo));
    synthetic->toucher.dmgFlags = effectiveFlags;
    synthetic->toucher.damage = damage;
    synthetic->toucherFlags = TOUCH_ON | TOUCH_HIT;

    // Prime the same state a real collision-check hit would have left behind,
    // then let the enemy's own update consume it (hurt anim, knockback, death).
    // Same trick Anchor already uses for destructible walls in HookHandlers.cpp.
    actor->colChkInfo.damage = damage;

    if (state.acCollider != nullptr) {
        // ...including the effect nibble, which is the half enemies branch on.
        //
        // Written INSIDE this branch, not before it: on the no-collider path below
        // no collision is simulated at all, so an effect left on the actor with no
        // matching AC_HIT is read by the nine enemies that gate their whole update
        // on damageEffect alone (EnTest_Update, EnZf_Update, EnTite_Update,
        // EnMb_Update, EnGeldB_Update, EnAm_Update, EnBw_Update, EnDodongo_Update,
        // EnPeehat_Update) -- they would stall for a frame on a blow that never
        // happened locally.
        //
        // Lifetime: on the normal path the reset runs AFTER the actor's update
        // (z_actor.c:2710) and the packet drain runs after that (game.c:356, past
        // gameState->main), so this survives into the update that reads it. Three
        // holes, all pre-existing and all shared with colChkInfo.damage -- an actor
        // that reaches a reset without having run its update drops the remote blow:
        // outside the culling volume, or with freezeTimer still running (the guard
        // is z_actor.c:2687, reset at :2710), or skipped entirely while the player
        // is in a cutscene or an ocarina/dialogue state (reset at z_actor.c:2669).
        actor->colChkInfo.damageEffect = ResolveDamageEffect(actor, effectiveFlags);
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

// dmgFlags of the hit sitting on the actor's AC collider right now — the
// lethal blow's flags when called from the same frame's defeat / kill hook.
// 0 when nothing is recorded (the receiver then falls back to DMG_SLASH).
static uint32_t CurrentHitDmgFlags(TrackedState& st) {
    uint32_t dmgFlags = 0;
    if (st.acCollider != nullptr) {
        ForEachColliderInfo(st.acCollider, [&](ColliderInfo* info) {
            if (dmgFlags == 0 && (info->bumperFlags & BUMP_HIT) && info->acHitInfo != NULL) {
                dmgFlags = info->acHitInfo->toucher.dmgFlags;
            }
        });
    }
    return dmgFlags;
}

// Enemies whose defeat is not a despawn: the sender's copy grows back, so a
// force-kill on the mirror would delete an enemy that is supposed to return.
// En_Karebaba's withered stalk is the case the non-permanent death path was
// written for.
static bool IsRegrower(Actor* actor) {
    return actor->id == ACTOR_EN_KAREBABA;
}

// Keep a dying copy's pending hit lethal until its own damage code takes the
// death branch. Every frame: health 1, damage 1 (Actor_ApplyDamage: damage >=
// health -> 0 -> death), and the AC hit re-primed on its collider. The enemy's
// state machine may not consume for a few frames (a Stalchild ignores AC hits
// while flinching, and its SetAC wipes the flag at the end of its update), so a
// single injection could land as a 0-damage flinch; re-arming makes the FIRST
// consumption, whenever it happens, a death — the same animation the authority
// showed. No drift/debt bookkeeping here: that was done once by ApplyRemoteHit.
static void PrimeLethalHit(Actor* actor, TrackedState& st) {
    actor->colChkInfo.health = 1;
    actor->colChkInfo.damage = 1;
    if (st.acCollider == nullptr) {
        return;
    }
    // An enemy that branches on damageEffect reads 0 without this and treats the
    // lethal blow as nothing at all -- that is the whole reason a mirrored En_Ny,
    // Nabooru or Dead Hand never died and had to be force-killed 30 frames later.
    //
    // Resolved from the REAL lethal flags, the same ones the synthetic toucher
    // below carries, so the two always describe one blow. Substituting a fixed
    // DMG_SLASH here was tried and reverted: entry 0x0A is the Giant's Knife, and
    // on En_Po_Sisters it yields effect 0xE, which z_en_po_sisters.c:1167 swallows
    // without applying any damage -- a sister that died correctly before would
    // hang until the force-kill. On En_Tp it yields SHOCKING where a boomerang
    // kill yields INSULATING, so the dying mirror could electrocute the local
    // player for a kill he never made.
    //
    // What this does NOT fix, deliberately: En_Test on a light arrow, and
    // En_GeldB / En_Zf / En_Tite on ice, branch to a stun path that never reaches
    // their death code, and health is re-armed to 1 every frame so the
    // `health == 0` exit from stun cannot fire either. Those four still fall
    // through to the force-kill net -- exactly as they did before this change.
    // Making them die properly needs a per-actor answer, not a global one.
    uint32_t lethalFlags = st.lethalDmgFlags != 0 ? st.lethalDmgFlags : DMG_SLASH;
    actor->colChkInfo.damageEffect = ResolveDamageEffect(actor, lethalFlags);
    ColliderInfo* synthetic = &syntheticToucherPool[syntheticToucherIndex];
    syntheticToucherIndex = (syntheticToucherIndex + 1) % 8;
    memset(synthetic, 0, sizeof(ColliderInfo));
    synthetic->toucher.dmgFlags = lethalFlags;
    synthetic->toucher.damage = 1;
    synthetic->toucherFlags = TOUCH_ON | TOUCH_HIT;
    Collider* col = st.acCollider;
    col->acFlags |= AC_HIT;
    col->ac = st.lethalAttacker;
    Vec3s hitPos = { (int16_t)actor->world.pos.x, (int16_t)actor->world.pos.y, (int16_t)actor->world.pos.z };
    ForEachColliderInfo(col, [&](ColliderInfo* info) {
        info->bumperFlags |= BUMP_HIT;
        info->acHitInfo = synthetic;
        info->bumper.hitPos = hitPos;
    });
}

void ApplyRemoteDeath(Actor* actor, Actor* attacker, bool permanent, uint32_t dmgFlags) {
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
    state.lethalDmgFlags = dmgFlags;
    state.lethalAttacker = attacker;

    // The dying latch engages only for deaths we actually apply (echoes and
    // duplicates above must not strand a live enemy unculled/unmirrored).
    ReleaseForDeath(actor);

    // One lethal point, whatever the enemy's own accounting: Actor_ApplyDamage
    // (health -= damage, clamped), "health -= n" and "health--" all reach 0 from
    // 1. The stream may already have driven the mirror's health to 0 — a
    // decrement-style death (En_Ba, Parasitic Tentacle) then underflowed to 255
    // and the copy stood there unkillable while the authority's was gone.
    // The real dmgFlags travel with the packet so flag-gated enemies (boomerang
    // only, fire only…) accept the hit.
    actor->colChkInfo.health = 1;
    Vec3s hitPos = { (int16_t)actor->world.pos.x, (int16_t)actor->world.pos.y, (int16_t)actor->world.pos.z };
    ApplyRemoteHit(actor, 1, dmgFlags, hitPos, 0, attacker);
    // Only Actor_Kill deaths force-kill as a last resort. Defeat-hook enemies
    // (En_Karebaba regrowers) consume hits in few action states — force-killing
    // one that never consumed the synthetic hit would permanently despawn an
    // enemy the sender's copy regrows.
    // A defeat-hook death (permanent=false) still has to land. The synthetic
    // lethal hit above can be swallowed — i-frames, a custom damage path, or an
    // action state that only reacts with a flinch — and the mirror's copy then
    // bends over, gets back up and keeps fighting while the authority's is
    // already gone (Hyrule Field Stalchildren, 2026-09-05). Arm the force-kill
    // net for those too; only enemies that genuinely regrow on the sender's copy
    // are exempt, which is what permanent=false was protecting in the first place.
    // A copy whose own AI never really ran (suppressed since it spawned) has no
    // usable death to play: its actionFunc is still the spawn/intro one, which
    // for most enemies ignores hits — the Stalchild kept "rising from the ground"
    // for the full grace period and was force-killed at frame 30 (P2 saw it bend,
    // get up, vanish). The authority's copy is already gone; end ours now, with
    // the generic defeat dust so it still reads as a kill.
    // A copy spawned by ENEMY_SPAWN was suppressed a frame or two after spawning,
    // so its actionFunc is still the spawn one, which for some enemies reads no
    // hits at all (En_Skb: rising from the ground). Give it a SHORT window to take
    // its own death rather than killing it outright: dying natively is what makes
    // this client drop its own collectibles (CarrySync.h states the discipline —
    // drops are per-client, not replicated) and play the enemy's real death. Only
    // if that window passes untouched does the force-kill below step in with a
    // generic flame. Copies that have been running locally keep the full grace.
    if (permanent || !IsRegrower(actor)) {
        state.pendingKillFrames = (state.remoteSpawned && state.localUpdates < REMOTE_DEATH_MIN_LOCAL_UPDATES)
                                      ? REMOTE_DEATH_QUICK_GRACE
                                      : REMOTE_DEATH_GRACE;
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
    int16_t atkId = ACTOR_PLAYER;
    bool hit = false;

    auto scan = [&](Collider* col) {
        if (col == nullptr || col->actor != actor || !(col->acFlags & AC_HIT)) {
            return;
        }
        // Only OUR player's hits are requests. Two other things set AC_HIT on a
        // mirrored enemy's collider and must be left alone:
        //  - a puppet's replicated sword swing landing on our copy (the authority
        //    already applied that hit itself; forwarding it echoed it back as a
        //    second hit, and consuming it here flashed the enemy for nothing);
        //  - our own synthetic hit from ApplyRemoteHit/ApplyRemoteDeath, waiting
        //    for the enemy's update to consume it. Eating it here zeroed
        //    colChkInfo.damage, so the enemy consumed a 0-damage hit: a flinch
        //    instead of a death (Hyrule Field Stalchildren, 2026-09-05).
        Actor* attacker = col->ac;
        if (attacker != NULL &&
            (attacker->id == ACTOR_EN_OE2 || (attacker->parent != NULL && attacker->parent->id == ACTOR_EN_OE2))) {
            return;
        }
        bool synthetic = false;
        ForEachColliderInfo(col, [&](ColliderInfo* info) {
            if (IsSyntheticHitInfo(info->acHitInfo)) {
                synthetic = true;
            }
        });
        if (synthetic) {
            return;
        }
        hit = true;
        ForEachColliderInfo(col, [&](ColliderInfo* info) {
            if (dmgFlags == 0 && (info->bumperFlags & BUMP_HIT) && info->acHitInfo != NULL) {
                dmgFlags = info->acHitInfo->toucher.dmgFlags;
                hitPos = info->bumper.hitPos;
                if (col->ac != NULL) {
                    atkId = col->ac->id; // some enemies branch on it (Barinade vs boomerang)
                }
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
    uint8_t damage = actor->colChkInfo.damage;
    uint8_t effect = actor->colChkInfo.damageEffect;
    actor->colChkInfo.damage = 0;

    // The enemy's own damage table has already ruled on this hit: the collision
    // pass filled damage/damageEffect on OUR copy before we got here. Both zero
    // means the weapon does nothing to this enemy, so there is nothing to request.
    //
    // Forwarding it anyway was not harmless. ApplyRemoteHit deliberately forces
    // AC_HIT on the authority's copy without re-consulting the table (the hit was
    // already validated here), so an enemy that counts HITS instead of reading
    // colChkInfo.damage loses health for free -- z_en_vm.c:400 does a bare
    // health--, and a slingshot seed therefore chipped a Beamos that only bombs
    // are supposed to hurt.
    //
    // Deku nuts still pass: zero damage, but a non-zero stun damageEffect.
    if (damage == 0 && effect == 0) {
        return;
    }

    st.lastHitReqTick = tickCounter;

    Anchor::Instance->SendPacket_EnemyHitRequest(actor, st.key, damage, dmgFlags, hitPos, atkId);

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
    size_t colIndex = 0;
    bool customPos = CustomColliderPos(actor);
    // Cursor into r.touchParams (two entries per element). The array is ragged and
    // st.colliders is ordered by first submission, not by declaration, so pairing
    // by position is only safe once OUR profile -- shape and element count, per
    // collider -- matches the authority's entry for entry. Anything else and the
    // whole block is dropped rather than applied off by one element. The same
    // profile gates colRadius, which indexes st.colliders directly.
    size_t touchCursor = 0;
    bool profileMatches = false;
    if (!r.colProfile.empty()) {
        size_t colCount = 0;
        bool ok = true;
        for (Collider* col : st.colliders) {
            uint32_t profile = 0;
            if (col != nullptr && col->actor == actor) {
                uint32_t elems = 0;
                ForEachColliderInfo(col, [&](ColliderInfo* info) { elems++; });
                profile = (uint32_t)col->shape | (elems << 8);
            }
            if (colCount >= r.colProfile.size() || r.colProfile[colCount] != profile) {
                ok = false;
                break;
            }
            colCount++;
        }
        profileMatches = ok && colCount == r.colProfile.size();
        if (!profileMatches && !st.warnedColProfile) {
            st.warnedColProfile = true; // once per tracked actor: this runs every frame
            SPDLOG_WARN("[EnemySync] collider profile mismatch id={} local={} remote={}", actor->id,
                        st.colliders.size(), r.colProfile.size());
        }
    }
    bool touchUsable = profileMatches && !r.touchParams.empty();
    for (Collider* col : st.colliders) {
        size_t myIndex = colIndex++;
        if (col == nullptr || col->actor != actor) {
            continue;
        }
        // Arm this collider the way the authority's AI had it. Without this the
        // mirror uses its own frozen flags: an enemy that disarms while idle
        // (En_Eiyer buried, a retracted Deku Baba) would stay harmless and
        // unhittable here for the rest of the fight. Absent (older peer) → keep
        // the local flags, which is the previous behaviour.
        if (myIndex < r.colEnable.size()) {
            uint8_t bits = r.colEnable[myIndex];
            col->atFlags = (bits & 1) ? (col->atFlags | AT_ON) : (col->atFlags & ~AT_ON);
            col->acFlags = (bits & 2) ? (col->acFlags | AC_ON) : (col->acFlags & ~AC_ON);
            col->ocFlags1 = (bits & 4) ? (col->ocFlags1 | OC1_ON) : (col->ocFlags1 & ~OC1_ON);
        }
        // Vulnerability mask (see RemoteEnemyState::colDmgFlags). 0 means the
        // authority skipped this collider (several elements), so leave ours alone.
        if (myIndex < r.colDmgFlags.size() && r.colDmgFlags[myIndex] != 0) {
            uint32_t mask = r.colDmgFlags[myIndex];
            int32_t n = 0;
            ForEachColliderInfo(col, [&](ColliderInfo* info) { n++; });
            if (n == 1) {
                ForEachColliderInfo(col, [&](ColliderInfo* info) { info->bumper.dmgFlags = mask; });
            }
        }
        // What this collider hurts US with (see RemoteEnemyState::touchParams).
        // TOUCH_HIT and TOUCH_DREW_HITMARK are this frame's OWN results, written by
        // the local CollisionCheck: keep ours and take only the configuration bits.
        if (touchUsable) {
            ForEachColliderInfo(col, [&](ColliderInfo* info) {
                uint32_t packed = r.touchParams[touchCursor];
                uint32_t dmgFlags = r.touchParams[touchCursor + 1];
                touchCursor += 2;
                constexpr uint8_t RESULT_BITS = TOUCH_HIT | TOUCH_DREW_HITMARK;
                info->toucherFlags = (uint8_t)((info->toucherFlags & RESULT_BITS) | ((packed & 0xFF) & ~RESULT_BITS));
                info->toucher.damage = (uint8_t)((packed >> 8) & 0xFF);
                info->toucher.effect = (uint8_t)((packed >> 16) & 0xFF);
                info->toucher.dmgFlags = dmgFlags;
            });
        }
        // AI-resized cylinder (see RemoteEnemyState::colRadius). Gated on the same
        // profile as touchParams -- it indexes st.colliders directly, so on a
        // permuted list it would write one cylinder's radius onto another's (a
        // Freezard carries three, of 30, 35 and 45). 0 means the authority had no
        // cylinder in that slot, so leave ours alone.
        if (profileMatches && col->shape == COLSHAPE_CYLINDER && myIndex < r.colRadius.size() &&
            r.colRadius[myIndex] != 0) {
            ((ColliderCylinder*)col)->dim.radius = (int16_t)r.colRadius[myIndex];
        }
        if (col->shape == COLSHAPE_CYLINDER) {
            // Repositioned from the streamed world.pos (ApplyPose ran first) — accurate.
            // Adapters that centre their cylinders elsewhere (Phantom Ganon: chest
            // limb / spear tip) set them in DeserializeExtras, which ran just before.
            if (!customPos) {
                Collider_UpdateCylinder(actor, (ColliderCylinder*)col);
            }
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
    Vec3f prevPos = actor->world.pos;
    actor->world.pos = r.pos;
    actor->shape.rot = r.rot;
    actor->focus.pos = r.focusPos;

    // Presentation flags the AI toggles per state (57 enemies do; ATTENTION_ENABLED
    // alone is 217 sites). Frozen on the mirror they make an enemy unlockable while
    // it attacks, or Z-targetable while it is buried. Only the bits below travel:
    // the culling bits are OURS on a mirror (ForceUncull), HOOKSHOT_ATTACHED and the
    // talk bits describe this client's own player interaction, and IGNORE_QUAKE is
    // local presentation.
    if (r.hasActorFlags) {
        actor->flags = (actor->flags & ~STREAMED_ACTOR_FLAGS) | (r.actorFlags & STREAMED_ACTOR_FLAGS);
    }
    if (r.hasYOffset) {
        actor->shape.yOffset = r.yOffset;
    }

    // Drag the hitboxes along IMMEDIATELY: cylinders re-center from world.pos
    // and JntSph spheres only refresh during PostLimbDraw — one frame late, on
    // top of network delay. On a fast flyer (Keese) that leaves the hitbox
    // trailing dozens of units behind the sprite, and thin player projectiles
    // (slingshot seeds) whiff through where the enemy visibly is.
    Vec3f d = { r.pos.x - prevPos.x, r.pos.y - prevPos.y, r.pos.z - prevPos.z };
    bool customPos = CustomColliderPos(actor);
    auto drag = [&](Collider* col) {
        if (col == nullptr || col->actor != actor) {
            return;
        }
        if (col->shape == COLSHAPE_CYLINDER) {
            if (customPos) {
                // Adapter-placed cylinder: drag it by the same delta instead of
                // snapping it to world.pos (DeserializeExtras re-places it anyway).
                ColliderCylinder* cyl = (ColliderCylinder*)col;
                cyl->dim.pos.x += (int16_t)d.x;
                cyl->dim.pos.y += (int16_t)d.y;
                cyl->dim.pos.z += (int16_t)d.z;
            } else {
                Collider_UpdateCylinder(actor, (ColliderCylinder*)col);
            }
        } else if (col->shape == COLSHAPE_JNTSPH) {
            ColliderJntSph* js = (ColliderJntSph*)col;
            for (int32_t i = 0; i < js->count; i++) {
                js->elements[i].dim.worldSphere.center.x += (int16_t)d.x;
                js->elements[i].dim.worldSphere.center.y += (int16_t)d.y;
                js->elements[i].dim.worldSphere.center.z += (int16_t)d.z;
            }
        }
    };
    // OnColliderSetAC AddCollider()s acCollider, so it is normally already in
    // the list. The JntSph drag is an accumulating +=, so dragging it a second
    // time put the hitbox a full delta AHEAD of the sprite instead of on it.
    bool acDragged = false;
    for (Collider* col : st.colliders) {
        drag(col);
        acDragged = acDragged || (col == st.acCollider);
    }
    if (!acDragged) {
        drag(st.acCollider);
    }
    if (r.hasScale) {
        actor->scale = r.scale;
    }
    // Not while a death is being applied. ApplyRemoteDeath has just set this copy
    // to its lethal health and injected the killing hit; a stream packet still in
    // flight from before the kill would put the health back up, the enemy would
    // consume the hit as a mere flinch, and it would only die from the force-kill
    // net half a second later (Hyrule Field Stalchildren, 2026-09-05: bent over,
    // got back up, then vanished).
    if (st.deathCooldown == 0 && st.pendingKillFrames == 0) {
        actor->colChkInfo.health = r.health;
        st.prevHealth = r.health;
    }

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
        // Death handoff: the local death plays on its own AI. Until the enemy
        // actually takes its death branch, keep the pending hit lethal (see
        // PrimeLethalHit) — this runs right before the enemy's update, after
        // anything else that could have touched health/damage this frame.
        if (st.dying && st.pendingKillFrames > 0 && actor->update != NULL && actor->colChkInfo.health > 0) {
            PrimeLethalHit(actor, st);
        }
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

    // Deferred-init enemies (EnPoh and friends) spend their first update(s)
    // waiting for their object bank: only then do they build their skeleton and
    // assign actor->draw. Suppressing before that leaves an invisible ghost
    // forever — let the local update run until the actor can actually draw.
    //
    // An adapter that declares mirrorWhileDrawless is exempt: its boss nulls its
    // OWN draw as part of the fight, so this heuristic would release the mirror
    // exactly during that sequence. Phantom Ganon spent the whole painting phase
    // that way — the released copy ran the horse's AI, spawned its own decoy
    // riders and energy balls next to the replicated ones, and swallowed every
    // arrow the mirror's player fired (the real boss took 100% of its damage from
    // the authority's side: localShare=2 remoteShare=0 on every HP drop).
    if (actor->draw == NULL && (adapter == nullptr || !adapter->mirrorWhileDrawless)) {
        return;
    }

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

    // Voice: the AI that plays this enemy's sounds is suppressed here, so a
    // mirrored enemy was silent — no roar, no footstep, no attack impact. Replay
    // what the authority played this frame, at OUR copy's position (ApplyPose ran
    // first). Consumed on play so one packet never sounds twice; adapters with
    // their own replay path opt out via ownsSfx.
    if (!r.sfx.empty()) {
        for (uint16_t id : r.sfx) {
            Audio_PlayActorSound2(actor, id);
        }
        r.sfx.clear();
    }

    SubmitColliders(actor, st, r);

    *should = false;
}

// ---------------------------------------------------------------------------
// M2: authority stream (snapshot + send, once per game frame)
// ---------------------------------------------------------------------------

// A float promoted to double serialises at full double precision: a world.pos of
// -1234.5677f becomes "-1234.5677490234375", 62 bytes for one vector. Rounding to
// centimetres in double first makes nlohmann print "-1234.57". OoT units are about
// a centimetre, so this is below anything visible, and it is emit-side only — the
// mirror reads the same field either way. NOT used for scale, where two decimals
// would round a 0.005 scale to 0.01.
static nlohmann::json RoundedVec(const Vec3f& v) {
    nlohmann::json j;
    j["x"] = std::round((double)v.x * 100.0) / 100.0;
    j["y"] = std::round((double)v.y * 100.0) / 100.0;
    j["z"] = std::round((double)v.z * 100.0) / 100.0;
    return j;
}

static nlohmann::json SnapshotEnemy(Actor* actor, TrackedState& st) {
    nlohmann::json e;
    e["key"] = st.key;
    e["pos"] = RoundedVec(actor->world.pos);
    e["rot"] = actor->shape.rot;
    e["fpos"] = RoundedVec(actor->focus.pos);
    e["hp"] = actor->colChkInfo.health;
    // actor.scale is animated in Update (e.g. Deku Baba emerge 0.5x->1x); the
    // suppressed mirror never recomputes it, so the head skeleton would inherit
    // the frozen buried scale. Base-Actor field → base stream, applied to every
    // mirrored enemy. Sent only when it actually changes: it is constant for
    // nearly the whole bestiary, and 0.01f spelled out as a double is 85 bytes
    // per enemy per frame — more than every field added in T31 put together. The
    // receiver's hasScale latch never re-arms, so omitting it keeps the last value.
    if (!st.scaleSent || actor->scale.x != st.lastSentScale.x || actor->scale.y != st.lastSentScale.y ||
        actor->scale.z != st.lastSentScale.z) {
        e["scale"] = actor->scale;
        st.lastSentScale = actor->scale;
        st.scaleSent = true;
    }
    e["yoff"] = actor->shape.yOffset;
    e["mask"] = st.submitMask;
    e["aflg"] = (uint32_t)(actor->flags & STREAMED_ACTOR_FLAGS);
    if (!st.pendingSfx.empty()) {
        e["sfx"] = st.pendingSfx;
        st.pendingSfx.clear();
    }
    // Per-collider AT/AC/OC enable bits, same capture order as st.colliders (and
    // as the mirror's SubmitColliders iteration). The mask says which classes the
    // real AI submitted this frame; these say which colliders were actually armed
    // when it did, state the mirror cannot recompute with its AI suppressed.
    {
        std::vector<uint8_t> cf;
        cf.reserve(st.colliders.size());
        for (Collider* col : st.colliders) {
            uint8_t bits = 0;
            if (col != nullptr && col->actor == actor) {
                if (col->atFlags & AT_ON) {
                    bits |= 1;
                }
                if (col->acFlags & AC_ON) {
                    bits |= 2;
                }
                if (col->ocFlags1 & OC1_ON) {
                    bits |= 4;
                }
            }
            cf.push_back(bits);
        }
        if (!cf.empty()) {
            e["cf"] = cf;
        }

        // Same order and index space as cf. A collider with several elements sends
        // 0 and is skipped on the other side: its elements can legitimately differ,
        // and none of the affected enemies toggle those.
        std::vector<uint32_t> bdf;
        bdf.reserve(st.colliders.size());
        for (Collider* col : st.colliders) {
            uint32_t mask = 0;
            if (col != nullptr && col->actor == actor) {
                int32_t n = 0;
                ForEachColliderInfo(col, [&](ColliderInfo* info) {
                    if (n == 0) {
                        mask = info->bumper.dmgFlags;
                    }
                    n++;
                });
                if (n != 1) {
                    mask = 0;
                }
            }
            bdf.push_back(mask);
        }
        if (!bdf.empty()) {
            e["bdf"] = bdf;
        }

        // Per-element attack parameters and cylinder radii (RemoteEnemyState::
        // touchParams / colRadius). Unlike cf/bdf the element list is ragged, so the
        // receiver walks st.colliders with the SAME predicate as here and pairs the
        // nth element with the nth pair. That pairing is only safe if both sides
        // captured the same colliders in the same ORDER, and they need not have:
        // st.colliders is filled in the order each collider was first submitted, so
        // a client whose copy first ran during BossSst_HeadVulnerable captures
        // [cylinder, jntSph] where one that started elsewhere captures
        // [jntSph, cylinder] -- same element total, every toucher off by one. So
        // send a profile alongside: shape and element count per collider. The
        // receiver applies te AND cr only when its own profile matches entry for
        // entry. Two colliders of the same shape AND element count are still
        // interchangeable, exactly as they already are for cf/bdf.
        std::vector<uint32_t> te;
        std::vector<int32_t> cr;
        std::vector<uint32_t> tec;
        cr.reserve(st.colliders.size());
        tec.reserve(st.colliders.size());
        for (Collider* col : st.colliders) {
            int32_t radius = 0;
            uint32_t profile = 0;
            if (col != nullptr && col->actor == actor) {
                uint32_t elems = 0;
                ForEachColliderInfo(col, [&](ColliderInfo* info) {
                    te.push_back((uint32_t)info->toucherFlags | ((uint32_t)info->toucher.damage << 8) |
                                 ((uint32_t)info->toucher.effect << 16));
                    te.push_back(info->toucher.dmgFlags);
                    elems++;
                });
                if (col->shape == COLSHAPE_CYLINDER) {
                    radius = ((ColliderCylinder*)col)->dim.radius;
                }
                profile = (uint32_t)col->shape | (elems << 8);
            }
            cr.push_back(radius);
            tec.push_back(profile);
        }
        // On change, plus one keyframe a second (see TOUCH_KEYFRAME_TICKS). The three
        // travel TOGETHER, empty ones included, because the receiver only clears what
        // a packet actually carries -- an empty array has to be able to say "nothing
        // here" (st.colliders never shrinks, so that means an arena reused by another
        // actor; the receiver then falls back to its own local values).
        if (!st.touchSent || te != st.lastSentTouch || cr != st.lastSentRadius || tec != st.lastSentProfile ||
            (tickCounter - st.lastTouchSendTick) >= TOUCH_KEYFRAME_TICKS) {
            e["te"] = te;
            e["tec"] = tec;
            e["cr"] = cr;
            st.lastSentTouch = te;
            st.lastSentRadius = cr;
            st.lastSentProfile = tec;
            st.lastTouchSendTick = tickCounter;
            st.touchSent = true;
        }
    }
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
            r.hasYOffset = e.contains("yoff");
            if (r.hasYOffset) {
                r.yOffset = e["yoff"].get<float>();
            }
            r.hasActorFlags = e.contains("aflg");
            if (r.hasActorFlags) {
                r.actorFlags = e["aflg"].get<uint32_t>() & STREAMED_ACTOR_FLAGS;
            }
            // Sounds are appended, not replaced: two stream packets can land
            // between two mirror steps, and dropping the older one would swallow
            // a one-shot (a death cry, an attack impact).
            if (e.contains("sfx") && e["sfx"].is_array() && r.sfx.size() < MAX_STREAMED_SFX) {
                for (const auto& id : e["sfx"]) {
                    if (r.sfx.size() >= MAX_STREAMED_SFX) {
                        break;
                    }
                    r.sfx.push_back(id.get<uint16_t>());
                }
            }
            // Per-collider enable bits. Cleared unconditionally: a peer that stops
            // sending them (or an enemy whose capture list emptied) must fall back
            // to the local flags rather than keep last frame's arming.
            r.colEnable.clear();
            if (e.contains("cf")) {
                const auto& cf = e["cf"];
                if (cf.is_array() && cf.size() <= (size_t)MAX_TRACKED_COLLIDERS) {
                    r.colEnable.reserve(cf.size());
                    for (const auto& b : cf) {
                        r.colEnable.push_back(b.get<uint8_t>());
                    }
                } else {
                    SPDLOG_WARN("[EnemySync] Bad cf size {} for key={:#x}", cf.size(), key);
                }
            }
            r.colDmgFlags.clear();
            if (e.contains("bdf")) {
                const auto& bdf = e["bdf"];
                if (bdf.is_array() && bdf.size() <= (size_t)MAX_TRACKED_COLLIDERS) {
                    r.colDmgFlags.reserve(bdf.size());
                    for (const auto& m : bdf) {
                        r.colDmgFlags.push_back(m.get<uint32_t>());
                    }
                } else {
                    SPDLOG_WARN("[EnemySync] Bad bdf size {} for key={:#x}", bdf.size(), key);
                }
            }
            // Per-element attack parameters: two entries per element, so the bound is
            // the collider cap times the largest element count we ever stream.
            if (e.contains("te")) {
                const auto& te = e["te"];
                r.touchParams.clear();
                if (te.is_array() && te.size() <= (size_t)MAX_TRACKED_COLLIDERS * MAX_COLLIDER_ELEMENTS * 2) {
                    r.touchParams.reserve(te.size());
                    for (const auto& v : te) {
                        r.touchParams.push_back(v.get<uint32_t>());
                    }
                } else {
                    SPDLOG_WARN("[EnemySync] Bad te size {} for key={:#x}", te.size(), key);
                }
            }
            // The three attack-parameter arrays are NOT cleared when absent: unlike
            // cf/bdf they are sent on change plus a keyframe a second, so most packets
            // carry none of them and clearing would blank an armed enemy 19 frames out
            // of 20. A peer that never sends them (older build) simply leaves them
            // empty, which is the fall-back-to-local-flags path. An enemy that loses
            // its colliders sends them empty EXPLICITLY, which does clear them.
            if (e.contains("tec")) {
                const auto& tec = e["tec"];
                r.colProfile.clear();
                if (tec.is_array() && tec.size() <= (size_t)MAX_TRACKED_COLLIDERS) {
                    r.colProfile.reserve(tec.size());
                    for (const auto& v : tec) {
                        r.colProfile.push_back(v.get<uint32_t>());
                    }
                } else {
                    SPDLOG_WARN("[EnemySync] Bad tec size {} for key={:#x}", tec.size(), key);
                }
            }
            if (e.contains("cr")) {
                const auto& cr = e["cr"];
                r.colRadius.clear();
                if (cr.is_array() && cr.size() <= (size_t)MAX_TRACKED_COLLIDERS) {
                    r.colRadius.reserve(cr.size());
                    for (const auto& v : cr) {
                        r.colRadius.push_back(v.get<int32_t>());
                    }
                } else {
                    SPDLOG_WARN("[EnemySync] Bad cr size {} for key={:#x}", cr.size(), key);
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
            // ...and never for one that is dying: its death is already in flight,
            // and the mirror that just destroyed its copy would spawn a fresh one
            // from the re-announce (observed in Hyrule Field: Stalchildren coming
            // back on one machine only, a second after the other killed them).
            bool rebroadcast =
                st.dynamicKey && !st.projectile && !st.dying && (tickCounter % SPAWN_REBROADCAST_TICKS) == 0;
            // Relax the health>0 gate for projectiles (they spawn with health 0) and
            // for adapter-declared healthless actors (energy ball, decoy riders).
            if ((st.needsSpawnBroadcast || rebroadcast) &&
                (actor->colChkInfo.health > 0 || st.projectile || Healthless(actor))) {
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
extern "C" s32 gActorInitDepth; // z_actor.c: > 0 while an actor's Init runs

static void OnEnemyActorSpawn(Actor* actor) {
    if (!(IsTrackedCategory(actor) || IsSyncedProjectile(actor)) || gPlayState == NULL || IsTrackingExcluded(actor) ||
        !MirroringEnabled()) {
        return;
    }
    if (gPlayState->numSetupActors != 0) {
        return;
    }
    // Children spawned from inside another actor's Init (Tailpasaran: 7 tails
    // per head, spawned by EnTp_Init) exist identically on every client — the
    // mirror's Init is never suppressed — so they keep deterministic occurrence
    // keys exactly like room-listed actors. A dynamic key here would broadcast
    // an ENEMY_SPAWN the other side materialises as a second, headless copy
    // (head == NULL crash in EnTp_Tail_FollowHead / EnTp_UpdateDamage the frame
    // the copy wakes for its death). Room-list drains already pass above; this
    // covers heads whose Init was deferred until their object finished loading.
    if (gActorInitDepth > 0) {
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
    // Barinade stumps (16..18) and door (19): spawned locally by the
    // support-cut replay / intro on every client (BossVaAdapter) — a remote
    // copy would double them.
    if (actorId == ACTOR_BOSS_VA && (int16_t)params >= 16) {
        return;
    }
    // En_Bb flame-trail links: excluded from tracking above, so a current peer no
    // longer announces them. Refused here too because the sender is the one that
    // decides, and a peer on an older binary would still crash us -- the replica
    // has no targetActor and EnBb_FlameTrail dereferences it unguarded.
    if (actorId == ACTOR_EN_BB && ((int16_t)params == ENBB_P_FLAME_TRAIL || (int16_t)params == ENBB_P_KILL_TRAIL)) {
        return;
    }
    // Dark Link: excluded from tracking above, because En_Blkobj spawns one on every
    // client by itself. Refused here for the same reason as the En_Bb trails -- a peer
    // on an older binary still announces it, and the copy would double ours.
    if (actorId == ACTOR_EN_TORCH2) {
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
        if (parent != keyToActor.end() && parent->second->update != NULL) {
            actor->parent = parent->second; // a killed parent would be freed under the child
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
            authority[e["key"].get<uint64_t>()] = { JsonBool(e, "alive"), e["hp"].get<uint8_t>() };
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

// Authority-side capture of an actor's sound (see Anchor_OnActorSfx below).
void NoteActorSfx(Actor* actor, uint16_t sfxId) {
    if (actor == nullptr || !SyncEnabled() || !MirroringEnabled() || gPlayState == NULL || !IsLocalAuthority()) {
        return;
    }
    auto it = tracked.find(actor);
    if (it == tracked.end() || it->second.projectile || it->second.dying) {
        return;
    }
    const ActorSyncAdapter* adapter = GetAdapter(actor->id);
    if (adapter != nullptr && adapter->ownsSfx) {
        return; // its own mirror path already replays this actor's sounds
    }
    if (it->second.pendingSfx.size() < MAX_STREAMED_SFX) {
        it->second.pendingSfx.push_back(sfxId);
    }
}

static void OnEnemyActorUpdate(Actor* actor) {
    auto it = tracked.find(actor);
    if (it == tracked.end()) {
        return;
    }
    TrackedState& state = it->second;

    // How far this copy's own state machine has advanced. A remote-spawned enemy
    // is suppressed within a frame or two of spawning, so its actionFunc is still
    // the spawn/intro one; released for death, it resumes THAT (a Stalchild
    // rising from the ground ignores every hit) instead of dying. See
    // ApplyRemoteDeath.
    if (!state.suppressed && !state.dying && state.localUpdates < UINT16_MAX) {
        state.localUpdates++;
    }

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
            ESYNC_LOG("[EnemySync] FORCE-KILL id={} key={:#x} hp={} (death never consumed)", actor->id, state.key,
                      actor->colChkInfo.health);
            actor->colChkInfo.health = 0;
            // Its own death VFX needed the death state we are skipping. Stand in
            // with the game's standard defeat flame at the body centre so the kill
            // still reads as one on this screen.
            Vec3f zero = { 0.0f, 0.0f, 0.0f };
            Vec3f at = actor->focus.pos;
            if (at.x == 0.0f && at.y == 0.0f && at.z == 0.0f) {
                at = actor->world.pos;
            }
            EffectSsDeadDb_Spawn(gPlayState, &at, &zero, &zero, 130, 20, 255, 255, 150, 170, 255, 0, 0, 1, 9, true);
            // ...and the drop went with that death state. Collectibles are NOT
            // replicated in this project: every client rolls its own from its own
            // copy's death (CarrySync.h states the discipline), which is why the
            // two screens can already show a different rupee. Rolling it here keeps
            // that model instead of leaving this player with nothing at all.
            // Skipped for bosses (adapter-driven, they drop through their cutscene)
            // and for projectiles, which never drop.
            if (GetAdapter(actor->id) == nullptr && !state.projectile) {
                Item_DropCollectibleRandom(gPlayState, actor, &actor->world.pos, 0x10);
            }
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

        // A lethal blow already travels as ENEMY_DIED, which sets the mirror's
        // health and injects its own lethal hit. Sending the ENEMY_HIT as well
        // made the mirror consume TWO synthetic hits for one sword swing: the
        // first landed as a non-lethal flinch (the copy bent over and got back
        // up) and only the second killed it, a visible half-second behind the
        // authority. Health 0 = the death packet owns this blow.
        if (localShare > 0 && health == 0) {
            ESYNC_LOG("[EnemySync] HP drop id={} key={:#x} lethal — hit folded into the death packet",
                      actor->id, state.key);
        }
        if (localShare > 0 && health > 0) {
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

    // Healthless adapter actors (their health is 0 by construction): an
    // Actor_Kill is never a death to replay through a lethal synthetic hit — it
    // is a removal, mirrored as a despawn when the adapter asks for it, whatever
    // the key kind (a decoy rider's horse carries an occurrence key).
    if (Healthless(actor)) {
        const ActorSyncAdapter* adapter = GetAdapter(actor->id);
        if (state.deathCooldown == 0 && MirroringEnabled() && IsLocalAuthority() && adapter->ReplicateKill != nullptr &&
            adapter->ReplicateKill(actor)) {
            state.deathCooldown = DEATH_COOLDOWN_FRAMES;
            Anchor::Instance->SendPacket_EnemyDespawn(state.key);
        }
        return;
    }

    // Only broadcast real deaths (health exhausted), not despawns/cleanup kills
    if (state.deathCooldown == 0 && actor->colChkInfo.health == 0) {
        state.deathCooldown = DEATH_COOLDOWN_FRAMES;
        Anchor::Instance->SendPacket_EnemyDied(actor, state.key, /*permanent=*/true, CurrentHitDmgFlags(state));
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
    ESYNC_LOG("[EnemySync] DEFEAT id={} key={:#x} -> broadcasting death", actor->id, state.key);
    // Defeat-hook deaths (regrowers like En_Karebaba never Actor_Kill): the
    // receiver must not force-kill if its copy can't consume the lethal hit.
    Anchor::Instance->SendPacket_EnemyDied(actor, state.key, /*permanent=*/false, CurrentHitDmgFlags(state));
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

// Called from z_actor.c's actor-sound wrappers. On the authority, records the
// sound so mirrors can replay it on their own copy (their AI is suppressed and
// would otherwise stay mute). No-op everywhere else, including on a mirror
// replaying a streamed sound, so this can never echo back.
extern "C" void Anchor_OnActorSfx(Actor* actor, uint16_t sfxId) {
    EnemySync::NoteActorSfx(actor, sfxId);
}

// For overlays that DRIVE another actor from the outside — a trigger volume
// steering an enemy, one actor repositioning another. Those helpers run on every
// client, but if their target is a suppressed mirror their writes fight the
// stream: the pin restores the streamed pose each frame while the local helper
// keeps shoving it somewhere else. Ask this before writing into another actor.
extern "C" s32 Anchor_IsMirroredActor(Actor* actor) {
    return (actor != NULL && EnemySync::IsSuppressed(actor)) ? 1 : 0;
}
