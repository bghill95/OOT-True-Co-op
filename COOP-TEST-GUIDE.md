# Co-op Enemy Mirroring — Play Test Guide (M2 + Gohma)

## What's new since your last test

- **Enemies mirror**: one game (the "authority") runs enemy AI; the other shows the exact same enemies — same position, movement, animation. When a mob activates because one player got close, the other player sees the same thing.
- **Hits work from both sides**: the mirrored player's hits are validated by the authority through the enemy's own damage code (vulnerability windows, i-frames all respected). Deaths and item drops still happen naturally on both screens.
- **Dynamic spawns sync**: Stalchildren at night, Gohma larvae, Octorok rocks — spawn on both screens.
- **Gohma is a co-op boss fight** (first boss slice; others follow the same adapter pattern later).
- **Controller isolation**: each instance is pinned to its own Xbox pad (test-a = first pad, test-b = second pad), and pads keep working when the window isn't focused. Keyboard always drives the focused window.

## How to launch

1. Plug in both Xbox controllers **before** launching (pad order = connection order; if the pads feel swapped, swap the physical controllers or edit `Controllers.Port0GamepadOrdinal` in each instance's `shipofharkinian.json`).
2. Run `launch-coop-test.bat`. Both instances auto-load their save (File 1 / File 2) — no menu input needed.
3. Optional fast travel for testing: set `"TestAutoWarpEntrance"` in `CVars.gRemote.Anchor` (in each `shipofharkinian.json`) to `0` (Deku Tree lobby) or `1039` (Gohma's arena, 0x40F). `-1` = off. Both saves already have the "began Gohma battle" flag set, so the boss intro is the short version.

## Test script

1. **Deku Baba mirror** (Deku Tree lobby): Player A walks up to a Baba while B watches from a distance. Expected: it emerges and lunges identically on both screens, tracking A. Then B kills one: hit lands, death animation + drop appear on **both** screens.
2. **Second enemy type**: repeat on a Skulltula — proves the generic path.
3. **Contact damage**: B stands in an enemy's attack — B takes damage on B's screen.
4. **Gohma intro**: enter the boss room together. Each screen plays its own (short) intro; afterwards one Gohma, same spot, same movement on both screens.
5. **Vulnerability windows**: A stuns Gohma (slingshot to the red eye), B sword-hits the stunned eye — damage counts once, eye/flash visuals match. B attacking the closed eye does nothing on either screen (the authority's Gohma rejects it).
6. **Larvae wave**: Gohma climbs and lays eggs — eggs fall and hatch on both screens. B kills one: dies on both, no crash (this exercises the parent-pointer fix).
7. **Defeat**: final blow from either player. Defeat cutscene plays on both screens; heart container appears for both (one grab shares it); blue warp appears (its exact spot may differ slightly between screens — known cosmetic); room stays cleared after re-entry.
8. **Input isolation**: both pads at once with only one window focused — each pad must only drive its own window.

## Known limitations (accepted for this round)

- Enemies aim at the authority player's Link (the other player can still hit and be hit). Nearest-player targeting is the first follow-up.
- Enemy sound effects/particles that come from enemy AI play only on the authority's screen; hit sounds are local everywhere.
- Floormasters intentionally don't mirror (their split/merge logic isn't mirror-safe) — they still share HP the M1 way.
- Blue warp position after Gohma may differ between screens.
- If one game hard-crashes, the other's enemies freeze ~1.5 s, then resume under local AI.

## If something looks wrong

Logs: `test-a\logs\Ship of Harkinian.log` / `test-b`. Grep for `Fuzzy`, `No match`, `limb mismatch`, `ghost-kill` — those are the desync canaries. Kill switch: set `"EnemySyncMirroring": 0` under `CVars.gRemote.Anchor` to fall back to pure M1 (shared HP only).
