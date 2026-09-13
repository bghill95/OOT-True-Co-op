#include "DynaSync.h"
#include "Anchor.h"
#include "EnemySync.h" // PackKey, authority election, ESYNC_LOG
#include "JsonConversions.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Bg_Po_Event/z_bg_po_event.h"
#include "src/overlays/actors/ovl_Bg_Jya_Cobra/z_bg_jya_cobra.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Rock/z_bg_hidan_rock.h"
#include "src/overlays/actors/ovl_Bg_Ice_Objects/z_bg_ice_objects.h"
#include "src/overlays/actors/ovl_Bg_Gnd_Iceblock/z_bg_gnd_iceblock.h"
#include "src/overlays/actors/ovl_En_Trap/z_en_trap.h" // SPIKETRAP_MODE_*, for the pinned split
extern PlayState* gPlayState;
}

namespace DynaSync {

// Authority resends every tracked actor at this cadence even when idle, so
// mirrors stay suppressed while an authority is present (a mirror falling back
// to local AI on an idle-but-diverged platform would drift, e.g. a sunk
// Ydan_Hasi plank floating back up). Must be < STALE_TICKS.
static constexpr uint64_t KEYFRAME_TICKS = 20;

// A mirrored actor whose stream went quiet this long reverts to local AI
// (authority left/disconnected — same discipline as EnemySync's stale path).
static constexpr uint64_t STALE_TICKS = 45;

// Positions are floats driven by AI math; ignore sub-visible jitter.
static constexpr float POS_EPSILON = 0.01f;

// A local push (dyna.unk_150, written only by OUR player) claims a block; the
// claim stays fresh while pushing and while the block still slides, plus this
// grace window. Streamed with "own":1 so peers (the scene authority included)
// mirror the claimant instead of running their own copy.
static constexpr uint64_t CLAIM_TICKS = 45;

// Whitelist: scene-placed DynaPoly actors whose MOTION should mirror.
// Grow this table dungeon by dungeon; never add flag-driven state here -- a flag
// that replicates already puts both clients on the same motion, and streaming the
// position on top of it only fights the local state machine. That rule was broken
// once, by BG_MIZU_MOVEBG: the Water Temple platforms take their height straight
// from the LOCAL waterBoxes, so pinning the mirror to the authority's position
// dragged a rider up to 700 units through his own water surface. It is now split
// by type (see its case below), and BG_MIZU_UZU is out entirely (2 instances in
// the game: MIZUsin and men) -- its whole
// Update is the iron-boots collision toggle plus a spin and a sound, and its Draw
// is empty, so there was never anything to mirror.
// What remains excluded from flag replication: the forest elevator and the tower
// collapse, in SetFlag.cpp.
static bool IsSyncedDynaActor(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_BG_YDAN_HASI:      // Deku Tree: water plank + seesaw
        case ACTOR_BG_DDAN_JD:        // Dodongo's Cavern: cycling pillars/platform
        case ACTOR_BG_HAKA_TRAP:      // Shadow Temple: guillotines / spike traps (cyclic)
        case ACTOR_BG_HAKA_MEGANEBG:  // Shadow Temple: moving invisible platforms
        case ACTOR_BG_HIDAN_RSEKIZOU: // Fire Temple: rotating spiked statues
        case ACTOR_BG_JYA_1FLIFT:     // Spirit Temple: 1F elevator (see the ride note below)
        case ACTOR_BG_JYA_LIFT:       // Spirit Temple: chain lift
        case ACTOR_BG_JYA_ZURERUKABE: // Spirit Temple: sliding walls (cyclic)
        case ACTOR_BG_MENKURI_KAITEN: // GTG: rotating ring platform (see the ride note below)
        case ACTOR_BG_RELAY_OBJECTS:  // Windmill: rotating center structure
        case ACTOR_BG_SPOT01_FUSYA:   // Kakariko: windmill sails seen from outside —
                                      // cosmetic, NO DynaPoly, so it is fed to
                                      // OnDynaRegistered by an OnActorInit hook instead
        case ACTOR_BG_SPOT18_BASKET:  // Goron City: spinning vase
        case ACTOR_BG_HAKA_SHIP:      // Shadow Temple: boat (flag then scripted sail — flag
                                      // replay times differ, the hull must not)
        case ACTOR_EN_SIOFUKI:        // WATER TEMPLE: water-spout platforms (flag + timer +
                                      // oscillation). Counted in the scene data: 6 instances,
                                      // all in MIZUsin, NONE in Jabu-Jabu -- this comment said
                                      // "Jabu" until 2026-09-11 and was simply wrong.
        case ACTOR_EN_BROB:           // Jabu: wobbling blob platform (timers + proximity).
                                      // 7 instances, all in bdan -- verified.
            // NOT one-shot switch-flag movers (Bg_Ddan_Kd stairs, Hamsteps, gates,
            // shutters...): Anchor's flag sync already replays those on both
            // clients. NOT flag-driven movers at all, replicated flag or not.
            // NOT pushblocks (phase B, needs
            // per-actor claim). NOT Obj_Lift collapsing platforms (per-player
            // stand trigger). NOT boss-room platforms (adapter territory).
            return true;

        // The other half of the Bg_Hidan_Sima split. params 0 is the rider-driven
        // platform and belongs to IsRideClaimable; every other value runs
        // func_8088E760, a pure timer loop that swings the platform around home.pos
        // and arms the fire jet. Without this case it fell out of tracking
        // ALTOGETHER when it left the ride list -- IsClaimableDynaActor falls
        // through to IsRideClaimable, so OnDynaRegistered dropped it and each
        // client swung it on a phase set by its own room load.
        case ACTOR_BG_HIDAN_SIMA:
            return actor->params != 0;

        // The Bg_Bdan_Objects split, the study the note below asked for. One overlay,
        // four params, three different answers:
        //   params 0  Big Octo platform  -> RIDE. func_8086C76C moves it +965 only if
        //             IsPlayerOnTop, i.e. the trigger is in the branch that moves it.
        //   params 3  descending platform -> RIDE. func_8086CB10, same shape: the
        //             trigger sets the flag AND drops home.pos.y by 200.
        //   params 1  elevator            -> synced, here. func_8086C874 drives it with
        //             sinf(timer) unconditionally; IsPlayerOnTop only picks the camera
        //             setting, so it is NOT a ride (the Bg_Jya_1flift mistake).
        //   params 2  water level         -> returns before DynaPoly_SetBgActor, so it
        //             is tracked by nobody, and it does not need to be: its flag is
        //             re-read every frame in func_8086C9A8 and converges on its own.
        case ACTOR_BG_BDAN_OBJECTS:
            return actor->params == 1;

        // Zora's Fountain ice floes. Free-phase drifters: Bobbing runs from room load
        // with a randomly seeded phase, and the orbiting variant TRANSLATES on a
        // radius-100 circle, so two clients can be 200 units apart on the route to the
        // Ice Cavern. NOT a ride -- Roll and SinkUnderPlayer do read the trigger, but
        // sinking four units is the rider's CONTRIBUTION, not what moves the platform
        // (same arbitration as EN_SIOFUKI / EN_BROB above). Accepted: the mirror only
        // sees a floe dip under the authority's Link.
        //
        // The orbiting TWINS (low nibble 3) are excluded, and syncing them would be
        // worse than leaving them alone. PosKey is the quantised spawn position plus
        // the actor id, with no instance discriminant -- and BgSpot08Iceblock_Init
        // registers its DynaPoly BEFORE SpawnTwinFloe, which spawns the second floe at
        // the parent's own home.pos and only then offsets the parent. Both halves
        // therefore register at the same rounded position with the same id: one key for
        // two actors. They would share a RemoteDynaState and the mirror would pin both
        // floes on top of each other, leaving nothing at all at the opposite point of
        // the orbit -- where the peer is standing on one. A drop into the water instead
        // of a phase offset.
        case ACTOR_BG_SPOT08_ICEBLOCK:
            return (actor->params & 0xF) != 3;

        // Water Temple moving platforms. Types 0-6 take their height straight from the
        // LOCAL waterBoxes or from a replicated flag, so streaming a position on top of
        // them only fights the local state machine -- that is what dragged a rider 700
        // units through his own water surface. Type 7 is a different actor wearing the
        // same overlay: func_8089E650 walks a scene path with Actor_MoveXYZ, no flag, no
        // water, no resync point, from the moment the room loads. Exactly the free-phase
        // signature BG_MORI_HASHIRA4 is here for -- and it is a platform you stand on.
        // Counted in the scene data (2026-09-11): 17 instances in the whole game.
        // Types 0-5 are the eight Water Temple platforms this case excludes, all in
        // MIZUsin rooms 0/1/3/6. Type 7 is nine instances -- eight in MIZUsin room 5
        // and ONE in jyasinzou (Spirit Temple) room 23, so this case reaches beyond
        // the Water Temple; it is the same free-phase path walker there, so the
        // treatment holds, but do not read this case as dungeon-scoped.
        case ACTOR_BG_MIZU_MOVEBG:
            return (((uint16_t)actor->params >> 0xC) & 0xF) == 7; // MOVEBG_TYPE, private to the overlay

        case ACTOR_BG_MORI_HASHIRA4:
            // Forest Temple, the four rotating pillars (params 0 after Init masks
            // it): BgMoriHashira4_PillarsRotate does
            //     shape.rot.y = world.rot.y += 0x96
            // every frame, unconditionally, from the moment the room loads. No
            // flag, no trigger, no resync point — so the phase is decided by WHEN
            // each client loaded the room and the offset never converges. Same
            // family as BG_SPOT08_ICEBLOCK above.
            // The other params are the gates: one-shot switch-flag movers the flag
            // sync already replays on both clients, so they stay out.
            return actor->params == 0;

        default:
            return false;
    }
}

