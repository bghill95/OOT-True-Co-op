# OOT True Coop

Turning *The Legend of Zelda: Ocarina of Time* into a **true local co-op game**: two windows, two controllers, one shared world — where the enemies and bosses are actually shared, not just the overworld.

This is a fork of [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) (the OoT PC port) built on top of [garrettjoecox's Anchor](https://github.com/garrettjoecox/OOT) multiplayer mod. Stock Anchor already syncs players, inventory, save flags, and room clears between game instances — but every player fights their *own* copies of the enemies. This project adds the missing piece: **synced enemies and bosses**, so two players fight the same monsters together.

## What works so far

- **M1 — Shared HP:** enemies and bosses share one HP pool across both games; a kill on one screen is a kill on both.
- **M2 — Host-authority enemy mirroring:** one instance (the "authority") runs the enemy AI; the other renders the exact same enemies — position, movement, and animation. Hits from the mirrored player are validated by the authority through the enemy's real damage code, so vulnerability windows and i-frames are respected. Deaths and item drops happen naturally on both screens.
- **M3 — Dynamic spawn sync + first co-op boss:** runtime spawns (Stalchildren at night, Gohma larvae, Octorok rocks) appear on both screens, and **Gohma is a fully co-op boss fight** — stun her with one player, slash the eye with the other.
- **Controller isolation:** each game instance is pinned to its own gamepad and keeps receiving input while unfocused, so two players can play on one PC. This lives in the companion [libultraship fork](https://github.com/bghill95/libultraship) (branch `enemy-sync-input`), which this repo pulls in as a submodule.

## How it's built

The enemy-sync layer lives in [`soh/soh/Network/Anchor/`](soh/soh/Network/Anchor/): an `EnemySync` core, ~25 packet types for the Anchor relay, hook handlers into the game's collision/skeleton-animation code, and a **boss adapter pattern** (`ActorSyncAdapter`) — Gohma's adapter is the first, and more bosses are added by writing new adapters rather than new sync plumbing. A CVar kill switch (`EnemySyncMirroring`) falls back to plain shared-HP mode if mirroring ever misbehaves.

## Running it

1. Build this repo (see the upstream [Ship of Harkinian build docs](https://github.com/HarbourMasters/Shipwright/blob/develop/docs/BUILDING.md)) — the `libultraship` submodule already points at the co-op fork.
2. You need your **own, legally dumped** copy of the game. No ROMs or game assets are included in (or accepted into) this repository.
3. Run two game instances plus an [Anchor relay server](https://github.com/garrettjoecox/anchor), point both at the same room, and enable enemy sync under `CVars.gRemote.Anchor` in each instance's `shipofharkinian.json`.
4. Full details, a play-test script, and troubleshooting (desync log canaries, the kill switch) are in [COOP-TEST-GUIDE.md](COOP-TEST-GUIDE.md).

## Known limitations / roadmap

- Enemies aim at the authority player's Link; nearest-player targeting is the next planned step.
- Enemy AI sound effects/particles play only on the authority's screen (hit sounds are local everywhere).
- Floormasters don't mirror yet (their split/merge logic isn't mirror-safe) — they share HP the M1 way.
- Gohma's blue warp can appear in a slightly different spot on each screen (cosmetic).
- If one game crashes, the other's enemies pause ~1.5 s and then resume under local AI.
- Roadmap: more boss adapters through the same `ActorSyncAdapter` interface, nearest-player aggro, mirrored SFX.

## Credits

All the heavy lifting of getting OoT running natively on PC is the [HarbourMasters / Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) team's work, and the multiplayer foundation is [garrettjoecox's Anchor](https://github.com/garrettjoecox/OOT) ([relay server](https://github.com/garrettjoecox/anchor)). This fork just teaches the enemies to show up on both screens. We do not condone piracy — bring your own cartridge dump.
