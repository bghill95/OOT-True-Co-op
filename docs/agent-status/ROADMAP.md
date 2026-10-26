# OOT True Coop roadmap

Each item under Planned milestones and Future development ideas carries one label: [confirmed by owner] (the owner's word, with its date and source), [approved by record] (an owner decision written in the repository, cited) or [suggestion] (an agent's proposal, not approved). Priorities come from the owner, never from commit activity. A section with nothing on record holds exactly: No roadmap is recorded for this project; none is invented.

## Current milestone
None: the project is recorded as Paused (unconfirmed) (`STATUS.md`, Lifecycle status), and no milestone is recorded after M3, dynamic spawn sync and the first co-op boss (Gohma), which ec103b7 implemented (root `README.md`, "What works so far"). The play-test script for M2 and Gohma is in `COOP-TEST-GUIDE.md`; no result of it is recorded.

## Confirmed priorities
No priority is confirmed for the paused project. The root `README.md` (f3ff1fb, 2026-07-04) called nearest-player targeting "the next planned step" (Planned milestones).

## Planned milestones
- More boss adapters through the same `ActorSyncAdapter` interface (root `README.md`, "Known limitations / roadmap", f3ff1fb); since then b738c76 added a Deku Baba adapter, and the description of the open, unreviewed pull request #2 lists adapters for further bosses [approved by record]
- Nearest-player aggro, "the next planned step" (root `README.md`, same section); c1187fb's commit message says authority enemies now perceive the nearest player, and the README was not updated afterwards, so whether the item is done is the owner's call [approved by record]
- Mirrored enemy sound effects ("mirrored SFX"; enemy AI sound effects play only on the authority's screen) (root `README.md`, same section); the description of pull request #2 says enemy sounds are streamed [approved by record]

## Dependencies
- Ship of Harkinian (https://github.com/HarbourMasters/Shipwright) and the Anchor mod (https://github.com/garrettjoecox/OOT) upstream: this fork's base is bfa3813, the head of the Anchor repository's `anchor-next` branch (checked 2026-10-26).
- The companion libultraship fork (https://github.com/bghill95/libultraship, branch `enemy-sync-input`) for controller isolation, pinned as the `libultraship` submodule (`.gitmodules`; root `README.md`).
- For every play session: two game instances, a legally dumped copy of the game and an Anchor relay server (https://github.com/garrettjoecox/anchor) (root `README.md`, "Running it").
- A build with remote control enabled: the CI build passes `-DBUILD_REMOTE_CONTROL=1` (`.github/workflows/generate-builds.yml`), and the comments on issue #1 trace a startup crash to a build without it.
- Pull request #2 (open, from an outside contributor's fork) changes 177 files on top of `enemy-sync` (17814 lines added, 981 removed) and, by its description, covers two of the planned items.

## Known risks
- The limitations the root `README.md` lists (as of f3ff1fb): enemy AI sound effects and particles play only on the authority's screen; Floormasters do not mirror and share HP only; Gohma's blue warp can appear in a slightly different spot on each screen; if one game crashes, the other's enemies pause about 1.5 s and then resume under local AI.
- The root `README.md` and `COOP-TEST-GUIDE.md` describe the state at f3ff1fb; the sixteen later commits are described only in their commit messages.
- The head b738c76 has no CI build result: its run stopped at "Install latest SDL", before any build job (`VERIFICATION.md`).
- The rig, the packet injector and the play-test launcher that the commit messages and the test guide rely on are not in the repository, so their checks cannot be repeated from it.

## Future development ideas
- Bring the root `README.md` and `COOP-TEST-GUIDE.md` up to date with the sixteen commits after f3ff1fb (agent's proposal) [suggestion]
- Name the `BUILD_REMOTE_CONTROL` build flag in the root `README.md`'s build step; the reporter of issue #1 suggested adding their build command, which sets it (agent's proposal) [suggestion]