// PINNED: free-phase OBSTACLES whose position must agree but whose Update must keep
// running on the mirror. Third regime, and it exists because `synced` is the wrong
// answer for a hazard.
//
// `synced` suppresses the mirror's Update. For a platform that costs nothing. For
// these two actors the Update IS the hazard: it is where the collider is submitted
// and where the damage is dealt --
//   En_Goroiwa  z_en_goroiwa.c:761-767, the only CollisionCheck_SetAT/SetOC in the
//               overlay, plus the knockback in EnGoroiwa_Roll (:605-607) and the
//               reverse/wait state machine;
//   En_Trap     z_en_trap.c:173-179, `play->damagePlayer(play, -4)` + func_8002F7A0
//               inline in the Update on a plain xzDistToPlayer test -- there is no AT
//               collider at all -- plus the SetOC/SetAC at :384-386.
// Classed `synced` they would stop hurting the mirroring player ALTOGETHER: a boulder
// and five blades that are lethal for whoever happens to hold the scene authority and
// purely decorative for the other. That is the BG_HAKA_TUBO defect this file already
// paid for once (see IsClaimableDynaActor: the small-key jar no mirror could break).
//
// `claimable` -- which is exactly "pin the position, keep the Update running" -- is
// not available to them either: its claim signal is ((DynaPolyActor*)actor)->unk_150
// (push) or ->interactFlags (ride), and NEITHER actor is a DynaPolyActor. Grep both
// overlays: no DynaPoly_SetBgActor, no DynaPolyActor in the struct. That cast would
// read past the end of a plain Actor. Hence a separate regime rather than a reuse.
//
// So: pin world.pos/shape.rot from the authority's stream every frame and leave
// *should alone. The mirror's own AI advances at most one update from the pinned
// value before being pinned again, and each client keeps hurting ITS OWN Link from a
// position both clients now agree on. Neither actor registers DynaPoly, so both are
// fed to OnDynaRegistered by an OnActorInit hook, like BG_SPOT01_FUSYA.
//
// NOT a place for player-REACTIVE obstacles: see the En_Trap split below. Pinning one
// of those is strictly worse than leaving it alone, because its correct behaviour is
// already per-client.
static bool IsPinnedDynaActor(const Actor* actor) {
    switch (actor->id) {
        // Rolling boulders. EnGoroiwa_Init ends on EnGoroiwa_SetupRoll (:558), so the
        // boulder starts down its scene path the instant the room loads: the phase is
        // decided by WHEN each client loaded the room and never converges -- the
        // BG_MORI_HASHIRA4 signature. On top of that the trajectory is PLAYER-DEPENDENT
        // (EnGoroiwa_Roll reverses on AT_HIT, :589-599), so the two copies cannot be
        // kept in step by determinism alone; the position has to be streamed.
        // All 20 placements are the same free-phase path roller, so the whole id
        // qualifies -- counted in the scene data: HIDAN r5 x6 / r10 x3 / r13 x1,
        // MIZUsin r12 x2, ganontika r8 x2, men r2 x2, spot04 x1, spot09 x1, spot16 x2.
        // The overlay was already patched for co-op twice (the Anchor_IsSessionActive
        // pause at :743 and the AT_HIT owner guard at :605); both removed a DIVERGENCE
        // SOURCE, neither aligns the phase. This does.
        case ACTOR_EN_GOROIWA:
            return true;

        // Spike traps, but only the two free-phase modes. EnTrap_Init/EnTrap_Update
        // branch on the mode bits of the low params byte (params &= 0xFF at :72):
        //   LINEAR (0x10)   shuttles at a constant speed from the room load, reversing
        //                   on a wall or a poly 30 units ahead -> free phase.
        //   CIRCULAR (0x20) vAngularPos += vAngularVel every update from the room load
        //                   (:211-212) -> free phase, the EN_TRAP twin of
        //                   BG_SPOT08_ICEBLOCK's orbiting floe.
        //   FOUR-WAY        neither bit (0x30 would take the LINEAR branch first, so it
        //                   is unreachable; the engine's own test is
        //                   `!(params & (LINEAR | CIRCULAR))`, :168). It SITS
        //                   AT home.pos and lunges at whoever comes within 200 units,
        //                   picking its direction from yawTowardsPlayer (:313-319).
        //                   That is not free phase, it is per-player by design and it
        //                   is already CORRECT on both clients. Pinning it would aim
        //                   P2's blade at P1 and never at P2. EXCLUDED.
        // Counted in the scene data: 35 placements (24 LINEAR, 8 CIRCULAR, 3 FOUR-WAY);
        // an earlier comment here said 27, which silently dropped the 8 circular ones.
        // The only four-way ones are
        // the three in ddan r9 (params 0x5F01 / 0x6F01 / 0x5F09). The five of
        // ganontika r17 (0x0410, 0x0610 x3, 0x0A10) are all LINEAR.
        //
        // PosKey checked, because BG_SPOT08_ICEBLOCK's twin floes are the standing
        // warning here: across all 35 En_Trap and all 20 En_Goroiwa placements exactly
        // ONE pair shares a spawn position, the two circular traps of HAKAdan r9, both
        // at (2110, -1343, 3368) with params 0x1220 and 0x1620. They do NOT collide,
        // and not by luck of rounding: EnTrap_Init places a circular trap at
        // home + (0, 0, radius) with radius = (upperParams & 0xF) * 40 (:79-82), i.e.
        // z+80 and z+240, and OnActorInit runs after Init. No other pair is even close.
        //
        // CIRCULAR IS EXCLUDED TOO, and that is a correction to the first version of
        // this function, which pinned it. Pinning a circular trap does NOTHING: the
        // hook runs at z_actor.c:2703 and the actor's own Update at :2704, with
        // nothing in between, and EnTrap_Update does not ADVANCE a circular trap, it
        // RECOMPUTES it absolutely --
        //     world.pos.x = vRadius * SinS(vAngularPos) + home.pos.x   (:217)
        //     world.pos.z = vRadius * CosS(vAngularPos) + home.pos.z   (:218)
        //     world.pos.y = floorHeight                                (:219)
        // -- so the pinned position is overwritten on all three axes in the same
        // frame, before Collider_UpdateCylinder and before the Draw. The phase gap
        // the regime exists to close is exactly as wide as before, and we would be
        // paying a DYNA_STATE entry per tick per trap for it.
        //
        // LINEAR is not in that case and genuinely works: it moves through
        // Actor_MoveXZGravity (:372) with x/z restored from posTemp (:374-380), i.e.
        // purely incremental, so the pin adds on top.
        //
        // What a circular trap actually needs on the wire is vAngularPos, not a
        // position — the rest of its equation is identical on every client (home.pos
        // from the scene, vRadius and vAngularVel from the params). Classing it
        // `synced` is NOT the answer even though BG_SPOT08_ICEBLOCK's floe is synced:
        // the floe is a platform, while a suppressed Update here removes the inline
        // play->damagePlayer at :177 and leaves a decorative blade. 8 placements stay
        // out of sync until someone streams the angle: jyasinzou r0/r1/r27,
        // ice_doukutu r1/r9, men r9, HAKAdan r9 x2.
        case ACTOR_EN_TRAP:
            return (actor->params & SPIKETRAP_MODE_LINEAR) != 0;

        default:
            return false;
    }
}

