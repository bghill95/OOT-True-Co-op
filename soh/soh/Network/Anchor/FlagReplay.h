#ifndef NETWORK_ANCHOR_FLAG_REPLAY_H
#define NETWORK_ANCHOR_FLAG_REPLAY_H
#ifdef __cplusplus

/**
 * FlagReplay - live reaction to REMOTE scene flags for actors that only read
 * their flag at Init.
 *
 * SET_FLAG already converges the save state, and most flag-gated actors re-test
 * their flag every frame (En_Box, Obj_Switch, ...) so they react on their own.
 * A handful read it ONCE in *_Init and then sit in a wait state driven purely by
 * local interaction (a key, an explosion, a lift). When the peer performs that
 * interaction, our copy gets the flag but never re-reads it: the door stays
 * shut, the stairs stay up, the pillar stays standing until the scene reloads.
 *
 * Each hook here mirrors the base layer's HookHandlers pattern (BG_BOMBWALL &
 * co): a ShouldActorUpdate hook that, while the actor is still in its untouched
 * wait state and the flag is set, replays the vanilla transition - WITHOUT the
 * side effects that already rode the network (key decrement, flag set) and
 * without hijacking the peer's camera (no OnePointCutscene).
 *
 * Covered: DOOR_GERUDO (cell grates: the key was already consumed on both
 * sides by the dungeon-items sync), BG_DDAN_KD (Dodongo stairs: explosion-
 * triggered, and bomb replicas have no AT), BG_JYA_MEGAMI (Spirit statue face,
 * same explosion reason), BG_HEAVY_BLOCK (golden-gauntlet pillar, not carried
 * through CarrySync), OBJ_BEAN (child planting).
 *
 * Not covered on purpose: actors Actor_Kill'ed at Init when the flag is unset
 * (BG_JYA_BLOCK) - nothing left to react; per-client consumables (OBJ_LIFT,
 * DOOR_KILLER) - replaying would remove a platform/door the peer has not used.
 */
namespace FlagReplay {

void RegisterHooks(bool isConnected);

} // namespace FlagReplay

#endif // __cplusplus
#endif // NETWORK_ANCHOR_FLAG_REPLAY_H
