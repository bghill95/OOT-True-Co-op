# OOT True Coop decisions

Append only, oldest first. An entry is never rewritten: when a later decision replaces it, its Status becomes Superseded and it gains a last line `- Superseded by: DEC-nnn`. Alternatives and rationale come only from the cited record; where the record states none, the field reads `not recorded`. The record here is the root `README.md` and the commit messages, since the repository keeps no decision log; dates are UTC, and a commit's local date can be a day earlier.

## DEC-001: Sync enemies over Anchor, starting with shared HP (M1)
- Date: 2026-07-03
- Status: Accepted
- Context: Stock Anchor syncs players, inventory, save flags and room clears between game instances, but every player fights their own copies of the enemies (root `README.md`).
- Decision: Add an enemy-sync layer to the Anchor code, first as shared HP: every enemy and boss actor is tracked under a deterministic identity (its room and a per-room occurrence index), local damage is broadcast, and each receiver replays the hit through the enemy's own collision-damage path (27f6fcb).
- Alternatives considered: not recorded
- Rationale: two players should fight the same monsters together (root `README.md`); replaying through the enemy's own damage path lets hurt animations, knockback and deaths play out naturally (27f6fcb).
- Consequences: Both clients still ran their own enemy AI, so enemy positions were not mirrored, a known M1 limitation left to M2 (27f6fcb; DEC-002); the layer lives in `soh/soh/Network/Anchor/` (root `README.md`).
- References: 27f6fcb; root `README.md` ("What works so far", "How it's built")

## DEC-002: Host-authority enemy mirroring (M2)
- Date: 2026-07-04
- Status: Accepted
- Context: Under M1 each client ran its own enemy AI (27f6fcb).
- Decision: One same-scene client, the authority (the lowest client id), runs the enemy AI and streams every tracked enemy's position, rotation, health and joint state each frame; the other clients suppress those enemies' updates and apply the stream; a mirror's hits go to the authority, which applies them through the enemy's own damage code; an enemy with no fresh stream falls back to local AI (ec103b7).
- Alternatives considered: not recorded
- Rationale: vulnerability windows and i-frames are then checked by the enemy's own code (ec103b7; root `README.md`, M2).
- Consequences: The root `README.md` lists as a limitation that enemies aim at the authority player's Link; c1187fb later pointed the authority's enemy perception at the nearest player, and f1c05e2 limited the authority to a living or reviving player, with a fallback when both are dead. Deaths and item drops stay local on both screens (ec103b7).
- References: ec103b7, c1187fb, f1c05e2; root `README.md` (M2)

## DEC-003: Boss adapters through one interface, Gohma first (M3)
- Date: 2026-07-04
- Status: Accepted
- Context: M3 makes Gohma the first co-op boss; ec103b7 adds a boss adapter framework of per-actor extras and phase callbacks, used for Gohma's eye, environment and tail extras and for its intro, fight and defeat phases.
- Decision: Each boss gets an adapter behind the `ActorSyncAdapter` interface (`soh/soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h`); Gohma's adapter is the first (ec103b7; root `README.md`, "How it's built").
- Alternatives considered: not recorded
- Rationale: more bosses are added by writing new adapters rather than new sync plumbing (root `README.md`).
- Consequences: Gohma became a co-op boss fight (root `README.md`, M3); b738c76 added a Deku Baba adapter through the same interface; more boss adapters are on the roadmap (`ROADMAP.md`).
- References: ec103b7, b738c76; `soh/soh/Network/Anchor/BossAdapters/ActorSyncAdapter.h`; root `README.md`

## DEC-004: Floormasters stay out of mirroring
- Date: 2026-07-04
- Status: Accepted
- Context: Floormasters split and merge, through parent and child actor logic (ec103b7; root `README.md`).
- Decision: Exclude Floormasters (`En_Floormas`) from mirroring; they keep M1's shared HP (ec103b7).
- Alternatives considered: not recorded
- Rationale: their split and merge logic is not mirror-safe (ec103b7; root `README.md`).
- Consequences: A known limitation in the root `README.md` and `COOP-TEST-GUIDE.md`; c1187fb leaves the permanently local Floormaster out of the nearest-player perception override.
- References: ec103b7, c1187fb; root `README.md`; `COOP-TEST-GUIDE.md`

## DEC-005: A kill switch back to shared-HP mode
- Date: 2026-07-04
- Status: Accepted
- Context: M2's mirroring runs on top of M1's shared HP (ec103b7).
- Decision: The CVar `EnemySyncMirroring` turns mirroring off and falls back to M1's shared HP (ec103b7; root `README.md`).
- Alternatives considered: not recorded
- Rationale: a fallback "if mirroring ever misbehaves" (root `README.md`, "How it's built").
- Consequences: 0599601 made turning it off mid-scene run one cleanup sweep instead of stranding enemies; `COOP-TEST-GUIDE.md` names it as the fallback when something looks wrong.
- References: ec103b7, 0599601; root `README.md`; `COOP-TEST-GUIDE.md`

## DEC-006: Controller isolation in a libultraship fork, as a submodule
- Date: 2026-07-04
- Status: Accepted
- Context: Two players on one PC need each game instance pinned to its own gamepad and receiving input while unfocused (root `README.md`, "Controller isolation").
- Decision: The input changes live in the companion fork https://github.com/bghill95/libultraship (branch `enemy-sync-input`), and `.gitmodules` points the `libultraship` submodule at it (f3ff1fb); ec103b7 and 0599601 moved the submodule pointer.
- Alternatives considered: not recorded
- Rationale: not recorded beyond the feature's purpose, two players on one PC (root `README.md`).
- Consequences: A build needs the submodule from the companion fork (root `README.md`, "Running it"); one of the seven review fixes in 0599601, gamepad hot-plug pinning, is in that fork.
- References: f3ff1fb, ec103b7, 0599601; `.gitmodules`; root `README.md`

## DEC-007: One ordered per-frame dispatcher for the Anchor modules
- Date: 2026-07-05
- Status: Accepted
- Context: The game's hook runner iterates an unordered map, so the Anchor modules' assumptions about which per-frame tick runs first rested on an undefined order; a final review marked this Important (b906a25, FIX B).
- Decision: Replace the seven separate per-frame registrations with one Anchor dispatcher that calls the ticks in a documented order, from the packet drain and the life-state tick through enemy sync to music sync (b906a25).
- Alternatives considered: not recorded
- Rationale: some modules read state that another sets earlier in the same frame, such as the life state that enemy sync and music sync read (b906a25).
- Consequences: No tick body changed, and each tick's connection condition and the disconnect teardown were kept (b906a25).
- References: b906a25