static bool IsRideClaimable(const Actor* actor);

// Claimable: movable by WHOEVER touches it (push blocks). Motion is streamed
// by the claimant instead of the scene authority while a claim is fresh.
static bool IsClaimableDynaActor(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_BG_PO_EVENT: // Forest Temple: Amy's picture blocks (the paintings share the id, type >= 2)
            return ((const BgPoEvent*)actor)->type < 2;
        // Fire Temple stone. Only type 0 is the push block whose claim signal is
        // dyna.unk_150; the other type is a launch pad triggered by standing on it,
        // so it belongs to the ride list below and never claims by push force.
        // Left claimable, it could not claim at all: pinned at rest by the stream
        // while its state machine kept running, it stuck in the "rising" state,
        // where unk_16C = (pos.y + 50 - home.y + 40) / 80 evaluates to 1.125 with
        // pos.y == home.y -- and BgHidanRock_Update submits an AT cylinder every
        // frame that unk_16C is positive. A stone at rest became a permanent pillar
        // of fire on the mirror, for anyone who had stepped on it once.
        case ACTOR_BG_HIDAN_ROCK:
            return ((const BgHidanRock*)actor)->type == 0 ? true : IsRideClaimable(actor);
        // Shadow Temple skull jars. NOT a push block -- it is here for the one thing
        // `claimable` grants that `synced` does not: the update KEEPS RUNNING on the
        // mirror. It was synced, and the whole of BgHakaTubo_Idle sat in the
        // suppressed update: the three CollisionCheck_Set{AC,AT,OC} calls
        // (z_bg_haka_tubo.c:141-143), the break test, the ITEM00_SMALL_KEY drop
        // (:206) and the Actor_Kill (:216). On the mirror the jar submitted NO
        // collider at all -- sword, boomerang and bombs went through it -- so the
        // jar that holds a small key was unbreakable for anyone who was not the
        // scene authority, and the flame circle it draws was harmless while the
        // authority's copy burned. (The key is the `else` branch, i.e. any room but
        // 12; the three spinning jars of room 12 drop rupees, ammo or a Keese.)
        // Spinning stays streamed: the room-12 jars rewrite
        // world.pos/shape.rot from their own rot every frame, and the pin is applied
        // BEFORE the update, so the mirror runs at most one frame of its own spin
        // from the authority's value -- it cannot drift. The push claim is harmless
        // here (nobody rotates a jar by walking into it, and a claim only moves who
        // streams the same circle). Breaking is replicated on its own channel, in
        // BreakSync: the pin makes the jar hittable, BreakSync makes the two copies
        // die together.
        case ACTOR_BG_HAKA_TUBO:
            return true;
        // Push blocks. NOT "every dungeon", as this said until 2026-09-11: counted
        // in the scene data, 31 instances over 8 scenes (jyasinzou 9, ddan 6,
        // MIZUsin 5, Bmori1 4, HAKAdan 3, men 2, HIDAN 1, ydan 1) and absent from
        // Jabu-Jabu, the Well, the Ice Cavern and Ganon's Castle/Tower -- the last
        // two have BG_GND_ICEBLOCK instead, two lines below. Every placed type is a
        // plain push block, so claimable is right for the whole id.
        case ACTOR_OBJ_OSHIHIKI:
        case ACTOR_BG_MORI_KAITENKABE: // Forest Temple: push-rotated basement wall
        case ACTOR_BG_JYA_COBRA:       // Spirit Temple: push-rotated cobra mirror
        case ACTOR_BG_ICE_OBJECTS:     // Ice Cavern: sliding ice blocks
        case ACTOR_BG_GND_ICEBLOCK:    // Ganon's Castle: sliding ice blocks
        case ACTOR_BG_PUSHBOX:         // generic push crate
        case ACTOR_BG_SPOT15_RRBOX:    // Lon Lon Ranch: push crate
        case ACTOR_BG_HAKA:            // Graveyard/tombs: pushable gravestones
        case ACTOR_BG_SPOT18_OBJ:      // Goron City: push statue (latches a flag once placed)
            return true;
        default:
            return IsRideClaimable(actor);
    }
}

