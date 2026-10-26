# OOT True Coop status

## Project and repository
OOT True Coop: a fork of Ship of Harkinian (https://github.com/HarbourMasters/Shipwright, the Ocarina of Time PC port) built on the Anchor multiplayer mod (https://github.com/garrettjoecox/OOT); the fork adds synced enemies and bosses, so that two players on one PC fight the same monsters together (root `README.md`).
Repository: https://github.com/bghill95/OOT-True-Co-op (public, default branch `enemy-sync`). Its history up to bfa3813 (2025-11-14), the head of the Anchor repository's `anchor-next` branch (checked 2026-10-26), is upstream work; the fork's own work is the 20 commits from 27f6fcb (2026-07-03) to b738c76 (2026-07-12). Controller isolation lives in the companion fork https://github.com/bghill95/libultraship (public, branch `enemy-sync-input`), which `.gitmodules` names as the `libultraship` submodule's source; the submodule commit is that branch's head (checked 2026-10-26).

## Business Context
- Project type: Personal (the owner, 2026-10-25)
- Parent product: Not applicable
- Component role: Not applicable
- Related repositories: Not applicable
- Commercialization status: Not applicable
- Client ID: Not Applicable (Personal by the owner's classification, 2026-10-25)
- Engagement ID: Not Applicable (Personal by the owner's classification, 2026-10-25)
- Compensation model: Not Applicable (Personal by the owner's classification, 2026-10-25)
- Contract status: Not Applicable (Personal by the owner's classification, 2026-10-25)
- Contract reference: Not Applicable (Personal by the owner's classification, 2026-10-25)
- Commercial owner: Not Applicable (Personal by the owner's classification, 2026-10-25)
- Associated business deliverables: None recorded
- Current work billing status: Non-billable
- This change's billing class: Non-billable (documentation only)
- Source of classification and date confirmed: the owner's classification, 2026-10-25

## Lifecycle status
Paused (project status, not a technical state; a planning seed of the Navideus Agent Reporting Standard v1.1 rollout, 2026-10-25, not yet confirmed by the owner. The repository records no pause and no reason for one, and is consistent with it: no commit on `enemy-sync` since b738c76 on 2026-07-12; the open pull request #2 and issue #1, both from outside the project, have no comment from the owner; not archived on GitHub)

## Development phase
Recorded as Paused (unconfirmed, see Lifecycle status) after milestones M1 to M3 and follow-on co-op work. M1 (shared enemy and boss HP) is 27f6fcb; M2 and M3 (host-authority mirroring, dynamic spawn sync, Gohma as a co-op boss) are ec103b7, with seven review fixes in 0599601; f3ff1fb (2026-07-04) added the root `README.md` and `COOP-TEST-GUIDE.md`. Sixteen later commits, baa611c (2026-07-04) to b738c76 (2026-07-12), add, by their commit messages, a lossless hit pipeline, nearest-player perception for authority enemies, projectile and quad-collider sync, a companion fairy per remote player, boss-room co-entry, co-op death with spectate and revival, cutscene sync, a shared rupee wallet, music sync and a Deku Baba mirror adapter. Last known state: the head b738c76, whose CI build stopped in a dependency step, before the project's own code was built (`VERIFICATION.md`); the README and the test guide still describe the state at f3ff1fb.

## Current milestone
No milestone is recorded after M3. M1 to M3 are the historical milestone record (root `README.md`, "What works so far"); the work after f3ff1fb carries no milestone name, and the plan its commit messages cite (workstream W2 in cdb03d3, Feature C in 76d27db, numbered tasks) is not in the repository.

## Implementation status
A block for the default branch and for each branch holding unmerged work, at most three; every other branch is one line above the first block. States per README.md "Lifecycle states", revisions per "Revision traceability". Dates are UTC; a commit's local date can be a day earlier.

No other branch exists: the remote holds `enemy-sync` only, apart from the records' own `agent-status`; the unmerged work of pull request #2 sits on a branch of the contributor's own fork, not of this repository (checked 2026-10-26).

### enemy-sync
- Head: b738c76 (2026-07-12), the default branch
- Last code commit: b738c76 (2026-07-12): a Deku Baba mirror adapter (`soh/soh/Network/Anchor/BossAdapters/EnDekubabaAdapter.cpp`), streamed actor scale, and the collider cap raised from 6 to 12
- Implemented: yes, at b738c76: milestones M1 to M3 (27f6fcb, ec103b7) and the sixteen later co-op commits (Development phase), by the root `README.md` and the commit messages
- Merged: is the default branch (the fork's 20 commits are linear, with no merge commit)
- Tested: a rig check at b738c76 reported Passed in its own commit message (two instances, a zero-input boot and a packet injector; command and output not recorded), not re-run; its CI build Failed at the "Install latest SDL" step, before any build job ran (`VERIFICATION.md`, Cited results)
- Independently reviewed: Not Verified: no review record covers b738c76; the earlier reviews that commit messages cite (0599601, f1c05e2, b906a25) are listed in `VERIFICATION.md`
- Deployed: Not applicable: the repository defines no deployment environment; players build the game and run two instances locally with an Anchor relay server (root `README.md`, "Running it")
- Released: Not applicable: no release process is defined (no tags, no GitHub releases); the root `README.md` has players build from source

## Verification status
- Records check: Passed on the files of the commit that carries these records (VER-001, Opus)
- CI build (`generate-builds` workflow, no test step): Passed at f3ff1fb (2026-07-04); Failed at b738c76 (2026-07-12) before any build job ran; no run exists for the commits between them; cited, not re-run (`VERIFICATION.md`)
- Developer checks reported in commit messages: Passed at ec103b7, 0599601 and b738c76 (a two-instance rig, with a packet injector at the latter two) and at faf0cf2 (music seek), cited; not re-run
- Play-test script (`COOP-TEST-GUIDE.md`): no result recorded; Not Run
- Independent review: Not Verified at b738c76

## Open blockers
- None recorded in the repository; the reason for the pause is not recorded (Uncertainties).

## Next concrete engineering action
None is on record as the owner's: the project is recorded as Paused (unconfirmed) and resuming is the owner's decision. Resuming requires, by the root `README.md` ("Running it"), a build with the `libultraship` submodule from the companion fork (the upstream build docs; a copy is `docs/BUILDING.md`), a legally dumped copy of the game and an Anchor relay server (`ROADMAP.md`, Dependencies); the play-test launcher and instance folders that `COOP-TEST-GUIDE.md` uses (`launch-coop-test.bat`, `test-a`, `test-b`) are not in the repository. Suggestion (agent, not approved): decide on pull request #2 and answer issue #1 first, then get a CI build of `enemy-sync` past its dependency step and record it in `VERIFICATION.md`.

## GitHub issue/PR links
- Checked 2026-10-26: one open pull request and one open issue, both from outside the project; no release, no tag, and no comment from the owner on either.
- https://github.com/bghill95/OOT-True-Co-op/pull/2: open, not a draft, from an outside contributor's fork, opened 2026-09-14, updated 2026-10-03: "Sync the rest of the shared world: objects, effects, bosses, and the endgame" (125 commits, 177 files changed); no review. The build and format runs that the owner's account started for it on 2026-09-17 failed, and the runs for its latest push (2026-10-03) await a maintainer's approval (https://github.com/bghill95/OOT-True-Co-op/actions/workflows/generate-builds.yml?query=branch%3Acoop-world-sync).
- https://github.com/bghill95/OOT-True-Co-op/issues/1: open, opened 2026-08-27, updated 2026-09-15: a Windows build that crashes at startup; a commenter, the author of pull request #2, traces it to building without `BUILD_REMOTE_CONTROL` (2026-09-14), a flag the CI build sets (`.github/workflows/generate-builds.yml`) and the root `README.md` does not mention.

## Revisions
- Default-branch commit: b738c76 (2026-07-12)
- Implementation commit: b738c76
- Commit tested: b738c76 (its rig check cited from the commit message, and its CI build Failed in a dependency step, before the project's own code was built; neither re-run)
- Commit reviewed: Not Verified
- Commit deployed: Not applicable: no deployment environment is defined (a game mod built and run locally)
- Records revision: this commit

## Last verified
2026-10-26T06:55Z (Opus: the standard's checker on these records, VER-001; Fable's verification has not run yet)

## Uncertainties
- Paused is the rollout's planning seed, not confirmed; the reason for the pause is not recorded (the owner can settle both).
- Where the plan that commit messages cite (workstream W2, Feature C, numbered tasks), the review findings file `enemy-sync-review-findings.md` named in 0599601, and the two-instance rig with its packet injector live: none is in the repository (the owner).
- Whether the README's next planned step, nearest-player targeting, is done: c1187fb's commit message says authority enemies now perceive the nearest player, and the README has not changed since f3ff1fb (the owner).
- What pull request #2 holds beyond its description, and whether it will be reviewed, merged or closed (the owner).
- Why the CI build at b738c76 failed at "Install latest SDL": the run record names the step only, so the code at b738c76 has no CI build result.