// Ride-claimables: platforms driven by whoever STANDS on them. The trigger is
// DYNA_INTERACT_PLAYER_ON_TOP, written by our own Player's bgcheck (a puppet
// never sets it), never dyna.unk_150. Unlike push blocks their update rewrites
// world.pos from private state every frame (Obj_Bean replays its path points,
// Mori_Elevator picks a target from OUR room and flag), so a mirror must be
// suppressed outright: the pin cannot fight the update. Our rider still claims
// it — interactFlags are read in the hook, before the update would run.
// THE RULE THIS LIST ENCODES, learned the expensive way (Fire Temple audit,
// 2026-09-06): a dyna actor whose Update reads DynaPolyActor_IsPlayerOnTop or
// IsPlayerAbove belongs HERE, never in IsSyncedDynaActor. Classed `synced`, the
// platform moves for NOBODY. The mirror never evaluates the test because its
// Update is suppressed; and the authority's own test is false too, because a
// puppet never writes DYNA_INTERACT_PLAYER_ON_TOP -- DynaPoly_SetPlayerOnTop and
// SetPlayerAbove have exactly two callers, both in z_player.c, and
// DummyPlayer_Update never calls Actor_UpdateBgCheckInfo. The player who steps on
// it is stuck until the OTHER one steps on it too.
//
// This list had been grown case by case and never swept, so five actors sat in
// the synced list with that exact signature. Before adding anything to
// IsSyncedDynaActor, grep its overlay for those two functions.
//
// Deliberately NOT moved here, though they do read the trigger -- each needs its
// own study rather than a blanket reclassification, and a wrong `ride` is as bad
// as a wrong `synced`. BG_BDAN_OBJECTS was on this list; the study got done and
// params 0 and 3 are now rides below. What is left:
//   BG_HAKA_SHIP      the voyage is scripted, the trigger appears only in the
//                     final CrashFall; making the whole ship a ride would hand
//                     the scripted run to whoever stands on it.
//   BG_MENKURI_KAITEN the trigger's output is a switch flag, which already
//                     replicates; the rotation itself is not rider-driven.
//   EN_SIOFUKI, EN_BROB  both sit in IsSyncedDynaActor above and do read the
//                     trigger, but their motion is driven by a flag, a timer and
//                     an oscillation -- standing on them is an extra input, not
//                     what makes them move -- so `ride` would hand the wrong
//                     thing to the rider.
//   BG_JYA_1FLIFT     read the trigger, and was briefly moved here, wrongly. Its
//                     tempIsRiding only chooses between CAM_SET_FIRE_PLATFORM and
//                     CAM_SET_DUNGEON0; the motion is BgJya1flift_Move ->
//                     DelayMove -> ChangeDirection, a timed round trip started by
//                     a switch flag. Same shape as the two above.
//
// READING THE TRIGGER IS NOT ENOUGH -- it has to be the branch that MOVES the
// platform, in the branch this instance actually runs (see BG_HIDAN_SIMA, where
// only params 0 qualifies). That distinction cost a wrong classification here.
static bool IsRideClaimable(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_OBJ_BEAN: // magic bean leaf platform (adult); the child soil never moves
            return LINK_AGE_IN_YEARS == YEARS_ADULT;
        case ACTOR_BG_MORI_ELEVATOR:  // Forest Temple elevator (flag 0x1B stays per-client by design)
        case ACTOR_OBJ_ELEVATOR:      // stone elevator (Gerudo Training Ground), toggles on each rider
        case ACTOR_BG_HIDAN_SYOKU:   // Fire Temple: elevator pillar, rises under its rider
        case ACTOR_BG_HIDAN_FSLIFT:  // Fire Temple: hookshot elevator, rises while ridden
        case ACTOR_BG_GND_FIREMEIRO: // Ganon's Castle fire trial: sinks, shakes and rises under its rider
            return true;
        // Only params 0 is the platform that sinks under its rider: BgHidanSima_Init
        // installs the two states that read IsPlayerOnTop for that value alone. Any
        // other params gets func_8088E760, a pure timer loop that swings the platform
        // around home.pos and arms the fire jet -- no rider involved. Reading the
        // trigger somewhere in the file is not enough; it has to be the branch this
        // instance actually runs.
        case ACTOR_BG_HIDAN_SIMA:
            return actor->params == 0;
        // Jabu-Jabu, the platform half of Bg_Bdan_Objects -- see the split in
        // IsSyncedDynaActor. params 0 rises 965 units under its rider (func_8086C76C),
        // params 3 drops 200 (func_8086CB10); in both the trigger IS the branch that
        // moves it. As synced they moved for nobody: the mirror never ran the test, and
        // the authority's test is false for the puppet, which writes no PLAYER_ON_TOP.
        // Making params 0 a ride also revives its OnePointCutscene and the spike
        // platform's only CollisionCheck_SetAT, both of which sat in the same
        // suppressed Update.
        case ACTOR_BG_BDAN_OBJECTS:
            return actor->params == 0 || actor->params == 3;
        case ACTOR_BG_HIDAN_ROCK: // Fire Temple: the non-zero type, a launch pad
            return ((const BgHidanRock*)actor)->type != 0;
        default:
            return false;
    }
}

struct TrackedDyna {
    uint64_t key = 0;
    Vec3f lastSentPos{};
    Vec3s lastSentRot{};
    bool everSent = false;
    bool suppressed = false;
    bool claimable = false;
    bool ride = false;          // claimed by standing on it, mirrored with update suppressed
    bool pinned = false;        // position mirrored, update NEVER suppressed (see IsPinnedDynaActor)
    bool hadUncullFlag = false; // actor's own culling setting, restored on release
    uint64_t localClaimTick = 0; // last tick our player pushed/moved it
    uint64_t softClaimTick = 0;  // last tick a module asked us to keep driving it (HoldSoftClaim)
    Vec3f prevPos{};             // motion detection for claim upkeep
    Vec3s prevRot{};             // idem — push-rotated walls/mirrors move rot only
};

// Latest streamed state per key (game-thread only: written by the queued
// DYNA_STATE handler, read by the ShouldActorUpdate mirror step).
struct RemoteDynaState {
    uint64_t rxTick = 0;
    Vec3f pos{};
    Vec3s rot{};
};

// A soft claim ("own":2): the sender is running a scripted sequence on the
// actor (the Amy puzzle driver) and wants everyone to follow, but yields to a
// real push claim, and between two soft claimants the lower client id wins.
struct RemoteSoftClaim {
    uint64_t rxTick = 0;
    uint32_t senderId = 0;
};

static std::unordered_map<Actor*, TrackedDyna> sTracked;
static std::unordered_map<uint64_t, RemoteDynaState> sRemote;
static std::unordered_map<uint64_t, uint64_t> sRemoteClaims; // key -> last claimed-entry rx tick
static std::unordered_map<uint64_t, RemoteSoftClaim> sRemoteSoft; // key -> last soft-claimed entry
static uint64_t sTick = 0;

static bool Fresh(uint64_t tick, uint64_t window) {
    return tick != 0 && (sTick - tick) <= window;
}

// True when a fresh remote soft claim outranks our own (any remote soft claim
// when we hold none; the lower client id when both hold one).
static bool RemoteSoftWins(const TrackedDyna& st) {
    auto it = sRemoteSoft.find(st.key);
    if (it == sRemoteSoft.end() || !Fresh(it->second.rxTick, CLAIM_TICKS)) {
        return false;
    }
    if (!Fresh(st.softClaimTick, CLAIM_TICKS)) {
        return true;
    }
    return Anchor::Instance == nullptr || it->second.senderId < Anchor::Instance->ownClientId;
}

bool SyncEnabled() {
    return CVarGetInteger(CVAR_REMOTE_ANCHOR("DynaSync"), 1);
}

// Give an actor its own culling setting back. Whitelisted platforms are noisy
// (Shadow Temple guillotines, the windmill): left uncullable they keep running
// and playing SFX from across the map once syncing stops.
static void RestoreCull(Actor* actor, const TrackedDyna& st) {
    if (!st.hadUncullFlag) {
        actor->flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
    }
}

// Safe on both paths that call it: OnActorDestroy has already dropped every
// actor of the outgoing scene, so what is left in sTracked is still alive.
static void Reset() {
    for (auto& [actor, st] : sTracked) {
        RestoreCull(actor, st);
    }
    sTracked.clear();
    sRemote.clear();
    sRemoteClaims.clear();
    sRemoteSoft.clear();
}

// Identity = quantized spawn position + actor id, computed at Init before the
// actor ever moves. NOT ActorListIndex: the same scene loads different actor
// lists per setup layer (age, time of day), so the same platform can sit at a
// different index on each client (seen live: Kakariko windmill sails, index 6
// on one side, 5 on the other). Spawn positions are identical everywhere.
static uint64_t PosKey(const Actor* actor) {
    int16_t qx = (int16_t)lroundf(actor->world.pos.x);
    int16_t qy = (int16_t)lroundf(actor->world.pos.y);
    int16_t qz = (int16_t)lroundf(actor->world.pos.z);
    return ((uint64_t)(uint16_t)qx << 48) | ((uint64_t)(uint16_t)qy << 32) | ((uint64_t)(uint16_t)qz << 16) |
           (uint16_t)actor->id;
}

// Fires from DynaPoly_SetBgActor (z_bgcheck.c) during the actor's own Init
// (or from OnActorInit for whitelisted spinners with no DynaPoly).
static void OnDynaRegistered(Actor* actor) {
    if (!SyncEnabled() || sTracked.contains(actor) ||
        (!IsSyncedDynaActor(actor) && !IsClaimableDynaActor(actor) && !IsPinnedDynaActor(actor))) {
        return;
    }

    TrackedDyna st;
    st.key = PosKey(actor);
    st.claimable = IsClaimableDynaActor(actor);
    st.ride = IsRideClaimable(actor);
    st.pinned = IsPinnedDynaActor(actor);
    st.prevPos = actor->world.pos;
    st.hadUncullFlag = (actor->flags & ACTOR_FLAG_UPDATE_CULLING_DISABLED) != 0;
    sTracked[actor] = st;
    ESYNC_LOG("[DynaSync] tracked id={} params={:#x} key={:#x}", actor->id, (uint16_t)actor->params, st.key);

    // The update-culling gate in Actor_UpdateAll sits BEFORE the
    // ShouldActorUpdate hook: the authority needs the AI running while its
    // player is far, and the mirror needs the hook firing to apply the stream.
    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
}

// Mirror step, in the actor's own update slot. Fresh stream => apply + suppress;
// stale/no stream => local AI (never suppress into silence).
static void OnShouldDynaUpdate(Actor* actor, bool* should) {
    auto it = sTracked.find(actor);
    if (it == sTracked.end() || gPlayState == NULL) {
        return;
    }
    TrackedDyna& st = it->second;
    if (!SyncEnabled()) {
        // Kill switch flipped mid-scene: hand culling back, keep the entry so
        // flipping it on again re-arms on the next update the actor gets.
        RestoreCull(actor, st);
        return;
    }
    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;

    if (st.claimable) {
        // Claim upkeep: a nonzero push force means OUR player is moving it;
        // keep the claim while it still slides after release.
        bool moved = fabsf(actor->world.pos.x - st.prevPos.x) > POS_EPSILON ||
                     fabsf(actor->world.pos.y - st.prevPos.y) > POS_EPSILON ||
                     fabsf(actor->world.pos.z - st.prevPos.z) > POS_EPSILON ||
                     actor->shape.rot.x != st.prevRot.x || actor->shape.rot.y != st.prevRot.y ||
                     actor->shape.rot.z != st.prevRot.z;
        st.prevPos = actor->world.pos;
        st.prevRot = actor->shape.rot;
        // KNOWN, ACCEPTED: taking a ride claim can snap the platform.
        //
        // DYNA_STATE carries pos and shape.rot and nothing else, so timer,
        // actionFunc and the direction flags stay frozen at their Init values on a
        // mirror -- the stream refreshes faster than it goes stale, so the update
        // never resumes to advance them. Some of these actors RECOMPUTE world.pos
        // from home.pos instead of integrating it, so the first resumed frame can
        // jump to wherever the frozen phase says (up to 540 units on the Fire
        // Temple pillar), and the claimant then streams that jump.
        //
        // A guard requiring the platform to be near its registration position was
        // tried here and WITHDRAWN: it traps players. Obj_Elevator toggles to
        // home.pos.y + N*80 and STAYS up; Bg_Mori_Elevator parks at a fixed y of
        // its own. Neither is ever near its spawn point when you next step on it,
        // so the guard denied the claim, left the update suppressed, and left the
        // rider stuck on a platform that would not move -- exactly the block the
        // ride classification exists to remove, and strictly worse than a jump.
        // Fixing this properly means rebuilding each actor's phase from the pinned
        // position on resume, per actor, the way BG_HAKA and BG_JYA_COBRA already
        // do for their own fields further down.
        bool localSignal = st.ride ? (((DynaPolyActor*)actor)->interactFlags & DYNA_INTERACT_PLAYER_ON_TOP) != 0
                                   : fabsf(((DynaPolyActor*)actor)->unk_150) > 0.001f;
        if (localSignal || (Fresh(st.localClaimTick, CLAIM_TICKS) && moved && !st.suppressed)) {
            st.localClaimTick = sTick;
        }
        if (Fresh(st.localClaimTick, CLAIM_TICKS)) {
            st.suppressed = false;
            return; // we own it: local simulation, PerFrameTick streams it
        }
        // A remote push claim outranks everything else; otherwise a soft claim
        // we hold keeps our copy live unless a better-ranked peer holds one.
        auto rc = sRemoteClaims.find(st.key);
        bool remoteHard = rc != sRemoteClaims.end() && Fresh(rc->second, CLAIM_TICKS);
        if (!remoteHard && Fresh(st.softClaimTick, CLAIM_TICKS) && !RemoteSoftWins(st)) {
            st.suppressed = false;
            return;
        }
        // Unclaimed by us: mirror a fresh remote stream below — the scene
        // authority included (a claimant elsewhere outranks the authority).
    } else if (EnemySync::IsLocalAuthority()) {
        st.suppressed = false;
        return;
    }

    auto rs = sRemote.find(st.key);
    if (rs == sRemote.end() || (sTick - rs->second.rxTick) > STALE_TICKS) {
        if (st.suppressed) {
            st.suppressed = false;
            ESYNC_LOG("[DynaSync] stale key={:#x} -> local AI", st.key);
        }
        return;
    }

    actor->world.pos = rs->second.pos;
    actor->shape.rot = rs->second.rot;
    // Bg_Haka repurposes minVelocityY as its pull distance: the draw offsets the
    // dirt patch back by it (else the dirt slides along with the stone) and a
    // claim takeover recomputes pos from it. Reconstruct it from the streamed
    // displacement projected on the facing axis.
    if (actor->id == ACTOR_BG_HAKA) {
        // The pull axis is shape.rot.y FLIPPED: Bg_Haka sets world.rot.y =
        // shape.rot.y + 0x8000 when a pull starts — which never happens on the
        // mirror, so world.rot.y is not usable here. Set it too, so a claim
        // takeover by our player continues the pull instead of teleporting.
        int16_t pullYaw = (int16_t)(actor->shape.rot.y + 0x8000);
        actor->world.rot.y = pullYaw;
        actor->minVelocityY = (actor->world.pos.x - actor->home.pos.x) * Math_SinS(pullYaw) +
                              (actor->world.pos.z - actor->home.pos.z) * Math_CosS(pullYaw);
    }
    // Bg_Jya_Cobra keeps its rotation as a DETENT INDEX, not as an angle:
    // unk_16C counts eighths and the actor recomputes
    //   shape.rot.y = unk_16C * 0x2000 + home.rot.y + unk_170
    // whenever it is pushed (z_bg_jya_cobra.c:496-502). The mirror is claimable,
    // so its update keeps running but never increments unk_16C — only the
    // pusher's copy does. The moment the mirroring player pushes in turn, that
    // stale index is written back over the streamed angle and the mirror SNAPS
    // to the orientation from before the peer's rotations. Rebuild the index
    // from the streamed angle, same idea as Bg_Haka's pull distance above.
    // unk_170 is the in-progress sweep, zero at rest, so the streamed angle is
    // a whole number of detents whenever it matters.
    if (actor->id == ACTOR_BG_JYA_COBRA) {
        BgJyaCobra* cobra = (BgJyaCobra*)actor;
        int32_t delta = (int16_t)(actor->shape.rot.y - actor->home.rot.y);
        // Round to nearest eighth rather than truncating: mid-sweep frames would
        // otherwise land a detent short and undo part of the peer's turn.
        cobra->unk_16C = (s16)(((delta + 0x1000) >> 13) & 7);
    }
    // The two sliding-ice-block puzzles keep their destination in targetPos, written
    // only at Init, at the end of a slide, and by Reset -- so never on the mirror,
    // whose update stays in Idle. That alone would be harmless, except
    // BgIceObjects_SetNextTarget refreshes ONE axis (z_bg_ice_objects.c:93 or :103)
    // while Slide steps BOTH (:166-167): the mirroring player's first push therefore
    // drags the block back along the OTHER axis, in a diagonal, and rebroadcasts it.
    // The Ice Cavern maze alternates axes by construction, so it breaks on the second
    // push whenever the two players take turns. Re-anchor the destination on the
    // streamed position each frame; at rest that is exactly what the authority holds.
    if (actor->id == ACTOR_BG_ICE_OBJECTS) {
        ((BgIceObjects*)actor)->targetPos = actor->world.pos;
    }
    // Same defect in Ganon's water trial, plus a grid index in a file static that the
    // overlay has to rebuild for us.
    if (actor->id == ACTOR_BG_GND_ICEBLOCK) {
        ((BgGndIceblock*)actor)->targetPos = actor->world.pos;
        BgGndIceblock_CoopRebuildGridIndex((BgGndIceblock*)actor);
    }
    st.suppressed = true;
    // Pinned obstacles keep their update RUNNING, unconditionally: the whole point of
    // the regime is that the mirror still submits its colliders and still damages its
    // OWN Link, from the position just written above (see IsPinnedDynaActor). They are
    // never claimable, so the branches below would suppress them.
    if (st.pinned) {
        return;
    }
    // Claimables keep their update RUNNING while mirroring: the block's own
    // update is what offers the grab (A) to our player, so a suppressed block
    // could never be claimed by anyone but the authority. Its idle AI can't
    // fight the stream — the position is overwritten every frame above, and the
    // first real push (dyna.unk_150) flips it to a local claim before this.
    // Ride-claimables are the exception: their update WOULD fight the pin (see
    // IsRideClaimable), and standing on them needs no update to be detected.
    if (!st.claimable || st.ride) {
        *should = false;
    }
    // DynaPoly_UpdateBgActorTransforms (end of Actor_UpdateAll) re-reads
    // world.pos/shape.rot afterwards, so the collision mesh follows.
}

void Ingest(uint32_t senderClientId, const nlohmann::json& payload) {
    try {
        if (!payload.contains("dynas")) {
            return;
        }
        bool fromAuthority = senderClientId == EnemySync::CurrentAuthorityId();
        for (const auto& e : payload["dynas"]) {
            if (!e.contains("key") || !e.contains("pos") || !e.contains("rot")) {
                continue;
            }
            bool owned = e.value("own", 0) != 0;
            // Accept the scene authority's stream, plus any claimant's own
            // entries. A stale non-authority sender with no claim is dropped.
            if (!owned && !fromAuthority) {
                continue;
            }
            uint64_t key = e["key"].get<uint64_t>();
            if (e.value("own", 0) == 2) {
                sRemoteSoft[key] = RemoteSoftClaim{ sTick, senderClientId };
            } else if (owned) {
                sRemoteClaims[key] = sTick;
            }
            RemoteDynaState& r = sRemote[key];
            r.rxTick = sTick;
            r.pos = e["pos"].get<Vec3f>();
            r.rot = e["rot"].get<Vec3s>();
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("[DynaSync] malformed DYNA_STATE: {}", e.what());
    }
}

static bool MovedSinceLastSend(const Actor* actor, const TrackedDyna& st) {
    return fabsf(actor->world.pos.x - st.lastSentPos.x) > POS_EPSILON ||
           fabsf(actor->world.pos.y - st.lastSentPos.y) > POS_EPSILON ||
           fabsf(actor->world.pos.z - st.lastSentPos.z) > POS_EPSILON ||
           actor->shape.rot.x != st.lastSentRot.x || actor->shape.rot.y != st.lastSentRot.y ||
           actor->shape.rot.z != st.lastSentRot.z;
}

void PerFrameTick() {
    sTick++;

    if (!SyncEnabled() || gPlayState == NULL || !Anchor::Instance->IsSaveLoaded()) {
        return;
    }

    bool isAuthority = EnemySync::IsLocalAuthority();
    bool keyframe = (sTick % KEYFRAME_TICKS) == 0;
    nlohmann::json entries = nlohmann::json::array();

    for (auto& [actor, st] : sTracked) {
        bool localClaim = st.claimable && Fresh(st.localClaimTick, CLAIM_TICKS);
        bool remoteClaim = false;
        bool softClaim = false;
        if (st.claimable) {
            auto rc = sRemoteClaims.find(st.key);
            remoteClaim = rc != sRemoteClaims.end() && Fresh(rc->second, CLAIM_TICKS);
            auto rs = sRemoteSoft.find(st.key);
            bool remoteSoft = rs != sRemoteSoft.end() && Fresh(rs->second.rxTick, CLAIM_TICKS);
            softClaim = !localClaim && !remoteClaim && Fresh(st.softClaimTick, CLAIM_TICKS) && !RemoteSoftWins(st);
            remoteClaim = remoteClaim || (remoteSoft && !softClaim);
        }
        // The claimant streams its blocks; the authority streams the rest.
        if (!(localClaim || softClaim || (isAuthority && !remoteClaim))) {
            continue;
        }
        if (!keyframe && st.everSent && !MovedSinceLastSend(actor, st)) {
            continue;
        }
        nlohmann::json e;
        e["key"] = st.key;
        e["pos"] = actor->world.pos;
        e["rot"] = actor->shape.rot;
        if (localClaim) {
            e["own"] = 1;
        } else if (softClaim) {
            e["own"] = 2;
        }
        entries.push_back(e);
        st.lastSentPos = actor->world.pos;
        st.lastSentRot = actor->shape.rot;
        st.everSent = true;
    }

    if (!entries.empty()) {
        Anchor::Instance->SendPacket_DynaState(entries);
    }
}

// True while this copy is a mirror of a fresh remote stream (authority or a
// remote claimant): its scripted motion is not ours to run.
bool IsRemoteDriven(const Actor* actor) {
    auto it = sTracked.find(const_cast<Actor*>(actor));
    return it != sTracked.end() && it->second.suppressed;
}

// Keep driving a claimable actor from this client for CLAIM_TICKS more ticks:
// our copy stays live and streams with "own":2 unless someone is pushing it or
// a lower client id holds its own soft claim. Called every frame by the module
// running the scripted sequence (the Amy puzzle), so a peer entering the room
// later mirrors the puzzle in progress instead of overriding it as authority.
void HoldSoftClaim(Actor* actor) {
    auto it = sTracked.find(actor);
    if (it != sTracked.end() && it->second.claimable) {
        it->second.softClaimTick = sTick;
    }
}

void RegisterHooks(bool isConnected) {
    // While disconnected the OnActorDestroy/OnSceneInit hooks are unregistered,
    // so the Actor* map would go stale; drop everything (EnemySync's HIGH-fix
    // discipline). Tracking rebuilds on the next scene load.
    if (!isConnected) {
        Reset();
    }

    // NOTE: PerFrameTick is NOT registered here; the Anchor per-frame
    // dispatcher (HookHandlers.cpp) drives it in explicit order.

    COND_HOOK(OnSceneInit, isConnected, [](int16_t sceneNum) { Reset(); });

    COND_HOOK(OnDynaPolySetBgActor, isConnected, [](void* actor, int32_t bgId) { OnDynaRegistered((Actor*)actor); });

    // Whitelisted spinners with no DynaPoly collision never reach the hook
    // above; track them from their plain actor init.
    COND_ID_HOOK(OnActorInit, ACTOR_BG_SPOT01_FUSYA, isConnected,
                 [](void* actor) { OnDynaRegistered((Actor*)actor); });

    // Same door for the pinned obstacles: neither registers DynaPoly, so
    // OnDynaPolySetBgActor never fires for them. OnActorInit runs AFTER the actor's own
    // Init (z_actor.c:1265-1271), which is what makes the key usable: EnGoroiwa_Init has
    // already run EnGoroiwa_TeleportToWaypoint(.., 0) (:555), so PosKey quantises path
    // waypoint 0 -- the same value on every client -- and EnTrap_Init has already masked
    // params to its low byte (:72), which is what IsPinnedDynaActor reads.
    COND_ID_HOOK(OnActorInit, ACTOR_EN_GOROIWA, isConnected, [](void* actor) { OnDynaRegistered((Actor*)actor); });
    COND_ID_HOOK(OnActorInit, ACTOR_EN_TRAP, isConnected, [](void* actor) { OnDynaRegistered((Actor*)actor); });

    COND_HOOK(OnActorDestroy, isConnected, [](void* actor) { sTracked.erase((Actor*)actor); });

    COND_HOOK(ShouldActorUpdate, isConnected,
              [](void* actor, bool* should) { OnShouldDynaUpdate((Actor*)actor, should); });
}

} // namespace DynaSync
